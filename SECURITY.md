# Security Policy

## Supported versions

The fork publishes one supported line at a time. The current line is the
package built from the source in this repository.

| Version | Package revision | Tag | Supported |
|---|---|---|---|
| `5.1.107.89` | `105` (last released revision) | `proot-5.1.107.89-105` | Yes |
| `5.1.107.89` | `106` (in this tree, not released yet) | — | Yes, as the development line |
| `5.1.107.89` | `104` and earlier | earlier releases | No. Upgrade to the latest release. |

The upstream base version is `5.1.107.x`. The upstream PRoot project does its
own security maintenance, and the fixes that upstream publishes are tracked in
[`proot/docs/security/FIXES.md`](proot/docs/security/FIXES.md), section H.

The released revision comes from the release tags of this repository; the
revision of the tree under development is the `TERMUX_PKG_REVISION` of
[`proot/ci/termux/packages/proot/build.sh`](proot/ci/termux/packages/proot/build.sh),
which is bumped before every commit that touches the packaged source.

## Reporting a vulnerability

Report the vulnerability privately, through the security advisory form of this
repository:

- <https://github.com/Leonisaurov/proot-termux/security/advisories/new>

Use the private advisory, not a public issue, so that the details stay
confidential until a fix exists.

Fallback contact: the maintainer address recorded in the git history of this
repository, `leonisaurov@gmail.com`. The same address is used if the advisory
form is not available.

Please include:

- the version or the package revision that reproduces the problem, and whether
  the build is the release package or a local native build;
- the flags and the guest layout involved, in particular whether the guest
  runs with `--termux-paths` or with a rootfs;
- the steps to reproduce, and the observed behaviour against the expected one;
- the impact you believe it has.

### Problems that belong to upstream

- A defect in the upstream PRoot source that is not introduced by this fork
  should be reported to the upstream project:
  <https://github.com/termux/proot/issues>.
- A defect in the Termux packaging framework inherited from termux-packages
  should be reported to that project.

This fork still tracks those problems, because its source is derived from
them. Reporting upstream does not replace the fork's own tracking, and a
report sent upstream is not a report to the fork unless it is also sent here.

### What is in scope

- A sandbox escape, or a way to observe the host filesystem, processes or
  network from inside the guest.
- A way to make the guest see a state that does not exist on the host, or to
  hide a state that the isolation flags promise to expose.
- A failure of the control channel that lets a guest bypass an authorization
  decision.
- A weakness in the network policy, the virtual network, or the resource
  limits that lets a guest exceed the requested restriction.

The following are not vulnerabilities in this fork:

- A behaviour documented as a limitation of the Android kernel, such as the
  absence of the chained syscall mechanism on ARM64, or the `seccomp` filter
  of the platform that blocks `reboot(2)` before proot can see it.
- A guest that reaches a path it was explicitly bound to, or that uses the
  `--proxy` network on purpose.
- A report that depends on running the code in a configuration this
  repository does not build, such as a build that defines `USERLAND`, unless
  the same problem is also reachable in the released package.

## Hardening model

Proot traces its guest with `ptrace` and installs a `seccomp` filter to
intercept the system calls it has to mediate. The isolation features of this
fork are explicit extensions that are compiled in but inert until their flag
is passed, so that a run without flags takes the same path as upstream. The
governing rule is "emulate, never deny": when a natural result can be
produced, proot produces it instead of returning `EPERM` or `ENOSYS`, so the
guest cannot tell the difference between a sandboxed system and a native one.
The security-relevant extensions are the network policy, the strict proc view,
the resource limits, the identity emulation, the virtual network, the
supervisor with its `--exec` clients, and the file-descriptor and symlink
compatibility extensions. Each one is documented in
[`proot/docs/security/FIXES.md`](proot/docs/security/FIXES.md) and in
[`proot/docs/operations/usage.md`](proot/docs/operations/usage.md).

## Disclosure timeline

This is the fork's policy. It is not a commitment made on behalf of the
upstream project or of any other project.

| Stage | Target |
|---|---|
| Acknowledgement of the report | Within 7 days |
| Triage: severity, reproducibility, affected versions | Within 14 days |
| Fix and released revision | Within 90 days of the report |
| Public advisory | At the release of the fix, or earlier if the reporter agrees |

If a fix takes longer than the target, the reason is stated in the advisory
before the deadline passes. A report that cannot be reproduced is closed with
the reason, and it can be reopened with new information.

The reporter is credited in the advisory unless they ask not to be.

## Security-relevant files

| File | What it controls | Documented in |
|---|---|---|
| `proot/src/extension/net_policy/net_policy.c` | The static network policy: the `off`, `deny` and `allow` modes, the bind and destination rules, and the handover of the wildcard destinations to the control channel | [`proot/docs/security/FIXES.md`](proot/docs/security/FIXES.md), [`proot/control-api/PROTOCOL.md`](proot/control-api/PROTOCOL.md) |
| `proot/src/extension/proc_isolation/proc_isolation.c` | The strict guest view of `/proc`: the listing, the synthetic files, the descriptor state, the confinement of `ptrace`, `process_vm`, `kill` and `pidfd_open` | [`proot/docs/architecture/proc-leaks.md`](proot/docs/architecture/proc-leaks.md), [`proot/docs/security/FIXES.md`](proot/docs/security/FIXES.md) |
| `proot/src/extension/resource_limit/resource_limit.c` | The resource limits and the gate on the process creation inside the instance | [`proot/docs/operations/usage.md`](proot/docs/operations/usage.md), [`proot/docs/security/FIXES.md`](proot/docs/security/FIXES.md) |
| `proot/src/extension/fake_id0/fake_id0.c` | The identity emulation, the permission emulation, and the meta files that record the owner, group and mode perceived by the guest | [`proot/docs/security/FIXES.md`](proot/docs/security/FIXES.md), section K |
| `proot/src/extension/virtual_net/virtual_net.c` | The virtual network over abstract Unix sockets and its shared registry | [`proot/docs/operations/usage.md`](proot/docs/operations/usage.md), [`proot/docs/security/FIXES.md`](proot/docs/security/FIXES.md) |
| `proot/src/supervise/supervise.c` | The supervisor: the control socket, the peer authentication with `SO_PEERCRED`, and the `--exec` clients | [`proot/docs/operations/usage.md`](proot/docs/operations/usage.md), [`proot/docs/security/FIXES.md`](proot/docs/security/FIXES.md) |
| `proot/src/syscall/seccomp.c` | The interception filter: the syscall list, the exit stops that are requested, and the rewrite of the system calls that need one | [`proot/docs/operations/usage.md`](proot/docs/operations/usage.md), [`proot/docs/security/FIXES.md`](proot/docs/security/FIXES.md) |
| `proot/src/path/proc.c` | The translation of the `/proc` paths of the host into the paths of the guest | [`proot/docs/architecture/proc-leaks.md`](proot/docs/architecture/proc-leaks.md), [`proot/docs/security/FIXES.md`](proot/docs/security/FIXES.md) |

A change to any of these files is reviewed as a security change, even when
its purpose is a performance or a compatibility fix.
