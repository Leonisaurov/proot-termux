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
grep -F -- 'environment_removed: LD_PRELOAD LD_LIBRARY_PATH' <<< "$printed" >/dev/null
echo "PASS: --print exposes generated argv"

output=$("$PROOT_EXEC" --config "$CONFIG" -- \
    "$PREFIX/bin/sh" -c 'printf PROOT_EXEC_OVERRIDE_OK')
test "$output" = PROOT_EXEC_OVERRIDE_OK
echo "PASS: command override after --"

python3 - "$ROOT/scripts/tools/proot_exec.py" "$CONFIG" <<'PY'
import importlib.util
import os
import sys

module_path, config_path = sys.argv[1:]
spec = importlib.util.spec_from_file_location("proot_exec_under_test", module_path)
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
sys.modules[spec.name] = module
spec.loader.exec_module(module)
os.environ["LD_PRELOAD"] = "proot-exec-sentinel"
os.environ["LD_LIBRARY_PATH"] = "proot-exec-sentinel"
env = module.ExecConfig.from_file(config_path).environment_for_exec()
assert "LD_PRELOAD" not in env, env
assert "LD_LIBRARY_PATH" not in env, env
PY
echo "PASS: host preload variables are removed before PRoot"

default_output=$(CDPATH= cd -- "$DEFAULT_DIR" && "$PROOT_EXEC")
test "$default_output" = PROOT_EXEC_DEFAULT_OK
echo "PASS: proot-exec.conf is the default without --config"

main_config="$ROOT/../proot-exec.conf"
main_output=$("$PROOT_EXEC" --config "$main_config" -- \
    "$PREFIX/bin/sh" -c 'printf PROOT_EXEC_MAIN_PROFILE_OK')
test "$main_output" = PROOT_EXEC_MAIN_PROFILE_OK
echo "PASS: repository Codex-compatible profile executes"

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

echo "=== SUMMARY: proot-exec PASS=9 FAIL=0 ==="
