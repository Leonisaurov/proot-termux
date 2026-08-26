"""Async integration between the PTY, PRCT channel and policy."""
from __future__ import annotations

import os

from ..async_channel import AsyncControlChannel
from .. import ChannelState, ControlEOF
from ..pty_launcher import PtyProotProcess
from .policy import SessionPolicy


class HarnessSession:
    def __init__(self, process: PtyProotProcess, policy=None):
        self.process = process
        # Transfer ownership after the synchronous HELLO. There is exactly
        # one socket object and one closer for the rest of the session.
        self.channel: AsyncControlChannel = AsyncControlChannel.from_socket(
            process.control_channel.detach_socket())
        self.channel.state = ChannelState.READY
        self.policy = policy or SessionPolicy()
        self.output = bytearray()
        self.failed = None
        self.lifecycle = 'running'
        self.last_error = None

    def start_readers(self, loop, on_pty, on_event):
        loop.add_reader(self.process.master_fd, self._read_pty, loop, on_pty)
        loop.add_reader(self.channel.fileno(), self._read_control, loop, on_event)

    def _read_pty(self, loop, callback):
        try:
            data = os.read(self.process.master_fd, 65536)
        except OSError:
            data = b''
        if data:
            callback(data)
        elif self.process.returncode is not None:
            self.lifecycle = 'process_exited'

    def _read_control(self, loop, callback):
        try:
            while True:
                event = self.channel.receive()
                if event is None: break
                self.policy.receive(event)
                callback(event)
        except Exception as exc:
            # EOF after the guest has exited is normal lifecycle, not a
            # protocol failure.  Only surface peer closure while PRoot is
            # still alive as a fail-closed diagnostic.
            if isinstance(exc, ControlEOF) and self.process.returncode is not None:
                self.lifecycle = 'process_exited'
                try:
                    loop.remove_reader(self.channel.fileno())
                except (OSError, ValueError):
                    pass
                return
            self.failed = exc
            self.last_error = exc
            self.lifecycle = 'prct_failed'
            try:
                loop.remove_reader(self.channel.fileno())
            except (OSError, ValueError):
                pass
            callback(exc)

    def write(self, data):
        return os.write(self.process.master_fd, data)

    def decide(self, request_id, decision, persistent=False):
        if self.lifecycle in ('prct_failed', 'closed'):
            raise RuntimeError('PRCT is not available')
        self.channel.respond(request_id, decision, 'interactive decision', persistent=persistent)
        self.channel.flush()
        self.policy.decide(request_id, decision, persistent)

    def close(self, loop=None):
        if self.lifecycle == 'closed':
            return
        self.lifecycle = 'closed'
        if loop:
            loop.remove_reader(self.process.master_fd)
            loop.remove_reader(self.channel.fileno())
        self.channel.close()
        self.process.close()
