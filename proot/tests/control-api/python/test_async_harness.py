import socket
import struct
import unittest

from control_api import (HEADER, MAGIC, VERSION, Message, ChannelState,
                         PathRequest, ControlError, ControlChannel)
from control_api.async_channel import AsyncControlChannel
from control_api.proot_tui.policy import SessionPolicy
from control_api.proot_tui.presets import HarnessConfig, build_config
from control_api.termux_paths import (TERMUX_RUNTIME_BIND_FILES, TERMUX_RUNTIME_BIND_PATHS,
                          termux_runtime_bindings)


class TestAsyncChannel(unittest.TestCase):
    def setUp(self):
        self.a, self.b = socket.socketpair()
        self.channel = AsyncControlChannel.from_socket(self.a)

    def tearDown(self):
        self.channel.close()
        self.b.close()

    def frame(self, typ, rid, payload=b''):
        return HEADER.pack(MAGIC, VERSION, typ, len(payload), rid) + payload

    def test_fragmented_hello_and_path(self):
        payload = struct.pack('<II1024s1024s', 1, 7, b'/guest/x\0'.ljust(1024, b'\0'), b'\0' * 1024)
        raw = self.frame(Message.HELLO, 0) + self.frame(Message.PATH_ACCESS_REQUEST, 9, payload)
        self.b.send(raw[:5]); self.assertIsNone(self.channel.receive())
        self.b.send(raw[5:])
        event = self.channel.receive()
        self.assertIsInstance(event, PathRequest)
        self.assertEqual(event.path, '/guest/x')

    def test_eof_is_terminal_and_does_not_approve(self):
        self.b.close()
        with self.assertRaises(ControlError): self.channel.receive()
        self.assertEqual(self.channel.state, ChannelState.FAILED)

    def test_command_result_id_is_checked(self):
        self.b.send(self.frame(Message.HELLO, 0))
        self.channel.receive()
        payload = struct.pack('<iIIII', 0, 0, 0, 0, 0)
        self.b.send(self.frame(Message.COMMAND_RESULT, 99, payload))
        with self.assertRaises(ControlError): self.channel.receive()
        self.assertEqual(self.channel.state, ChannelState.FAILED)


class TestPolicyAndPresets(unittest.TestCase):
    def test_requests_are_pending_until_explicit_decision(self):
        req = PathRequest(Message.PATH_ACCESS_REQUEST, 4, 1, 0, '/x', '')
        policy = SessionPolicy()
        policy.receive(req)
        self.assertIn(4, policy.pending)
        policy.decide(4, 1, persistent=True)
        self.assertEqual(len(policy.rules), 1)

    def test_termux_defaults_are_explicit(self):
        from control_api.proot_tui.presets import default_config
        cfg = default_config()
        args = build_config(cfg).args
        # This is harness metadata; PRoot itself does not implement the
        # termux-isolated launcher flag.  The mode is expressed by bindings.
        self.assertTrue(cfg.termux_paths)
        self.assertNotIn('--termux-paths', args)
        self.assertIn('--net-policy', args)
        self.assertIn('--net-allow', args)
        self.assertIn('*', args)
        self.assertIn(':mask', ' '.join(args))
        self.assertNotIn('--proc-isolated', args)
        self.assertEqual(build_config(cfg).env['TERM'], 'xterm-kitty')
        binds = {(b.host, b.guest, b.mode) for b in cfg.binds}
        for binding in termux_runtime_bindings():
            self.assertIn(binding, binds)
        self.assertGreaterEqual(len(TERMUX_RUNTIME_BIND_PATHS), 8)
        self.assertGreaterEqual(len(TERMUX_RUNTIME_BIND_FILES), 3)


    def test_channel_socket_can_be_transferred_once(self):
        a, b = socket.socketpair()
        channel = ControlChannel.from_socket(a)
        transferred = channel.detach_socket()
        self.assertTrue(channel.closed)
        self.assertEqual(channel.sock, None)
        transferred.close(); b.close()
        with self.assertRaises(Exception): channel.detach_socket()


if __name__ == '__main__':
    unittest.main()
