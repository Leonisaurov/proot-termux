"""Real fish/PRoot coverage for the terminal widget.

The test is intentionally skipped on machines without the local Termux PRoot
and fish binaries; when present it exercises the same PTY and PRCT path as the
harness and approves only the fixture's startup accesses.
"""
import os
import select
import shutil
import time
import unittest

from control_api import NetRequest, PathRequest
from control_api.pty_launcher import PtyProotProcess
from control_api.proot_tui.presets import build_config, default_config
from control_api.proot_tui.terminal import ExternalTerminal


@unittest.skipUnless(shutil.which('proot') and shutil.which('fish'),
                     'local PRoot and fish are required')
class FishTerminalIntegrationTests(unittest.TestCase):
    def test_startup_editing_utf8_and_ansi(self):
        config = default_config(shell='fish', command=('fish', '--no-config', '-i'))
        process = PtyProotProcess.spawn(build_config(config))

        class Session:
            def __init__(self, child):
                self.process = child
                self.writes = []

            def write(self, data):
                self.writes.append(data)
                return os.write(self.process.master_fd, data)

        session = Session(process)
        terminal = ExternalTerminal(session)
        terminal._resize(24, 80)
        phase = 0
        completion_sent = False
        raw = bytearray()
        commands = [
            b"printf '\\033[38;5;196mFISH-ANSI\\033[0m\\n'\r",
            "printf 'unicode: café ☃\\n'\r".encode('utf-8'),
        ]
        deadline = time.monotonic() + 7
        try:
            while time.monotonic() < deadline and (
                    phase < len(commands) or not any(
                        'unicode: café ☃' in line.plain for line in terminal._lines)
                    or not completion_sent or not any(
                        'bin/' in line.plain for line in terminal._lines)):
                ready, _, _ = select.select(
                    [process.master_fd, process.control_channel.sock], [], [], .05)
                if process.master_fd in ready:
                    try:
                        data = os.read(process.master_fd, 65536)
                    except OSError:
                        data = b''
                    if data:
                        raw.extend(data)
                        terminal.feed_bytes(data)
                        rows = [line.plain for line in terminal._lines]
                        ready_for_command = phase < len(commands) and any(
                            '>' in row for row in rows)
                        if phase == 1:
                            ready_for_command = ready_for_command and any(
                                'FISH-ANSI' in row for row in rows)
                        if ready_for_command:
                            os.write(process.master_fd, commands[phase])
                            phase += 1
                        elif phase == len(commands) and not completion_sent and any(
                                'unicode: café ☃' in row for row in rows):
                            os.write(process.master_fd,
                                     b'echo /data/data/com.termux/files/usr/b\t\t')
                            completion_sent = True
                if process.control_channel.sock in ready:
                    event = process.control_channel.receive()
                    if isinstance(event, (PathRequest, NetRequest)):
                        process.control_channel.allow_once(event.request_id)

            rows = [line for line in terminal._lines if line.plain]
            text = '\n'.join(line.plain for line in rows)
            self.assertEqual(phase, len(commands), text)
            self.assertTrue(completion_sent)
            self.assertTrue(any('bin/' in line.plain for line in rows), text)
            self.assertTrue(terminal.kitty_keyboard)
            self.assertIn('FISH-ANSI', text)
            self.assertIn('unicode: café ☃', text)
            self.assertNotIn('696e646e', text)
            self.assertTrue(any(b'\x1bP1+r696e646e=' in data
                               for data in session.writes))
            ansi = next(line for line in rows if line.plain.strip() == 'FISH-ANSI')
            self.assertTrue(any('#ff0000' in str(span.style) for span in ansi.spans))
            self.assertIn(b'\x1bP+q', raw)
        finally:
            process.close()
