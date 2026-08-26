"""Self-contained ANSI terminal widget for an externally owned PTY.

Only Textual and Rich are used. The widget never starts a second process.
"""
from __future__ import annotations

import codecs
from collections import deque
import termios
import unicodedata

from rich.console import Group
from rich.style import Style
from rich.text import Text
from textual.widget import Widget

try:
    from wcwidth import wcwidth as _wcwidth
except ImportError:
    _wcwidth = None

COLORS = {30:'black',31:'red',32:'green',33:'yellow',34:'blue',35:'magenta',36:'cyan',37:'white',
          90:'bright_black',91:'bright_red',92:'bright_green',93:'bright_yellow',94:'bright_blue',
          95:'bright_magenta',96:'bright_cyan',97:'bright_white'}


class ExternalTerminal(Widget, can_focus=True):
    """ANSI/VT renderer with PTY input, resize, color, and scrollback."""
    DEFAULT_CSS = 'ExternalTerminal { background: black; color: white; }'

    def __init__(self, session, scrollback=10000, **kwargs):
        super().__init__(**kwargs)
        self.session = session
        self.max_scrollback = scrollback
        self.rows, self.cols = 24, 80
        self.cursor_x = self.cursor_y = 0
        self._wrap_pending = False
        self.autowrap = True
        self.insert_mode = False
        self.application_cursor = False
        self.origin_mode = False
        self.newline_mode = False
        self.top_margin, self.bottom_margin = 0, self.rows - 1
        self.fg, self.bg = 'white', 'black'
        self.bold = self.dim = self.italic = self.blink = False
        self.underline = self.strike = self.reverse = False
        self.conceal = self.overline = False
        self.cursor_visible = True
        self.cursor_style = 2
        self.bracketed_paste = False
        self.focus_reporting = False
        self.kitty_keyboard = False
        self.kitty_keyboard_flags = 0
        self._link = None
        self.title = ''
        self.cwd = ''
        self.clipboard_text = ''
        self._last_printed = ' '
        self._lines = [Text('') for _ in range(self.rows)]
        self._scrollback = deque(maxlen=scrollback)
        self.mouse_tracking = False
        self.mouse_mode = 0
        self.click_events = False
        self._view_offset = 0
        self._escape = ''
        self._decoder = codecs.getincrementaldecoder('utf-8')('replace')
        self._saved_cursor = (0, 0)
        self._alternate = None
        # Textual may ask for a render more than once between PTY reads. Keep
        # the Rich tree until terminal state actually changes; rebuilding and
        # copying 24 rows for every framework repaint makes cursor movement
        # needlessly expensive on Android.
        # Do not call this ``_render_cache``: Textual owns that private name
        # and stores its internal _RenderCache there.
        self._terminal_render_cache = None
        self._terminal_render_dirty = True

    def on_mount(self):
        self._resize(self.size.height or 24, self.size.width or 80)

    def on_resize(self, event):
        self._resize(max(1, event.size.height), max(1, event.size.width))

    def _resize(self, rows, cols):
        if rows == self.rows and cols == self.cols:
            return
        self.rows, self.cols = rows, cols
        self.bottom_margin = rows - 1
        self._lines = (self._lines + [Text('') for _ in range(rows)])[:rows]
        self._terminal_render_dirty = True
        try:
            self.session.process.resize(rows, cols)
        except OSError:
            pass
        self.refresh()

    async def on_key(self, event):
        # App-level bindings must not see terminal input. In particular, shell
        # users routinely type a/d/r/f and fish consumes many control keys.
        # Ctrl-Q remains the one explicit harness escape hatch.
        if event.key == 'ctrl+q':
            self.app.exit()
            event.stop()
            return
        value = self._key_bytes(event.key, event.character)
        if value:
            self.session.write(value.encode())
        # Stop even for keys that have no byte representation. This prevents
        # Textual's app bindings from stealing focus/input while the terminal
        # owns keyboard focus.
        event.stop()

    def _key_bytes(self, key, character=None):
        modifiers = 1
        if 'shift+' in key:
            modifiers += 1
        if 'alt+' in key or 'meta+' in key:
            modifiers += 2
        if 'ctrl+' in key:
            modifiers += 4
        base = key.split('+')[-1]
        controls = {
            'enter': (13, '\r'), 'backspace': (127, '\x7f'),
            'tab': (9, '\t'), 'escape': (27, '\x1b'),
            'up': (1, 'A'), 'down': (1, 'B'), 'right': (1, 'C'),
            'left': (1, 'D'), 'home': (1, 'H'), 'end': (1, 'F'),
            'delete': (3, '~'), 'insert': (2, '~'),
            'pageup': (5, '~'), 'pagedown': (6, '~'),
            'f1': (11, '~'), 'f2': (12, '~'), 'f3': (13, '~'),
            'f4': (14, '~'), 'f5': (15, '~'), 'f6': (17, '~'),
            'f7': (18, '~'), 'f8': (19, '~'), 'f9': (20, '~'),
            'f10': (21, '~'), 'f11': (23, '~'), 'f12': (24, '~'),
        }
        if 'ctrl+' in key and len(base) == 1 and 'a' <= base.lower() <= 'z':
            return bytes((ord(base) - 96,)).decode('latin1')
        if base in controls:
            code, suffix = controls[base]
            if self.kitty_keyboard:
                if suffix in 'ABCDHF':
                    return f'\x1b[1;{modifiers}{suffix}'
                if suffix == '~':
                    return f'\x1b[{code};{modifiers}~'
                return f'\x1b[{code};{modifiers}u'
            if base == 'tab' and modifiers != 1:
                return '\x1b[Z' if modifiers == 2 else '\x1b[1;2Z'
            if base in ('up', 'down', 'left', 'right') and self.application_cursor and modifiers == 1:
                return f'\x1bO{suffix}'
            if suffix == '~':
                return f'\x1b[{code};{modifiers}~' if modifiers != 1 else f'\x1b[{code}~'
            return suffix if base in ('enter', 'backspace', 'tab', 'escape') else f'\x1b[1;{modifiers}{suffix}'
        if character:
            if self.kitty_keyboard and ('ctrl+' in key or 'alt+' in key or 'meta+' in key):
                return f'\x1b[{ord(character)};{modifiers}u'
            if 'alt+' in key or 'meta+' in key:
                return '\x1b' + character
            return character
        return ''

    def on_paste(self, event):
        text = getattr(event, 'text', '')
        if text:
            if self.bracketed_paste:
                text = '\x1b[200~' + text + '\x1b[201~'
            self.session.write(text.encode())
        event.stop()

    def on_focus(self):
        if self.focus_reporting:
            self._write_terminal_response(b'\x1b[I')

    def on_blur(self):
        if self.focus_reporting:
            self._write_terminal_response(b'\x1b[O')

    def _mouse_sequence(self, event, button, release=False):
        if not self.mouse_tracking and not self.click_events:
            return
        x = max(1, min(self.cols, int(event.x) + 1))
        y = max(1, min(self.rows, int(event.y) + 1))
        suffix = 'm' if release else 'M'
        self.session.write(f'\x1b[<{button};{x};{y}{suffix}'.encode())

    def on_mouse_down(self, event):
        self.focus()
        button = {1: 0, 2: 1, 3: 2}.get(getattr(event, 'button', 1), 0)
        self._mouse_sequence(event, button)
        event.stop()

    def on_mouse_up(self, event):
        button = {1: 0, 2: 1, 3: 2}.get(getattr(event, 'button', 1), 0)
        self._mouse_sequence(event, button, release=True)
        event.stop()

    def on_mouse_move(self, event):
        if self.mouse_tracking and self.mouse_mode in (1002, 1003):
            self._mouse_sequence(event, 32)
            event.stop()

    def on_mouse_scroll_up(self, event):
        if self.mouse_tracking:
            self._mouse_sequence(event, 64)
        else:
            self._view_offset = min(len(self._scrollback), self._view_offset + 3)
            self._terminal_render_dirty = True
            self.refresh(repaint=True, layout=False)
        event.stop()

    def on_mouse_scroll_down(self, event):
        if self.mouse_tracking:
            self._mouse_sequence(event, 65)
        else:
            self._view_offset = max(0, self._view_offset - 3)
            self._terminal_render_dirty = True
            self.refresh(repaint=True, layout=False)
        event.stop()

    def feed_bytes(self, data):
        if data:
            self._terminal_render_dirty = True
        text = self._escape + self._decoder.decode(data, final=False)
        if data:
            self._view_offset = 0
        # Accept both 7-bit ESC forms and the equivalent C1 controls. Android
        # terminals and multiplexers are allowed to emit either form.
        text = (text.replace('\x9b', '\x1b[')
                    .replace('\x9d', '\x1b]')
                    .replace('\x90', '\x1bP')
                    .replace('\x98', '\x1bX')
                    .replace('\x9e', '\x1b^')
                    .replace('\x9f', '\x1b_')
                    .replace('\x9c', '\x1b\\'))
        self._escape = ''
        plain = []
        index = 0

        def flush_plain():
            if plain:
                self._feed_plain(''.join(plain))
                plain.clear()

        while index < len(text):
            if text[index] != '\x1b':
                plain.append(text[index])
                index += 1
                continue
            flush_plain()
            if index + 1 >= len(text):
                self._escape = '\x1b'
                break
            kind = text[index + 1]
            if kind == '[':
                final = index + 2
                while final < len(text) and not ('@' <= text[final] <= '~'):
                    final += 1
                if final == len(text):
                    self._escape = text[index:]
                    break
                self._csi(text[index + 2:final], text[final])
                index = final + 1
            elif kind == ']':
                end = index + 2
                while end < len(text):
                    if text[end] == '\x07':
                        break
                    if text[end] == '\x1b' and end + 1 < len(text) and text[end + 1] == '\\':
                        break
                    end += 1
                if end == len(text):
                    self._escape = text[index:]
                    break
                self._respond_osc(text[index + 2:end])
                index = end + (2 if text[end] == '\x1b' else 1)
            elif kind in ('P', '^', '_'):
                # DCS/SOS/APC are used by fish/terminfo for capability
                # queries and hyperlinks. They are metadata, never screen
                # text, and terminate with BEL or ST (ESC backslash).
                end = index + 2
                while end < len(text):
                    if text[end] == '\x07':
                        break
                    if text[end] == '\x1b' and end + 1 < len(text) and text[end + 1] == '\\':
                        break
                    end += 1
                if end == len(text):
                    self._escape = text[index:]
                    break
                self._respond_dcs(text[index + 2:end])
                index = end + (2 if text[end] == '\x1b' else 1)
            elif kind in ('#', '(', ')', '*', '+', '-', '.', '/'):
                if index + 2 >= len(text):
                    self._escape = text[index:]
                    break
                self._escape_double(kind, text[index + 2])
                index += 3
            else:
                self._escape_single(kind)
                index += 2
        flush_plain()
        # Avoid a layout pass: PTY output changes pixels, not widget geometry.
        self.refresh(repaint=True, layout=False)

    def _escape_single(self, command):
        if command in ('7', 's'):
            self._saved_cursor = (self.cursor_x, self.cursor_y)
        elif command in ('8', 'u'):
            self.cursor_x, self.cursor_y = self._saved_cursor
        elif command == 'c':
            self._lines = [Text('') for _ in range(self.rows)]
            self.cursor_x = self.cursor_y = 0
            self.top_margin, self.bottom_margin = 0, self.rows - 1
            self.autowrap = True
            self.insert_mode = False
            self._reset_style()
        elif command == 'D':
            self._newline()
        elif command == 'E':
            self._newline()
            self.cursor_x = 0
        elif command == 'M':
            if self.cursor_y > self.top_margin:
                self.cursor_y -= 1
            else:
                self._scroll_down(1)
        elif command == 'H':
            # HTS: the renderer uses the standard tab stops every 8 columns.
            return

    def _escape_double(self, intermediate, command):
        if intermediate == '#' and command == '8':
            self._lines = [Text('E' * self.cols) for _ in range(self.rows)]
            self.cursor_x = self.cursor_y = 0

    def _feed_plain(self, text):
        for char in text:
            if char == '\r':
                self.cursor_x = 0
                self._wrap_pending = False
            elif char == '\n':
                self._newline()
                if self.newline_mode:
                    self.cursor_x = 0
            elif char == '\b':
                self.cursor_x = max(0, self.cursor_x - 1)
                self._wrap_pending = False
            elif char == '\t':
                self.cursor_x = min(self.cols - 1, ((self.cursor_x // 8) + 1) * 8)
            elif char in ('\x0b', '\x0c'):
                self._newline()
            elif ord(char) >= 32:
                if self._wrap_pending:
                    self._newline()
                    self._wrap_pending = False
                line = self._lines[self.cursor_y]
                plain = line.plain
                index = self._text_index_at_column(line, self.cursor_x)
                width = self._char_width(char)
                if self.insert_mode:
                    line = (line[:index] + Text(' ' * max(1, width),
                                                style=self._style()) + line[index:])
                    self._lines[self.cursor_y] = line
                if index < len(line.plain):
                    line.plain = line.plain[:index] + char + line.plain[index + 1:]
                else:
                    current_width = self._display_width(line.plain)
                    line.append(' ' * max(0, self.cursor_x - current_width) + char)
                line.stylize(self._style(), index, index + 1)
                self._last_printed = char
                if self.autowrap and self.cursor_x + width >= self.cols:
                    self.cursor_x = self.cols - 1
                    self._wrap_pending = True
                else:
                    self.cursor_x = min(self.cols - 1, self.cursor_x + width)

    @staticmethod
    def _char_width(char):
        if _wcwidth is not None:
            width = _wcwidth(char)
            return max(0, width)
        if unicodedata.combining(char):
            return 0
        if unicodedata.east_asian_width(char) in ('W', 'F'):
            return 2
        return 1

    def _display_width(self, text):
        return sum(self._char_width(char) for char in text)

    def _text_index_at_column(self, line, column):
        width = 0
        for index, char in enumerate(line.plain):
            if width >= column:
                return index
            width += self._char_width(char)
        return len(line.plain)

    def _style(self):
        return Style(color=self.fg, bgcolor=self.bg, bold=self.bold,
                     dim=self.dim, italic=self.italic,
                     underline=self.underline, strike=self.strike,
                     reverse=self.reverse, blink=self.blink,
                     conceal=self.conceal, overline=self.overline,
                     link=self._link)

    def _reset_style(self):
        self.fg, self.bg = 'white', 'black'
        self.bold = self.dim = self.italic = self.blink = False
        self.underline = self.strike = self.reverse = False
        self.conceal = self.overline = False

    def _newline(self):
        self.cursor_x = 0
        self._wrap_pending = False
        if self.cursor_y < self.bottom_margin:
            self.cursor_y += 1
        else:
            self._scroll_region(1, save_scrollback=True)

    def _csi(self, raw, command):
        private = raw.startswith('?')
        prefix = raw[0] if raw[:1] in ('?', '>', '!', '=') else ''
        values = raw[1:] if prefix else raw
        # fish and modern readline variants may emit colon sub-parameters
        # (notably SGR truecolor). Empty fields are separators, not errors.
        values = values.replace(':', ';')
        nums = []
        for value in values.split(';') if values else ():
            try:
                nums.append(int(value.strip()))
            except ValueError:
                # Kitty keyboard protocol and terminal queries use private
                # parameter prefixes (>, !) that do not affect the screen.
                continue
        # ED (J) and EL (K) default to mode 0 when Ps is omitted. Most
        # movement and editing commands default to one cell/line instead.
        n = nums[0] if nums else (0 if command in ('J', 'K') else 1)
        # fish probes terminal features during startup and waits for these
        # replies before drawing its prompt. A renderer that only consumes
        # queries leaves fish looking frozen even though the PTY is healthy.
        if private and command == 'u' and not values:
            self._write_terminal_response(b'\x1b[?0u')
            return
        if prefix == '>' and command == 'q':
            self._write_terminal_response(b'\x1b[>0;0;0q')
            return
        if command == 'c' and not private:
            self._write_terminal_response(b'\x1b[?1;2c')
            return
        if command == 'n' and n == 5:
            self._write_terminal_response(b'\x1b[0n')
            return
        if command == 'n' and n == 6:
            self._write_terminal_response(
                f'\x1b[{self.cursor_y + 1};{self.cursor_x + 1}R'.encode())
            return
        if command == 'q' and not nums and not private:
            self._write_terminal_response(b'\x1bP>|proot-tui 1.0\x1b\\')
            return
        if prefix in ('=', '?') and command == 'u' and nums:
            self.kitty_keyboard = nums[0] != 0
            return
        if command == 'S':
            self._scroll_content(n)
            return
        if command == 'T':
            self._scroll_down(n)
            return
        if command == 'b':
            for _ in range(n):
                self._feed_plain(self._last_printed)
            return
        if command in 'ABCDEFGHdfG' or command in ('K', 'J'):
            # Any explicit cursor/erase operation cancels a pending
            # last-column wrap. Keeping it set makes fish's xenl/indn probe
            # wrap a later character onto an incorrect row.
            self._wrap_pending = False
        if command == 'm' and not prefix:
            codes = nums or [0]
            index = 0
            while index < len(codes):
                code = codes[index]
                if code == 0: self._reset_style()
                elif code == 1: self.bold = True
                elif code == 2: self.dim = True
                elif code == 3: self.italic = True
                elif code == 4: self.underline = True
                elif code == 5 or code == 6: self.blink = True
                elif code == 7: self.reverse = True
                elif code == 9: self.strike = True
                elif code == 8: self.conceal = True
                elif code == 53: self.overline = True
                elif code == 22: self.bold = False
                elif code == 23: self.italic = False
                elif code == 24: self.underline = False
                elif code == 27: self.reverse = False
                elif code == 29: self.strike = False
                elif code == 25: self.blink = False
                elif code == 28: self.conceal = False
                elif code == 55: self.overline = False
                elif code == 39: self.fg = 'white'
                elif code == 49: self.bg = 'black'
                elif code in COLORS: self.fg = COLORS[code]
                elif 40 <= code <= 47: self.bg = COLORS.get(code - 10, 'black')
                elif 100 <= code <= 107: self.bg = COLORS.get(code - 70, 'black')
                elif code in (38, 48) and index + 1 < len(codes):
                    foreground = code == 38
                    mode = codes[index + 1]
                    if mode == 5 and index + 2 < len(codes):
                        color = self._indexed_color(codes[index + 2])
                        if foreground: self.fg = color
                        else: self.bg = color
                        index += 2
                    elif mode == 2 and index + 4 < len(codes):
                        color = '#%02x%02x%02x' % tuple(max(0, min(255, value)) for value in codes[index + 2:index + 5])
                        if foreground: self.fg = color
                        else: self.bg = color
                        index += 4
                index += 1
        elif command in 'Hf':
            self.cursor_y = min(self.rows - 1, max(0, n - 1))
            self.cursor_x = min(self.cols - 1, max(0, (nums[1] if len(nums) > 1 else 1) - 1))
        elif command == 'J':
            self._erase_display(n)
        elif command == 'K':
            self._erase_line(n)
        elif command == 'A': self.cursor_y = max(0, self.cursor_y - n)
        elif command == 'B': self.cursor_y = min(self.rows - 1, self.cursor_y + n)
        elif command == 'C': self.cursor_x = min(self.cols - 1, self.cursor_x + n)
        elif command == 'D': self.cursor_x = max(0, self.cursor_x - n)
        elif command == 'E': self.cursor_y = min(self.rows - 1, self.cursor_y + n); self.cursor_x = 0
        elif command == 'F': self.cursor_y = max(0, self.cursor_y - n); self.cursor_x = 0
        elif command == 'G': self.cursor_x = min(self.cols - 1, max(0, n - 1))
        elif command == 'd': self.cursor_y = min(self.rows - 1, max(0, n - 1))
        elif command == '@': self._insert_chars(n)
        elif command == 'P': self._delete_chars(n)
        elif command == 'X': self._erase_chars(n)
        elif command == 'L': self._insert_lines(n)
        elif command == 'M': self._delete_lines(n)
        elif command == 'I': self.cursor_x = min(self.cols - 1, self._next_tab(self.cursor_x, n))
        elif command == 'Z': self.cursor_x = max(0, self._previous_tab(self.cursor_x, n))
        elif command == 'g': pass
        elif command == 's' and not private: self._saved_cursor = (self.cursor_x, self.cursor_y)
        elif command == 'u' and not private: self.cursor_x, self.cursor_y = self._saved_cursor
        elif command == 'q' and not private and not prefix:
            self.cursor_style = n if n in (2, 4, 6) else 2
        elif prefix == '>' and command == 'm':
            # Kitty keyboard progressive-enhancement request. This is not
            # SGR: interpreting it as style makes fish's prompt permanently
            # bold/underlined.
            if nums and nums[0] == 4:
                self.kitty_keyboard_flags = nums[1] if len(nums) > 1 else 0
                self.kitty_keyboard = self.kitty_keyboard_flags != 0
        elif command == 'h' and not private and 4 in nums:
            self.insert_mode = True
        elif command == 'l' and not private and 4 in nums:
            self.insert_mode = False
        elif command == 'r' and not private:
            top = nums[0] if nums and nums[0] else 1
            bottom = nums[1] if len(nums) > 1 and nums[1] else self.rows
            self.top_margin = max(0, min(self.rows - 1, top - 1))
            self.bottom_margin = max(self.top_margin, min(self.rows - 1, bottom - 1))
            self.cursor_x = 0
            self.cursor_y = self.top_margin
        elif private and command == 'h' and any(code in nums for code in (47, 1047, 1049)):
            self._enter_alternate()
        elif private and command == 'l' and any(code in nums for code in (47, 1047, 1049)):
            self._leave_alternate()
        elif private and command == 'h' and any(code in nums for code in (1000, 1002, 1003)):
            self.mouse_tracking = True
            self.mouse_mode = next(code for code in (1003, 1002, 1000) if code in nums)
        elif private and command == 'l' and any(code in nums for code in (1000, 1002, 1003)):
            self.mouse_tracking = False
            self.mouse_mode = 0
        elif private and command == 'h' and 25 in nums:
            self.cursor_visible = True
        elif private and command == 'l' and 25 in nums:
            self.cursor_visible = False
        elif private and command == 'h' and 1004 in nums:
            self.focus_reporting = True
        elif private and command == 'l' and 1004 in nums:
            self.focus_reporting = False
        elif private and command == 'h' and 2004 in nums:
            self.bracketed_paste = True
        elif private and command == 'l' and 2004 in nums:
            self.bracketed_paste = False
        elif private and command == 'h' and 1006 in nums:
            self.mouse_tracking = True
        elif private and command == 'l' and 1006 in nums:
            self.mouse_tracking = False
        elif private and command == 'h' and 1 in nums:
            self.application_cursor = True
        elif private and command == 'l' and 1 in nums:
            self.application_cursor = False
        elif private and command == 'h' and 6 in nums:
            self.origin_mode = True
        elif private and command == 'l' and 6 in nums:
            self.origin_mode = False
        elif private and command == 'h' and 7 in nums:
            self.autowrap = True
        elif private and command == 'l' and 7 in nums:
            self.autowrap = False

    def _erase_display(self, mode):
        if mode == 3:
            self._scrollback.clear()
        if mode == 2:
            self._lines = [Text('') for _ in range(self.rows)]
        elif mode == 1:
            for row in range(0, self.cursor_y):
                self._lines[row] = Text('')
            self._erase_line(1)
        else:  # ED 0: cursor through end of display
            self._erase_line(0)
            for row in range(self.cursor_y + 1, self.rows):
                self._lines[row] = Text('')

    def _write_terminal_response(self, data):
        process = getattr(self.session, 'process', None)
        fd = getattr(process, 'master_fd', None)
        try:
            if fd is not None:
                # Capability replies are input from the terminal's point of
                # view.  If ECHO is left enabled, the line discipline can
                # reflect the printable hex part of a DCS reply back to the
                # child, which fish then renders as a bogus prompt line.
                # Interactive shells render their own input, so ECHO must
                # remain disabled for the PTY, just like a real terminal
                # emulator does for a full-screen line editor.
                quiet = list(termios.tcgetattr(fd))
                quiet[3] &= ~(termios.ECHO | termios.ECHONL)
                termios.tcsetattr(fd, termios.TCSANOW, quiet)
            self.session.write(data)
        except (OSError, AttributeError, termios.error):
            return

    def _respond_osc(self, body):
        # OSC 11;? asks for the current background. Return a conservative
        # black background in the standard rgb form; fish only needs a valid
        # response to select its contrast palette.
        if body == '11;?':
            self._write_terminal_response(b'\x1b]11;rgb:0000/0000/0000\x1b\\')
        elif body in ('10;?', '10;rgb:'):
            self._write_terminal_response(b'\x1b]10;rgb:ffff/ffff/ffff\x1b\\')
        elif body.startswith('8;;'):
            self._link = body[3:] or None
        elif body.startswith('0;') or body.startswith('1;') or body.startswith('2;'):
            self.title = body.split(';', 1)[1]
        elif body.startswith('7;'):
            self.cwd = body[2:]
        elif body.startswith('52;'):
            self._handle_clipboard_osc(body)
        elif body.startswith('133;'):
            # Semantic prompt markers are state for mouse/copy integration;
            # they must not become terminal text.
            self.click_events = body.startswith('133;A;') and 'click_events=1' in body

    def _handle_clipboard_osc(self, body):
        # OSC 52 is a shell -> terminal copy request. Keep the decoded value
        # in memory and use Textual's clipboard bridge when available; do not
        # invoke Android commands or expose it in the side panel.
        try:
            import base64
            _, selection, encoded = body.split(';', 2)
            if encoded == '?':
                return
            value = base64.b64decode(encoded, validate=True).decode('utf-8', 'replace')
            self.clipboard_text = value
            try:
                app = self.app
            except Exception:
                app = None
            copier = getattr(app, 'copy_to_clipboard', None)
            if copier:
                copier(value)
        except (ValueError, UnicodeError):
            return

    def _respond_dcs(self, body):
        # fish uses XTGETTCAP while entering its interactive mode.  The
        # request is DCS +q <hex-name> ST. fish 4.x consumes the response as
        # DCS 1+r <hex-name>=<hex-value> ST (the response selector differs
        # from the request selector). Replying to arbitrary DCS
        # traffic is unsafe (applications use DCS for private protocols), so
        # only answer the two capabilities fish currently asks for.  The
        # bytes are written to the PTY input, never fed back to the screen.
        if not body.startswith('+q'):
            return
        name = body[2:]
        values = {
            # terminfo indn: ESC [ %p1%d S
            '696e646e': '1b5b257031256453',
            # query-os-name: uname -s on the Termux/Linux host
            '71756572792d6f732d6e616d65': '4c696e7578',
        }
        value = values.get(name)
        if value is None:
            return
        response = f'\x1bP1+r{name}={value}\x1b\\'.encode('ascii')
        self._write_terminal_response(response)

    def _scroll_content(self, count):
        self._scroll_region(count, save_scrollback=True)

    def _scroll_region(self, count, save_scrollback=False):
        count = max(1, min(count, self.bottom_margin - self.top_margin + 1))
        for _ in range(count):
            removed = self._lines.pop(self.top_margin)
            if save_scrollback and self.top_margin == 0:
                self._scrollback.append(removed)
            self._lines.insert(self.bottom_margin, Text(''))

    def _scroll_down(self, count):
        count = max(1, min(count, self.bottom_margin - self.top_margin + 1))
        for _ in range(count):
            del self._lines[self.bottom_margin]
            self._lines.insert(self.top_margin, Text(''))

    @staticmethod
    def _next_tab(column, count):
        for _ in range(max(1, count)):
            column = ((column // 8) + 1) * 8
        return column

    @staticmethod
    def _previous_tab(column, count):
        for _ in range(max(1, count)):
            column = max(0, ((max(0, column - 1) // 8) * 8))
        return column

    def _enter_alternate(self):
        if self._alternate is not None:
            return
        self._alternate = (self._lines, self.cursor_x, self.cursor_y,
                           self._saved_cursor)
        self._lines = [Text('') for _ in range(self.rows)]
        self.cursor_x = self.cursor_y = 0
        self._saved_cursor = (0, 0)

    def _leave_alternate(self):
        if self._alternate is None:
            return
        self._lines, self.cursor_x, self.cursor_y, self._saved_cursor = self._alternate
        self._alternate = None

    def _erase_line(self, mode=0):
        line = self._lines[self.cursor_y]
        index = self._text_index_at_column(line, self.cursor_x)
        if mode == 2:
            self._lines[self.cursor_y] = Text('')
        elif mode == 1:
            self._lines[self.cursor_y] = line[index:]
        else:
            self._lines[self.cursor_y] = line[:index]

    def _insert_chars(self, count):
        line = self._lines[self.cursor_y]
        index = self._text_index_at_column(line, self.cursor_x)
        inserted = Text(' ' * count, style=self._style())
        self._lines[self.cursor_y] = (line[:index] + inserted + line[index:])[:self.cols]

    def _delete_chars(self, count):
        line = self._lines[self.cursor_y]
        index = self._text_index_at_column(line, self.cursor_x)
        self._lines[self.cursor_y] = line[:index] + line[index + count:]

    def _erase_chars(self, count):
        line = self._lines[self.cursor_y]
        index = self._text_index_at_column(line, self.cursor_x)
        erased = Text(' ' * count, style=self._style())
        self._lines[self.cursor_y] = line[:index] + erased + line[index + count:]

    def _insert_lines(self, count):
        count = max(1, min(count, self.bottom_margin - self.cursor_y + 1))
        for _ in range(count):
            self._lines.insert(self.cursor_y, Text(''))
            del self._lines[self.bottom_margin + 1]

    def _delete_lines(self, count):
        count = max(1, min(count, self.bottom_margin - self.cursor_y + 1))
        for _ in range(count):
            del self._lines[self.cursor_y]
            self._lines.insert(self.bottom_margin, Text(''))

    @staticmethod
    def _indexed_color(index):
        basic = ['black', 'red', 'green', 'yellow', 'blue', 'magenta', 'cyan', 'white']
        if index < 8:
            return basic[index]
        if index < 16:
            return 'bright_' + basic[index - 8]
        if index < 232:
            value = index - 16
            red, value = divmod(value, 36)
            green, blue = divmod(value, 6)
            levels = (0, 95, 135, 175, 215, 255)
            return '#%02x%02x%02x' % (levels[red], levels[green], levels[blue])
        gray = 8 + (index - 232) * 10
        return '#%02x%02x%02x' % (gray, gray, gray)

    def render(self):
        if not self._terminal_render_dirty and self._terminal_render_cache is not None:
            return self._terminal_render_cache
        # The live screen is already exactly ``rows`` lines. Avoid flattening
        # the complete scrollback and copying every Rich Text on each cursor
        # movement; only the cursor row needs a private copy for styling.
        if self._view_offset == 0:
            lines = list(self._lines)
        else:
            history = list(self._scrollback) + self._lines
            end = len(history) - self._view_offset
            start = max(0, end - self.rows)
            lines = history[start:end]
        if len(lines) < self.rows:
            lines = [Text('') for _ in range(self.rows - len(lines))] + lines
        cursor_row = len(lines) - self.rows + self.cursor_y if self._view_offset == 0 else -1
        if self.cursor_visible and 0 <= cursor_row < len(lines):
            line = lines[cursor_row].copy()
            lines[cursor_row] = line
            index = self._text_index_at_column(line, self.cursor_x)
            if index >= len(line.plain):
                line.append(' ')
            if self.cursor_style == 2:
                line.stylize(Style(reverse=True), index, index + 1)
            else:
                line.stylize(Style(underline=True), index, index + 1)
        self._terminal_render_cache = Group(*(line if line else Text('') for line in lines))
        self._terminal_render_dirty = False
        return self._terminal_render_cache
