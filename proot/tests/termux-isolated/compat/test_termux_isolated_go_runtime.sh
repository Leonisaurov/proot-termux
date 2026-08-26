#!/data/data/com.termux/files/usr/bin/bash
# Regression: proc-isolated /proc/self/maps must retain valid guest address
# ranges. Go/cgo runtimes use them while initializing their thread stack.
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
ISOLATED="$ROOT/bin/termux-isolated"
PROOT_EXEC="$ROOT/bin/proot-exec"
CONFIG="$ROOT/../proot-exec.conf"

if [ ! -x "$ISOLATED" ]; then
    echo "SKIP: termux-isolated not found at $ISOLATED"
    exit 0
fi

maps=$("$ISOLATED" --termux-paths --proc-isolated -- sh -c 'cat /proc/self/maps')
printf '%s\n' "$maps" | awk '
    BEGIN { bad = 0 }
    NF < 2 { next }
    {
        split($1, range, "-")
        if (range[1] !~ /^[0-9a-fA-F]+$/ || range[2] !~ /^[0-9a-fA-F]+$/ ||
            range[1] ~ /^0+$/ || range[2] ~ /^0+$/)
            bad = 1
    }
    END { exit bad }
'
printf '%s\n' "$maps" | grep -F ' [stack]' >/dev/null
echo "PASS: proc-isolated preserves valid /proc/self/maps ranges"

task_maps=$("$ISOLATED" --termux-paths --proc-isolated -- sh -c \
    'cat /proc/self/task/$$/maps')
printf '%s\n' "$task_maps" | grep -F ' [stack]' >/dev/null
echo "PASS: proc-isolated exposes valid thread maps"

if command -v gh >/dev/null 2>&1 && [ -x "$PROOT_EXEC" ] && [ -f "$CONFIG" ]; then
    gh_output=$("$PROOT_EXEC" --config "$CONFIG" -- gh version)
    printf '%s\n' "$gh_output" | grep -F 'gh version' >/dev/null
    echo "PASS: gh runs in the Codex-compatible proot-exec profile"
else
    echo "SKIP: gh or proot-exec Codex-compatible profile is unavailable"
fi

echo "=== SUMMARY: proc-isolated Go runtime PASS ==="
