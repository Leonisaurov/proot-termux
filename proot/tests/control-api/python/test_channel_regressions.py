"""PRCT transport regressions using local socketpairs, without a guest."""
import socket
import struct
import unittest
from unittest.mock import Mock

from control_api import (HEADER, MAGIC, VERSION, ChannelState, ControlChannel,
                         ControlEOF, Desynchronized, InvalidFrame, Message)
from control_api.async_channel import AsyncControlChannel


def frame(kind, rid, payload=b''):
    return HEADER.pack(MAGIC, VERSION, kind, len(payload), rid) + payload


class ChannelRegressions(unittest.TestCase):
    def pair(self, cls):
        a, b = socket.socketpair()
        channel = cls.from_socket(a)
        self.addCleanup(b.close)
        self.addCleanup(channel.close)
        return channel, b

    def test_queued_events_delivered_before_eof(self):
        channel, peer = self.pair(AsyncControlChannel)
        payload = struct.pack('<II1024s1024s', 1, 0, b'/guest', b'')
        peer.sendall(frame(Message.HELLO, 0) +
                     frame(Message.PATH_ACCESS_REQUEST, 1, payload) +
                     frame(Message.PATH_ACCESS_REQUEST, 2, payload))
        peer.shutdown(socket.SHUT_WR)
        self.assertEqual(channel.receive().request_id, 1)
        self.assertEqual(channel.receive().request_id, 2)
        with self.assertRaises(ControlEOF):
            channel.receive()

    def test_invalid_utf8_is_terminal(self):
        for cls in (ControlChannel, AsyncControlChannel):
            with self.subTest(channel=cls.__name__):
                channel, peer = self.pair(cls)
                payload = struct.pack('<II1024s1024s', 1, 0, b'/\xff', b'')
                peer.sendall(frame(Message.HELLO, 0) + frame(Message.PATH_ACCESS_REQUEST, 1, payload))
                with self.assertRaises(InvalidFrame):
                    channel.receive()
                self.assertEqual(channel.state, ChannelState.FAILED)

    def test_unterminated_string_is_terminal(self):
        for cls in (ControlChannel, AsyncControlChannel):
            with self.subTest(channel=cls.__name__):
                channel, peer = self.pair(cls)
                payload = struct.pack('<II1024s1024s', 1, 0, b'/' * 1024, b'')
                peer.sendall(frame(Message.HELLO, 0) + frame(Message.PATH_ACCESS_REQUEST, 1, payload))
                with self.assertRaises(InvalidFrame):
                    channel.receive()
                self.assertEqual(channel.state, ChannelState.FAILED)

    def test_partial_sync_write_retries_backpressure(self):
        sock = Mock()
        sock.send.side_effect = [7, BlockingIOError(), 13]
        channel = ControlChannel(sock)
        self.addCleanup(channel.close)
        from unittest.mock import patch
        with patch('control_api.select.select', return_value=([], [sock], [])):
            channel.send_frame(Message.GET_STATE, 1)
        self.assertEqual(sock.send.call_count, 3)
        self.assertEqual(channel.state, ChannelState.CREATED)

    def test_zero_async_write_fails_instead_of_spinning(self):
        sock = Mock()
        # A subsequent exception bounds the test even with the old loop.
        sock.send.side_effect = [0, RuntimeError('unexpected retry')]
        channel = AsyncControlChannel(sock)
        self.addCleanup(channel.close)
        channel.send_frame(Message.GET_STATE, 1)
        with self.assertRaises(ControlEOF):
            channel.flush()
        self.assertEqual(channel.state, ChannelState.FAILED)

    def test_flush_cannot_reuse_failed_channel(self):
        channel, _ = self.pair(AsyncControlChannel)
        channel.send_frame(Message.GET_STATE, 1)
        channel.state = ChannelState.FAILED
        with self.assertRaises(Desynchronized):
            channel.flush()

    def test_pty_handshake_failure_closes_owned_socket(self):
        from unittest.mock import patch
        from control_api.pty_launcher import PtyProotProcess
        sockets = []
        original = socket.socketpair
        def pair(*args, **kwargs):
            result = original(*args, **kwargs)
            sockets.extend(result)
            return result
        process = Mock()
        with patch('control_api.pty_launcher.socket.socketpair', side_effect=pair), \
             patch('control_api.pty_launcher.subprocess.Popen', return_value=process), \
             patch('control_api.ControlChannel.handshake', side_effect=InvalidFrame('HELLO')):
            with self.assertRaises(InvalidFrame):
                PtyProotProcess.spawn()
        self.assertTrue(all(sock.fileno() == -1 for sock in sockets))
        process.kill.assert_called_once()
        process.wait.assert_called_once()
