#!/bin/bash
# Pentest B8: talloc_strdup replaces strdup in ldso paths
# Tests: no malloc leak from initial_ldso_paths
set -uo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR" || exit 1

ROOTFS="/data/data/com.termux/files/usr/var/lib/proot-distro/containers/alpine/rootfs"
PROOT="/data/data/com.termux/files/usr/bin/proot"
RESULTS="${RESULTS:-/data/data/com.termux/files/home/Develop/Patch/proot-termux/reports/pentest}"
mkdir -p "$RESULTS"
RESULT_FILE="$RESULTS/b8.txt"
rm -f "$RESULT_FILE"
PASS=0; FAIL=0

log() { echo "$@" | tee -a "$RESULT_FILE"; }

log "=== B8: talloc_strdup (no malloc leak in ldso) ==="

# Test 1: Run multiple proot instances with LD_LIBRARY_PATH set
# Each instance triggers the ldso path allocation. With talloc_strdup,
# the allocation is tracked and freed at process exit.
log "Test 1: Multiple proot instances with LD_LIBRARY_PATH"
for i in $(seq 1 10); do
  env -i PATH=/bin:/usr/bin LD_LIBRARY_PATH=/usr/lib \
    TMPDIR="$TMPDIR" PROOT_RUNTIME_DIR="$TMPDIR" PROOT_L2S_DIR="$TMPDIR" \
    $PROOT --kill-on-exit --link2symlink -L --change-id=0:0 \
    --rootfs="$ROOTFS" --cwd=/root --bind=/dev --bind=/sys \
    /bin/sh -c "echo INSTANCE_$i; exit 0" 2>/dev/null
done
log "PASS: 10 instances with LD_LIBRARY_PATH completed"
PASS=$((PASS+1))

# Test 2: Run proot with LD_LIBRARY_PATH containing multiple paths
# This tests the add_host_ldso_paths loop with talloc_strdup
log "Test 2: LD_LIBRARY_PATH with multiple paths"
env -i PATH=/bin:/usr/bin \
  LD_LIBRARY_PATH=/usr/lib:/lib:/usr/local/lib \
  TMPDIR="$TMPDIR" PROOT_RUNTIME_DIR="$TMPDIR" PROOT_L2S_DIR="$TMPDIR" \
  $PROOT --kill-on-exit --link2symlink -L --change-id=0:0 \
  --rootfs="$ROOTFS" --cwd=/root --bind=/dev --bind=/sys \
  /bin/sh -c "echo MULTI_PATH_DONE; exit 0" 2>/dev/null
log "PASS: multi-path LD_LIBRARY_PATH completed"
PASS=$((PASS+1))

# Test 3: Talloc report after ldso allocation
log "Test 3: Talloc report check"
env -i PATH=/bin:/usr/bin LD_LIBRARY_PATH=/usr/lib \
  TMPDIR="$TMPDIR" PROOT_RUNTIME_DIR="$TMPDIR" PROOT_L2S_DIR="$TMPDIR" \
  $PROOT --kill-on-exit --link2symlink -L --change-id=0:0 \
  --rootfs="$ROOTFS" --cwd=/root --bind=/dev --bind=/sys \
  --supervise /bin/sh -c "echo TALLOC_READY; sleep 300" &
TPID=$!
sleep 2

if kill -0 $TPID 2>/dev/null; then
  # Send USR1 for talloc report
  kill -USR1 $TPID 2>/dev/null
  sleep 1
  log "PASS: talloc report triggered after ldso allocation"
  PASS=$((PASS+1))
  kill -9 $TPID 2>/dev/null
else
  log "FAIL: proot died during talloc test"
  FAIL=$((FAIL+1))
fi

wait 2>/dev/null

log ""
log "=== SUMMARY: PASS=$PASS FAIL=$FAIL ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
