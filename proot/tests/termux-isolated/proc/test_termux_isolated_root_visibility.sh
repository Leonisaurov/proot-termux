#!/usr/bin/env bash
# Regression for guest-root links and intentional Android/Termux path binds.
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
TERMUX_ISOLATED="${TERMUX_ISOLATED:-$REPO_ROOT/bin/termux-isolated}"
TERMUX_PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
TERMUX_ROOT="${TERMUX_PREFIX%/usr}"
ROOTFS="${PROOT_TEST_ROOTFS:-$TERMUX_ROOT/usr/var/lib/proot-distro/containers/alpine/rootfs}"

if [[ ! -x "$TERMUX_ISOLATED" ]]; then
    echo "SKIP: termux-isolated wrapper not found at $TERMUX_ISOLATED"
    exit 0
fi

probe() {
    local mode=$1 isolation=$2
    shift 2
    local -a args=(--cwd /usr)
    if [[ "$mode" == termux ]]; then
        args+=(--termux-paths)
    fi
    if [[ "$isolation" == strict ]]; then
        args+=(--proc-isolated)
    else
        args+=(--no-proc-isolated)
    fi
    "$TERMUX_ISOLATED" "${args[@]}" -- sh -c '
        for path in /proc/self/root /proc/self/cwd /proc/self/exe; do
            if value=$(readlink "$path" 2>/dev/null); then
                printf "%s=%s\n" "$path" "$value"
            else
                printf "%s=UNREADABLE\n" "$path"
            fi
        done
        for path in \
            /usr/bin/sh \
            /home \
            /data/data/com.termux/files/usr/bin/proot \
            /proc/self/root/usr/bin/sh \
            /proc/self/root/data/data/com.termux/files/usr/bin/proot \
            /system/etc/hosts \
            /proc/self/root/system/etc/hosts \
            /vendor; do
            if test -r "$path"; then
                printf "READABLE:%s\n" "$path"
            else
                printf "BLOCKED:%s\n" "$path"
            fi
        done
    '
}

contains() {
    local output=$1 expected=$2
    case "$output" in
        *"$expected"*) return 0 ;;
        *)
            printf '%s\n' "$output" >&2
            echo "FAIL: expected $expected" >&2
            return 1
            ;;
    esac
}

check_strict_termux_paths() {
    local output
    output=$(probe termux strict)
    contains "$output" '/proc/self/root=UNREADABLE'
    contains "$output" '/proc/self/cwd=/usr'
    contains "$output" 'READABLE:/data/data/com.termux/files/usr/bin/proot'
    contains "$output" 'READABLE:/proc/self/root/data/data/com.termux/files/usr/bin/proot'
    contains "$output" 'BLOCKED:/home'
    contains "$output" 'READABLE:/system/etc/hosts'
    contains "$output" 'READABLE:/proc/self/root/system/etc/hosts'
    case "$output" in
        *'/proc/self/exe=/data/data/'*)
            printf '%s\n' "$output" >&2
            echo "FAIL: termux-paths self/exe unexpectedly lost guest translation" >&2
            return 1
            ;;
    esac
    echo "PASS: strict termux-paths keeps intentional Termux guest paths"
}

check_compat_termux_paths() {
    local output
    output=$(probe termux compatibility)
    contains "$output" '/proc/self/root=/'
    contains "$output" '/proc/self/cwd=/usr'
    echo "PASS: termux-paths compatibility mode preserves the host-root link"
}

check_strict_rootfs() {
    local output
    output=$(probe rootfs strict)
    contains "$output" '/proc/self/root=UNREADABLE'
    contains "$output" '/proc/self/cwd=/usr'
    contains "$output" 'READABLE:/usr/bin/sh'
    contains "$output" 'READABLE:/home'
    contains "$output" 'READABLE:/proc/self/root/usr/bin/sh'
    contains "$output" 'BLOCKED:/data/data/com.termux/files/usr/bin/proot'
    contains "$output" 'BLOCKED:/proc/self/root/data/data/com.termux/files/usr/bin/proot'
    contains "$output" 'READABLE:/system/etc/hosts'
    contains "$output" 'READABLE:/proc/self/root/system/etc/hosts'
    case "$output" in
        *'/proc/self/exe=/data/data/'*)
            printf '%s\n' "$output" >&2
            echo "FAIL: rootfs self/exe exposed a host Termux path" >&2
            return 1
            ;;
    esac
    echo "PASS: strict rootfs keeps guest root separate from the host prefix"
}

check_compat_rootfs() {
    local output
    output=$(probe rootfs compatibility)
    contains "$output" "/proc/self/root=$TERMUX_ROOT"
    contains "$output" '/proc/self/cwd=/usr'
    echo "PASS: rootfs compatibility mode exposes its documented backing root"
}

check_strict_termux_paths
check_compat_termux_paths
if [[ ! -d "$ROOTFS" ]]; then
    echo "SKIP: rootfs fixture not found at $ROOTFS"
    echo "=== SUMMARY: root visibility PASS=2 SKIP=2 FAIL=0 ==="
    exit 0
fi
check_strict_rootfs
check_compat_rootfs
echo "=== SUMMARY: root visibility PASS=4 SKIP=0 FAIL=0 ==="
