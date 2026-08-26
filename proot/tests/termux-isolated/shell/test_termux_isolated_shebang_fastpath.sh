#!/usr/bin/env bash
# Verify Termux shebangs without inheriting termux-exec's LD_PRELOAD.
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
TERMUX_ISOLATED="${TERMUX_ISOLATED:-$REPO_ROOT/bin/termux-isolated}"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
FIXTURE="$PREFIX/tmp/termux-isolated-shebang-fastpath-$$"

cleanup() {

  rm -f "$FIXTURE"
}
trap cleanup EXIT

cat > "$FIXTURE" <<'EOF'
#!/bin/bash
printf 'FAST_SHEBANG_OK\n'
EOF
chmod +x "$FIXTURE"

run_case() {
  local name=$1
  shift
  local guest_script=$1
  shift
  local output
  output=$("$TERMUX_ISOLATED" "$@" -- env -u LD_PRELOAD "$guest_script" 2>&1)
  test "$output" = FAST_SHEBANG_OK || {
    printf 'FAIL: %s: %s\n' "$name" "$output" >&2
    return 1
  }
  printf 'PASS: %s\n' "$name"
}

run_case "termux-paths aliases" "$PREFIX/tmp/termux-isolated-shebang-fastpath-$$" \
  --proot-arg "--bind=$PREFIX/bin:/bin:ro" \
  --proot-arg "--bind=$PREFIX/bin:/usr/bin:ro" \
  --termux-paths
run_case "termux-paths strict aliases" "$PREFIX/tmp/termux-isolated-shebang-fastpath-$$" \
  --termux-paths --rw-dir "$PREFIX/tmp"
run_case "rootfs existing paths" "/tmp/termux-isolated-shebang-fastpath-$$"
printf '%s\n' '=== SUMMARY: shebang fastpath PASS=3 FAIL=0 ==='
