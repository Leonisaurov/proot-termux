"""Non-blocking PRCT channel used by event-loop based consumers.

This module deliberately contains no policy.  ``poll``/``receive`` are cheap
and never wait for a complete frame; callers register ``fileno()`` with their
event loop and call ``receive`` until it returns ``None``.
"""
from __future__ import annotations

import errno
import socket
import struct
from collections import deque

from . import (  # re-exported by the package
    HEADER, MAGIC, MAX_FRAME, VERSION, ChannelState, CommandResult,
    ControlEOF, ControlError, Desynchronized, InvalidFrame, Message, NetRequest,
    PathRequest, ShadowEvent, _cstring, _guest,
)


class AsyncControlChannel:
    """Incremental, non-blocking PRCT codec.

    ``receive`` returns an event, ``None`` when more bytes are needed, and
    raises a terminal ``ControlError`` on EOF or malformed input.  Outgoing
    frames are queued and ``flush`` should be called whenever the descriptor
    is writable.
    """

    def __init__(self, sock: socket.socket, *, max_frame=MAX_FRAME):
        self.sock = sock
        self.sock.setblocking(False)
        self.max_frame = max_frame
        self.state = ChannelState.CREATED
        self._input = bytearray()
        self._events = deque()
        self._output = deque()
        self._next_id = 1
        self._commands = set()
        self.closed = False

    @classmethod
    def from_socket(cls, sock):
        return cls(sock)

    @classmethod
    def from_fd(cls, fd):
        return cls(socket.socket(fileno=fd))

    def fileno(self):
        return self.sock.fileno()

    @property
    def wants_write(self):
        return bool(self._output)

    def _fail(self, exc):
        if self.state not in (ChannelState.CLOSED, ChannelState.FAILED):
            self.state = ChannelState.FAILED
        raise exc

    def _decode(self, typ, rid, payload):
        try:
            message = Message(typ)
        except ValueError:
            self._fail(InvalidFrame("message type"))
        if message is Message.HELLO:
            if rid != 0 or payload or self.state is not ChannelState.CREATED:
                self._fail(InvalidFrame("HELLO"))
            self.state = ChannelState.READY
            return None
        if self.state is ChannelState.CREATED:
            self._fail(InvalidFrame("HELLO required"))
        if message is Message.NET_ACCESS_REQUEST:
            if len(payload) != 230:
                self._fail(InvalidFrame("net payload"))
            x = struct.unpack('<IiiHHHH16sBB64s128s', payload)
            if ((x[0] in (1, 2) and x[3] not in (2, 10)) or (x[0] == 3 and x[3] != 0) or (x[0] == 5 and x[3] == 0)):
                self._fail(InvalidFrame("net family"))
            return NetRequest(message, rid, x[0], x[1], x[3], x[4], x[5], x[6],
                              x[7], x[8], x[9], _cstring(x[10]), _cstring(x[11]))
        if message in (Message.PATH_ACCESS_REQUEST, Message.SHADOW_EVENT):
            if len(payload) != 2056:
                self._fail(InvalidFrame("path payload"))
            op, reason, path, other = struct.unpack('<II1024s1024s', payload)
            cls = PathRequest if message is Message.PATH_ACCESS_REQUEST else ShadowEvent
            return cls(message, rid, op, reason, _guest(path), _guest(other))
        if message is Message.COMMAND_RESULT:
            if len(payload) != 20 or rid not in self._commands:
                self._fail(InvalidFrame("unexpected command result"))
            self._commands.remove(rid)
            return CommandResult(message, rid, *struct.unpack('<iIIII', payload))
        self._fail(InvalidFrame("not an event"))

    def _parse(self):
        while True:
            if len(self._input) < HEADER.size:
                return
            magic, version, typ, size, rid = HEADER.unpack(self._input[:HEADER.size])
            if magic != MAGIC or version != VERSION or size > self.max_frame:
                self._fail(InvalidFrame("header"))
            total = HEADER.size + size
            if len(self._input) < total:
                return
            del self._input[:HEADER.size]
            payload = bytes(self._input[:size])
            del self._input[:size]
            event = self._decode(typ, rid, payload)
            if event is not None:
                self._events.append(event)

    def receive(self):
        if self.state in (ChannelState.FAILED, ChannelState.CLOSED):
            raise Desynchronized("channel is terminal")
        if self._events:
            return self._events.popleft()
        try:
            while True:
                try:
                    data = self.sock.recv(65536)
                    if not data:
                        self._fail(ControlEOF("peer closed"))
                    self._input.extend(data)
                    self._parse()
                except BlockingIOError:
                    break
                except OSError as exc:
                    if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                        break
                    self._fail(Desynchronized(str(exc)))
                if self._events:
                    break
            return self._events.popleft() if self._events else None
        except ControlError:
            self.state = ChannelState.FAILED
            raise

    def send_frame(self, typ, request_id=0, payload=b''):
        if self.state in (ChannelState.FAILED, ChannelState.CLOSED):
            raise Desynchronized("channel is terminal")
        payload = bytes(payload)
        if len(payload) > self.max_frame:
            raise InvalidFrame("payload too large")
        self._output.append(memoryview(HEADER.pack(MAGIC, VERSION, int(typ), len(payload), request_id) + payload))

    def flush(self):
        if self.state in (ChannelState.FAILED, ChannelState.CLOSED):
            raise Desynchronized("channel is terminal")
        while self._output:
            frame = self._output[0]
            try:
                sent = self.sock.send(frame)
            except BlockingIOError:
                return
            except OSError as exc:
                self._fail(Desynchronized(str(exc)))
            if sent == 0:
                self._fail(ControlEOF("peer closed"))
            if sent:
                frame = frame[sent:]
                if frame:
                    self._output[0] = frame
                else:
                    self._output.popleft()

    def respond(self, request_id, decision, reason='', reason_code=0, persistent=False):
        raw = reason.encode('utf-8')
        if len(raw) >= 96 or not 0 <= reason_code <= 255:
            raise ValueError('invalid decision')
        from control_api import Decision
        d = Decision(decision)
        typ = {(Decision.ALLOW, False): Message.ALLOW_ONCE,
               (Decision.ALLOW, True): Message.ALLOW_ALWAYS,
               (Decision.DENY, False): Message.DENY_ONCE,
               (Decision.DENY, True): Message.DENY_ALWAYS}[d, persistent]
        self.send_frame(typ, request_id, struct.pack('<BB96s', int(d), reason_code, raw + b'\0' * (96-len(raw))))

    def command(self, typ, payload=b''):
        rid = self._next_id
        self._next_id += 1
        self._commands.add(rid)
        self.send_frame(typ, rid, payload)
        return rid

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.state = ChannelState.CLOSED
        self.sock.close()
