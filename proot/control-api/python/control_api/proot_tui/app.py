"""Textual application for a live PRCT session."""
from __future__ import annotations

import asyncio

from .policy import SessionPolicy
from .presets import HarnessConfig, build_config

try:
    from textual.app import App, ComposeResult
    from textual.containers import Horizontal, Vertical
    from textual.widgets import Button, Footer, Header, Static
    from .widgets import EnvironmentPanel, PendingList, RequestPanel, RuleList
except ImportError:
    App = None


if App is not None:
    from .terminal import ExternalTerminal

    class ProotTuiApp(App):
        CSS = """
        Screen { layout: vertical; }
        #body { height: 1fr; }
        #terminal { width: 1fr; border: solid $accent; }
        #side { width: 34; border: solid $panel; }
        #request { height: auto; padding: 1; }
        #actions { height: auto; }
        """
        BINDINGS = [('a', 'allow_once', 'Allow'), ('A', 'allow_always', 'Always'),
                    ('d', 'deny_once', 'Deny'), ('D', 'deny_always', 'Deny always'),
                    ('r', 'reveal', 'Reveal'), ('R', 'restore', 'Restore'),
                    ('f', 'forget', 'Forget'), ('j', 'next_request', 'Next request'),
                    ('k', 'previous_request', 'Previous request'),
                    ('tab', 'focus_next', 'Focus'), ('ctrl+q', 'quit', 'Quit')]

        def __init__(self, session, config: HarnessConfig):
            super().__init__()
            self.session, self.config = session, config
            self.current_id = None
            self.history = session.policy.history
            self._state_snapshot = None

        def compose(self) -> ComposeResult:
            yield Header(show_clock=False)
            with Horizontal(id='body'):
                yield ExternalTerminal(self.session, id='terminal')
                with Vertical(id='side'):
                    yield EnvironmentPanel(id='environment')
                    yield RequestPanel('(no pending request)', id='request')
                    yield PendingList(id='pending')
                    yield Button('Allow once [a]', id='allow')
                    yield Button('Allow always [A]', id='allow-always')
                    yield Button('Deny once [d]', id='deny')
                    yield Button('Deny always [D]', id='deny-always')
                    yield RuleList(id='rules')
            yield Footer()

        def on_mount(self):
            self._focus_terminal()
            self._refresh_state()
            self.session.start_readers(asyncio.get_running_loop(), self.feed_terminal, self.receive_event)
            # Lifecycle state changes are infrequent. A low-rate watchdog is
            # enough for process exit detection; terminal output itself is
            # delivered directly by the PTY reader and never waits for this.
            self.set_interval(.25, self._pump)

        def _focus_terminal(self):
            """Return keyboard ownership to the PTY after a side-panel action."""
            try:
                terminal = self.query_one('#terminal')
                terminal.focus()
            except Exception:
                # The callback can run while Textual is tearing down the
                # screen; focus restoration must never break session cleanup.
                pass

        def _restore_terminal_focus(self):
            # Defer until the button/modal key event has finished propagating.
            # Focusing immediately can be overwritten by Textual's own focus
            # handling for the clicked Button.
            try:
                self.call_after_refresh(self._focus_terminal)
            except AttributeError:
                self.set_timer(0, self._focus_terminal)

        def _pump(self):
            # Never exit automatically.  A dead guest or failed channel stays
            # inspectable until the user explicitly closes the session.
            self._refresh_state()

        def _refresh_state(self):
            lifecycle = self.session.lifecycle
            error = str(self.session.last_error) if self.session.last_error else None
            snapshot = (self.session.process.pid, lifecycle, error,
                        len(self.session.policy.pending), self.config.net_policy,
                        self.config.proc_isolated, self.config.with_storage)
            if snapshot == self._state_snapshot:
                return
            self._state_snapshot = snapshot
            self.query_one('#environment').update_config(
                self.config, self.session.process.pid,
                'failed' if lifecycle == 'prct_failed' else 'ready', lifecycle,
                len(self.session.policy.pending), error)

        def _refresh_pending(self):
            self.query_one('#pending').set_pending(
                self.session.policy.pending_items(), self.current_id)

        def _set_current(self, request_id):
            request = self.session.policy.get_pending(request_id)
            if request is None:
                self.current_id = None
                self.query_one('#request').update('(no pending request)')
                return
            self.current_id = request_id
            self.query_one('#request').show_request(request)
            self._refresh_pending()
            self._refresh_state()

        def on_button_pressed(self, event):
            actions = {'allow': (1, False), 'allow-always': (1, True),
                       'deny': (0, False), 'deny-always': (0, True)}
            if self.current_id is not None and event.button.id in actions:
                d, persistent = actions[event.button.id]
                self._decide_current(d, persistent)

        def _decide_current(self, decision, persistent=False):
            if self.current_id is not None:
                try:
                    self.session.decide(self.current_id, decision, persistent)
                except Exception as exc:
                    self.session.failed = exc
                    self.session.last_error = exc
                    self.session.lifecycle = 'prct_failed'
                    self.query_one('#request').update(f'PRCT FAILED: {exc}')
                    self._restore_terminal_focus()
                    return
                next_id = next(iter(self.session.policy.pending), None)
                self._set_current(next_id)
                self._restore_terminal_focus()

        def action_allow_once(self): self._decide_current(1)
        def action_allow_always(self): self._decide_current(1, True)
        def action_deny_once(self): self._decide_current(0)
        def action_deny_always(self): self._decide_current(0, True)

        def action_next_request(self):
            ids = list(self.session.policy.pending)
            if not ids: return
            index = ids.index(self.current_id) if self.current_id in ids else -1
            self._set_current(ids[(index + 1) % len(ids)])

        def action_previous_request(self):
            ids = list(self.session.policy.pending)
            if not ids: return
            index = ids.index(self.current_id) if self.current_id in ids else 0
            self._set_current(ids[(index - 1) % len(ids)])

        def action_reveal(self):
            self.query_one('#request').update('REVEAL requires selecting a shadow event')

        def action_restore(self):
            self.query_one('#request').update('RESTORE requires selecting a shadow event')

        def action_forget(self):
            self.query_one('#request').update('Select a rule in the rules panel to forget it')

        def on_resize(self, event):
            try: self.session.process.resize(event.size.height - 2, max(1, event.size.width - 36))
            except OSError: pass

        def receive_event(self, event):
            if isinstance(event, Exception):
                self.query_one('#request').update(f'PRCT FAILED: {event}')
                return
            if hasattr(event, 'request_id') and event.type.name.endswith('REQUEST'):
                if self.current_id is None:
                    self._set_current(event.request_id)
                else:
                    self._refresh_pending()
            self._refresh_state()

        def feed_terminal(self, data):
            self.query_one('#terminal').feed_bytes(data)

else:
    class ProotTuiApp:
        def __init__(self, *args, **kwargs):
            raise RuntimeError('Textual is required from the Termux pacman environment')


def run_app(config: HarnessConfig):
    from control_api import PtyProotProcess
    from .session import HarnessSession
    process = PtyProotProcess.spawn(build_config(config))
    session = HarnessSession(process, SessionPolicy())
    app = ProotTuiApp(session, config)
    try:
        app.run()
    finally:
        session.close()
