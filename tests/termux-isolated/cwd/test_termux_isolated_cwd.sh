#!/usr/bin/env bash
# Regression test for --cwd in command and interactive-shell modes.
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
TERMUX_ISOLATED="${TERMUX_ISOLATED:-$REPO_ROOT/bin/termux-isolated}"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
FIXTURE="$TMPDIR/termux-isolated-cwd-$$"

if [[ ! -x "$TERMUX_ISOLATED" ]]; then
    echo "SKIP: termux-isolated wrapper not found at $TERMUX_ISOLATED"
    exit 0
fi

mkdir -p "$FIXTURE"
cleanup() { rmdir "$FIXTURE" 2>/dev/null || true; }
trap cleanup EXIT

expected_rootfs="/usr/tmp/$(basename "$FIXTURE")"
output=$("$TERMUX_ISOLATED" --cwd "$FIXTURE" -- sh -c 'pwd')
test "$output" = "$expected_rootfs"
echo "PASS: rootfs translates host --cwd to $expected_rootfs"

expected_termux="$PREFIX/tmp/$(basename "$FIXTURE")"
output=$("$TERMUX_ISOLATED" --termux-paths --cwd "$expected_termux" -- sh -c 'pwd')
test "$output" = "$expected_termux"
echo "PASS: termux-paths preserves guest --cwd"

python3 - "$TERMUX_ISOLATED" "$FIXTURE" <<'PY'
import os
import fcntl
import pty
import select
import subprocess
import sys
import termios
import time

wrapper, host_cwd = sys.argv[1:]
master, slave = pty.openpty()


def attach_controlling_tty():
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


try:
    child = subprocess.Popen(
        [wrapper, "--cwd", host_cwd],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        cwd=os.path.dirname(host_cwd),
        close_fds=True,
        preexec_fn=attach_controlling_tty,
    )
finally:
    os.close(slave)

try:
    time.sleep(0.25)
    # Drain the initial prompt before sending input. This mirrors the real
    # interactive regression and avoids racing fish startup on a PTY.
    while True:
        ready, _, _ = select.select([master], [], [], 0)
        if not ready:
            break
        try:
            os.read(master, 4096)
        except OSError:
            break
    os.write(master, b"pwd\nexit\n")
    output = bytearray()
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.25)
        if not ready:
            if child.poll() is not None:
                break
            continue
        try:
            output.extend(os.read(master, 4096))
        except OSError:
            break
    child.wait(timeout=5)
finally:
    os.close(master)

expected = "/usr/tmp/" + os.path.basename(host_cwd)
assert expected.encode() in output, output.decode(errors="replace")
print("PASS: interactive shell honors explicit --cwd")
PY

echo "=== SUMMARY: --cwd PASS=3 FAIL=0 ==="
