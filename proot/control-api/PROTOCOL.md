# PRCT control protocol

The `control-api` implementations consume the version 1 protocol used by
`proot --control-fd`. A channel is a full-duplex `SOCK_STREAM` (usually an
`AF_UNIX` socketpair).

The header and payload integers are written in the **native endianness of the
proot instance** that produced them. The implementation performs no byte-order
conversion: the wire structures are plain packed C layouts (`ControlHeader`,
`ControlNetRequest`, `ControlPathRequest`, `ControlDecision` in
`proot/src/extension/net_policy/net_policy.c`) that are copied to and from the
channel as-is.

Both peers must therefore agree on endianness. The reference consumers shipped
in this repository are little-endian only -- the Python codec refuses to start
on a big-endian host (`PRCT requires little-endian host`) and the Rust codec
uses `to_le_bytes`/`from_le_bytes` -- and the Termux aarch64 target is
little-endian, so both sides coincide in practice. A peer built for a
different endianness must not be used against that instance. Every frame has
this 20-byte header:

| offset | type | field |
|---:|---|---|
| 0 | u32 | magic `0x50524354` (`PRCT`) |
| 4 | u16 | version `1` |
| 6 | u16 | message type |
| 8 | u32 | payload size, at most 4096 |
| 12 | u64 | request id |

Payloads are packed C layouts. `NET_ACCESS_REQUEST` is 230 bytes:
`u32 operation, i32 guest_pid, i32 host_pid, u16 family, u16 protocol,
u16 guest_port, u16 host_port, u8 address[16], u8 virtual_class,
u8 real_exposure, char proxy[64], char domain[128]`.

For IP operations, family is AF_INET (2) or AF_INET6 (10). PUBLICATION uses
family 0. AF_UNIX uses family 1: `address[0]` is the endpoint kind (`0`
unnamed, `1` pathname, `2` abstract), `address[1]` is the exact byte length,
and `domain[0:length]` carries the guest endpoint bytes. Abstract names are
opaque bytes and may contain NUL; they are never parsed as C strings or guest
paths. Unnamed endpoints have length zero. Host paths are never placed in the
frame or returned in errors. SOCKET uses the requested nonzero socket domain
and carries zero address and ports; authorizing SOCKET does not authorize a
later BIND or CONNECT.
`PATH_ACCESS_REQUEST` is 2056 bytes (`u32 operation, u32 reason,
char path[1024], char other_path[1024]`). A `SHADOW_EVENT` has the same
payload shape as a path request. Decision payloads are 98 bytes:
`u8 decision, u8 reason_code, char reason[96]`.

Commands use either `NET_COMMAND` (28 bytes: operation, family, port,
address[16], decision, reserved[3]) or `PATH_COMMAND` (2060 bytes: operation,
scope, decision, path[1024], other_path[1024]). `COMMAND_RESULT` is 20 bytes:
`i32 status, u32 flags, u32 dynamic_path_rules, u32 dynamic_net_rules,
u32 shadows`.

Message values are: HELLO 1, NET_ACCESS_REQUEST 2, PATH_ACCESS_REQUEST 3,
SHADOW_EVENT 4, COMMAND_RESULT 5; ALLOW_ONCE 16, ALLOW_ALWAYS 17,
DENY_ONCE 18, DENY_ALWAYS 19, FORGET 20, SET_RULE 21, REVEAL_SHADOW 22,
RESTORE_SHADOW 23, GET_STATE 24. Network operations are BIND 1, CONNECT 2,
PUBLICATION 3, DNS 4, SOCKET 5. SOCKET requests authorize creation of a non-unspecified socket family; their address and ports are zero. Path operations are READ 1, WRITE 2, CREATE 3,
DELETE 4, RENAME 5, METADATA 6. Shadow scopes are NODE 1 and RECURSIVE 2.

Implementations read incrementally, apply a default 1000 ms timeout, and use
one absolute deadline for a complete header plus payload. They fail closed on
malformed magic/version/type/size/payload, truncation, EOF, timeout, or
request-id mismatch. Paths are absolute guest paths of at most 1023 bytes;
host paths are never accepted or exposed.

When `--control-fd` is active, an `UNKNOWN` network destination is not a
virtual-network classification and is never implicitly allowed. The static
network policy may use only `*`, `tcp://*`, or `udp://*` to hand it to PRCT;
the harness must still return an allow decision. External guest paths produce
`PATH_ACCESS_REQUEST` unless an explicit read-only binding covers a read or
metadata operation. Writes against a read-only binding, and reads against a
write-only binding, remain denied or mediated. The current working directory
and its descendants may be handled by core path semantics, but proot has no
implicit Android, Termux, or rootfs bindings. Mutating operations remain
subject to PRCT and static binding permissions.

## Versioning and compatibility

The `version` field is a hard gate, not a negotiation. Proot accepts only
version `1`; a frame with any other version is treated as malformed, the
channel is marked failed and the operation is denied (`-EACCES`). There is no
capability exchange, no downgrade and no fallback to an older layout.

Because the payload layouts are fixed by that version, a version-1 peer must
also agree on the sizes and field order documented above: 230 bytes for
`NET_ACCESS_REQUEST`, 2056 bytes for `PATH_ACCESS_REQUEST` and `SHADOW_EVENT`,
98 bytes for a decision, 28 bytes for `NET_COMMAND`, 2060 bytes for
`PATH_COMMAND` and 20 bytes for `COMMAND_RESULT`. A peer that changes any of
them is no longer version 1.

Compatibility is a consumer-side property. `control-api` requires a proot that
implements this protocol, but proot never requires `control-api` and never
depends on it. The protocol is an explicit opt-in capability of the proot
executable (`--control-fd`): without that flag proot does not speak PRCT at
all and keeps its direct, compatible network behaviour.

## Channel lifecycle

The channel starts in `CREATED`. `HELLO` (type 1, id 0, empty payload) is
mandatory as the first frame and moves it to `READY`; `handshake()` can
consume it explicitly and `serve()`/`receive()` consume it implicitly. A
malformed frame, unknown type, invalid path, timeout, EOF, or desynchronised
response permanently moves the channel to `FAILED`. `close()` moves it to
`CLOSED`; neither terminal state can be reused.

The timeout is one absolute deadline for the complete header and payload. It is
not restarted for each `recv()` fragment. Decision payloads are always 98
bytes: decision, reason code, and a UTF-8 reason limited to 95 bytes plus the
zero padding. The four decision helpers are available in all implementations.

Commands use a locally generated non-zero request ID. While waiting for its
`COMMAND_RESULT`, access requests and shadows remain events and may be queued
or passed to a handler; they must not be answered as command results. The same
rule applies to `serve()`: only access requests can cause an automatic
decision; shadows and command results are notifications.
