# AGENTS.md — proot-termux (proot-only fork)

## Project Overview

Proot-only fork focused exclusively on proot. Cross-compiles proot for Android aarch64 using NDK r29 via Docker.

The source lives directly in `proot-source/` — no patches, no downloads.

## Build System

- **Main script**: `build-package.sh` — orchestrates the entire build pipeline
- **Docker**: `scripts/run-docker.sh` runs builds inside `ghcr.io/termux/package-builder:latest`
- **Cache**: GitHub Actions caches `~/.termux-build` for incremental builds (~20s when nothing changes)

### Key Commands

```bash
./scripts/run-docker.sh ./build-package.sh -I -a aarch64 --format pacman proot
```

### ✅ CI Activo — build-proot.yml

El workflow `build-proot.yml` está ACTIVO y se ejecuta automáticamente en pushes que toquen `packages/proot/**` o `proot-source/**`. Usar `gita notify build-proot.yml` para monitorear. La compilación local (`build-native.sh`) sigue disponible para desarrollo rápido.

El workflow `docker_image.yml` (imagen del builder) SÍ sigue deshabilitado con `if: false`.

```bash
./scripts/build-native.sh        # Compila + empaqueta (desarrollo local)
./scripts/build-native.sh -i     # Compila + instala + empaqueta (desarrollo local)
```

## Proot Package

## Critical Rules

### ⚠️ ALWAYS bump TERMUX_PKG_REVISION before commit

**Cada vez que modifiques archivos en `proot-source/src/` o `packages/proot/`, DEBES incrementar `TERMUX_PKG_REVISION` en `packages/proot/build.sh` ANTES de hacer commit.**

Sin esto, el workflow de CI no se disparará correctamente y los usuarios no recibirán la actualización.

Regla:
1. Editas código → 2. Bump REVISION → 3. Commit → 4. Push → 5. Verificar con `gita notify build-proot.yml`

**NUNCA hagas commit sin bumpear la revisión.** Es el error más común y el que más tiempo hace perder.

### ✅ Workflow de commit (no omitir pasos)

Cada vez que hagas cambios en el código, sigue este workflow SECUENCIALMENTE:

1. **Editas código** → modifica archivos en `proot-source/src/` o `packages/proot/`
2. **Bump REVISION** → incrementa `TERMUX_PKG_REVISION` en `packages/proot/build.sh`
3. **Commit** → `git add -A && git commit -m "<type>(<scope>): <summary>"`
4. **Push** → `git push origin master`
5. **Verificar CI** → `gita notify build-proot.yml 2>/dev/null | grep -E '(error|Successfully|done)'`

**NO saltees ningún paso.** El error más común es commitear sin bumpear, o pushear sin verificar el CI.

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

#### Bugs Fixed (revisions 12-18)

| Bug | Revision | Symptom | Fix |
|-----|----------|---------|-----|
| AF_INET6 not handled | 13 | `EINVAL` on `bind()` with IPv6 socket | Read `sockaddr_in6` correctly, extract port |
| `poke_word` corrupts stack | 14 | `getsockname()` returns `(0, bytes)` | Use `peek_uint32`/`poke_uint32` for `socklen_t` (4 bytes) |
| No cross-instance connect | 15 | Different proot can't connect | Add registry lookup in connect handler |
| `strncpy` truncates abstract name | 15 | Registry stores empty abstract name | Use `memcpy` instead of `strncpy` |
| socket() protocol=IPPROTO_TCP | 15 | Socket creation fails for AF_UNIX | Zero protocol arg when changing domain |
| REMOVED handler corrupts registry | 16 | 2nd curl sees count=0 in registry | Remove registry write from REMOVED handler |
| Missing closing brace | 14 | Compilation error | Add `}` in getpeername else block |
| Code polish across extension files | 17 | Redundant code, macros, style | Extracted `handle_port_translation()`, removed `DETAIL`/`APPEND` macros, `select()`→`poll()`, POSIX macros for IPv6, lazy init patterns |
| Static deliver_sigtrap kills exec children | 18 | --exec child killed by SIGTRAP (exit 133) | Replaced global static bool with per-tracee ptrace_options_set field |

### Modified Source Files

- `proot-source/src/extension/virtual_net/` — Virtual networking extension (5 files)
- `proot-source/src/cli/proot.h` — `--proxy` option, `handle_option_proxy` forward declaration
- `proot-source/src/cli/proot.c` — `handle_option_proxy()`, `-p` delegation to `vnp_add_expose()`
- `proot-source/src/cli/cli.c` — `--vnp-helper` dispatch, `print_usage()` null-safety fix
- `proot-source/src/extension/extension.h` — `vnp_callback` declaration
- `proot-source/src/GNUmakefile` — `virtual_net.o` and `virtual_net_helper.o`
- `proot-source/src/extension/resource_limit/` — Resource limits extension, guest-side: intercepción de `sched_getaffinity` (CPUs) y de `fork`/`clone`/`vfork` (proc-limit, `-EAGAIN`) (3 files: resource_limit.c/h/internal.h)
- `proot-source/src/cli/proot.h` — `--cpu-limit`/`--single-core`/`--mem-limit`/`--nice`/`--fd-limit`/`--proc-limit`/`--resource-isolated` options, `resource_config_apply()` declaration, `resource_config_proc_limit()` getter
- `proot-source/src/cli/proot.c` — `resource_config`, handlers, `resource_config_apply()` (solo affinity/nice/fd host-side), `init_guest_resource_limit()`, `resource_config_mem_limit_bytes()`, `resource_config_cpu_limit()`, `resource_config_proc_limit()`
- `proot-source/src/cli/cli.c` — `resource_config_apply()` hook in main (config_status, no pisa argc_offset)
- `proot-source/src/execve/exit.c` — `apply_mem_limit_to_tracee()` (post-exec, tracee-side)
- `proot-source/src/extension/extension.h` — `rlimit_callback` declaration
- `proot-source/src/GNUmakefile` — `resource_limit.o`

### Supervise Mode (`--supervise`) & Exec (`--exec <PID> <command>`)

Added in revision 18. Implements container-like process management.

`--supervise` keeps the event loop alive after the root tracee exits, listening
on an abstract Unix socket for `--exec` connections.

`--exec <PID> <command>` connects to a running supervisor and executes a command
inside its context (same rootfs, bindings, proxy network, etc).

#### How it works

1. **`--supervise`** modifies the event loop:
   - Uses `signalfd` + `poll()` instead of blocking `waitpid()` — zero overhead when not used
   - Creates an abstract listen socket: `@proot-exec-<PID>`
   - When the root tracee exits: logs the exit reason to
     `/data/data/com.termux/files/usr/tmp/proot-exit-<PID>.log`
   - Accepts `--exec` connections as long as it's alive

2. **`--exec <PID> <cmd...>`** runs as a separate proot invocation:
   - Connects to `@proot-exec-<PID>` abstract socket
   - Sends the command to execute
   - The supervisor `fork()`s a new tracee, applies the same context
   - The command runs with full path translation, bindings, and proxy network
   - When the command finishes, `--exec` exits with its exit code
   - If the supervisor isn't reachable, reads its exit log from the tmpdir

#### Usage

```bash
# Terminal 1: start a supervised server
proot --supervise --proxy web -r /data/data/com.termux/files/usr/var/lib/proot-distro/installed-rootfs/ubuntu /bin/server

# Terminal 2: run a healthcheck inside the same context
proot --exec $(pidof proot) curl -f http://127.0.0.1/health

# Check why a supervised process exited
cat /data/data/com.termux/files/usr/tmp/proot-exit-$(pidof proot).log
```

#### Key Files

| File | Purpose |
|------|---------|
| `supervise/supervise.h` | Public API: `supervise_init()`, `exec_connect()`, etc. |
| `supervise/supervise.c` | Implementation: abstract socket, signalfd, client tracking |
| `cli/proot.c` | `--supervise` handler |
| `cli/cli.c` | `--exec` dispatch before event loop |
| `tracee/event.c` | Poll-based event loop for supervise mode |
| `tracee/tracee.h` | `bool supervise` field in Tracee struct |

#### Protocol

Abstract socket: `@proot-exec-<PID>`

Request format (client → supervisor):
```
struct ExecRequest {
    int    argc;                          // Number of arguments
    char   argv[4096];                    // Args concatenated, \0-separated
    char   cwd[4096];                     // Working directory
};
```

Response format (supervisor → client):
```
struct ExecResponse {
    int    exit_status;   // WEXITSTATUS or 128+signal
    bool   signaled;      // true if killed by signal
    int    termsig;       // signal number if signaled
};
```

#### Zero Overhead Guarantee

When `--supervise` is NOT used, the event loop is **100% identical** to the
original proot code. There is no performance impact, no extra syscalls, no
changed behaviour.

When `--supervise` IS used, the only overhead is:
- One `signalfd` (1 fd)
- One `listen()` socket (1 fd)
- `poll()` instead of `waitpid()` — same blocking semantics
- `WNOHANG` loop after each SIGCHLD — negligible

#### Exit Log Format

File: `/data/data/com.termux/files/usr/tmp/proot-exit-<PID>.log`
```
process 'server' exited with status 1 (started 45s)
process 'server' killed by signal 9 (started 120s)
```

## Resource Limits (`--cpu-limit` y familia)

Added in revision 9. Implementa límites de recursos con diseño **híbrido**: algunos límites se aplican host-side (a proot mismo; el guest los hereda vía fork/exec) y otros tracee-side (a cada proceso del guest tras su execve).

### Flags

| Flag | Efecto | Lado |
|------|--------|------|
| `--cpu-limit N` | `sched_setaffinity()` a los N primeros cores (host) + el guest percibe N cores (intercepción de `sched_getaffinity`) | host + guest |
| `--single-core` | Alias de `--cpu-limit 1` | host + guest |
| `--mem-limit N[KMG]` | RLIMIT_AS aplicado AL GUEST (tracee-side, post-exec) — el host NO se limita | guest |
| `--nice N` | `setpriority(PRIO_PROCESS, 0, N)` (0..19, host) | host |
| `--fd-limit N` | `prlimit64(RLIMIT_NOFILE)` (mínimo 32, host) | host |
| `--proc-limit N` | Intercepción guest-side de `fork`/`clone`/`vfork`: al superar N procesos/threads vivos de ESTE proot, responde `-EAGAIN` al guest (semántica RLIMIT_NPROC). El resto del uid de Termux NO se ve afectado | guest |
| `--resource-isolated` | Combo: 1 core + nice 10 (sin mem/proc) | host + guest |

### Diseño híbrido (host-side vs tracee-side)

Los handlers de los flags (`handle_option_*` en `cli/proot.c`) solo guardan valores en `resource_config`. Después de `parse_config()`, `resource_config_apply()` (llamado desde `main()` en `cli/cli.c`) aplica los límites host-side en orden **affinity → nice → prlimits**:

1. **`--cpu-limit`/`--single-core`**: `sched_setaffinity(0, ...)` a los primeros N cores.
2. **`--nice`**: `setpriority(PRIO_PROCESS, 0, N)`.
3. **`--fd-limit`**: `prlimit64(RLIMIT_NOFILE)` (el único prlimit que queda host-side).

Los tracees heredan estos límites automáticamente vía fork/exec — no hace falta propagarlos.

**`--mem-limit` y `--proc-limit` son los límites tracee-side/guest-side**:

- **`--mem-limit`**: `RLIMIT_AS` NO se aplica a proot porque su espacio de direcciones virtuales en bionic es enorme (~10 GiB) y un cap de `RLIMIT_AS` lo haría crashear (SIGSEGV/SIGABRT). En su lugar `apply_mem_limit_to_tracee()` (`execve/exit.c`) aplica `prlimit64(pid, RLIMIT_AS)` a CADA tracee justo después de su execve, cuando su VSZ es pequeño. El guest es el que recibe ENOMEM si intenta crecer más allá del límite.
- **`--proc-limit`**: el viejo `prlimit64(RLIMIT_NPROC)` host-side se eliminó porque era **por uid** — afectaba a TODAS las sesiones de Termux del mismo usuario. Ahora la extensión `resource_limit` intercepta `fork`/`clone`/`vfork` en ENTER, cuenta los **tracees vivos de ESTE proot** (`get_tracees_list_head()` + `LIST_FOREACH` + `!tracee->terminated`) y, si el conteo >= N, hace `set_sysnum(PR_void)` + `poke_reg(SYSARG_RESULT, -EAGAIN)`: el guest recibe el error natural del kernel "Resource temporarily unavailable" y el hijo nunca se crea. Solo se cuentan los procesos/threads del sandbox — el resto del uid no se ve afectado.

### Guest-side: intercepción de `sched_getaffinity` y `fork`/`clone`

La extensión `extension/resource_limit/` (callback `rlimit_callback`) hace que el guest perciba solo N cores y solo N procesos/threads. Se inicializa con `--cpu-limit`/`--single-core`/`--resource-isolated` O con **solo `--proc-limit`** (`rlimit_configure()` no-op solo si ambos límites son 0).

#### `sched_getaffinity` (límite de CPUs)

La extensión hace que el guest perciba solo N cores:

1. Se filtra `sched_getaffinity` vía `extension->filtered_sysnums` (`FILTER_SYSEXIT`) — solo cuando `cpu_limit > 0`, **cero overhead** sin `--cpu-limit`.
2. El syscall real corre igual ("emulate, never deny"): el kernel resuelve pid 0 y devuelve el byte count.
3. En `SYSCALL_EXIT_END` se reescribe la máscara: solo bits 0..N-1. El valor de retorno se deja intacto.
4. El guest ve N cores: `nproc`, `taskset` y similares reportan el número limitado.

#### `fork`/`clone`/`vfork` (límite de procesos)

`--proc-limit N` limita los procesos/threads vivos de ESTE proot:

1. Se filtran `PR_clone`/`PR_clone3`/`PR_fork`/`PR_vfork` vía `extension->filtered_sysnums` (flags 0, ENTER-only) — solo cuando `proc_limit > 0`, **cero overhead** sin `--proc-limit`.
2. En `SYSCALL_ENTER_START` se cuenta los tracees vivos: `get_tracees_list_head()` + `LIST_FOREACH` + `!tracee->terminated`. El proceso actual ya cuenta (semántica RLIMIT_NPROC, que también cuenta threads).
3. Si `count >= N`: `set_sysnum(tracee, PR_void)` + `poke_reg(tracee, SYSARG_RESULT, -EAGAIN)` — el guest recibe "Resource temporarily unavailable" y el hijo nunca se crea.
4. En arm64 solo existen `PR_clone`/`PR_clone3` (`PR_fork`/`PR_vfork` se descartan vía `SYSCALL_AVOIDER`): toda creación de proceso/thread del guest pasa por `clone()`, que ya está en `proot_sysnums`.

### Decisiones clave

- **`--mem-limit` mínimo 16 MiB** (`RESOURCE_MEM_LIMIT_MIN`): valores menores se rechazan al parsear — el guest necesita un suelo de espacio virtual para cargar ejecutable, librerías y stack.
- **Validaciones al parsear** (rechazo con error claro, nunca EPERM espurio):
  - `--cpu-limit N` con N > cores reales (`sysconf(_SC_NPROCESSORS_CONF)`) → error.
  - `--nice` solo 0..19 (subir prioridad requiere root).
  - `--fd-limit` mínimo 32 (proot necesita un número justo de fds).
  - `--proc-limit` mínimo 1 (`n >= 1`): `--proc-limit 1` impide al guest crear cualquier hijo (el proceso actual ya cuenta) — semántica RLIMIT_NPROC correcta. Ya NO se valida contra los procesos actuales del uid (`count_processes_for_uid()` se eliminó con el viejo prlimit host-side).
- **`--mem-limit` se aplica a CADA tracee post-exec**: cubre también los tracees nuevos de `--supervise`/`--exec`.
- **`--resource-isolated`** deliberadamente NO fija mem/fd/proc.
- **"Last option wins"**: si se combina `--resource-isolated` con un `--cpu-limit`/`--nice` explícito, gana el último; la extensión refresca su config en vez de crear una segunda (`rlimit_configure()` hace double-init guard).
- **Fallo no fatal**: si la extensión guest-side no se inicializa, solo se avisa — el guest vería todos los cores del host y procesos ilimitados, pero los límites host-side (affinity, nice, fd) siguen aplicados.

### Interacción con `--supervise`/`--exec`

- Los tracees nuevos que el supervisor `fork()`ea reciben `--mem-limit` (el hook post-exec está en `translate_execve_exit()`).
- Los flags de resource limits del cliente `--exec` **NO se propagan**: el cliente solo conecta al supervisor y le envía el comando; **gana la configuración del supervisor**.
- **Gap conocido (`--proc-limit`)**: la creación inicial de tracees por parte del supervisor (los procesos que spawn ea vía `--exec`) NO pasa por el gate guest-side de `fork`/`clone` — esos tracees no se cuentan ni se bloquean. Solo los `fork()`/`clone()` posteriores de esos tracees sí pasan por el conteo y el límite. Es una limitación aceptada: el supervisor crea tracees host-side, fuera del alcance del gate.

### ARM64 note

- `--reboot-isolated` (y combinaciones con resource limits que dependan de `reboot()`) **no es testeable en ARM64 Android**: el seccomp del kernel bloquea `reboot()` antes de que proot pueda interceptarlo. Funciona en Linux estándar.

## How the Build Works

1. `TERMUX_PKG_SKIP_SRC_EXTRACT=true` skips download
2. `termux_step_pre_configure()` rsyncs `proot-source/` into build dir
3. `make` compiles with all features built-in (no patches)
4. Package step creates `.pkg.tar.xz`

## GitHub Actions Workflow

El workflow `build-proot.yml` está ACTIVO y se ejecuta en cada push que toque `packages/proot/**` o `proot-source/**`.

### `build-proot.yml`
- **Trigger**: Push a `packages/proot/**` o `proot-source/**`
- **Steps**: Clone → zram → restore cache → prepare → build → collect → release → save cache → artifact
- **Caching**: `~/.termux-build` mounted into Docker via `TERMUX_DOCKER_RUN_EXTRA_ARGS`
- **Cache key**: hash de `packages/proot/build.sh`, `packages/libtalloc/build.sh`, `packages/libandroid-shmem/build.sh`
- **Downloads**: `gh release download proot-latest -R Leonisaurov/proot-termux -p "*.pkg.tar.xz"`

### `docker_image.yml`
- Builds/pushes `ghcr.io/leonisaurov/package-builder:latest`

## Workflow Monitoring

Siempre que se lance un workflow, usar `gita notify` para esperar el resultado y leer los logs:

```bash
cd /data/data/com.termux/files/home/Develop/Patch/proot-termux
gita notify build-proot.yml 2>/dev/null | grep -E '(error|##\[error\]|mbind|Success)'
```

- Exit code 0 = éxito, 1 = falló, 2 = cancelado
- `gita notify` bloquea hasta que el workflow termina (o retorna inmediatamente si ya terminó)
- NO usar `timeout`

### Repositorios

Hay dos remotes configurados:

| Remote | URL | Estado |
|--------|-----|--------|
| `origin` | `https://github.com/Leonisaurov/proot-termux.git` | ✅ Activo (preferido) |
| `upstream` | `https://github.com/Leonisaurov/termux-packages.git` | ⛔ Deprecado |

**Siempre usar `origin` para pushes.** `upstream` está deprecado y ya no debe usarse.

```bash
git push origin master
```

## Development

1. Edit files in `proot-source/src/`
2. Bump `TERMUX_PKG_REVISION` in `packages/proot/build.sh` ← **IMPORTANTE: no olvidar**
3. Commit with descriptive message
4. Build local → `./scripts/build-native.sh -i`
5. Opcional: crear package → `./scripts/build-native.sh`

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

## Local Build (método actual)

### build-native.sh

El script `scripts/build-native.sh` automatiza la compilación nativa de proot en Termux.

```bash
# Uso básico — compila + empaqueta
./scripts/build-native.sh

# Compilar e instalar al sistema
./scripts/build-native.sh -i

# Clean build (make clean previo) + instalar
./scripts/build-native.sh -c -i

# Solo empaquetar desde binarios existentes
./scripts/build-native.sh --skip-build

# Configurar paralelismo (default: 2, seguro contra OOM)
./scripts/build-native.sh -j4
```

### Requisitos

Los paquetes Termux necesarios ya están instalados:
- `clang` (compilador, vía symlink `gcc`)
- `make`, `binutils`
- `libtalloc`, `libandroid-shmem` (librerías)
- `bsdtar`, `xz-utils` (para empaquetado)

### Output

| Tipo | Ruta |
|------|------|
| Binario | `$PREFIX/bin/proot` |
| Loaders | `$PREFIX/libexec/proot/{loader,loader32}` |
| Script | `$PREFIX/bin/termux-chroot` |
| Paquete | `proot-<version>-<revision>-aarch64.pkg.tar.xz` (en raíz del repo) |

### Notas

- **No requiere Docker.** Compila directamente con clang de Termux.
- **OOM-safe.** Por defecto usa `-j2`. En dispositivos con <4GB RAM, puedes forzar `-j1`.
- **No produce Release en GitHub.** Es solo para uso local/desarrollo.

## Pentest / Hardening Testing

El repo incluye un kit de pentest y un reporte de vulneraciones:

- **`pentest/`** — 5 programas C (p_fs, p_sys, p_proc, p_net, p_kernel) que prueban escapes del sandbox. Compilados con gcc dentro del rootfs alpine.
- **`vulneration-report.md`** — reporte completo de hallazgos y emulaciones implementadas.
- **Wrappers de prueba** (NO commitear): `alpine_rootfs` (escenario default) y `alpine_rootfs_hardened` (con isolation flags). Ambos usan el rootfs de proot-distro (`$PREFIX/var/lib/proot-distro/containers/alpine/rootfs`) y bindean `pentest/` → `/pentest`.

Uso:
```bash
./alpine_rootfs /pentest/p_sys           # escenario default
./alpine_rootfs_hardened /pentest/p_sys  # escenario hardened
```

Nota: el wrapper usa `env -i` que BORRA PROOT_VERBOSE — para debug verbose hay que inyectarla dentro del env del wrapper.

## Agent Configuration

The orchestrator agent lives at `~/.config/opencode/agent/orquestador.md`.

### Notes for the orchestrator

- Do not close the session after completing tasks unless explicitly
  asked by the user. The user decides when the session ends.
- Keep the working state available — the user may want to continue
  with additional features or adjustments.
- When in doubt, ask "¿Algo más?" instead of assuming completion.

## Isolation Features

### Available Flags

| Flag | What it blocks | Error returned |
|------|---------------|----------------|
| `--proc-isolated` | Host processes in /proc/, /proc/cpuinfo, meminfo, mountinfo, environ, netlink sockets, unshare, mount | ENOENT, filtered, ENOSYS, EACCES |
| `--ptrace-isolated` | ptrace, process_vm_readv/writev, kill to host PIDs | ESRCH |
| `--reboot-isolated` | reboot syscall | Re-exec's proot with same args (actual restart) |
| `--swap-isolated` | swapon, swapoff | ENOSYS |
| `--kexec-isolated` | kexec_load | 0 (void, éxito emulado) |
| `--ioport-isolated` | iopl, ioperm (ARM64 no-op) | 0 (success) |
| `--bpf-isolated` | bpf syscall | ENOSYS |
| `--perf-isolated` | perf_event_open | ENOENT |
| `--handle-isolated` | open_by_handle_at | EOPNOTSUPP |
| `--proc-isolation` | (legacy) Combines --proc-isolated + --ptrace-isolated |

> **Nota**: los flags de *resource limits* (`--cpu-limit`, `--mem-limit`, `--nice`, `--fd-limit`, `--proc-limit`, `--resource-isolated`) NO son flags de isolation: no bloquean syscalls, solo limitan recursos. Viven en su propia sección: [Resource Limits](#resource-limits---cpu-limit-y-familia).

### Philosophy: Emulate, Never Deny

NUNCA responder "Operation not permitted" (EPERM) o "Function not implemented" (ENOSYS)
cuando se puede EMULAR un resultado natural. Un proceso con `--change-id=0:0` cree que es
root — nunca debe ver un guardián negándole nada, solo errores naturales (ESRCH = "no
existe") o éxito emulado (0) sin efecto real.

| Vector | Resultado (hardened) | Mecanismo |
|--------|----------------------|-----------|
| ptrace a PID host | ESRCH | --ptrace-isolated |
| process_vm_readv/writev a PID host | ESRCH | --ptrace-isolated |
| pidfd_open a PID host | ESRCH | --ptrace-isolated (añadido en 97884c26) |
| kill a PID host | ESRCH | --ptrace-isolated |
| unshare(CLONE_NEWNS) | 0 (emulado, sin namespace real) | --proc-isolated void+0 |
| mount(tmpfs) | 0 (emulado, sin mount real) | --proc-isolated (built-in emula bindings) |
| socket(AF_NETLINK) | AF_UNIX fake (SO_DOMAIN=AF_UNIX) | --proc-isolated sustituye dominio |
| /proc/self/maps | guest-pure (loader eliminado + paths host→guest) | --proc-isolated (a85c0d0) |
| /proc/cpuinfo, meminfo, mountinfo, environ | ENOENT | --proc-isolated |
| /proc listado | solo pids propios | --proc-isolated (getdents filtrado) |
| bpf() | ENOSYS | --bpf-isolated |
| perf_event_open() | ENOENT | --perf-isolated |
| open_by_handle_at() | EOPNOTSUPP | --handle-isolated |
| kexec_load() | 0 (void) | --kexec-isolated |
| io_uring_setup() | ENOSYS | bloqueado por defecto |
| userfaultfd() | EPERM | kernel Android |
| chroot() | ENOENT | traducción de paths |
| /dev/mem, /dev/kmem, /dev/port | ENOENT | no existen en rootfs |
| mknod() | nodo creado pero open→ENOENT | kernel/SELinux (sin fix necesario) |

### ARM64 Limitations

- **Chained syscall mechanism** (socket → connect → dup3 → close) does not work on ARM64. The kernel does not re-read argument registers (x0-x5) when the PC is rewound via ptrace. Only x8 (syscall number) can be changed via NT_ARM_SYSTEM_CALL. (This was the reason `--allow-internet` was removed in f516c4b87f.)
- **reboot()** on Android is blocked by kernel seccomp filter before proot can intercept it. Works on standard Linux.
- **process_vm_writev** to other proot processes is currently allowed (not a host escape, but intra-sandbox).

### Commit History (recent)

- **Revision 9**: Resource Limits (`--cpu-limit`, `--single-core`, `--mem-limit`, `--nice`, `--fd-limit`, `--proc-limit`, `--resource-isolated`) — diseño híbrido host/guest

For full commit history: `git log --oneline`
