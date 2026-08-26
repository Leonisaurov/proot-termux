"""Fail-closed, session-only authorization state."""
from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass

from .. import Decision, NetRequest, PathRequest, ShadowEvent


@dataclass
class Rule:
    kind: str
    operation: int
    path: str = ''
    other_path: str = ''
    family: int = 0
    port: int = 0
    decision: Decision = Decision.DENY
    persistent: bool = True
    active: bool = True


class SessionPolicy:
    def __init__(self, history_limit=200):
        self.pending = OrderedDict()
        self.rules = OrderedDict()
        self.history = []
        self.history_limit = history_limit

    def record(self, event, **details):
        self.history.append({'event': event, **details})
        del self.history[:-self.history_limit]

    def receive(self, request):
        self.record('request', request=request)
        if isinstance(request, (PathRequest, NetRequest)):
            self.pending[request.request_id] = request
        elif isinstance(request, ShadowEvent):
            self.shadow(request)
        return request

    def pending_items(self):
        return tuple(self.pending.items())

    def get_pending(self, request_id):
        return self.pending.get(request_id)

    def decide(self, request_id, decision, persistent=False):
        request = self.pending.get(request_id)
        if request is None:
            raise KeyError(f'unknown pending request {request_id}')
        decision = Decision(decision)
        self.record('decision', request_id=request_id, decision=decision, persistent=persistent)
        if persistent and isinstance(request, (PathRequest, NetRequest)):
            key = self.key(request)
            self.rules[key] = Rule('path' if isinstance(request, PathRequest) else 'net',
                                   request.operation, getattr(request, 'path', ''),
                                   getattr(request, 'other_path', ''), getattr(request, 'family', 0),
                                   getattr(request, 'guest_port', 0), decision, True)
        self.pending.pop(request_id, None)
        return request

    @staticmethod
    def key(request):
        if isinstance(request, PathRequest):
            return ('path', request.operation, request.path, request.other_path)
        return ('net', request.operation, request.family, request.guest_port, request.domain)

    def forget_local(self, key):
        return self.rules.pop(key, None)

    def shadow(self, event: ShadowEvent):
        self.record('shadow', request=event)
