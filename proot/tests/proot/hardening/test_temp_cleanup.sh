#!/usr/bin/env bash
# Host-only unit regression of the actual temporary-directory destructor.
set -euo pipefail
: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
command -v clang >/dev/null
fixture=$(mktemp -d "$TMPDIR/proot-cleanup.XXXXXX")
trap 'rm -rf -- "$fixture"' EXIT
clang -D_GNU_SOURCE -I"$ROOT/src" "$ROOT/tests/proot/probes/temp_cleanup.c" \
    -ltalloc -o "$fixture/test"
"$fixture/test" "$fixture"
