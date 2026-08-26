#!/data/data/com.termux/files/usr/bin/bash
# Regression: proc_isolation must classify canonical and relative paths
# before they can expose host procfs entries.
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
ISOLATED="$ROOT/bin/termux-isolated"

if [ ! -x "$ISOLATED" ]; then
    echo "SKIP: termux-isolated not found at $ISOLATED"
    exit 0
fi

host_pid=$$
output=$(
    "$ISOLATED" --termux-paths --proc-isolated -- sh -c '
        for path in /proc/version /proc//version /proc/./version \
                    /proc/x/../version; do
            if head -c 1 "$path" >/dev/null 2>&1; then
                echo "LEAK:$path"
            else
                echo "BLOCK:$path"
            fi
        done
        cd /proc
        if head -c 1 version >/dev/null 2>&1; then
            echo LEAK:relative-version
        else
            echo BLOCK:relative-version
        fi
    '
)

if printf '%s\n' "$output" | grep -q '^LEAK:'; then
    printf '%s\n' "$output"
    echo "FAIL: non-canonical or relative proc path bypassed isolation" >&2
    exit 1
fi
printf '%s\n' "$output" | grep -c '^BLOCK:' | grep -q '^5$'

if "$ISOLATED" --termux-paths --proc-isolated -- sh -c \
        "readlink /proc/$host_pid/exe" >/dev/null 2>&1; then
    echo "FAIL: host PID readlink was reachable" >&2
    exit 1
fi

self_exe=$("$ISOLATED" --termux-paths --proc-isolated -- sh -c \
    'readlink /proc/self/exe')
case "$self_exe" in
    /*) ;;
    *) echo "FAIL: self/exe is not a guest absolute path" >&2; exit 1 ;;
esac
case "$self_exe" in
    /data/data/*) echo "FAIL: self/exe leaked a host Termux path" >&2; exit 1 ;;
esac

echo "PASS: canonical, relative, host-PID, and self proc paths are isolated"
