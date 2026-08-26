"""Small Textual widgets kept separate from protocol and policy code."""
from __future__ import annotations

try:
    from textual.containers import VerticalScroll
    from textual.widgets import Label, ListItem, ListView, Static
except ImportError:  # importing policy/presets must work without Textual
    VerticalScroll = Label = ListItem = ListView = Static = object


class EnvironmentPanel(VerticalScroll):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._last_text = None

    def compose(self):
        yield Label('', id='environment-text')

    def update_config(self, config, pid=None, prct='ready', process='running', pending=0, error=None):
        lines = [f"Mode: {'termux-paths' if config.termux_paths else 'rootfs'}",
                 f"Rootfs: {config.rootfs or '(Termux prefix)'}",
                 f"PRoot PID: {pid or '-'}", f"PRCT: {prct}", f"Process: {process}",
                 f"Network: {config.net_policy}",
                 f"Proc isolation: {'on' if config.proc_isolated else 'OFF (default)'}",
                 f"Storage: {'enabled' if config.with_storage else 'masked'}"]
        lines += [f"Pending requests: {pending}"]
        if error:
            lines.append(f"ERROR: {error}")
        if not config.proc_isolated:
            lines.append('WARNING: /proc is not isolated')
        text = '\n'.join(lines)
        if text != self._last_text:
            self._last_text = text
            self.query_one('#environment-text').update(text)


class RuleList(ListView):
    def set_rules(self, rules):
        self.clear()
        for key, rule in rules:
            self.append(ListItem(Label(f"{rule.kind} {rule.path or rule.domain} [{rule.decision.name.lower()}]"), id=str(hash(key))))


class PendingList(ListView):
    def set_pending(self, requests, active_id=None):
        self.clear()
        for request_id, request in requests:
            label = getattr(request, 'path', '') or getattr(request, 'domain', '') or 'network'
            prefix = '▶ ' if request_id == active_id else '  '
            self.append(ListItem(Label(f'{prefix}{request_id}: {label}'), id=f'request-{request_id}'))


class RequestPanel(Static):
    def show_request(self, request):
        if hasattr(request, 'path'):
            text = f"{request.type.name}\noperation={request.operation}\npath={request.path}\nother={request.other_path or '-'}"
        else:
            text = f"{request.type.name}\noperation={request.operation}\n{request.domain or request.address.hex()}:{request.guest_port}"
        self.update(text)
