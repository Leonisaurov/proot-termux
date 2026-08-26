#!/usr/bin/env bash
# Verify termux-isolated storage is opt-in and uses :mask bindings by default.
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
TERMUX_ISOLATED="${TERMUX_ISOLATED:-$REPO_ROOT/bin/termux-isolated}"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"

if [[ ! -x "$TERMUX_ISOLATED" ]]; then
	echo "SKIP: termux-isolated wrapper not found at $TERMUX_ISOLATED"
	exit 0
fi

check_masked() {
	local mode=$1
	shift
	"$TERMUX_ISOLATED" "$@" -- sh -c '
set -eu
for path in "$HOME/storage" /storage/emulated/0 /sdcard; do
    if test -r "$path"; then
        echo "VISIBLE:$path" >&2
        exit 1
    fi
done
'
	echo "PASS: storage masked by default ($mode)"
}

check_exposed() {
	local mode=$1
	shift
	if [[ ! -d "$PREFIX/../home/storage" && ! -d /storage/emulated/0 ]]; then
		echo "SKIP: internal storage is unavailable on this device ($mode)"
		return 0
	fi
	local guest_check=''
	if [[ -d "$PREFIX/../home/storage" ]]; then
		guest_check+='test -r "$HOME/storage"; '
	fi
	if [[ -d /storage/emulated/0 ]]; then
		guest_check+='test -r /storage/emulated/0; test -r /sdcard; '
	fi
	"$TERMUX_ISOLATED" "$@" -- sh -c "set -eu; $guest_check"
	echo "PASS: --with-storage exposes internal storage ($mode)"
}

check_masked "termux-paths" --termux-paths --cwd "$PREFIX"
check_exposed "termux-paths" --termux-paths --with-storage --cwd "$PREFIX"
check_masked "rootfs" --cwd /
check_exposed "rootfs" --with-storage --cwd /

echo "=== SUMMARY: storage PASS=4 FAIL=0 ==="
