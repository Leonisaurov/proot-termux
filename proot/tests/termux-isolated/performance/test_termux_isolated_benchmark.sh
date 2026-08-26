#!/usr/bin/env bash
# Measure command overhead inside one persistent interactive session.
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
TERMUX_ISOLATED="${TERMUX_ISOLATED:-$REPO_ROOT/bin/termux-isolated}"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"

python3 - "$TERMUX_ISOLATED" "$PREFIX" <<'PY'
import os
import pty
import select
import subprocess
import sys
import time

wrapper, prefix = sys.argv[1:]
iterations = 100


def read_until(master, marker, timeout=30):
    output = bytearray()
    deadline = time.monotonic() + timeout
    while marker not in output:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AssertionError(output.decode(errors="replace"))
        ready, _, _ = select.select([master], [], [], remaining)
        if not ready:
            raise AssertionError(output.decode(errors="replace"))
        try:
            output.extend(os.read(master, 4096))
        except OSError as error:
            raise AssertionError(output.decode(errors="replace")) from error
    return output


def measure(label, argv, guest_prefix=""):
    master, slave = pty.openpty()
    child = subprocess.Popen(argv, stdin=slave, stdout=slave, stderr=slave,
                             env=os.environ.copy(), close_fds=True)
    os.close(slave)
    try:
        time.sleep(0.5)
        os.write(master, b"printf '\\122\\105\\101\\104\\131\\137\\102\\105\\116\\103\\110\\n'\n")
        read_until(master, b"READY_BENCH")
        command = (
            f"{guest_prefix}sh -c 'i=0; while [ $i -lt {iterations} ]; do "
            "/bin/true; i=$((i+1)); done'; printf '\\105\\116\\104\\137\\102\\105\\116\\103\\110\\n'"
        ).encode() + b"\n"
        start = time.monotonic()
        os.write(master, command)
        read_until(master, b"END_BENCH")
        elapsed = time.monotonic() - start
        os.write(master, b"exit\n")
        try:
            child.wait(timeout=2)
        except subprocess.TimeoutExpired:
            # Some interactive shells keep the pty session alive after exit;
            # this benchmark only needs the measured command interval.
            child.kill()
            child.wait()
    finally:
        os.close(master)
        if child.poll() is None:
            child.kill()
            child.wait()
    print(f"{label}: {elapsed:.3f}s ({iterations} guest execs)", flush=True)
    return elapsed


direct = measure("direct persistent Termux shell", [f"{prefix}/bin/sh", "-i"])
isolated = measure("persistent termux-isolated", [wrapper, "--termux-paths",
                                                   "--", "sh", "-i"])
no_proc = measure("persistent termux-isolated --no-proc-isolated",
                  [wrapper, "--termux-paths", "--no-proc-isolated", "--",
                   "sh", "-i"])

print(f"persistent isolated/direct ratio: {isolated / direct:.1f}x", flush=True)
print(f"persistent no-proc/direct ratio: {no_proc / direct:.1f}x", flush=True)
print("=== SUMMARY: benchmark completed ===", flush=True)
PY
