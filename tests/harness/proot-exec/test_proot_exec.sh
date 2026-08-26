#!/usr/bin/env bash
# Validate config parsing, argv generation, variable expansion and execution.
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
PROOT_EXEC="${PROOT_EXEC:-$ROOT/bin/proot-exec}"
CONFIG="$SCRIPT_DIR/../fixtures/proot-exec-test.conf"
DEFAULT_DIR="$SCRIPT_DIR/../fixtures"

if [[ ! -x "$PROOT_EXEC" ]]; then
    echo "SKIP: proot-exec not found at $PROOT_EXEC"
    exit 0
fi

output=$("$PROOT_EXEC" --config "$CONFIG")
test "$output" = PROOT_EXEC_CONFIG_OK
echo "PASS: config launches PRoot and guest command"

printed=$("$PROOT_EXEC" --config "$CONFIG" --print)
grep -F -- '--net-policy deny' <<< "$printed" >/dev/null
grep -F -- "--net-allow '*'" <<< "$printed" >/dev/null
grep -F -- '--proxy proot-exec-test' <<< "$printed" >/dev/null
grep -F -- '/proot-exec-test-tmp:ro' <<< "$printed" >/dev/null
grep -F -- 'PROOT_EXEC_TEST=configured' <<< "$printed" >/dev/null
grep -F -- 'PROOT_EXEC_CONFIG_OK' <<< "$printed" >/dev/null
echo "PASS: --print exposes generated argv"

output=$("$PROOT_EXEC" --config "$CONFIG" -- \
    "$PREFIX/bin/sh" -c 'printf PROOT_EXEC_OVERRIDE_OK')
test "$output" = PROOT_EXEC_OVERRIDE_OK
echo "PASS: command override after --"

default_output=$(CDPATH= cd -- "$DEFAULT_DIR" && "$PROOT_EXEC")
test "$default_output" = PROOT_EXEC_DEFAULT_OK
echo "PASS: proot-exec.conf is the default without --config"

alias_output=$("$PROOT_EXEC" --config "$SCRIPT_DIR/../fixtures/proot-exec-command-alias.conf")
test "$alias_output" = PROOT_EXEC_CMD_OK
echo "PASS: [command].cmd alias"

"$PROOT_EXEC" --config "$CONFIG" --dry-run >/dev/null
echo "PASS: --dry-run validates without executing"

python3 - "$PROOT_EXEC" "$SCRIPT_DIR/../fixtures/proot-exec-control.conf" <<'PY'
import os
import socket
import struct
import subprocess
import sys

launcher, config = sys.argv[1:]
parent, child = socket.socketpair()
fd = child.fileno()
env = os.environ.copy()
env["CONTROL_FD"] = str(fd)
process = subprocess.Popen(
    [launcher, "--config", config],
    env=env,
    pass_fds=(fd,),
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)
child.close()
try:
    frame = parent.recv(20)
    assert len(frame) == 20, frame
    magic, version, message, size, request_id = struct.unpack("<IHHIQ", frame)
    assert (magic, version, message, size, request_id) == (
        0x50524354, 1, 1, 0, 0
    ), (magic, version, message, size, request_id)
    stdout, stderr = process.communicate(timeout=10)
    assert process.returncode == 0, stderr
    assert stdout == "PROOT_EXEC_CONTROL_OK", stdout
finally:
    parent.close()
    if process.poll() is None:
        process.kill()
        process.wait()
print("PASS: control_fd is inherited and PRoot HELLO reaches its peer")
PY

echo "=== SUMMARY: proot-exec PASS=7 FAIL=0 ==="
