# proot-termux

[![GitHub repo size](https://img.shields.io/github/repo-size/Leonisaurov/proot-termux)](https://github.com/Leonisaurov/proot-termux)
[![Build proot](https://github.com/Leonisaurov/proot-termux/actions/workflows/build-proot.yml/badge.svg)](https://github.com/Leonisaurov/proot-termux/actions/workflows/build-proot.yml)
[![Docker image](https://github.com/Leonisaurov/proot-termux/actions/workflows/docker_image.yml/badge.svg)](https://github.com/Leonisaurov/proot-termux/actions/workflows/docker_image.yml)
[![proot-latest](https://img.shields.io/github/v/release/Leonisaurov/proot-termux?include_prereleases&label=proot-latest)](https://github.com/Leonisaurov/proot-termux/releases/tag/proot-latest)

**proot-termux** is a minimal fork of [termux-packages](https://github.com/termux/termux-packages) that cross-compiles [proot](https://proot-me.github.io/) for Android **aarch64** using the Android NDK r29 via Docker. All other packages and build infrastructure have been stripped away — only proot remains.

The goal is a lean, automated build pipeline that produces a ready-to-install `.pkg.tar.xz` artifact on every push, with virtual networking extensions built in.

---

## Quick Build

```bash
./scripts/run-docker.sh ./build-package.sh -I -a aarch64 --format pacman proot
```

This single command:
1. Spins up the build container (`ghcr.io/leonisaurov/package-builder:latest`)
2. Resolves and builds dependencies (`libandroid-shmem`, `libtalloc`)
3. Cross-compiles proot for aarch64
4. Outputs a `.pkg.tar.xz` package in `output/`

---

## Proot Source

The proot source lives **directly in the repository** at [`proot-source/src/`](./proot-source/src/) — no patches, no downloads. This is a modified version of upstream proot that includes custom features (see below).

To modify proot:

1. Edit files under `proot-source/src/`
2. Bump `TERMUX_PKG_REVISION` in [`packages/proot/build.sh`](./packages/proot/build.sh)
3. Push — the [CI workflow](#cicd) triggers automatically

The build process copies `proot-source/` into the build directory via `rsync` during `termux_step_pre_configure()`, then compiles with `make`. No external source extraction is needed (`TERMUX_PKG_SKIP_SRC_EXTRACT=true`).

---

## Virtual Networking (`--proxy`)

This fork adds an **isolated virtual networking** layer to proot. Applications running inside the proot see normal TCP/IP (`AF_INET`/`AF_INET6` sockets), but traffic is transparently tunnelled over **Abstract Unix Domain Sockets**. No real network ports are consumed unless explicitly exposed.

Key capabilities:

- `--proxy NAME` — creates an isolated virtual network. Multiple proot instances with the same `NAME` can communicate.
- `-p HOST:PORT` — exposes a virtual port to the real network via a TCP→Unix bridge helper process.
- **Cross-instance isolation**: different `--proxy` names are fully isolated; no `--proxy` means no virtual network at all.
- **Port mapping** (`-p host:container`) coexists with virtual networking.

For the full technical reference — syscall translation, registry format, cross-instance token model, and known bugs — see [`AGENTS.md`](./AGENTS.md) (sections *Virtual Networking* and *Bugs Fixed*).

---

## Dependencies

Proot depends on two libraries, both built automatically by the `-I` flag in the build command:

| Dependency | Package Definition |
|---|---|
| `libandroid-shmem` | [`packages/libandroid-shmem/build.sh`](./packages/libandroid-shmem/build.sh) |
| `libtalloc` | [`packages/libtalloc/build.sh`](./packages/libtalloc/build.sh) |

---

## CI/CD

Two GitHub Actions workflows automate the entire build and release process.

### `build-proot.yml`

| Aspect | Detail |
|---|---|
| **Trigger** | Push to `packages/proot/**` or `proot-source/**` |
| **Runner** | `ubuntu-26.04` with 16 GB zram |
| **Cache** | `~/.termux-build` is cached with key based on `build.sh` hashes |
| **Build** | `./scripts/run-docker.sh ./build-package.sh -I -a aarch64 --format pacman proot` |
| **Release** | Creates/updates a `proot-latest` GitHub Release with the `.pkg.tar.xz` artifact |
| **Artifact** | Also uploaded as a workflow artifact (`proot-aarch64-<sha>`) |

**First run**: ~5 min (seeds the cache).  
**Subsequent runs**: ~20–30 s on cache hit.

### `docker_image.yml`

Builds and pushes the build container image to `ghcr.io/leonisaurov/package-builder:latest`. Triggered manually via `workflow_dispatch`.

---

## Repository

The canonical repository is:

**https://github.com/Leonisaurov/proot-termux**

All issues, releases, and CI runs are managed there. This is a standalone fork — not affiliated with the upstream Termux project.

---

## Development

1. Edit source files under `proot-source/src/`
2. Bump `TERMUX_PKG_REVISION` in `packages/proot/build.sh`
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

### `--isolate-pid`
Hide host processes from `/proc/` inside the sandbox (partial pid
namespace emulation). High effort: requires intercepting `readdir`
on `/proc` and `stat` on `/proc/[pid]/`.

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
