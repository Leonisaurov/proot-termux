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

## License

This project is derived from [termux-packages](https://github.com/termux/termux-packages) which is licensed under GPL-3.0. The proot source is licensed under GPL-2.0. See individual source files for details.
