#!/usr/bin/env bash
# Regression coverage for literal and grouped --proot-arg forms.
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
TERMUX_ISOLATED="${TERMUX_ISOLATED:-$REPO_ROOT/bin/termux-isolated}"

if [[ ! -x "$TERMUX_ISOLATED" ]]; then
    echo "SKIP: termux-isolated wrapper not found at $TERMUX_ISOLATED"
    exit 0
fi

output=$("$TERMUX_ISOLATED" --termux-paths --cwd "$PWD" \
    --proot-args "--net-policy deny" -- \
    sh -c 'printf PROOT_ARGS_OK')
test "$output" = PROOT_ARGS_OK
echo "PASS: grouped --proot-args reaches PRoot as separate arguments"

output=$("$TERMUX_ISOLATED" --termux-paths --cwd "$PWD" \
    --proot-arg=--net-policy --proot-arg=deny -- \
    sh -c 'printf PROOT_ARG_OK')
test "$output" = PROOT_ARG_OK
echo "PASS: repeated literal --proot-arg remains compatible"

echo "=== SUMMARY: proot args PASS=2 FAIL=0 ==="
