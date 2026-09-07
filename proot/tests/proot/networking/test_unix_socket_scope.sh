#!/usr/bin/env bash
# AF_UNIX regression: enumerate names and connect without protocol payloads.
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
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 is required for AF_UNIX coverage"
    exit 0
fi

python3 - "$TERMUX_ISOLATED" <<'PY'
import os
import socket
import struct
import subprocess
import sys
import tempfile

wrapper = sys.argv[1]
prefix = os.environ.get("PREFIX", "/data/data/com.termux/files/usr")
peercred = struct.Struct("3i")

guest_code = r'''
import os
import socket
import struct
import sys

kind, target = sys.argv[1:]
if kind == "abstract":
    target = "\0" + target
try:
    socket_names = os.listdir("/dev/socket")
except OSError as exc:
    print("ENUM_ERR:%s:%s" % (exc.errno, exc.strerror))
else:
    print("ENUM_SOCKET_COUNT:%d" % len(socket_names))

client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    client.connect(target)
    peer = client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
    pid, uid, gid = struct.unpack("3i", peer)
    print("CONNECT_OK:%s" % kind)
    print("GUEST_PEERCRED:%d:%d:%d" % (pid, uid, gid))
finally:
    client.close()
'''


def run_case(kind, target, server_address, bind_directory=None):
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    if bind_directory is not None:
        listener.bind(server_address)
    else:
        listener.bind(server_address)
    listener.listen(1)
    listener.settimeout(5)
    bind_arg = []
    guest_target = target
    if bind_directory is not None:
        bind_arg = ["--proot-arg", "--bind=%s:/unix" % bind_directory]
    command = [
        wrapper,
        "--termux-paths",
        "--cwd",
        prefix,
        "--proot-arg",
        "--net-policy",
        "--proot-arg",
        "deny",
        *bind_arg,
        "--",
        prefix + "/bin/python3",
        "-c",
        guest_code,
        kind,
        guest_target,
    ]
    child = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env={**os.environ, "TMPDIR": os.environ.get("TMPDIR", "/data/data/com.termux/files/usr/tmp")},
    )
    connection = None
    try:
        connection, _ = listener.accept()
        connection.settimeout(0.1)
        try:
            payload = connection.recv(1)
        except socket.timeout:
            payload = None
        assert payload in (None, b""), payload
        host_peer = peercred.unpack(connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        stdout, stderr = child.communicate(timeout=5)
    except Exception:
        if child.poll() is None:
            child.kill()
        stdout, stderr = child.communicate(timeout=5)
        raise AssertionError(
            "%s socket failed: exit=%r stdout=%r stderr=%r"
            % (kind, child.returncode, stdout, stderr)
        )
    finally:
        if connection is not None:
            connection.close()
        listener.close()

    assert child.returncode == 0, (child.returncode, stdout, stderr)
    assert "ENUM_SOCKET_COUNT:" in stdout, stdout
    assert "CONNECT_OK:%s" % kind in stdout, stdout
    assert "GUEST_PEERCRED:" in stdout, stdout
    assert host_peer[1] == os.getuid(), host_peer
    assert host_peer[2] == os.getgid(), host_peer
    print("%s pathname/abstract enumeration and connect: PASS" % kind)
    print("  guest: %s" % stdout.replace("\n", " | ").strip())
    print("  host peer credentials: pid=%d uid=%d gid=%d" % host_peer)


with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as directory:
    pathname = os.path.join(directory, "service.sock")
    run_case("pathname", "/unix/service.sock", pathname, directory)
    abstract_name = "proot-safe-unix-%d" % os.getpid()
    run_case("abstract", abstract_name, "\0" + abstract_name)
PY

echo "=== AF_UNIX scope: PASS=2 FAIL=0 ==="
