#!/usr/bin/env bash
# Core PRCT regression: malformed decisions must never authorize an operation.
set -euo pipefail
: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"
command -v python3 >/dev/null
command -v proot >/dev/null
python3 "$(dirname -- "$0")/fixtures/control_invalid_decisions.py"
