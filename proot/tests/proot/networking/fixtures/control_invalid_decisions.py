"""Exercise the core protocol with explicit Termux guest and host paths."""
import os
import socket
import struct
import subprocess
import tempfile
import unittest

HEADER = struct.Struct('<IHHIQ')
PREFIX = os.environ['PREFIX']


def send(peer, typ, rid, payload=b''):
    peer.sendall(HEADER.pack(0x50524354, 1, typ, len(payload), rid) + payload)


def exact(peer, size):
    data = bytearray()
    while len(data) < size:
        part = peer.recv(size - len(data))
        if not part:
            raise EOFError
        data.extend(part)
    return bytes(data)


class InvalidDecisions(unittest.TestCase):
    def run_case(self, kind, malformed):
        with tempfile.TemporaryDirectory(prefix='prct-invalid.', dir=os.environ['TMPDIR']) as host:
            # -r / deliberately preserves Termux guest paths. Runtime is host-only.
            target = '/audit-control/target'
            with open(host + '/target', 'w') as fixture:
                fixture.write('original')
            bindings = ['-b', host + ':/audit-control:ro'] if kind == 'path' else []
            code = ('import socket\n' if kind == 'net' else '')
            operation = 'socket.socket().close()' if kind == 'net' else f'open({target!r}, "w").close()'
            code += f'for _ in range(2):\n try:\n  {operation}\n  print("AUTHORIZED", flush=True)\n except OSError:\n  pass\n'
            peer, child_fd = socket.socketpair()
            peer.settimeout(5)
            env = {key: os.environ[key] for key in ('PREFIX', 'PATH')}
            env.update(TMPDIR=host, PROOT_TMP_DIR=host, PROOT_RUNTIME_DIR=host)
            process = subprocess.Popen(
                [os.environ.get('PROOT', PREFIX + '/bin/proot'), '-r', '/', '-w', PREFIX,
                 *bindings, '--kill-on-exit', '--control-fd', str(child_fd.fileno()),
                 PREFIX + '/bin/python3', '-c', code],
                pass_fds=(child_fd.fileno(),), env=env,
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            child_fd.close()
            injected = False
            try:
                while True:
                    try:
                        magic, version, typ, size, rid = HEADER.unpack(exact(peer, HEADER.size))
                        self.assertEqual((magic, version), (0x50524354, 1))
                        self.assertLessEqual(size, 4096)
                        payload = exact(peer, size)
                    except (EOFError, ConnectionResetError):
                        break
                    if typ in (1, 4, 5):
                        continue
                    self.assertIn(typ, (2, 3))
                    is_target = (kind == 'net' and typ == 2) or (
                        kind == 'path' and typ == 3 and payload[8:1032].split(b'\0', 1)[0] == target.encode())
                    if is_target:
                        self.assertFalse(injected, 'failed channel was reused for authorization')
                        injected = True
                        good = struct.pack('<BB96s', 1, 0, b'audit')
                        if malformed == 'commands_only':
                            for index in range(32):
                                send(peer, 24, rid + 1000 + index)
                        elif malformed == 'result_as_decision':
                            send(peer, 5, rid, good)
                        elif malformed == 'oversized':
                            send(peer, 16, rid, good + b'x')
                        else:
                            send(peer, 18, rid, good)  # DENY header with ALLOW payload
                    else:
                        send(peer, 16, rid, struct.pack('<BB96s', 1, 0, b'startup'))
                try:
                    out, _ = process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    out, _ = process.communicate(timeout=5)
                self.assertTrue(injected)
                self.assertNotIn(b'AUTHORIZED', out)
                if kind == 'path':
                    with open(host + '/target') as fixture:
                        self.assertEqual(fixture.read(), 'original')
            finally:
                peer.close()
                if process.poll() is None:
                    process.kill()
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=5)

    def test_invalid_decisions(self):
        for kind in ('net', 'path'):
            for malformed in ('result_as_decision', 'oversized', 'contradiction', 'commands_only'):
                with self.subTest(kind=kind, malformed=malformed):
                    self.run_case(kind, malformed)


if __name__ == '__main__':
    unittest.main()
