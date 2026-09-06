#!/usr/bin/env bash
# Host protocol tests; guest paths deliberately use the Termux prefix (-r /).
set -euo pipefail
: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"
command -v python3 >/dev/null
command -v proot >/dev/null
python3 - <<'PY'
import array
import os
import socket
import struct
import subprocess
import tempfile
import time

prefix = os.environ['PREFIX']
proot = os.environ.get('PROOT', prefix + '/bin/proot')
with tempfile.TemporaryDirectory(prefix='supervise-protocol.', dir=os.environ['TMPDIR']) as host:
    env = dict(PATH=prefix + '/bin', PREFIX=prefix, TMPDIR=host,
               PROOT_TMP_DIR=host, PROOT_RUNTIME_DIR=host)
    process = subprocess.Popen([proot, '-r', '/', '-w', prefix, '--kill-on-exit',
                                '--supervise', prefix + '/bin/sleep', '30'],
                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        # The core uses a zero-padded sockaddr_un, including its trailing zeros.
        address = (b'\0proot-exec-' + str(process.pid).encode()).ljust(108, b'\0')
        def connect():
            end = time.monotonic() + 3
            while True:
                peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                peer.settimeout(3)
                try:
                    peer.connect(address)
                    return peer
                except ConnectionRefusedError:
                    peer.close()
                    if time.monotonic() >= end:
                        raise
                    time.sleep(.02)
        def rejected(peer):
            try:
                assert peer.recv(64) == b'', 'malformed client received an exec response'
            except ConnectionResetError:
                pass
        argv = (prefix + '/bin/true').encode() + b'\0'
        valid = struct.pack('=i4096s4096s', 1, argv, b'')
        with open(os.devnull, 'rb+') as null:
            with connect() as peer:
                peer.sendmsg([b'x'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                     array.array('i', [null.fileno()] * 3))])
                peer.sendall(struct.pack('=i4096s4096s', 1, argv, b'x' * 4096))
                rejected(peer)
            with connect() as peer:
                peer.sendall(b'x' + valid)  # no SCM_RIGHTS descriptors
                rejected(peer)
            with connect() as peer:
                peer.sendmsg([b'x'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                     array.array('i', [null.fileno()] * 5))])
                peer.sendall(valid)
                rejected(peer)
            with connect() as peer:
                # An idle peer must have a bounded lifetime in the event loop.
                rejected(peer)
        result = subprocess.run([proot, '--exec', str(process.pid), prefix + '/bin/true'],
                                env=env, capture_output=True, timeout=5)
        assert result.returncode == 0, result.stderr
        print('supervisor malformed requests and recovery: PASS')
    finally:
        if process.poll() is None:
            process.kill()
        process.communicate()
PY
