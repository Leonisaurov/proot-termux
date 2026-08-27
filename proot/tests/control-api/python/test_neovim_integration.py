"""Regression coverage for Neovim's local sockets under the PRCT harness."""
import os
import select
import shutil
import time
import unittest

from control_api import ControlEOF, NetRequest, PathRequest
from control_api.pty_launcher import PtyProotProcess
from control_api.proot_tui.presets import build_config, default_config


@unittest.skipUnless(shutil.which('proot') and shutil.which('nvim'),
                     'local PRoot and Neovim are required')
class NeovimIntegrationTests(unittest.TestCase):
    def test_unix_socket_does_not_become_invalid_net_request(self):
        config = default_config(command=('nvim', '--clean'))
        process = PtyProotProcess.spawn(build_config(config))
        invalid_families = []
        events = 0
        deadline = time.monotonic() + 8
        quit_sent = False
        try:
            while time.monotonic() < deadline:
                ready, _, _ = select.select(
                    [process.master_fd, process.control_channel.sock], [], [], .05)
                if process.master_fd in ready:
                    try:
                        data = os.read(process.master_fd, 65536)
                    except OSError:
                        data = b''
                    if data and not quit_sent:
                        # Neovim is in normal mode after startup; leave it
                        # through its own UI so the PTY lifecycle is covered.
                        os.write(process.master_fd, b'\x1b:qa!\r')
                        quit_sent = True
                if process.control_channel.sock in ready:
                    try:
                        event = process.control_channel.receive()
                    except ControlEOF:
                        # PRoot closes PRCT as part of a normal Neovim exit;
                        # the child poll can lag the socket EOF by one tick.
                        try:
                            process.process.wait(timeout=1)
                        except Exception:
                            pass
                        break
                    if event is None:
                        continue
                    events += 1
                    if isinstance(event, (PathRequest, NetRequest)):
                        if isinstance(event, NetRequest) and event.family == 0:
                            invalid_families.append((event.operation, event.family))
                        process.control_channel.allow_once(event.request_id)
                if quit_sent and process.returncode is not None:
                    break
            self.assertGreater(events, 0)
            self.assertEqual(invalid_families, [])
        finally:
            process.close()

    def test_unix_socket_bind_in_tmpdir_is_authorized(self):
        tmpdir = os.environ.get("TMPDIR", "/data/data/com.termux/files/usr/tmp")
        socket_path = os.path.join(tmpdir, "prct-unix-bind-test")
        code = (
            "import os,socket; p=os.path.join(os.environ['TMPDIR'],"
            "'prct-unix-bind-test'); s=socket.socket(socket.AF_UNIX); "
            "s.bind(p); s.close(); os.unlink(p)")
        config = default_config(command=("python3", "-c", code))
        process = PtyProotProcess.spawn(build_config(config))
        socket_events = 0
        output = bytearray()
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                ready, _, _ = select.select(
                    [process.master_fd, process.control_channel.sock], [], [], .1)
                if process.master_fd in ready:
                    try:
                        output.extend(os.read(process.master_fd, 65536))
                    except OSError:
                        pass
                if process.control_channel.sock in ready:
                    try:
                        event = process.control_channel.receive()
                    except Exception:
                        try:
                            process.process.wait(timeout=1)
                        except Exception:
                            pass
                        break
                    if isinstance(event, NetRequest) and event.operation == 5:
                        socket_events += 1
                        self.assertEqual(event.family, 1)
                    if isinstance(event, (PathRequest, NetRequest)):
                        process.control_channel.allow_once(event.request_id)
                if process.returncode is not None:
                    break
            self.assertEqual(process.returncode, 0, output.decode(errors="replace"))
            self.assertEqual(socket_events, 1)
        finally:
            process.close()
            try:
                os.unlink(socket_path)
            except FileNotFoundError:
                pass


if __name__ == '__main__':
    unittest.main()
