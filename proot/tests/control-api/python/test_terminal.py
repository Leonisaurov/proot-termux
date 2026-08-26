import asyncio
import unittest

from control_api.proot_tui.terminal import ExternalTerminal


class _Process:
    def resize(self, rows, cols):
        pass


class _Session:
    process = _Process()

    def __init__(self):
        self.writes = []

    def write(self, data):
        self.writes.append(data)


class _Key:
    def __init__(self, key, character=None):
        self.key = key
        self.character = character
        self.stopped = False

    def stop(self):
        self.stopped = True


class TerminalRendererTests(unittest.TestCase):
    def make_terminal(self):
        terminal = ExternalTerminal(_Session())
        terminal._resize(6, 40)
        return terminal

    def test_fragmented_truecolor_and_fish_repaint(self):
        terminal = self.make_terminal()
        terminal.feed_bytes(b'\x1b[38;2;12;34')
        terminal.feed_bytes(b';56mFish\x1b[0m\r\x1b[2K> ')
        self.assertEqual(terminal._lines[0].plain, '> ')
        self.assertEqual(terminal._escape, '')
        self.assertEqual(terminal.fg, 'white')

    def test_render_cache_is_invalidated_only_by_terminal_changes(self):
        terminal = self.make_terminal()
        terminal.feed_bytes(b'neovim status line')
        first = terminal.render()
        self.assertIs(first, terminal.render())
        self.assertNotIn('_RenderCache', type(first).__name__)
        terminal.feed_bytes(b'\x1b[1D')
        self.assertIsNot(first, terminal.render())
        self.assertIs(terminal.render(), terminal.render())

    def test_vt_default_erase_modes_keep_text_before_cursor(self):
        terminal = self.make_terminal()
        terminal.feed_bytes(b'fish prompt\x1b[K')
        self.assertEqual(terminal._lines[0].plain, 'fish prompt')
        terminal.feed_bytes(b'\r\x1b[2K')
        self.assertEqual(terminal._lines[0].plain, '')

    def test_cursor_modes_and_extended_sgr(self):
        terminal = self.make_terminal()
        terminal.feed_bytes(b'\x1b[1;3;4;7;9mstyled\x1b[22;23;24;27;29m')
        self.assertTrue(terminal._lines[0].spans)
        terminal.feed_bytes(b'\x1b[?25l')
        self.assertFalse(terminal.cursor_visible)
        terminal.feed_bytes(b'\x1b[?25h')
        self.assertTrue(terminal.cursor_visible)

    def test_fish_capability_queries_are_not_screen_text(self):
        terminal = self.make_terminal()
        terminal.feed_bytes(
            b'\x1b[?u\x1b[>0q\x1b]11;?\x1b\\'
            b'\x1b[?1049h\x1bP+q696e646e\x1b\\\x1b[?1049l')
        self.assertEqual(terminal._lines[0].plain, '')
        self.assertIsNone(terminal._alternate)
        writes = b''.join(terminal.session.writes)
        self.assertIn(b'\x1b[?0u', writes)
        self.assertIn(b'\x1b]11;rgb:0000/0000/0000\x1b\\', writes)
        self.assertIn(b'\x1bP1+r696e646e=1b5b257031256453\x1b\\', writes)

    def test_dcs_fragment_is_buffered(self):
        terminal = self.make_terminal()
        terminal.feed_bytes(b'\x1bP+q696')
        terminal.feed_bytes(b'e\x1b\\Prompt')
        self.assertEqual(terminal._lines[0].plain, 'Prompt')
        self.assertEqual(terminal._escape, '')

    def test_unknown_dcs_is_consumed_and_known_fish_capabilities_are_answered(self):
        terminal = self.make_terminal()
        terminal.feed_bytes(b'\x1bP+q696e646e\x1b\\')
        terminal.feed_bytes(b'\x1bP+q71756572792d6f732d6e616d65\x1b\\')
        writes = b''.join(terminal.session.writes)
        self.assertIn(b'696e646e=1b5b257031256453', writes)
        self.assertIn(b'71756572792d6f732d6e616d65=4c696e7578', writes)
        terminal.feed_bytes(b'\x1bP+q6e6f6e65\x1b\\Prompt')
        self.assertEqual(terminal._lines[0].plain, 'Prompt')

    def test_fish_scrolling_paste_focus_and_kitty_keys(self):
        terminal = self.make_terminal()
        terminal.feed_bytes(b'one\ntwo\nthree\x1b[1S')
        self.assertIn('one', [line.plain for line in terminal._scrollback])
        terminal.feed_bytes(b'\x1b[?2004h\x1b[=5u')
        self.assertTrue(terminal.bracketed_paste)
        self.assertTrue(terminal.kitty_keyboard)
        class Paste:
            text = 'echo fish'
            def stop(self): pass
        terminal.on_paste(Paste())
        self.assertEqual(terminal.session.writes[-1], b'\x1b[200~echo fish\x1b[201~')
        asyncio.run(terminal.on_key(_Key('up')))
        self.assertEqual(terminal.session.writes[-1], b'\x1b[1;1A')
        asyncio.run(terminal.on_key(_Key('f5')))
        self.assertEqual(terminal.session.writes[-1], b'\x1b[15;1~')
        terminal.kitty_keyboard = False
        asyncio.run(terminal.on_key(_Key('shift+tab')))
        self.assertEqual(terminal.session.writes[-1], b'\x1b[Z')

    def test_terminal_focus_consumes_shell_keys(self):
        terminal = self.make_terminal()
        event = _Key('a', 'a')
        asyncio.run(terminal.on_key(event))
        self.assertEqual(terminal.session.writes, [b'a'])
        self.assertTrue(event.stopped)

    def test_incremental_utf8_private_kitty_and_insert_mode(self):
        terminal = self.make_terminal()
        terminal.feed_bytes('界'.encode('utf-8')[:1])
        self.assertEqual(terminal._lines[0].plain, '')
        terminal.feed_bytes('界'.encode('utf-8')[1:])
        self.assertEqual(terminal._lines[0].plain, '界')
        terminal.feed_bytes(b'\rX\x1b[4hY\x1b[4l')
        self.assertEqual(terminal._lines[0].plain, 'XY')
        terminal.feed_bytes(b'\x1b[>4;1mZ')
        self.assertTrue(terminal.kitty_keyboard)
        self.assertFalse(terminal.bold)

    def test_osc_metadata_clipboard_mouse_and_view_scrollback(self):
        terminal = self.make_terminal()
        terminal.feed_bytes(
            b'\x1b]0;fish title\x1b\\'
            b'\x1b]7;file://localhost/home/user\x1b\\'
            b'\x1b]52;c;SGVsbG8=\x1b\\'
            b'\x1b]133;A;click_events=1\x1b\\')
        self.assertEqual(terminal.title, 'fish title')
        self.assertEqual(terminal.cwd, 'file://localhost/home/user')
        self.assertEqual(terminal.clipboard_text, 'Hello')
        self.assertTrue(terminal.click_events)
        terminal.feed_bytes(b'one\ntwo\nthree\nfour\nfive\nsix\n')
        terminal._view_offset = 2
        rendered = terminal.render()
        self.assertIn('four', rendered.plain if hasattr(rendered, 'plain') else '\n'.join(x.plain for x in terminal._lines))


if __name__ == '__main__':
    unittest.main()
