"""Real control-fd coverage for a writable Termux TMPDIR."""
import os
import select
import shutil
import time
import unittest

from control_api import PathRequest
from control_api.pty_launcher import PtyProotProcess
from control_api.proot_tui.presets import build_config, default_config


@unittest.skipUnless(shutil.which("proot") and shutil.which("python3"),
                     "local PRoot and Python are required")
class TmpdirControlIntegrationTests(unittest.TestCase):
    def test_tmpdir_write_waits_for_control_fd(self):
        tmpdir = os.environ.get("TMPDIR", "/data/data/com.termux/files/usr/tmp")
        code = 'printf control-fd > "$TMPDIR/control-fd-test"'
        config = default_config(command=("sh", "-c", code))
        tmpdir = os.environ.get("TMPDIR", "/data/data/com.termux/files/usr/tmp")
        # Remove only the helper's narrow RW bind. The prefix remains RO, so
        # this proves the mutation reaches PRCT and remains fail-closed.
        config.binds = [binding for binding in config.binds
                        if not (binding.host == tmpdir and binding.mode == "rw")]

        process = PtyProotProcess.spawn(build_config(config))
        requests = []
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
                        # The child may exit just after PRCT EOF; reap it so
                        # the test does not mistake that normal race for a
                        # protocol failure.
                        try:
                            process.process.wait(timeout=1)
                        except Exception:
                            pass
                        break
                    if isinstance(event, PathRequest):
                        requests.append(event)
                        process.control_channel.deny_once(event.request_id)
                if process.returncode is not None:
                    break
            self.assertNotEqual(process.returncode, 0, output.decode(errors="replace"))
            self.assertTrue(any(
                request.operation in (2, 3) and request.path.startswith(tmpdir)
                for request in requests), [request.path for request in requests])
        finally:
            process.close()
            try:
                os.unlink(os.path.join(tmpdir, "control-fd-test"))
            except FileNotFoundError:
                pass


@unittest.skipUnless(shutil.which("proot") and shutil.which("python3"),
                     "local PRoot and Python are required")
class TmpdirBindIntegrationTests(unittest.TestCase):
    def test_default_tmpdir_bind_is_writable(self):
        config = default_config(command=(
            "sh", "-c", 'test -w "$TMPDIR" && printf ok > "$TMPDIR/default-bind-test"'))
        process = PtyProotProcess.spawn(build_config(config))
        requests = []
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and process.returncode is None:
                ready, _, _ = select.select(
                    [process.master_fd, process.control_channel.sock], [], [], .1)
                if process.control_channel.sock in ready:
                    try:
                        event = process.control_channel.receive()
                    except Exception:
                        try:
                            process.process.wait(timeout=1)
                        except Exception:
                            pass
                        break
                    if isinstance(event, PathRequest):
                        requests.append(event)
                        process.control_channel.allow_once(event.request_id)
            self.assertEqual(process.returncode, 0)
            tmpdir = os.environ.get("TMPDIR", "/data/data/com.termux/files/usr/tmp")
            self.assertFalse(any(request.path.startswith(tmpdir) for request in requests),
                             [request.path for request in requests])
        finally:
            process.close()
            try:
                os.unlink(os.path.join(
                    os.environ.get("TMPDIR", "/data/data/com.termux/files/usr/tmp"),
                    "default-bind-test"))
            except FileNotFoundError:
                pass
