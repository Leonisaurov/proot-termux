#!/usr/bin/env bash
# Reproduce loopback accept/accept4 across direct, virtual, and published paths.
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
TERMUX_ISOLATED="${TERMUX_ISOLATED:-$REPO_ROOT/bin/termux-isolated}"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
CC="${CC:-$PREFIX/bin/clang}"
FIXTURE="$TMPDIR/proot-accept-fixture-$$"
trap 'rm -f "$FIXTURE"' EXIT

if [[ ! -x "$TERMUX_ISOLATED" || ! -x "$CC" ]]; then
    echo "SKIP: termux-isolated or clang is unavailable"
    exit 0
fi

"$CC" -O2 "$SCRIPT_DIR/fixtures/accept_fixture.c" -o "$FIXTURE"

python3 - "$TERMUX_ISOLATED" "$FIXTURE" "$PREFIX" <<'PY'
import os
import select
import socket
import subprocess
import sys
import time

wrapper, fixture, prefix = sys.argv[1:]


def read_line(process, timeout=5):
    ready, _, _ = select.select([process.stdout], [], [], timeout)
    if not ready:
        raise AssertionError("server did not report READY")
    line = process.stdout.readline()
    if not line:
        output, error = process.communicate(timeout=2)
        raise AssertionError("server exited: output=%r error=%r" % (output, error))
    return line.strip()


def command(proot_args, role, family, accept_kind, port):
    result = [wrapper, "--termux-paths", "--cwd", prefix]
    for argument in proot_args:
        result.extend(("--proot-arg", argument))
    result.extend(("--", fixture, role, str(family), accept_kind, str(port)))
    return result


def finish(process, label):
    try:
        output, error = process.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        output, error = process.communicate(timeout=5)
        raise AssertionError("%s timed out: output=%r error=%r" % (label, output, error))
    assert process.returncode == 0, "%s exit=%r output=%r error=%r" % (
        label, process.returncode, output, error)
    return output, error


def run_internal(mode, family, accept_kind, port):
    proxy = "accept-matrix-%d" % os.getpid() if mode == "proxy" else None
    args = ["--proxy", proxy] if proxy else []
    server = subprocess.Popen(
        command(args, "server", family, accept_kind, port),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    try:
        ready = read_line(server)
        assert ready.startswith("READY:%d:%s:" % (socket.AF_INET6 if family == 6 else socket.AF_INET, accept_kind)), ready
        client = subprocess.run(
            command(args, "client", family, accept_kind, port),
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert client.returncode == 0, (client.returncode, client.stdout, client.stderr)
        assert "CLIENT_OK:" in client.stdout, client.stdout
        output, error = finish(server, "%s IPv%d %s" % (mode, family, accept_kind))
        assert "ACCEPT_OK:" in output, (output, error)
        if mode == "proxy":
            # Android Bionic requires the peer family to match the AF_UNIX fd.
            expected_family, expected_length = socket.AF_UNIX, "2"
        else:
            expected_family = socket.AF_INET6 if family == 6 else socket.AF_INET
            expected_length = "16" if family == 4 else "28"
        assert "ACCEPT_OK:%d:" % expected_family in output, (output, error)
        assert ":%s" % expected_length in output, (output, error)
        print("PASS: %s IPv%d %s direct guest client" % (mode, family, accept_kind))
    except Exception:
        if server.poll() is None:
            server.kill()
        output, error = server.communicate(timeout=5)
        raise AssertionError("%s IPv%d %s failed: output=%r error=%r" %
                             (mode, family, accept_kind, output, error))


def run_published(family, accept_kind, host_port):
    proxy = "accept-publish-%d" % os.getpid()
    args = ["--proxy", proxy, "-p", "%d:%d" % (host_port, host_port)]
    server = subprocess.Popen(
        command(args, "server", family, accept_kind, host_port),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    try:
        ready = read_line(server)
        assert ready.startswith("READY:"), ready
        with socket.create_connection(("127.0.0.1", host_port), timeout=5):
            pass
        output, error = finish(server, "published IPv%d %s" % (family, accept_kind))
        assert "ACCEPT_OK:" in output, (output, error)
        # Published proxy listeners also return the underlying AF_UNIX peer.
        assert "ACCEPT_OK:%d:" % socket.AF_UNIX in output, (output, error)
        assert ":2" in output, (output, error)
        print("PASS: published IPv%d %s host client" % (family, accept_kind))
    except Exception:
        if server.poll() is None:
            server.kill()
        output, error = server.communicate(timeout=5)
        raise AssertionError("published IPv%d %s failed: output=%r error=%r" %
                             (family, accept_kind, output, error))


port_base = 20000 + (os.getpid() % 20000)
case = 0
for mode in ("direct", "proxy"):
    for family in (4, 6):
        for accept_kind in ("accept", "accept4"):
            run_internal(mode, family, accept_kind, port_base + case)
            case += 1

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
    probe.bind(("127.0.0.1", 0))
    published_port = probe.getsockname()[1]
run_published(4, "accept", published_port)
print("=== accept matrix: PASS=9 FAIL=0 ===")
PY
