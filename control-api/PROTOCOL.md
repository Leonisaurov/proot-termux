# PRCT control protocol

The `control-api` implementations consume the version 1 protocol used by
`proot --control-fd`. A channel is a full-duplex `SOCK_STREAM` (usually an
`AF_UNIX` socketpair).

All integers are little-endian. This is the wire format used by the Termux
aarch64 build; implementations reject big-endian hosts. Every frame has this
20-byte header:

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
PUBLICATION 3, DNS 4. Path operations are READ 1, WRITE 2, CREATE 3,
DELETE 4, RENAME 5, METADATA 6. Shadow scopes are NODE 1 and RECURSIVE 2.

Implementations read incrementally, apply a default 1000 ms timeout, and use
one absolute deadline for a complete header plus payload. They fail closed on
malformed magic/version/type/size/payload, truncation, EOF, timeout, or
request-id mismatch. Paths are absolute guest paths of at most 1023 bytes;
host paths are never accepted or exposed.

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
