# Python API reference and harness cookbook

This document describes the public Python surface of `control-api` and the
way the interactive harness uses it. The protocol wire contract is documented
separately in [`../PROTOCOL.md`](../PROTOCOL.md).

The package has two layers:

| Layer | Module | Use |
|---|---|---|
| synchronous core | `control_api.py` | small scripts and blocking consumers |
| event-loop integration | `async_channel.py`, `pty_launcher.py` | Textual, asyncio, and PTY applications |

Neither layer contains a Termux policy. Termux bindings and defaults belong to
`proot_tui.presets`.

## Installation and imports

From the repository root:

```bash
PYTHONPATH=proot/control-api/python python3 -c \
  'from control_api import ControlChannel, PathRequest, NetRequest'
```

For an editable installation, install the Python package using the Termux
Python/pacman environment. Textual is an optional runtime dependency supplied
by that environment; this project does not clone a terminal widget or patch
the system Textual package.

The compatibility imports are intentionally short:

```python
from control_api import (
    AsyncControlChannel, ControlChannel, Decision, NetOperation,
    PathOperation, PathRequest, NetRequest, ProotConfig, ProotProcess,
    PtyProotProcess,
)
```

`AsyncControlChannel`, `PtyProotProcess`, and the Termux binding helpers are
loaded lazily through `control_api.__getattr__`; importing the synchronous
codec does not require Textual.

## Enumerations

```python
Message.HELLO                  # 1
Message.NET_ACCESS_REQUEST     # 2
Message.PATH_ACCESS_REQUEST    # 3
Message.SHADOW_EVENT           # 4
Message.COMMAND_RESULT         # 5

NetOperation.BIND              # 1
NetOperation.CONNECT           # 2
NetOperation.PUBLICATION       # 3
NetOperation.DNS               # 4
NetOperation.SOCKET            # 5

PathOperation.READ             # 1
PathOperation.WRITE            # 2
PathOperation.CREATE           # 3
PathOperation.DELETE           # 4
PathOperation.RENAME           # 5
PathOperation.METADATA         # 6

Decision.DENY                  # 0
Decision.ALLOW                 # 1
ShadowScope.NODE               # 1
ShadowScope.RECURSIVE          # 2
```

`SOCKET` is a permission for socket creation itself. For example,
`socket(AF_UNIX, ...)` arrives as a `NetRequest` with `operation == 5`,
`family == 1`, and zero address and ports. It is independent from a later
Internet `BIND` or `CONNECT` request.

## Request objects

All request objects are frozen dataclasses and have `type` and `request_id`.
Request IDs must be echoed unchanged in a decision.

### `NetRequest`

| Field | Meaning |
|---|---|
| `operation` | `NetOperation` value, represented as an integer |
| `guest_pid` | guest-visible process ID |
| `family` | address family; IPv4=2, IPv6=10, Unix sockets commonly=1 |
| `protocol` | socket protocol, when supplied |
| `guest_port` | guest-visible port |
| `host_port` | host-side port for mapped/public traffic |
| `address` | 16-byte address field; decode according to `family` |
| `virtual_class` | PRoot virtual-network classification |
| `real_exposure` | whether real host exposure is involved |
| `proxy` | proxy name, if any |
| `domain` | DNS/domain detail, if any |

### `PathRequest` and `ShadowEvent`

Both expose `operation`, `reason`, `path`, and `other_path`. Paths are absolute
guest paths only. The API never resolves them to host paths. A
`PathRequest` blocks the guest until a decision is sent. A `ShadowEvent` is a
notification and must never receive an allow/deny response.

### `CommandResult`

Command results expose `status`, `flags`, `dynamic_path_rules`,
`dynamic_net_rules`, and `shadows`. A zero `status` means the command
succeeded; command-specific details are represented by the counters and
flags.

## Synchronous `ControlChannel`

`ControlChannel` owns the socket passed to it. It is non-blocking internally,
but `receive()` and `recv_frame()` wait for a complete frame up to the
configured deadline.

### Constructing and handshaking

```python
import socket
from control_api import ControlChannel

channel = ControlChannel.from_socket(sock, timeout=1.0)
# Or, when a launcher passed an inherited descriptor:
channel = ControlChannel.from_fd(3, timeout=1.0)
channel.handshake()                 # mandatory HELLO validation
```

The channel transitions `CREATED → READY`. EOF, timeout, malformed frames,
invalid paths, or an ID mismatch transition it to `FAILED`; a failed channel
cannot be reused. `close()` is idempotent and transitions to `CLOSED`.

### Receiving and deciding

For a simple blocking harness:

```python
from control_api import Decision, NetRequest, PathRequest

while True:
    request = channel.receive()
    if isinstance(request, (PathRequest, NetRequest)):
        # The guest remains stopped until this is sent.
        channel.respond(request.request_id, Decision.DENY,
                        reason="manual policy", reason_code=0)
    elif request is not None:
        # ShadowEvent and CommandResult are notifications/results.
        print(request)
```

Convenience methods are equivalent to `respond`:

```python
channel.allow_once(request_id)
channel.allow_always(request_id, reason="session rule")
channel.deny_once(request_id)
channel.deny_always(request_id, reason="blocked")
```

`reason` is UTF-8 and limited to 95 bytes. `reason_code` is 0–255.
`persistent=True` means session-only `ALLOW_ALWAYS`/`DENY_ALWAYS`; it is not
written to disk.

`serve(handler)` performs the handshake if needed and loops until close. The
handler receives every event. For an access request it may return:

```python
Decision.ALLOW
(Decision.ALLOW, "approved", 0, True)
None                         # do not answer automatically
```

The tuple is `(decision, reason, reason_code, persistent)`. Return values for
shadows and command results are ignored. A handler that returns `None` for an
access request leaves the guest blocked, so an interactive consumer should
store the request and answer it later rather than using `serve` as a hidden
allowlist.

### Commands

Commands use a new nonzero request ID and return `CommandResult`. While a
command is pending, new access requests and shadows may arrive. They must be
handled, not mistaken for the command result:

```python
def handle_event(event):
    if isinstance(event, (PathRequest, NetRequest)):
        channel.allow_once(event.request_id)
    else:
        print("notification", event)

result = channel.get_state(handler=handle_event)
if result.status != 0:
    raise RuntimeError(f"GET_STATE failed: {result.status}")
```

Available helpers:

```python
channel.set_rule(
    operation=PathOperation.READ,
    path="/guest/config",
    other_path="",
    decision=Decision.ALLOW,
    handler=handle_event,
)
channel.set_rule(
    operation=NetOperation.CONNECT,
    family=2, port=443, address=b"\x7f\x00\x00\x01" + b"\0" * 12,
    decision=Decision.ALLOW,
    handler=handle_event,
)
channel.forget(operation=PathOperation.READ, path="/guest/config",
                handler=handle_event)
channel.reveal_shadow("/guest/hidden", ShadowScope.RECURSIVE,
                      handler=handle_event)
channel.restore_shadow("/guest/hidden", ShadowScope.RECURSIVE,
                       handler=handle_event)
state = channel.get_state(handler=handle_event)
```

If `handler` is omitted, interleaved events are queued internally and can be
read with subsequent `receive()` calls. Do not stop reading the channel after
sending a command: the guest can immediately generate another request.

`detach_socket()` transfers ownership to another channel implementation and
closes the `ControlChannel` view. It is the handoff used by the Textual
harness.

## `ProotConfig` and `ProotProcess`

`ProotConfig` is the generic non-PTY launcher configuration:

```python
config = ProotConfig(
    proot_path="proot",
    args=("-r", "/rootfs", "-b", "/host/data:/guest/data:ro"),
    guest_command=("/bin/sh", "-i"),
    env={"TERM": "xterm-256color"},
    cwd=None,
    timeout=1.0,
    keep_stdin=True,
    grace_period=0.5,
)
process = ProotProcess.spawn(config)
```

`ProotProcess` exposes `process`, `channel`, `stdout`, `stderr`, `pid`, and
`returncode`. Call `close()` exactly once or repeatedly (it is idempotent).
The launcher creates the control socketpair and appends `--control-fd`; callers
must not put that option in `args`.

## `PtyProotProcess`

The PTY launcher is the correct choice for a terminal application:

```python
from control_api.pty_launcher import PtyProotProcess

process = PtyProotProcess.spawn(config)
try:
    process.resize(45, 134)
    os.write(process.master_fd, b"echo ready\r")
    # Read process.master_fd and process.control_channel.sock with select,
    # asyncio.add_reader, or Textual's event loop.
finally:
    process.close()
```

Public fields and methods:

| Member | Contract |
|---|---|
| `master_fd` | nonblocking PTY master for shell I/O |
| `control_channel` | synchronous PRCT channel after HELLO |
| `pid` / `returncode` | process identity and poll result |
| `resize(rows, cols)` | sends `TIOCSWINSZ` and `SIGWINCH` |
| `terminate()` | sends SIGTERM to the process group |
| `kill()` | sends SIGKILL to the process group |
| `close()` | closes PRCT/PTY and cleans the process group |

## `AsyncControlChannel`

This channel never waits for a complete frame. Register `fileno()` with the
event loop and call `receive()` whenever it is readable:

```python
import asyncio
from control_api.async_channel import AsyncControlChannel
from control_api import ChannelState, ControlEOF, NetRequest, PathRequest

async_channel = AsyncControlChannel.from_socket(sock)
async_channel.state = ChannelState.READY  # HELLO was validated synchronously

def on_control_readable():
    try:
        while True:
            event = async_channel.receive()
            if event is None:
                break
            if isinstance(event, (PathRequest, NetRequest)):
                async_channel.respond(event.request_id, 0)  # explicit deny
                async_channel.flush()
            else:
                handle_notification(event)
    except ControlEOF:
        handle_process_exit()

loop = asyncio.get_running_loop()
loop.add_reader(async_channel.fileno(), on_control_readable)
```

Important members:

- `fileno()` — descriptor for `add_reader`.
- `receive()` — returns one decoded event, `None` when more bytes are needed,
  and raises a terminal `ControlError` on EOF or malformed input.
- `respond()` — queues a decision; it does not guarantee a write.
- `flush()` — performs nonblocking writes and must be called after decisions.
- `wants_write` — true while output remains queued; an application may use it
  to register `loop.add_writer` and call `flush()` until false.
- `command(type, payload)` — queues a command and returns its request ID.
  Continue processing access events while waiting for its `CommandResult`.
- `close()` — idempotent descriptor close.

### Textual handoff

The harness first uses `PtyProotProcess` to validate HELLO, then transfers the
same descriptor:

```python
session = HarnessSession(process, SessionPolicy())
session.start_readers(loop, terminal.feed_bytes, app.receive_event)
```

`HarnessSession` owns the async channel after `detach_socket()`:

- `start_readers(loop, on_pty, on_event)` registers PTY and PRCT readers;
- `write(data)` writes terminal input to the PTY;
- `decide(request_id, decision, persistent=False)` queues and flushes a
  decision, then updates the in-memory policy;
- `close(loop)` removes readers, closes PRCT, and cleans the process.

The session reports `running`, `process_exited`, `prct_failed`, or `closed` in
`lifecycle`. A peer failure never approves pending requests.

## Session policy and presets

`SessionPolicy` is deliberately memory-only:

```python
policy.receive(request)
pending = policy.pending_items()
policy.decide(request_id, Decision.ALLOW, persistent=True)
rule = policy.rules[policy.key(request)]
```

`persistent=True` means “always for this session”, not persistent storage.
`ShadowEvent` is recorded but never placed in the decision queue.

Preset helpers:

```python
from control_api.proot_tui.presets import (
    Binding, HarnessConfig, build_config, default_config, parse_bind,
)

config = default_config(shell="fish", command=("fish", "--no-config", "-i"))
config.binds.append(parse_bind("/host/project:/guest/project:ro"))
process_config = build_config(config)
```

`default_config()` supplies the Termux preset: explicit runtime binds,
read-only prefix/home, writable canonical `$TMPDIR`, masked storage,
`--net-policy deny`, `--net-allow *`, `xterm-kitty`, and proc isolation off
unless requested. `build_config()` translates this to generic PRoot arguments;
it does not add implicit PRoot behavior.

Before starting Textual, `proot_tui.preflight.check(config)` verifies that the
configured PRoot is discoverable and that the installed Termux Textual version
is compatible (`>=7.5,<9`). It raises `PreflightError` with a user-facing
diagnostic; it does not install packages or mutate the system.

CLI equivalent from the repository root:

```bash
./proot/bin/sample-harness
./proot/bin/sample-harness --shell fish -- --no-config -i
./proot/bin/sample-harness --rootfs "$ROOTFS" --bind "$PWD:/work:ro" --proc-isolated
./proot/bin/sample-harness --with-storage --rw-dir "$PWD/work"
```

## Complete interactive flow

The lifecycle of a robust harness is:

1. Resolve and validate `HarnessConfig`.
2. Spawn `PtyProotProcess`; do not add `--control-fd` yourself.
3. Validate HELLO before presenting the terminal.
4. Transfer the socket to `AsyncControlChannel` if using an event loop.
5. Register readers for both `master_fd` and `fileno()`.
6. Feed PTY bytes to the terminal renderer and write keyboard/mouse bytes back.
7. Put every `PathRequest`/`NetRequest` into a pending UI queue.
8. Wait for an explicit allow/deny decision; never auto-approve unknown
   events.
9. Continue servicing access events while commands wait for `CommandResult`.
10. On EOF, malformed frame, timeout, or process death, mark the session
    failed/closed, approve nothing pending, and clean all descriptors.

## `ExternalTerminal`

`proot_tui.terminal.ExternalTerminal` is a Textual `Widget` with
`can_focus=True`. It is constructed with an object exposing `write(data)` and
`process.resize(rows, cols)`—`HarnessSession` satisfies that contract:

```python
from control_api.proot_tui.terminal import ExternalTerminal

terminal = ExternalTerminal(session, scrollback=10_000, id="terminal")
terminal.feed_bytes(pty_output)       # decode/interpret ANSI and VT input
terminal.on_key(key_event)            # Textual sends keyboard bytes to PTY
terminal._resize(rows, cols)           # normally called by Textual on resize
```

The widget handles ANSI/VT state, UTF-8, alternate screens, mouse tracking,
bracketed paste, Kitty keyboard, scrollback, and terminal capability replies.
The PTY reader should call `feed_bytes` on the event loop; it should not call
`render()` directly or perform synchronous reads from the widget.

`proot_tui.app.run_app(config)` is the complete application entry point. The
module entry point is equivalent:

```bash
PYTHONPATH=proot/control-api/python python3 -m control_api.proot_tui
```

## Errors and ownership

| Exception/state | Meaning | Action |
|---|---|---|
| `ControlEOF` | peer closed the PRCT stream | treat as process lifecycle or failure, then close |
| `ControlTimeout` | complete frame missed its deadline | fail closed; do not reuse channel |
| `InvalidFrame` | malformed header/payload/request | fail closed and report diagnostics |
| `Desynchronized` | channel used after terminal state or IDs mismatch | discard channel and process |
| `ChannelState.FAILED` | no more protocol operations are safe | do not approve pending requests |

The channel owns its socket. The PTY launcher owns the PTY and process group.
After `detach_socket()`, the new async channel owns the control socket. Closing
the same descriptor from multiple owners is a bug; use one owner and an
idempotent `close()` path.

## Verification

Run the complete local suite from the checkout:

```bash
PYTHONPATH=proot/control-api/python python3 -m unittest discover -s proot/tests/control-api/python -v
```

The integration tests cover fragmented frames, command interleaving,
fail-closed EOF/timeout behavior, fish styling and completion, Neovim Unix
sockets, TMPDIR authorization, PTY resize, and TUI startup/shutdown.
