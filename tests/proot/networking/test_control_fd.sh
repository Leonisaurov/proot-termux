#!/bin/bash
# Fast CLI/protocol presence checks. Runtime framing coverage uses the native
# binary and is intentionally kept separate from the Alpine fixture tests.
set -euo pipefail

# Tests run in the Termux app namespace. Do not inherit runner/container
# temporaries such as /tmp or /data/local/tmp: those paths are not part of the
# native Termux environment and would leak into guest path requests.
TMPDIR=/data/data/com.termux/files/usr/tmp
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

REPO_ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
TERMUX_ISOLATED="${TERMUX_ISOLATED:-$REPO_ROOT/bin/termux-isolated}"
if [[ ! -x "$TERMUX_ISOLATED" ]]; then
	echo "SKIP: termux-isolated wrapper not installed at $TERMUX_ISOLATED"
	exit 0
fi

HELP=$("$TERMUX_ISOLATED" --termux-paths --proot-arg --help -- 2>&1 || true)
grep -q -- '--control-fd' <<<"$HELP"
obsolete_option="--net""-ask"
if grep -q -- "$obsolete_option" <<<"$HELP"; then
	echo "FAIL: obsolete network ask option is still advertised" >&2
	exit 1
fi
grep -q -- '--hide' <<<"$HELP"
printf '%s\n' 'control-fd CLI: PASS'

python3 - "$TERMUX_ISOLATED" <<'PY'
import os
import fcntl
import socket
import struct
import subprocess
import tempfile

wrapper = os.environ.get("TERMUX_ISOLATED", __import__("sys").argv[1])
prefix = os.environ.get("PREFIX", "/data/data/com.termux/files/usr")
magic, version = 0x50524354, 1
header = struct.Struct("<IHHIQ")
decision = struct.Struct("<BB96s")
path_request_size = 4 + 4 + 1024 + 1024

def proot_command(args, guest):
    command = [wrapper, "--termux-paths", "--cwd", prefix]
    for arg in args:
        command.extend(("--proot-arg", arg))
    return command + ["--"] + guest

guest_sh = prefix + "/bin/sh"

def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise AssertionError("control channel closed")
        data.extend(chunk)
    return bytes(data)

def recv_frame(sock):
    raw = recv_exact(sock, header.size)
    h = header.unpack(raw)
    assert h[0] == magic and h[1] == version and h[3] <= 4096, h
    return h, recv_exact(sock, h[3])

with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as host:
    target = os.path.join(host, "file")
    with open(target, "w") as stream:
        stream.write("initial\n")
    parent, child_end = socket.socketpair()
    parent_copy = os.dup(parent.fileno())
    parent.close()
    parent = socket.socket(fileno=parent_copy)
    os.dup2(child_end.fileno(), 3)
    fcntl.fcntl(3, fcntl.F_SETFD, 0)
    child_end.close()
    command = proot_command(["--bind=" + host + ":/control:ro", "--control-fd", "3"],
                            [guest_sh, "-c", "set -e; echo one >/control/file; echo two >/control/file"])
    child = subprocess.Popen(command, pass_fds=(3,), stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True,
                             env={**os.environ,
                                  "PATH": os.environ.get("PATH", "/data/data/com.termux/files/usr/bin")})
    parent.settimeout(3)
    try:
        hello, payload = recv_frame(parent)
    except Exception:
        if child.poll() is None:
            child.terminate()
        out, err = child.communicate(timeout=3)
        raise AssertionError("no HELLO; exit=%r stdout=%r stderr=%r" %
                             (child.returncode, out, err))
    assert hello[2] == 1 and payload == b"", hello
    requests = []
    # PRCT approval must not elevate a :ro binding.  The first approved WRITE
    # therefore fails physically and set -e terminates before a second write.
    while len(requests) < 1:
        try:
            frame, payload = recv_frame(parent)
        except Exception:
            if child.poll() is None:
                child.terminate()
            out, err = child.communicate(timeout=3)
            raise AssertionError("path request timeout after %d requests; exit=%r stdout=%r stderr=%r" %
                                 (len(requests), child.returncode, out, err))
        if frame[2] == 4:
            # Shadow notifications are spontaneous and do not consume a
            # decision; the following PATH_ACCESS_REQUEST is correlated by
            # its own request_id.
            assert frame[3] == path_request_size
            continue
        if frame[2] == 3:
            assert frame[3] == path_request_size
            operation, reason = struct.unpack_from("<II", payload)
            path = payload[8:1032].split(b"\0", 1)[0].decode()
            # Infrastructure paths are no longer implicit exemptions.  This
            # test harness explicitly allows startup paths and waits for the
            # request that belongs to the test.
            if path != "/control/file":
                response = decision.pack(1, 0, b"startup" + b"\0" * 89)
                parent.sendall(header.pack(magic, version, 16, len(response), frame[4]) + response)
                continue
            requests.append((operation, reason))
            reply_type = 16
            response = decision.pack(1, 0, b"test" + b"\0" * 92)
            frame_out = header.pack(magic, version, reply_type, len(response), frame[4]) + response
            # Deliberately fragment both header and payload.
            parent.sendall(frame_out[:7])
            parent.sendall(frame_out[7:])
        else:
            raise AssertionError("unexpected frame type: %r" % (frame,))
    stdout, stderr = child.communicate(timeout=3)
    assert len(requests) == 1, requests
    assert child.returncode != 0, child.returncode
    assert "proot warning:" not in stderr, stderr
    assert open(target).read() == "initial\n"
print("control-fd fragmented/path decisions: PASS")
PY

python3 - "$TERMUX_ISOLATED" <<'PY'
import fcntl
import os
import shlex
import shutil
import socket
import struct
import subprocess
import tempfile

wrapper = os.environ.get("TERMUX_ISOLATED", __import__("sys").argv[1])
prefix = os.environ.get("PREFIX", "/data/data/com.termux/files/usr")
guest_sh = prefix + "/bin/sh"
magic, version = 0x50524354, 1
header = struct.Struct("<IHHIQ")
decision = struct.Struct("<BB96s")
command = struct.Struct("<III1024s1024s")

def proot_command(args, guest):
    command = [wrapper, "--termux-paths", "--cwd", prefix]
    for arg in args:
        command.extend(("--proot-arg", arg))
    return command + ["--"] + guest

def frame(sock):
    raw = sock.recv(header.size, socket.MSG_WAITALL)
    if len(raw) != header.size:
        raise AssertionError("control channel closed")
    h = header.unpack(raw)
    payload = sock.recv(h[3], socket.MSG_WAITALL)
    if len(payload) != h[3]:
        raise AssertionError("truncated control payload")
    return h, payload

with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as host:
    target = os.path.join(host, "file")
    open(target, "w").close()
    parent, child_end = socket.socketpair()
    saved = os.dup(parent.fileno())
    parent.close()
    parent = socket.socket(fileno=saved)
    os.dup2(child_end.fileno(), 3)
    fcntl.fcntl(3, fcntl.F_SETFD, 0)
    child_end.close()
    child = subprocess.Popen(
        proot_command(["--bind=" + host + ":/control:ro", "--control-fd", "3"],
                      [guest_sh, "-c", "test ! -e /proc/self/fd/3; cat /control/file >/dev/null"]),
        pass_fds=(3,), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, env={**os.environ,
                       "PATH": os.environ.get("PATH", "/data/data/com.termux/files/usr/bin")})
    parent.settimeout(3)
    hello, payload = frame(parent)
    assert hello[2] == 1 and payload == b"", hello
    rule = command.pack(1, 0, 1, b"/control/file\0".ljust(1024, b"\0"),
                        b"\0" * 1024)
    parent.sendall(header.pack(magic, version, 21, len(rule), 77) + rule)
    result, payload = frame(parent)
    assert result[2] == 5 and result[4] == 77 and result[3] >= 4, result
    parent.settimeout(0.2)
    extra_requests = []
    while child.poll() is None:
        try:
            pending, pending_payload = frame(parent)
        except TimeoutError:
            continue
        if pending[2] == 3:
            extra_requests.append(pending_payload[8:1032].split(b"\0", 1)[0].decode())
            response = decision.pack(1, 0, b"test" + b"\0" * 92)
            parent.sendall(header.pack(magic, version, 16, len(response), pending[4]) + response)
    out, err = child.communicate(timeout=3)
    assert child.returncode == 0, (child.returncode, out, err)
    assert open(target).read() == "", open(target).read()
    assert "/control/file" not in extra_requests, extra_requests
print("control-fd proactive rule: PASS")
PY

python3 - "$TERMUX_ISOLATED" <<'PY'
import fcntl
import os
import shlex
import shutil
import socket
import struct
import subprocess
import tempfile

wrapper = os.environ.get("TERMUX_ISOLATED", __import__("sys").argv[1])
prefix = os.environ.get("PREFIX", "/data/data/com.termux/files/usr")
guest_sh = prefix + "/bin/sh"
magic, version = 0x50524354, 1
header = struct.Struct("<IHHIQ")
decision = struct.Struct("<BB96s")
command = struct.Struct("<III1024s1024s")

def proot_command(args, guest):
    command = [wrapper, "--termux-paths", "--cwd", prefix]
    for arg in args:
        command.extend(("--proot-arg", arg))
    return command + ["--"] + guest

def recv_frame(sock):
    raw = sock.recv(header.size, socket.MSG_WAITALL)
    assert len(raw) == header.size, "control channel closed"
    h = header.unpack(raw)
    payload = sock.recv(h[3], socket.MSG_WAITALL)
    assert len(payload) == h[3], "truncated control payload"
    return h, payload

def run_reveal(scope, expected_visible):
    host_shadow = tempfile.mkdtemp(dir=prefix + "/share")
    # This case stays in the real Termux path view: host and guest paths are
    # both inside the Termux prefix, and no /usr rootfs path is invented.
    guest_sh = prefix + "/bin/sh"
    guest_etc = host_shadow
    guest_passwd = guest_etc + "/passwd"
    with open(os.path.join(host_shadow, "passwd"), "w") as stream:
        stream.write("shadow-test\n")
    shell = ("test -d {etc}; found=0; for p in {etc}/*; do "
             "[ \"$p\" = {passwd} ] && found=1; done; test $found -eq 1").format(
                 etc=shlex.quote(guest_etc), passwd=shlex.quote(guest_passwd))
    parent, child_end = socket.socketpair()
    saved_parent = os.dup(parent.fileno())
    parent.close()
    parent = socket.socket(fileno=saved_parent)
    os.dup2(child_end.fileno(), 3)
    fcntl.fcntl(3, fcntl.F_SETFD, 0)
    child_end.close()
    child = subprocess.Popen(
        proot_command(["--hide", guest_etc, "--control-fd", "3"],
                      [guest_sh, "-c", shell]),
        pass_fds=(3,), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, env={**os.environ,
                       "PATH": os.environ.get("PATH", "/data/data/com.termux/files/usr/bin")})
    hello, payload = recv_frame(parent)
    assert hello[2] == 1 and payload == b"", hello
    rule = command.pack(1, scope, 0, guest_etc.encode() + b"\0" * (1024 - len(guest_etc) - 1),
                        b"\0" * 1024)
    parent.sendall(header.pack(magic, version, 22, len(rule), 91 + scope) + rule)
    while True:
        result, payload = recv_frame(parent)
        if result[2] == 3:
            response = decision.pack(1, 0, b"startup" + b"\0" * 89)
            parent.sendall(header.pack(magic, version, 16, len(response), result[4]) + response)
            continue
        if result[2] == 4:
            continue
        break
    assert result[2] == 5 and result[4] == 91 + scope, result
    parent.settimeout(0.2)
    while child.poll() is None:
        try:
            pending, pending_payload = recv_frame(parent)
        except TimeoutError:
            continue
        if pending[2] == 3:
            response = decision.pack(1, 0, b"post-reveal" + b"\0" * 86)
            parent.sendall(header.pack(magic, version, 16, len(response), pending[4]) + response)
        elif pending[2] == 4:
            continue
    out, err = child.communicate(timeout=3)
    visible = child.returncode == 0
    shutil.rmtree(host_shadow)
    assert visible == expected_visible, (scope, child.returncode, out, err)
    parent.close()

run_reveal(1, False)  # node: /etc is visible, descendants remain hidden
run_reveal(2, True)   # recursive: descendants are visible
print("control-fd shadow reveal scopes: PASS")
PY

GUEST_PREFIX=/data/data/com.termux/files/usr
if env -i PATH="$GUEST_PREFIX/bin" PREFIX="$GUEST_PREFIX" "$TERMUX_ISOLATED" --termux-paths --cwd "$GUEST_PREFIX" \
        --proot-arg --hide --proot-arg "$GUEST_PREFIX/etc/passwd" -- "$GUEST_PREFIX/bin/sh" -c \
        "! test -e '$GUEST_PREFIX/etc/passwd' && ! ls '$GUEST_PREFIX/etc' | grep -qx passwd"; then
    echo 'hide lookup: PASS'
else
    echo 'hide lookup: FAIL' >&2
    exit 1
fi

mask_dir=$(mktemp -d "$TMPDIR/control-mask.XXXXXX")
trap 'rm -f "$mask_dir/file"; rmdir "$mask_dir" 2>/dev/null || true' EXIT
touch "$mask_dir/file"
if env -i PATH="$GUEST_PREFIX/bin" PREFIX="$GUEST_PREFIX" "$TERMUX_ISOLATED" --termux-paths --cwd "$GUEST_PREFIX" \
        --proot-arg "--bind=$mask_dir:/mask:mask" -- "$GUEST_PREFIX/bin/sh" -c \
        'test -d /mask && ! ls /mask >/dev/null 2>&1 && ! cat /mask/file >/dev/null 2>&1'; then
    echo 'mask binding: PASS'
else
    echo 'mask binding: FAIL' >&2
    exit 1
fi
