# PRCT control API

This is an external consumer of proot. It requires a proot build that supports
the PRCT control protocol and the corresponding `--control-fd` capability. A
proot build without that protocol support cannot run this API successfully.

The dependency is one-way: proot remains usable without this directory and
does not depend on these libraries, launchers, presets, or policies.

This directory contains three small consumers for `proot --control-fd`:

* `python/control_api.py` — synchronous, standard library only.
* `rust/` — a `std`-only path dependency with no async runtime.
* `bun/control_api.ts` — TypeScript using Bun/Node stream primitives.

All three expose raw `recv_frame`/`send_frame` and a decoded `receive`/`recv`
layer. The first frame on every normal channel must be an empty `HELLO` with
request id zero. A harness owns policy: `serve` automatically answers only
`NET_ACCESS_REQUEST` and `PATH_ACCESS_REQUEST`; `SHADOW_EVENT` and
`COMMAND_RESULT` are delivered to the handler without turning its return
value into an access decision. No implementation accepts host paths or
attempts to decide access automatically. See [PROTOCOL.md](PROTOCOL.md) for
exact packed layouts and [examples/](examples/) for short harnesses.

## Integrated launcher

The launchers create an `AF_UNIX` stream `socketpair`, pass the child end as a
heritable descriptor, add `--control-fd` themselves, capture stdout/stderr,
and validate `HELLO` before returning a ready process. Do not put
`--control-fd` in the user argument list. Closing the API end is cooperative;
the launcher waits its grace period and then terminates/kills a still-running
proot.

The launcher owns this setup and its policy. It must pass all desired rootfs,
binding, environment, authorization, and Termux-specific options explicitly;
none of those settings are implied or installed as proot defaults. A consumer
may offer presets such as `load termux`, but those presets belong to the
consumer and are not built into proot.

The control channel is fail-closed. With it enabled, PRCT still authorizes
network destinations classified as `UNKNOWN` (the static `*`, `tcp://*`, and
`udp://*` rules only hand such requests off), and external guest filesystem
paths even when a read-only binding is readable. Proot has no fixed list of
Android, Termux, or rootfs paths exempt from the protocol: the harness decides
what to allow and answers the PRCT request. Approvals never change static
binding permissions.

El harness debe seguir leyendo el control fd después de enviar un comando: el
guest puede generar inmediatamente nuevas solicitudes de ruta o red. Leer
solo el resultado del comando y luego esperar al proceso puede bloquearlo y
producir un error engañoso del loader.

Python (the launcher fills in `--control-fd` and validates `HELLO` before
returning):

```python
from control_api import Decision, NetRequest, PathRequest, ProotConfig, ProotProcess
c = ProotConfig(
    args=("-r", "/rootfs"),
    guest_command=("/bin/sh", "-c", "echo ok"),
)
p = ProotProcess.spawn(c)
p.channel.serve(lambda r: (Decision.ALLOW, "approved")
                if isinstance(r, (NetRequest, PathRequest)) else None)
p.close()
```

### Resource ownership (Python)

`ControlChannel.from_fd(fd)` takes ownership of `fd`; `close()` closes it.
`ControlChannel.from_socket(sock)` likewise takes ownership of the supplied
socket and its `close()` is idempotent. `ProotProcess.close()` closes the
control channel, waits for the child (terminating or killing it after the
configured grace period), and closes `stdout` and `stderr`. If those streams
are exposed to application code, read them before calling `close()`, or use
`p.process.communicate()` first.

If `ProotProcess.spawn()` fails, it closes both socketpair ends, and cleans up
any created child process and its output streams before re-raising the error.

Rust uses `ProotCommand::default()` with `args` and `guest_command` fields;
Bun uses `new ProotCommand({args, guestCommand}).spawn()`. Both expose the
channel, PID, stdout and stderr. Rust keeps guest stdin separate from the
control descriptor. The API is only framing, validation, dispatch and process
plumbing: it does not define permission policy or translate guest paths to
host paths.

## Embedding a reactive harness

Python:

```python
from control_api import ControlChannel, Decision, NetRequest, PathRequest
ch = ControlChannel.from_fd(3)
ch.serve(lambda r: Decision.ALLOW
        if isinstance(r, (NetRequest, PathRequest)) else None)
```

Rust:

```rust
let mut ch = ControlChannel::from_stream(stream, Duration::from_secs(1))?;
ch.serve(|r| Some(if matches!(r, Request::Net(_)) { Decision::Deny } else { Decision::Allow }))?;
```

Bun/TypeScript:

```ts
const ch = ControlChannel.fromFd(3);
await ch.serve(r => r.type === Message.NET_ACCESS_REQUEST ? Decision.DENY : Decision.ALLOW);
```
