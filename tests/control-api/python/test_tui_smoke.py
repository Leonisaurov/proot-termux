"""Headless real-PTY smoke test for the sample harness."""
import os
import pty
import select
import shutil
import signal
import subprocess
import time
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))

@unittest.skipUnless(shutil.which("nvim") and os.path.exists(
    os.path.join(ROOT, "bin", "sample-harness")),
    "sample harness and Neovim are required")
class HarnessTuiSmokeTests(unittest.TestCase):
    def _run_and_close(self, command):
        root = ROOT
        master, slave = pty.openpty()
        process = subprocess.Popen(
            [os.path.join(root, "bin", "sample-harness"), *command],
            stdin=slave, stdout=slave, stderr=slave, close_fds=True)
        os.close(slave)
        output = bytearray()
        deadline = time.monotonic() + 8
        sent_quit = False
        try:
            while time.monotonic() < deadline:
                ready, _, _ = select.select([master], [], [], .1)
                if master in ready:
                    try:
                        output.extend(os.read(master, 65536))
                    except OSError:
                        pass
                if not sent_quit and time.monotonic() >= deadline - 5:
                    os.write(master, b"\x11")  # Ctrl-Q
                    sent_quit = True
                if process.poll() is not None:
                    break
            if process.poll() is None:
                os.kill(process.pid, signal.SIGKILL)
                process.wait()
            text = output.decode(errors="replace")
            self.assertNotIn("PRCT FAILED", text)
            self.assertNotIn("channel is terminal", text)
            self.assertIn(process.returncode, (0, 130), text[-1000:])
        finally:
            try:
                os.close(master)
            except OSError:
                pass
            if process.poll() is None:
                process.kill()
                process.wait()

    def test_nvim_opens_and_ctrl_q_closes_without_prct_failure(self):
        self._run_and_close(["--shell", "nvim", "--", "--clean"])

    def test_default_sample_harness_opens_and_closes(self):
        self._run_and_close([])
