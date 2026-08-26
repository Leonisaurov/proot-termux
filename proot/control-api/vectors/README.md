# Golden vectors

The canonical vectors are kept as hex so they remain reviewable in source
control. `hello.hex` is a zero-payload HELLO frame; the other files contain
the header plus payload for representative request/response/command frames.
They are generated from the layouts in `PROTOCOL.md`, not from native struct
packing. Each language test should compare its raw framing against these
values before exercising a live socketpair. Files contain ASCII hex; decode
them with `bytes.fromhex` (Python), `Buffer.from(text.trim(), "hex")`
(Bun), or a small hex decoder (Rust). The shared set includes HELLO, an access
request, a decision, and a command result.
