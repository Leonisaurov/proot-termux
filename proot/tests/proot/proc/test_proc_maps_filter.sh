#!/data/data/com.termux/files/usr/bin/bash
# Regression: proc_isolation must keep filtering /proc/<pid>/maps after the
# read(2)/pread64(2) fast path (per-fd classification) was introduced.
# Covers plain read, pread, dup and fd-number reuse, and that synthesized
# files still work through the same EXIT hook.
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
ISOLATED="$ROOT/bin/termux-isolated"
FIXTURE="$TMPDIR/proc-maps-filter-$$.py"

if [ ! -x "$ISOLATED" ]; then
    echo "SKIP: termux-isolated not found at $ISOLATED"
    exit 0
fi

cleanup() { rm -f "$FIXTURE"; }
trap cleanup EXIT

cat > "$FIXTURE" <<'PY'
import os
import sys


def read_maps(path, pread=False):
    fd = os.open(path, os.O_RDONLY)
    try:
        if pread:
            return os.pread(fd, 1 << 20, 0).decode(errors="replace")
        return os.read(fd, 1 << 20).decode(errors="replace")
    finally:
        os.close(fd)


def assert_filtered(data, label):
    if "prooted-" in data or "/libexec/proot/loader" in data:
        print(f"LEAK:{label}")
        sys.exit(1)


assert_filtered(read_maps("/proc/self/maps"), "read")
assert_filtered(read_maps("/proc/self/maps", pread=True), "pread")

# fd-number reuse: a non-maps descriptor is classified NONMAPS, then closed
# and reopened as maps.  A stale cache entry would leak the loader paths.
a = os.open("/dev/null", os.O_RDONLY)
os.read(a, 1)
os.close(a)
fd = os.open("/proc/self/maps", os.O_RDONLY)
try:
    reuse = os.read(fd, 1 << 20).decode(errors="replace")
finally:
    os.close(fd)
if fd != a:
    print(f"SKIP: fd was not reused ({fd} != {a})")
else:
    assert_filtered(reuse, "fd-reuse")

# dup of a maps descriptor keeps the classification.
fd = os.open("/proc/self/maps", os.O_RDONLY)
dup = os.dup(fd)
try:
    assert_filtered(os.read(dup, 1 << 20).decode(errors="replace"), "dup")
finally:
    os.close(dup)
    os.close(fd)

# Synthesized files still flow through the read EXIT hook.
status = read_maps("/proc/self/status")
if "Name:\tproot-guest" not in status or "TracerPid:\t0" not in status:
    print("LEAK: synthetic status missing")
    sys.exit(1)

# Host pid maps stay unreachable.
try:
    os.stat("/proc/1/maps")
    print("LEAK: host pid maps reachable")
    sys.exit(1)
except (FileNotFoundError, ProcessLookupError):
    pass

print("MAPS_FILTER_OK")
PY

output=$("$ISOLATED" --termux-paths --proc-isolated -- python3 "$FIXTURE" 2>&1) || true
printf '%s\n' "$output"

if printf '%s\n' "$output" | grep -q '^LEAK:'; then
    echo "FAIL: proc maps filtering regressed" >&2
    exit 1
fi
printf '%s\n' "$output" | grep -q '^MAPS_FILTER_OK$' || {
    echo "FAIL: proc maps filter did not complete" >&2
    exit 1
}

echo "PASS: proc maps filtering (read, pread, dup, fd reuse)"
