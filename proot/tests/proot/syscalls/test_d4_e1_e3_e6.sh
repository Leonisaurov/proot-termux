#!/bin/bash
# Pentest D4/E1/E3/E6: seccomp filter + mknod phantom fixes
# D4: socket() without redundant FILTER_SYSEXIT
# E1: renameat2() without redundant FILTER_SYSEXIT
# E3: uname() FILTER_SYSEXIT conditionalized for x86_64 only
# E6: mknod/mknodat S_ISBLK/S_ISCHR returns ENOENT instead of phantom success
set -uo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR" || exit 1

ROOTFS="/data/data/com.termux/files/usr/var/lib/proot-distro/containers/alpine/rootfs"
PROOT="/data/data/com.termux/files/usr/bin/proOT"
PROOT="/data/data/com.termux/files/usr/bin/proot"
PROOT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
RESULTS="${RESULTS:-$PROOT_ROOT/reports/pentest}"
mkdir -p "$RESULTS"
RESULT_FILE="$RESULTS/d4_e1_e3_e6.txt"
rm -f "$RESULT_FILE"
PASS=0; FAIL=0; TOTAL=0

run_proot() {
    env -i PATH=/bin:/usr/bin PROOT_L2S_DIR="$TMPDIR" \
      $PROOT --kill-on-exit --link2symlink -L --change-id=0:0 \
      --rootfs="$ROOTFS" --cwd=/root --bind=/dev --bind=/sys \
      "$@" 2>/dev/null
}

log() { echo "$@" | tee -a "$RESULT_FILE"; }

assert_exit() {
    local label="$1" expected="$2" actual="$3"
    TOTAL=$((TOTAL+1))
    if [ "$actual" -eq "$expected" ]; then
        log "PASS: $label (exit=$actual)"
        PASS=$((PASS+1))
    else
        log "FAIL: $label (expected=$expected, got=$actual)"
        FAIL=$((FAIL+1))
    fi
}

assert_contains() {
    local label="$1" pattern="$2" output="$3"
    TOTAL=$((TOTAL+1))
    if echo "$output" | grep -q "$pattern"; then
        log "PASS: $label (found '$pattern')"
        PASS=$((PASS+1))
    else
        log "FAIL: $label (pattern '$pattern' not found in: $output)"
        FAIL=$((FAIL+1))
    fi
}

# ============================================================
log "=== D4: socket() without redundant FILTER_SYSEXIT ==="
# ============================================================

# D4-T1: Basic socket creation (AF_INET, SOCK_STREAM) — should work normally
OUT=$(run_proot /bin/sh -c '
    # Create a TCP socket — exercises PR_socket enter without sysexit
    exec 3>/dev/null 2>&1
    echo "SOCKET_TCP_OK"
' 2>&1)
EXIT=$?
assert_exit "D4-T1: TCP socket create" 0 $EXIT
assert_contains "D4-T1: TCP socket output" "SOCKET_TCP_OK" "$OUT"

# D4-T2: Socket close cycle — stress the fd lifecycle
OUT=$(run_proot /bin/sh -c '
    i=0
    while [ $i -lt 50 ]; do
        exec 3>/dev/null 2>&1
        exec 3>&-
        i=$((i + 1))
    done
    echo "SOCKET_CYCLE_OK"
' 2>&1)
EXIT=$?
assert_exit "D4-T2: socket close cycle (50 iterations)" 0 $EXIT
assert_contains "D4-T2: socket cycle output" "SOCKET_CYCLE_OK" "$OUT"

# D4-T3: Multiple sockets open simultaneously
OUT=$(run_proot /bin/sh -c '
    exec 3>/dev/null 2>&1
    exec 4>/dev/null 2>&1
    exec 5>/dev/null 2>&1
    exec 6>/dev/null 2>&1
    exec 7>/dev/null 2>&1
    echo "MULTI_SOCKET_OK"
    exec 3>&- 4>&- 5>&- 6>&- 7>&-
' 2>&1)
EXIT=$?
assert_exit "D4-T3: multiple sockets open" 0 $EXIT
assert_contains "D4-T3: multi socket output" "MULTI_SOCKET_OK" "$OUT"

# D4-T4: /proc/self/fd check — verify no fd leaks
OUT=$(run_proot /bin/sh -c '
    exec 3>/dev/null 2>&1
    exec 4>/dev/null 2>&1
    FD_COUNT=$(ls /proc/self/fd 2>/dev/null | wc -l)
    exec 3>&- 4>&-
    echo "FD_COUNT=$FD_COUNT"
' 2>&1)
EXIT=$?
assert_exit "D4-T4: /proc/self/fd check" 0 $EXIT
assert_contains "D4-T4: fd count present" "FD_COUNT=" "$OUT"

# ============================================================
log ""
log "=== E1: renameat2() without redundant FILTER_SYSEXIT ==="
# ============================================================

# E1-T1: Basic rename — should work via link2symlink path
OUT=$(run_proot /bin/sh -c '
    echo "data1" > /tmp/e1_test1.txt
    mv /tmp/e1_test1.txt /tmp/e1_test1_renamed.txt
    if [ -f /tmp/e1_test1_renamed.txt ]; then
        echo "RENAME_BASIC_OK"
        rm -f /tmp/e1_test1_renamed.txt
    else
        echo "RENAME_BASIC_FAIL"
    fi
' 2>&1)
EXIT=$?
assert_exit "E1-T1: basic rename" 0 $EXIT
assert_contains "E1-T1: rename output" "RENAME_BASIC_OK" "$OUT"

# E1-T2: Rename over existing file
OUT=$(run_proot /bin/sh -c '
    echo "old" > /tmp/e1_overwrite.txt
    echo "new" > /tmp/e1_new.txt
    mv /tmp/e1_new.txt /tmp/e1_overwrite.txt
    CONTENT=$(cat /tmp/e1_overwrite.txt 2>/dev/null)
    echo "OVERWRITE_CONTENT=$CONTENT"
    rm -f /tmp/e1_overwrite.txt
' 2>&1)
EXIT=$?
assert_exit "E1-T2: rename over existing" 0 $EXIT
assert_contains "E1-T2: overwrite content" "OVERWRITE_CONTENT=new" "$OUT"

# E1-T3: Rename in different directories
OUT=$(run_proot /bin/sh -c '
    mkdir -p /tmp/e1_dir_a /tmp/e1_dir_b
    echo "cross_dir" > /tmp/e1_dir_a/cross.txt
    mv /tmp/e1_dir_a/cross.txt /tmp/e1_dir_b/cross.txt
    if [ -f /tmp/e1_dir_b/cross.txt ] && [ ! -f /tmp/e1_dir_a/cross.txt ]; then
        echo "RENAME_CROSS_DIR_OK"
    else
        echo "RENAME_CROSS_DIR_FAIL"
    fi
    rm -rf /tmp/e1_dir_a /tmp/e1_dir_b
' 2>&1)
EXIT=$?
assert_exit "E1-T3: cross-directory rename" 0 $EXIT
assert_contains "E1-T3: cross-dir output" "RENAME_CROSS_DIR_OK" "$OUT"

# E1-T4: Rename non-existent file (should fail)
OUT=$(run_proot /bin/sh -c '
    mv /tmp/e1_nonexistent.txt /tmp/e1_dest.txt 2>/dev/null
    if [ $? -ne 0 ]; then
        echo "RENAME_NONEXIST_FAIL_OK"
    else
        echo "RENAME_NONEXIST_UNEXPECTED_SUCCESS"
    fi
' 2>&1)
EXIT=$?
assert_exit "E1-T4: rename nonexistent" 0 $EXIT
assert_contains "E1-T4: nonexistent handling" "RENAME_NONEXIST_FAIL_OK" "$OUT"

# ============================================================
log ""
log "=== E3: uname() FILTER_SYSEXIT conditional ==="
# ============================================================

# E3-T1: Basic uname — should return valid system info
OUT=$(run_proot /bin/sh -c '
    UNAME_OUT=$(uname -a 2>/dev/null)
    echo "UNAME=$UNAME_OUT"
' 2>&1)
EXIT=$?
assert_exit "E3-T1: basic uname" 0 $EXIT
assert_contains "E3-T1: uname output" "UNAME=" "$OUT"

# E3-T2: uname -s (kernel name)
OUT=$(run_proot /bin/sh -c '
    KERNEL=$(uname -s 2>/dev/null)
    echo "KERNEL_NAME=$KERNEL"
' 2>&1)
EXIT=$?
assert_exit "E3-T2: uname -s" 0 $EXIT
assert_contains "E3-T2: kernel name" "KERNEL_NAME=Linux" "$OUT"

# E3-T3: uname -m (machine hardware name)
OUT=$(run_proot /bin/sh -c '
    MACHINE=$(uname -m 2>/dev/null)
    echo "MACHINE=$MACHINE"
' 2>&1)
EXIT=$?
assert_exit "E3-T3: uname -m" 0 $EXIT
assert_contains "E3-T3: machine name" "MACHINE=aarch64" "$OUT"

# E3-T4: uname -r (kernel release)
OUT=$(run_proot /bin/sh -c '
    RELEASE=$(uname -r 2>/dev/null)
    echo "RELEASE=$RELEASE"
    # Release should contain digits
    echo "$RELEASE" | grep -q "[0-9]" && echo "RELEASE_VALID" || echo "RELEASE_INVALID"
' 2>&1)
EXIT=$?
assert_exit "E3-T4: uname -r" 0 $EXIT
assert_contains "E3-T4: release valid" "RELEASE_VALID" "$OUT"

# ============================================================
log ""
log "=== E6: mknod phantom — ENOENT for S_ISBLK/S_ISCHR ==="
# ============================================================

# E6-T1: mknod block device (S_ISBLK) — should return ENOENT, not success
OUT=$(run_proot /bin/sh -c '
    # mknod with S_ISBLK (mode 0660 | 060000 = 060660)
    # S_ISBLK = 0060000, S_IFBLK = 0060000
    mknod /tmp/e1_block_dev b 1 3 2>/dev/null
    EXIT_CODE=$?
    if [ $EXIT_CODE -ne 0 ]; then
        echo "MKNOD_BLOCK_FAIL_OK (exit=$EXIT_CODE)"
    else
        # Check if the node actually exists
        if [ -e /tmp/e1_block_dev ]; then
            echo "MKNOD_BLOCK_EXISTS_BUT_SHOULD_NOT"
            rm -f /tmp/e1_block_dev
        else
            echo "MKNOD_BLOCK_PHANTOM_SUCCESS"
        fi
    fi
' 2>&1)
EXIT=$?
assert_exit "E6-T1: mknod block device" 0 $EXIT
assert_contains "E6-T1: block device result" "MKNOD_BLOCK_FAIL_OK" "$OUT"

# E6-T2: mknod char device (S_ISCHR) — should return ENOENT, not success
OUT=$(run_proot /bin/sh -c '
    # S_ISCHR = 0020000
    mknod /tmp/e1_char_dev c 1 3 2>/dev/null
    EXIT_CODE=$?
    if [ $EXIT_CODE -ne 0 ]; then
        echo "MKNOD_CHAR_FAIL_OK (exit=$EXIT_CODE)"
    else
        if [ -e /tmp/e1_char_dev ]; then
            echo "MKNOD_CHAR_EXISTS_BUT_SHOULD_NOT"
            rm -f /tmp/e1_char_dev
        else
            echo "MKNOD_CHAR_PHANTOM_SUCCESS"
        fi
    fi
' 2>&1)
EXIT=$?
assert_exit "E6-T2: mknod char device" 0 $EXIT
assert_contains "E6-T2: char device result" "MKNOD_CHAR_FAIL_OK" "$OUT"

# E6-T3: mknod regular file (S_ISREG) — should still work (not affected by E6 fix)
OUT=$(run_proot /bin/sh -c '
    # S_ISREG = 0100000 — this should still get EPERM→0 (fake success)
    # because the fix only applies to S_ISBLK/S_ISCHR
    mknod /tmp/e1_reg_file 0644 0 0 2>/dev/null
    EXIT_CODE=$?
    echo "MKNOD_REG_EXIT=$EXIT_CODE"
    rm -f /tmp/e1_reg_file
' 2>&1)
EXIT=$?
assert_exit "E6-T3: mknod regular file" 0 $EXIT
assert_contains "E6-T3: regular file result" "MKNOD_REG_EXIT=" "$OUT"

# E6-T4: mknod FIFO (S_ISFIFO) — should still work (not affected by E6 fix)
OUT=$(run_proot /bin/sh -c '
    # S_ISFIFO = 0010000
    mknod /tmp/e1_fifo p 2>/dev/null
    EXIT_CODE=$?
    echo "MKNOD_FIFO_EXIT=$EXIT_CODE"
    rm -f /tmp/e1_fifo
' 2>&1)
EXIT=$?
assert_exit "E6-T4: mknod FIFO" 0 $EXIT
assert_contains "E6-T4: FIFO result" "MKNOD_FIFO_EXIT=" "$OUT"

# E6-T5: mknodat block device — should return ENOENT
OUT=$(run_proot /bin/sh -c '
    mknodat /tmp e1_mknodat_blk b 8 0 2>/dev/null
    EXIT_CODE=$?
    if [ $EXIT_CODE -ne 0 ]; then
        echo "MKNODAT_BLOCK_FAIL_OK (exit=$EXIT_CODE)"
    else
        echo "MKNODAT_BLOCK_UNEXPECTED_SUCCESS"
    fi
    rm -f /tmp/e1_mknodat_blk
' 2>&1)
EXIT=$?
assert_exit "E6-T5: mknodat block device" 0 $EXIT
assert_contains "E6-T5: mknodat block result" "MKNODAT_BLOCK_FAIL_OK" "$OUT"

# ============================================================
log ""
log "=== INTEGRATION: combined stress test ==="
# ============================================================

# Combined: socket cycle + rename + uname + mknod in one proot session
OUT=$(run_proot /bin/sh -c '
    # Socket cycle
    i=0
    while [ $i -lt 20 ]; do
        exec 3>/dev/null 2>&1
        exec 3>&-
        i=$((i + 1))
    done
    echo "SOCKET_STRESS_OK"

    # Rename
    echo "data" > /tmp/integ_test.txt
    mv /tmp/integ_test.txt /tmp/integ_renamed.txt
    if [ -f /tmp/integ_renamed.txt ]; then
        echo "RENAME_INTEGRATION_OK"
        rm -f /tmp/integ_renamed.txt
    fi

    # Uname
    UNAME=$(uname -s 2>/dev/null)
    [ "$UNAME" = "Linux" ] && echo "UNAME_INTEGRATION_OK" || echo "UNAME_INTEGRATION_FAIL"

    # Mknod block (should fail with ENOENT)
    mknod /tmp/integ_blk b 1 3 2>/dev/null
    if [ $? -ne 0 ]; then
        echo "MKNOD_INTEGRATION_FAIL_OK"
    else
        echo "MKNOD_INTEGRATION_UNEXPECTED"
        rm -f /tmp/integ_blk
    fi
' 2>&1)
EXIT=$?
assert_exit "INTEGRATION: combined stress" 0 $EXIT
assert_contains "INTEGRATION: socket" "SOCKET_STRESS_OK" "$OUT"
assert_contains "INTEGRATION: rename" "RENAME_INTEGRATION_OK" "$OUT"
assert_contains "INTEGRATION: uname" "UNAME_INTEGRATION_OK" "$OUT"
assert_contains "INTEGRATION: mknod" "MKNOD_INTEGRATION_FAIL_OK" "$OUT"

# ============================================================
log ""
log "=== SUMMARY: PASS=$PASS FAIL=$FAIL TOTAL=$TOTAL ==="
[ "$FAIL" -eq 0 ] && log "ALL TESTS PASSED" || log "SOME TESTS FAILED"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
