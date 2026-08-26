# Hermes sandbox integration proposal for proot-termux

## Purpose

Hermes uses this fork as a Termux sandbox with read-only host binds,
`--proc-isolation`, and the virtual-network extension. The fork already gives
Hermes the important primitive for a safe first network mode:

```text
proot --proxy NAME ...
```

With `--proxy NAME`, guest TCP/UDP sockets are translated to abstract Unix
sockets. Instances using the same name can communicate; different names are
isolated. Without `-p`, no real TCP port is opened and the virtual network has
no Internet path on Android ARM64. `-p HOST:GUEST` is different: it starts a
real TCP bridge and is an explicit exposure to the host network.

Hermes can therefore expose a session-level `direct` versus `proxy` choice
today. Hermes cannot currently ask the harness for permission before an
arbitrary guest process calls `bind(2)`: proot has no approval event channel in
the bind path, and command-text inspection is not a reliable substitute for a
syscall hook.

## Minimal upstream feature requested

Add an opt-in bind mediation mode, without coupling proot to Hermes or any UI.
The smallest useful interface would be:

```text
--bind-policy=allow       # current behavior
--bind-policy=deny        # reject guest bind requests
--bind-policy=ask FD      # notify a controller and wait for a decision
```

The exact CLI spelling may change, but the semantics should remain:

1. Intercept guest `bind(2)` before registering the virtual socket or starting
   a `-p` bridge.
2. Send a structured request over a dedicated inherited Unix FD or the
   existing `--supervise` control channel. Never use stdout/stderr for the
   protocol.
3. Include a request id, tracee PID, address family, guest address, guest port,
   proxy name, and whether the request would expose a host port.
4. Block that bind until the controller replies `allow` or `deny` for the
   request id.
5. Deny on timeout, malformed replies, closed controller, or supervisor exit.
6. Apply the same gate to `-p` host bridges; a bridge must never be created
   before approval.
7. Cancel pending requests when the tracee exits and ensure registry/helper
   cleanup is unchanged.

This keeps policy in the harness: proot only mediates the syscall and carries
the decision. It also works for Python, C, compiled servers, child processes,
IPv4, and IPv6, unlike shell heuristics.

## Controller expectations

The harness should present the request to the operator and return a decision
with a bounded lifetime. The UI may later support one-shot approval,
session-scoped approval, or a port/name allowlist, but those policies should
remain outside proot. The protocol must identify virtual and real exposure so
the UI can explain that a plain `--proxy` bind is visible only to peers in the
same virtual network while `-p` reaches the host network.

## Useful follow-up features

These are lower priority than bind mediation:

- expose virtual-network state and active registry entries for diagnostics;
- expose bridge lifecycle and selected host ports to the controller;
- complete `--fake-net` for interface/netlink discovery so `/proc`-adjacent
  network information does not reveal the host topology;
- normalize or mask Android cgroup identity where feasible;
- return clean, documented errors for intercepted ptrace attempts;
- provide supervisor events for tracee exit, bridge exit, and stale registry
  cleanup.

The existing ARM64 limitation must remain explicit: syscall argument
rewriting cannot implement a transparent Internet escape from a virtual
network. `--proxy` should continue to mean isolated virtual networking, not a
best-effort Internet proxy.

## Hermes integration contract

Hermes should pass `--proxy NAME` only when the user enables the sandbox
network mode. It should derive an isolated name per task when none is supplied,
restart persistent sessions atomically when the mode changes, and never add
`-p` implicitly. Once bind mediation exists, Hermes can connect `--bind-policy`
to its approval harness without changing the proot virtual-network model.
