# Python control-api and sample harness

The complete Python API reference and integration cookbook is in
[`python-api.md`](python-api.md). It covers synchronous and asynchronous channels, PTY
launching, commands, policy/session ownership, presets, Textual integration,
errors, and complete examples.

The package `control_api` is the small, standard-library PRCT codec and
synchronous launcher.  It remains usable on systems without Textual:

```python
from control_api import Decision, PathRequest, ProotConfig, ProotProcess

p = ProotProcess.spawn(ProotConfig(guest_command=('/bin/sh',)))
# A consumer must make an explicit decision for every PathRequest/NetRequest.
```

The interactive application expects a compatible Textual (7.5 through 8.x) to be supplied by the
Termux/pacman Python environment. The project does not download Textual or
terminal widgets from Git and does not alter the system package set. The
harness owns the PRoot PTY and includes its own ANSI renderer.
From the repository root, `./proot/bin/sample-harness` supplies
`proot/control-api/python` on `PYTHONPATH` and runs
`python -m control_api.proot_tui`. If the current directory is already
`proot/`, the equivalent command is `./bin/sample-harness`.

The default preset is intentionally explicit: `--termux-paths`, read-only
Termux/home bindings plus read-only Android runtime bindings for `/system`,
`/system_ext`, `/product`, `/vendor`, `/odm`, `/apex`, `/linkerconfig`,
`/dev`, and `/sys` when present. This covers the dynamic linker and
version-dependent APEX libraries without waiting for PRCT decisions. Explicit
`:ro` bindings authorize read and metadata operations at the PRoot layer;
writes remain denied/mediated. It also
adds a masked storage entry, `--net-policy deny`, and
`--net-allow '*'` to hand unknown destinations to the PRCT decision channel.
The latter is not an allow rule.  Proc isolation is off by default and is
shown as a warning in the side panel.  `Allow always` and `Deny always` are
session-only rules; nothing is persisted.

The helper adds only the canonical Termux `$TMPDIR` as an explicit `:rw`
binding. This narrow exception is required for editor state and Unix-domain
socket files; it does not expose `/data` or Android storage. A `:rw` binding
does not generate a redundant path prompt. A `:ro` binding is exempt for
reads/metadata but a mutation is still mediated and remains subject to the
static read-only check; a `:wo` binding behaves symmetrically for reads.

Network authorization includes socket creation itself. `socket()` and
`socketpair()` produce a `NET_ACCESS_REQUEST` with operation `SOCKET` (5),
the requested nonzero domain such as `AF_UNIX` (1), and zero address/ports.
The request must be answered before the socket syscall proceeds. Internet
bind/connect/publication requests continue to use their own operations, so
approving a local Unix socket never authorizes network traffic.

The TUI never translates or resolves paths in PRCT requests.  Requests contain
guest paths only, and no host path is displayed as an authorization detail.
On EOF, malformed frames, timeout, or process failure the channel enters a
terminal failed state and pending requests are not approved.

### Fish and terminal compatibility

The terminal widget is a real PTY endpoint; fish is not run through a line
buffer or a second shell.  The renderer handles the capabilities fish uses
during startup and interactive editing, including:

- incremental UTF-8, wide and combining characters, ANSI SGR colors,
  256-color and truecolor output, cursor styles, wrapping and erase modes;
- alternate screen, scroll regions, bounded scrollback, insert/delete
  operations, resize repainting and cursor-position reports;
- fish's XTGETTCAP requests (`indn` and `query-os-name`), answered with the
  `DCS 1+r` response fish 4.x consumes, without leaking the hexadecimal query
  into the screen;
- kitty keyboard protocol, application cursor keys, modifiers, function keys,
  bracketed paste, focus reporting and SGR mouse events;
- OSC title, working-directory, hyperlinks, prompt markers and clipboard
  requests.

Keyboard events are consumed by the terminal while it has focus, so fish owns
letters and control keys.  The side-panel controls are still available by
mouse; after a decision the application restores terminal focus.  `Ctrl-Q`
is reserved by the harness to close the session.

The default Termux preset does not add fish compatibility flags or disable
fish terminal features. It advertises `xterm-kitty` and truecolor so fish 4.x
enables its Kitty keyboard and XTGETTCAP negotiation against the renderer. A real fish
integration regression covers startup negotiation, styled output, Unicode and
Tab completion:

```bash
PYTHONPATH=proot/control-api/python python3 -m unittest discover -s proot/tests/control-api/python -v
```

Useful options include `--rootfs PATH`, `--bind HOST:GUEST[:ro|rw|mask]`,
`--rw-dir PATH`, `--proxy NAME`, `--proc-isolated`, `--with-storage`, and
`--no-proc-isolated`.  Press `a/A/d/D` to decide the selected request, `Tab` to
move focus, and `Ctrl-q` to close the session.
