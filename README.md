# proot-termux

[![GitHub repo size](https://img.shields.io/github/repo-size/Leonisaurov/proot-termux)](https://github.com/Leonisaurov/proot-termux)
[![Build proot](https://github.com/Leonisaurov/proot-termux/actions/workflows/build-proot.yml/badge.svg)](https://github.com/Leonisaurov/proot-termux/actions/workflows/build-proot.yml)
[![proot-latest](https://img.shields.io/github/v/release/Leonisaurov/proot-termux?include_prereleases&label=proot-latest)](https://github.com/Leonisaurov/proot-termux/releases/tag/proot-latest)

**proot-termux** is a minimal fork of [termux-packages](https://github.com/termux/termux-packages) that cross-compiles [proot](https://proot-me.github.io/) for Android **aarch64** using the Android NDK r29 via Docker. All other packages and build infrastructure have been stripped away — only proot remains.

The goal is a lean, automated build pipeline that produces a ready-to-install `.pkg.tar.xz` artifact on every push. Proot remains a standalone, multipurpose tool; its capabilities are selected explicitly by the caller.

## Declarative PRoot launcher

Los entrypoints auxiliares están agrupados por función: `bin/appimage-run`
ejecuta AppImages y los wrappers Alpine viven en `tests/rootfs/` para las
pruebas de PRoot.

Para describir y ejecutar una instancia completa de PRoot desde un archivo
TOML (`rootfs`, binds, proxy, política de red, `control_fd`, entorno y
comando), consulta [docs/tools/proot-exec.md](docs/tools/proot-exec.md) y copia
[examples/proot-exec/proot-exec.conf.example](examples/proot-exec/proot-exec.conf.example). El ejecutable es:

```bash
./bin/proot-exec --config ./proot-exec.conf --dry-run
./bin/proot-exec --config ./proot-exec.conf
```

---

## Quick Build

```bash
./ci/termux/scripts/run-docker.sh ./ci/termux/build-package.sh -I -a aarch64 --format pacman proot
```

This single command:
1. Spins up the build container (`ghcr.io/leonisaurov/package-builder:latest`)
2. Resolves and builds dependencies (`libandroid-shmem`, `libtalloc`)
3. Cross-compiles proot for aarch64
4. Outputs a `.pkg.tar.xz` package in `ci/termux/output/` in CI. Local builds
   write to `artifacts/packages/` by default.

---

## Proot Source

The proot source lives **directly in the repository** at [`proot-source/src/`](./proot-source/src/) — no patches, no downloads. The `proot-source/` directory intentionally contains only the source tree; repository-facing notes and licensing are in `docs/`. This is a modified version of upstream proot that includes custom features (see below).

To modify proot:

1. Edit files under `proot-source/src/`
2. Bump `TERMUX_PKG_REVISION` in [`ci/termux/packages/proot/build.sh`](./ci/termux/packages/proot/build.sh)
3. Push — the [CI workflow](#cicd) triggers automatically

For local Termux builds use [`scripts/build-native.sh`](./scripts/build-native.sh),
not `make` directly. Its jobs option takes a separate argument (`-j 2` or
`--jobs 2`; `-j2` is invalid). The CI build copies `proot-source/src/` into the
build directory via `rsync` during `termux_step_pre_configure()`, then invokes
the project makefile. No external source extraction is needed
(`TERMUX_PKG_SKIP_SRC_EXTRACT=true`).

Para probar, elige primero un modo de rutas y mantenlo: con
`--termux-paths` usa `$PREFIX/bin/sh` y `$PREFIX/etc`; con un rootfs usa
`/bin/sh` y `/etc`. `PROOT_TMP_DIR` y `PROOT_RUNTIME_DIR` son rutas host para
el propio proot, no rutas guest.

Las regresiones nuevas se añaden como scripts revisables bajo `tests/<tema>/`.
Primero se valida el script (`bash -n`, `git diff --check`) y después se
ejecuta el archivo con el flujo elevado de Termux; no se sustituyen por
comandos improvisados en la terminal. Los tests de `termux-isolated` deben usar
ese launcher y cubrir el modo de rutas que están verificando.

---

## Strict guest `/proc` (`--proc-isolated`)

`--proc-isolated` implements a strict procfs guest view. `/proc` lists only
the current instance's tracees and a small whitelist (`self`, `thread-self`
and supported synthetic files); host PIDs and sensitive entries such as
`/proc/net`, `/proc/sys`, `/proc/kcore`, `/proc/keys` and `/proc/kmsg` are not
exposed.

Global and per-process files are generated with Linux-compatible formats.
`status`, `stat`, `limits`, `maps`, `mountinfo`, `attr/current`, `io`,
`sched`, `pagemap`, `fdinfo` and related files do not copy host statistics or
mount topology. `maps`, `exe`, `cwd` and `/proc/*` symlinks preserve guest
paths, including non-canonical and relative `openat`/`readlink` access.

The mode tracks proc state by descriptor across partial reads, `dup`,
`fcntl`, `pread` and `close`, and maps `/proc/self` and `/proc/thread-self`
to the calling tracee. `--proc-isolated` also confines ptrace, process-vm,
kill and pidfd access to the instance.

When using `./bin/termux-isolated --termux-paths`, paths such as
`/data/data/com.termux/files/usr` are valid guest paths by design. With the
default rootfs mode, paths should instead remain inside that rootfs (for
example `/usr` or `/home`) and must not reveal Android host paths. Use
`--no-proc-isolated` is an explicit opt-out: it restores the previous procfs
behavior, including host procfs data, and is intended only for compatibility
comparisons. `./bin/termux-isolated` uses the strict view by default.

Android internal storage is unavailable by default. The launcher masks
`$HOME/storage`, `/storage/emulated/0`, `/storage/self/primary`, and `/sdcard`
with `:mask`, including access through the usual Termux storage symlinks. Pass
`--with-storage` explicitly when a guest needs the internal storage; this
replaces those masks with the corresponding read-write bindings.

The launcher keeps normal Termux shebangs such as `#!/usr/bin/env bash` and
`#!/bin/bash` working in both launcher modes by binding the Termux executable
directory at the conventional `/bin` and `/usr/bin` guest paths. It does not
inherit `libtermux-exec-ld-preload.so` into every child process, avoiding the
per-`execve` overhead of that interceptor. Scripts do not need to be rewritten
with `termux-fix-shebang`.

When started without a command, `bin/termux-isolated` launches the shell selected
by Termux (the persistent `~/.termux/shell` selection, with `$SHELL` as a
fallback; for example fish) in an explicit interactive login session. It
translates the Termux prefix to the guest prefix in rootfs mode. If that shell
is unavailable or outside the Termux prefix, it safely falls back to bash.

---

## Virtual Networking (`--proxy`)

This fork adds an **isolated virtual networking** layer to proot. Applications running inside the proot see normal TCP/IP (`AF_INET`/`AF_INET6` sockets), but traffic is transparently tunnelled over **Abstract Unix Domain Sockets**. No real network ports are consumed unless explicitly exposed.

Key capabilities:

- `--proxy NAME` — creates an isolated virtual network. Multiple proot instances with the same `NAME` can communicate.
- `-p HOST:PORT` — exposes a virtual port to the real network via a TCP→Unix bridge helper process.
- **Cross-instance isolation**: different `--proxy` names are fully isolated; no `--proxy` means no virtual network at all.
- **Port mapping** (`-p host:container`) coexists with virtual networking.

For the full technical reference — syscall translation, registry format, cross-instance token model, and known bugs — see [`AGENTS.md`](./AGENTS.md) (sections *Virtual Networking* and *Bugs Fixed*).

## Optional protocol integrations

Proot can expose explicit protocol capabilities such as `--control-fd`. A
consumer such as [`control-api`](./control-api/) requires a compatible proot
version and protocol implementation; without that compatibility, the consumer
cannot operate. A harness is the program that owns the control fd, receives
events, and decides how to respond.

The dependency is intentionally one-way: proot does not require `control-api`,
does not choose its authorization policy, and does not contain its launcher's
presets or application-specific configuration. `termux-isolated` is a separate
launcher with its own explicit Termux configuration. A harness may provide
presets, but it must pass the resulting options to proot and remain
responsible for their effects.

---

## Dependencies

Proot depends on two libraries, both built automatically by the `-I` flag in the build command:

| Dependency | Package Definition |
|---|---|
| `libandroid-shmem` | [`ci/termux/packages/libandroid-shmem/build.sh`](./ci/termux/packages/libandroid-shmem/build.sh) |
| `libtalloc` | [`ci/termux/packages/libtalloc/build.sh`](./ci/termux/packages/libtalloc/build.sh) |

---

## CI/CD

The CI/package-builder checkout is kept under [`ci/termux/`](./ci/termux/).
It contains the package definitions, `repo.json`, Docker wrapper, and Termux
build scripts, NDK compatibility patches, and package cleanup command
`clean.sh`. `.github/` remains at the repository root because that is the
path GitHub Actions requires. Local native compilation remains separate under
[`scripts/build-native.sh`](./scripts/build-native.sh).

The active GitHub Actions workflow automates the package build and release process.

### `build-proot.yml`

| Aspect | Detail |
|---|---|
| **Trigger** | Push to `ci/termux/packages/proot/**` or `proot-source/**` |
| **Runner** | `ubuntu-26.04` with 16 GB zram |
| **Cache** | `~/.termux-build` is cached with key based on `build.sh` hashes |
| **Build** | `./ci/termux/scripts/run-docker.sh ./ci/termux/build-package.sh -I -a aarch64 --format pacman proot` |
| **Release** | Creates/updates a `proot-latest` GitHub Release with the `.pkg.tar.xz` artifact |
| **Artifact** | Also uploaded as a workflow artifact (`proot-aarch64-<sha>`) |

**First run**: ~5 min (seeds the cache).  
**Subsequent runs**: ~20–30 s on cache hit.

The `docker_image.yml` workflow is currently disabled and is not part of the active build path.

---

## Repository

The canonical repository is:

**https://github.com/Leonisaurov/proot-termux**

All issues, releases, and CI runs are managed there. This is a standalone fork — not affiliated with the upstream Termux project.

---

## Development

1. Edit source files under `proot-source/src/`
2. Bump `TERMUX_PKG_REVISION` in `ci/termux/packages/proot/build.sh`
3. Commit and push — the workflow builds and releases automatically

### Commit Format

```
<type>(<scope>): <summary>
```

Types: `fix`, `enhance`, `chore`, `ci`

Example: `fix(virtual_net): handle AF_INET6 bind correctly`

---

## ARM64 Limitations

### Virtual Network + Internet Access

We experimented with adding a `--allow-internet` flag to enable internet
access while keeping virtual network functionality via `--proxy`. This
required a **chained syscall mechanism** (socket → connect → dup3 → close)
to replace AF_UNIX sockets with AF_INET ones for external traffic.

**The chain mechanism does not work on ARM64.** The Linux kernel on ARM64
does not re-read argument registers (x0-x5) when the instruction pointer
(PC) is rewound after a ptrace syscall-exit stop. Only the syscall number
(x8) can be changed (via `NT_ARM_SYSTEM_CALL`, added in Linux 4.17).
Without the ability to change argument registers, chained syscalls always
execute with the original syscall's arguments — making fd replacement
impossible.

This is a **kernel-level limitation** of ARM64's ptrace implementation,
not a proot bug. On x86_64 the same mechanism works because the kernel
re-reads all registers when a rewinded PC re-executes the `SVC` instruction.

**Status:** `--allow-internet` was removed. Without it, `--proxy` provides
virtual network isolation (all sockets become AF_UNIX, no internet access).
To access the internet, run proot without `--proxy`.

### Android: reboot() blocked by kernel seccomp

On Android, the kernel's seccomp filter (configured by `init.rc`) blocks
the `reboot()` syscall (NR 142) with `SECCOMP_RET_KILL` before proot or
any ptrace tracer can intercept it. This is a kernel-level restriction
that cannot be bypassed from userspace.

As a result, `--reboot-isolated` cannot emulate `reboot()` on Android.
The handler is correct and tested on standard Linux, but on Android the
syscall never reaches proot. The calling process is killed by the kernel
seccomp filter before any interception occurs.

Other isolation flags (`--proc-isolated`, `--ptrace-isolated`,
`--swap-isolated`, `--bpf-isolated`, `--perf-isolated`,
`--kexec-isolated`, `--ioport-isolated`, `--handle-isolated`) work
correctly on Android.

---

## Planned Features

### `--fake-net`
Fake network namespace: intercept `/proc/net/`, `/sys/class/net/`, and
netlink sockets (rtnetlink) to show only `lo` interface. Tools like
`ip addr`, `ifconfig`, `ss` would no longer expose real host interfaces.

Rationale: Currently `--proxy` intercepts TCP/UDP but network discovery
tools still leak host network information.

### `--hostname NAME`
Override `/proc/sys/kernel/hostname` and `hostname` syscall to show
a fake hostname inside the sandbox.

### `--hide-uid`
Intercept `/proc/self/status` and `getuid`/`geteuid` syscalls to show
fake UID/GID (complement to existing `--change-id`).

### `--seccomp-filter`
Install a seccomp-bpf filter inside the sandbox to block dangerous
syscalls: `ptrace`, `perf_event_open`, `bpf`, `kexec_load`,
`open_by_handle_at`, etc.

**Note on ptrace:** A `--seccomp-filter` would block `ptrace` syscall
inside the sandbox. This means debuggers (gdb, strace) would NOT work
inside the sandbox. If ptrace access is needed, use proot without
`--seccomp-filter`. Proot itself does NOT use seccomp-filter to restrict
the inner process — it only uses seccomp internally for interception.
A `--seccomp-filter` feature would add an ADDITIONAL seccomp filter on
top of proot's existing one.

### `--bpf-isolated`, `--perf-isolated`, `--handle-isolated`
Block bpf(), perf_event_open(), and open_by_handle_at() syscalls.
These are complex to emulate (require BPF bytecode interpreter, perf
counters, and filesystem handle translation respectively), so they
return ENOSYS/ENOENT/EOPNOTSUPP without revealing host existence.

Full emulation is possible but requires significant effort. See TODO.

---

## License

This project is derived from [termux-packages](https://github.com/termux/termux-packages) which is licensed under GPL-3.0. The proot source is licensed under GPL-2.0. See individual source files for details.
