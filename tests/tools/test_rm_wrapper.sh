#!/usr/bin/env bash
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
RM_WRAPPER="$ROOT/bin/rm"
FIXTURE=$(mktemp -d "$TMPDIR/rm-wrapper.XXXXXX")
SAFE_HOME="$FIXTURE/home"
OUTSIDE="$FIXTURE/outside"
mkdir -p "$SAFE_HOME/subdir" "$OUTSIDE"
printf protected > "$SAFE_HOME/subdir/file"
printf removable > "$OUTSIDE/file"

cleanup() { /data/data/com.termux/files/usr/bin/rm -rf -- "$FIXTURE"; }
trap cleanup EXIT

if HOME="$SAFE_HOME" "$RM_WRAPPER" -rf -- "$SAFE_HOME" >/dev/null 2>&1; then
    echo "FAIL: wrapper allowed removing HOME" >&2
    exit 1
fi
test -f "$SAFE_HOME/subdir/file"

if HOME="$SAFE_HOME" "$RM_WRAPPER" -rf -- "$SAFE_HOME/../home/subdir" >/dev/null 2>&1; then
    echo "FAIL: wrapper allowed removing a HOME descendant" >&2
    exit 1
fi
test -f "$SAFE_HOME/subdir/file"

HOME="$SAFE_HOME" "$RM_WRAPPER" -f -- "$OUTSIDE/file"
test ! -e "$OUTSIDE/file"
echo "PASS: rm wrapper protects HOME and permits unrelated paths"
