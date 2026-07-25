# AGENTS.md — termux-packages (proot-only fork)

## Project Overview

Proot-only fork of termux-packages. Cross-compiles proot for Android aarch64 using NDK r29 via Docker.

The source lives directly in `proot-source/` — no patches, no downloads.

## Build System

- **Main script**: `build-package.sh` — orchestrates the entire build pipeline
- **Docker**: `scripts/run-docker.sh` runs builds inside `ghcr.io/termux/package-builder:latest`
- **Cache**: GitHub Actions caches `~/.termux-build` for incremental builds (~20s when nothing changes)

### Key Commands

```bash
./scripts/run-docker.sh ./build-package.sh -I -a aarch64 --format pacman proot
```

## Proot Package

### Location
- `packages/proot/build.sh` — package definition
- `packages/proot/termux-chroot` — termux-chroot script template
- `proot-source/src/` — modified source (1.3 MB, tracked in repo)

### Dependencies
- `libandroid-shmem`, `libtalloc` (built automatically via `-I` flag)

### Original Features (pre-virtual_net)

1. **Port Mapping** (`-p host:container` / `--port host:container`):
   - Max 64 mappings, auto-finds free port if target occupied

2. **Auto-redirect** (`--protect-privileged-ports`):
   - Redirects `bind()` for ports < 1024 by +2000, finds next free port

3. **Bind Permissions**: Extended `-b` syntax with `:ro/:wo/:rw`

4. **Merge Bind** (`-m` / `--mbind`): Copies rootfs directory contents before bind

### Virtual Networking (`--proxy NAME`)

Added in revision 12-16. Implements isolated virtual networks using **Abstract Unix Domain Sockets** instead of real TCP/IP. No system ports are used unless explicitly exposed via `-p`.

#### How it works

When `--proxy web` is active:
1. **socket(AF_INET/AF_INET6)** → domain changed to `AF_UNIX`. Tracee receives an AF_UNIX socket but thinks it's AF_INET.
2. **bind(0.0.0.0:PORT)** → translated to `bind(AF_UNIX, @proot-vnet-{name}-{port}-{token})`. The socket binds to a unique abstract Unix socket name.
3. **listen()** → passes through (AF_UNIX sockets support listen natively).
4. **accept()** → passes through. The kernel handles AF_UNIX accept gracefully (the fd is tracked in SYSCALL_EXIT_END for getpeername).
5. **connect(127.0.0.1:PORT)** → translated to `connect(AF_UNIX, @proot-vnet-{name}-{port}-{token})`. Looks up the unique abstract name from the shared registry.
6. **getsockname()/getpeername()** → syscall voided at ENTER. Fake `sockaddr_in`/`sockaddr_in6` with loopback address (`127.0.0.1` or `::1`) and virtual port are written to the tracee's buffer.
7. **setsockopt(IPPROTO_TCP, ...)** → voided (TCP-level options don't apply to AF_UNIX sockets).
8. **close()** → removes fd from tracking map.

#### Cross-instance Communication

Multiple proot instances with the same `--proxy NAME` can communicate:

1. **Unique Instance Token**: Each proot instance generates a unique 32-bit token at initialization (mix of `clock_gettime` + stack address).
2. **Shared Registry**: A file (`registry.lock`) in `/data/data/com.termux/files/usr/tmp/proot-net/{name}/` maps `{virtual_port → unique_abstract_name}`. Protected by `flock(2)`.
3. **Bind** writes to registry (LOCK_EX). Creates a unique abstract socket `@proot-vnet-{name}-{port}-{token}`.
4. **Connect** reads from registry (LOCK_SH). Finds the unique abstract name and connects to it.

#### Port Exposure (`-p HOST:VIRTUAL` with --proxy)

When `-p 9090:80` is used with `--proxy web`:
- A **helper process** is spawned (re-exec'd proot with `--vnp-helper NAME`).
- The helper creates a real TCP listener on `0.0.0.0:9090`.
- For each incoming TCP connection, the helper connects to the abstract Unix socket `@proot-vnet-web-80-{token}` and bridges data bidirectionally via `select()` loop.
- The helper forks a child per connection (SIGCHLD ignored, auto-reap).

#### Isolation Guarantees

| Scenario | Result |
|----------|--------|
| Same `--proxy NAME` | ✅ Connects via abstract socket |
| Different `--proxy NAME` | ❌ Blocked (different registry) |
| No `--proxy` (inside rootfs) | ❌ Blocked (no virtual_net extension) |
| Native host (outside proot) | ❌ Blocked (no TCP listener) |

#### Key Files (virtual_net extension)

| File | Purpose |
|------|---------|
| `extension/virtual_net/virtual_net.h` | Public header |
| `extension/virtual_net/virtual_net_internal.h` | Internal types: VnpConfig, VnpFdEntry, registry structs, helpers |
| `extension/virtual_net/virtual_net.c` | Main extension (979 lines): callback, all syscall handlers, registry management |
| `extension/virtual_net/virtual_net_helper.c` | TCP→Unix bridge helper for `-p` expose (277 lines) |
| `extension/virtual_net/virtual_net_helper.h` | Helper entry point |
| `cli/proot.c` | `handle_option_proxy()`, `-p` delegation to virtual_net |
| `cli/proot.h` | `--proxy` option definition |
| `cli/cli.c` | `--vnp-helper` dispatch |
| `extension/extension.h` | `vnp_callback` declaration |

#### Registry Format

File: `registry.lock` (combined lock + data file, ~59KB)

```
struct VnpRegistryHeader {
    uint32_t magic;                          // "VPRG" = 0x50524F4E
    uint32_t count;
    uint32_t generation;
    struct VnpRegistryEntry entries[512];     // 512 × 116 bytes = 59392 bytes
}; // Total: 59404 bytes

struct VnpRegistryEntry {
    uint16_t virtual_port;
    // 2 bytes padding (for uint32_t alignment on aarch64)
    uint32_t instance_token;
    char     abstract_name[108];             // Leading \0 + abstract socket name
}; // Total: 116 bytes
```

#### Bugs Fixed (revisions 12-16)

| Bug | Revision | Symptom | Fix |
|-----|----------|---------|-----|
| AF_INET6 not handled | 13 | `EINVAL` on `bind()` with IPv6 socket | Read `sockaddr_in6` correctly, extract port |
| `poke_word` corrupts stack | 14 | `getsockname()` returns `(0, bytes)` | Use `peek_uint32`/`poke_uint32` for `socklen_t` (4 bytes) |
| No cross-instance connect | 15 | Different proot can't connect | Add registry lookup in connect handler |
| `strncpy` truncates abstract name | 15 | Registry stores empty abstract name | Use `memcpy` instead of `strncpy` |
| socket() protocol=IPPROTO_TCP | 15 | Socket creation fails for AF_UNIX | Zero protocol arg when changing domain |
| REMOVED handler corrupts registry | 16 | 2nd curl sees count=0 in registry | Remove registry write from REMOVED handler |
| Missing closing brace | 14 | Compilation error | Add `}` in getpeername else block |

### Modified Source Files

- `proot-source/src/extension/virtual_net/` — Virtual networking extension (5 files)
- `proot-source/src/cli/proot.h` — `--proxy` option, `handle_option_proxy` forward declaration
- `proot-source/src/cli/proot.c` — `handle_option_proxy()`, `-p` delegation to `vnp_add_expose()`
- `proot-source/src/cli/cli.c` — `--vnp-helper` dispatch, `print_usage()` null-safety fix
- `proot-source/src/extension/extension.h` — `vnp_callback` declaration
- `proot-source/src/GNUmakefile` — `virtual_net.o` and `virtual_net_helper.o`

## How the Build Works

1. `TERMUX_PKG_SKIP_SRC_EXTRACT=true` skips download
2. `termux_step_pre_configure()` rsyncs `proot-source/` into build dir
3. `make` compiles with all features built-in (no patches)
4. Package step creates `.pkg.tar.xz`

## GitHub Actions Workflow

### `build-proot.yml`
- **Trigger**: Push to `packages/proot/**` or `proot-source/**`
- **Steps**: Clone → zram → restore cache → prepare → build → collect → release → save cache → artifact
- **Caching**: `~/.termux-build` mounted into Docker via `TERMUX_DOCKER_RUN_EXTRA_ARGS`
- **Cache key**: hash of `packages/proot/build.sh`, `packages/libtalloc/build.sh`, `packages/libandroid-shmem/build.sh`
- **Downloads**: `gh release download proot-latest -R Leonisaurov/termux-packages -p "*.pkg.tar.xz"`

### `docker_image.yml`
- Builds/pushes `ghcr.io/leonisaurov/package-builder:latest`

## Workflow Monitoring

Siempre que se lance un workflow, usar `gita notify` para esperar el resultado y leer los logs:

```bash
cd /data/data/com.termux/files/home/Develop/Clones/termux-packages
gita notify build-proot.yml 2>/dev/null | grep -E '(error|##\[error\]|mbind|Success)'
```

- Exit code 0 = éxito, 1 = falló, 2 = cancelado
- `gita notify` bloquea hasta que el workflow termina (o retorna inmediatamente si ya terminó)
- NO usar `timeout`

## Development

1. Edit files in `proot-source/src/`
2. Bump `TERMUX_PKG_REVISION` in `packages/proot/build.sh`
3. Push — workflow triggers automatically
4. Siempre verificar con `gita notify build-proot.yml`

### Commit Guidelines

`<type>(<scope>): <summary>`

Types: `fix`, `enhance`, `chore`, `ci`

## Important Paths

| Path | Purpose |
|------|---------|
| `build-package.sh` | Main build script |
| `scripts/run-docker.sh` | Docker wrapper |
| `packages/proot/` | Proot definition |
| `proot-source/src/extension/virtual_net/` | Virtual networking extension |
| `.github/workflows/build-proot.yml` | CI workflow |
| `scripts/buildorder.py` | Dependency resolver (patched for proot-only) |
| `repo.json` | Package directories list |

## Known Issues

- **`buildorder.py`**: Patched to skip missing deps when building a specific package (not full build). This is needed because some remaining packages (libllvm, python) declare dependencies on packages we removed.
- **`/data` mount**: Not used in CI. `-m` flag in `run-docker.sh` mounts `/data` from host which causes permission issues on GHA runners. Cache is mounted via `TERMUX_DOCKER_RUN_EXTRA_ARGS`.
- **First CI run**: ~5 min (seeds cache). Subsequent runs: ~20-30s with cache hit.
- **Registry cleanup**: Stale entries in `registry.lock` are not cleaned up (they don't affect functionality). To manually clean: delete `/data/data/com.termux/files/usr/tmp/proot-net/`.

## Agent Configuration

The orchestrator agent lives at `~/.config/opencode/agent/orquestador.md`.
