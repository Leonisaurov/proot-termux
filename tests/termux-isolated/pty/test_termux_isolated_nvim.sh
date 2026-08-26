#!/usr/bin/env bash
# Verify Neovim can start and exit from an interactive termux-isolated PTY.
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

output=$("$TERMUX_ISOLATED" --termux-paths --cwd "$PWD" -- \
    nvim --headless --clean \
    '+lua assert(vim.env.ANDROID_HOME ~= vim.NIL)' '+qa!' 2>&1)
test -z "$output"
echo "PASS: Neovim receives Android toolchain environment"

python3 - "$TERMUX_ISOLATED" <<'PY'
import fcntl
import os
import pty
import select
import subprocess
import sys
import termios
import time

wrapper = sys.argv[1]
master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ,
            (24).to_bytes(2, "little") + (120).to_bytes(2, "little"))


def setup_tty():
    os.setsid()
    fcntl.ioctl(slave, termios.TIOCSCTTY, 0)


child = subprocess.Popen(
    [wrapper, "--termux-paths", "--cwd", os.getcwd(), "--",
     "sh", "-c", "tty; stty size"],
    stdin=slave, stdout=slave, stderr=slave,
    close_fds=True, preexec_fn=setup_tty,
)
os.close(slave)
output = bytearray()
try:
    deadline = time.monotonic() + 8
    while child.poll() is None and time.monotonic() < deadline:
        ready, _, _ = select.select([master], [], [], 0.25)
        if ready:
            try:
                output.extend(os.read(master, 4096))
            except OSError:
                break
    child.wait(timeout=2)
finally:
    os.close(master)

text = output.decode(errors="replace")
assert "/dev/pts/" in text and "24 120" in text, text
assert child.returncode == 0, text
print("PASS: guest preserves controlling PTY (/dev/pts)")
PY

echo "=== SUMMARY: Neovim/PTY PASS=2 FAIL=0 ==="
