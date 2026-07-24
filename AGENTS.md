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

### Port Mapping

1. **Explicit mapping** (`-p host:container` / `--port host:container`):
   - Max 64 mappings, auto-finds free port if target occupied

2. **Auto-redirect** (`--protect-privileged-ports`):
   - Redirects `bind()` for ports < 1024 by +2000, finds next free port

### Bind Permissions

Extended `-b` syntax with optional access mode:
- `-b /host:/guest:ro` — read-only (writes return `EROFS`)
- `-b /host:/guest:wo` — write-only (reads return `EACCES`)
- `-b /host:/guest:rw` — read-write (default, backward compatible)

Access mode enforced in `translate_path()` after canonicalization.
For `open`/`openat`, reads `O_WRONLY`/`O_RDWR` flags from tracee registers.

### Merge Bind (`-m` / `--mbind`)

Copies rootfs directory contents to host before creating a regular bind.
Files from rootfs overwrite any existing files at host.

```
proot -m /real/run:/run
proot --mbind /real/run:/run
```

- Copies `$ROOTFS/run/*` to `/real/run/` recursively (so `-m` is read as "mbind")
- Overwrites files that already exist at host (no EEXIST)
- Extra host files (not in rootfs) are preserved
- On proot exit, copied files are automatically cleaned up via talloc destructor
- If `$ROOTFS/run` doesn't exist or get_root() returns NULL → skip copy, bind proceeds

### Modified Source Files
- `proot-source/src/path/binding.h` — `BindingAccess`, `BindingType` enums, `Binding` struct extended, `check_binding_access()`
- `proot-source/src/path/binding.c` — mbind_prepare, copy_recursive, mbind_cleanup, check_binding_access, all new_binding() callers updated
- `proot-source/src/cli/proot.h` — `-p`/`--port`/`--protect-privileged-ports`/`-m`/`--mbind` options
- `proot-source/src/cli/proot.c` — handle_option_mbind, handle_option_b with :ro/:wo/:rw
- `proot-source/src/extension/extension.h` — `PortMapping`, `PortSwitchConfig` structs
- `proot-source/src/extension/port_switch/port_switch.c` — security hardening
- `proot-source/src/path/path.c` — access mode check in translate_path()
- `proot-source/src/extension/fake_id0/chroot.c` — updated new_binding() call

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
| `packages/libtalloc/` | Dependency |
| `packages/libandroid-shmem/` | Dependency |
| `packages/termux-keyring/` | GPG keys for -I dep install |
| `packages/termux-licenses/` | License files (GPL-2.0, etc.) |
| `packages/python/` | Python version (build variable) |
| `packages/libllvm/` | LLVM version (build variable) |
| `packages/termux-elf-cleaner/` | ELF cleaner version (build variable) |
| `packages/libc++/` | C++ stdlib (dep of libllvm/elf-cleaner) |
| `proot-source/src/` | Modified proot source (repo-tracked) |
| `.github/workflows/build-proot.yml` | CI workflow |
| `scripts/buildorder.py` | Dependency resolver (patched for proot-only) |
| `repo.json` | Package directories list |

## Known Issues

- **`buildorder.py`**: Patched to skip missing deps when building a specific package (not full build). This is needed because some remaining packages (libllvm, python) declare dependencies on packages we removed.
- **`/data` mount**: Not used in CI. `-m` flag in `run-docker.sh` mounts `/data` from host which causes permission issues on GHA runners. Cache is mounted via `TERMUX_DOCKER_RUN_EXTRA_ARGS`.
- **First CI run**: ~5 min (seeds cache). Subsequent runs: ~20-30s with cache hit.

## Agent Configuration

The orchestrator agent lives at `~/.config/opencode/agent/orquestador.md`.

### Latest Update (2026-07-23)

Overhauled the orchestrator agent based on research of 6 sources (Anthropic, CrewAI, AutoGen, LangGraph, OpenAI, Andrew Ng) and installed skills:
- `addyosmani/agent-skills@planning-and-task-breakdown` (15.8K installs)
- `qodex-ai/ai-agent-skills@multi-agent-orchestration` (1.9K installs)

Key changes:
- Added `read: allow` and `skill: allow` permissions (was read-only before)
- Added explicit **Planning Phase** before delegation (read context → map deps → choose pattern)
- Added **4 decomposition patterns**: sequential, parallel, hierarchical, vertical slicing
- Added structured task templates with acceptance criteria + verification steps
- Added **3-level error recovery**: retry with context → report & continue → suggest alternative
- Added **checkpoints** between phases for large work
- Added **7 edge case handlers** with specific actions
- Validated with `validate-agent.sh` (0 errors, 1 warning on prompt length — accepted trade-off)

Restart opencode after pulling to activate changes.

## TODO

### System V IPC Completo (cross-instance)

Implementar los tres mecanismos de System V IPC (shm, sem, msg) con persistencia entre instancias de proot, usando un registro basado en archivos compartidos en tmpfs.

#### Componentes

1. **Shared Memory (PSSR)**: Ver diseño abajo en "Proot Shared Memory Registry"
2. **Semáforos (semget/semop/semctl)**: Implementar semáforos System V sobre futex(2) o file-locks (flock), con persistencia en archivo compartido:
   - `registry.dat` extendido con entrada de semáforos: `[key:8, tipo:SEM, valor:4, nsems:4, pid:4]`
   - `semop()` traducido a operaciones atómicas sobre el archivo con flock() como mutex
   - Soporte para SEM_UNDO (deshacer al morir el proceso) via registro de undo en archivo separado
3. **Message Queues (msgget/msgsnd/msgrcv/msgctl)**: Implementar colas de mensajes sobre archivos:
   - Cada cola es un archivo FIFO especial o un archivo regular con locking
   - `msgsnd()` escribe al final del archivo (con flock)
   - `msgrcv()` lee del principio (con flock)
   - Soporte para msgtyp (selección por tipo) mediante búsqueda secuencial

#### Arquitectura unificada

```
/data/local/tmp/proot-ipc/
├── registry.lock         ← flock(2) global
├── shm.dat               ← registro de segmentos shm
├── sem.dat               ← registro de semáforos (key, nsems, permisos)
├── msg.dat               ← registro de colas (key, permisos)
├── shm_segments/
│   └── {key}.shm         ← archivo de respaldo del segmento
├── sem_values/
│   └── {key}.sem         ← valores actuales del semáforo
└── msg_queues/
    └── {key}.msg         ← cola de mensajes (formato binario: [tipo:4, len:4, data...])
```

#### Flujo General

- **INIT**: Al iniciar el helper, verificar que `/data/local/tmp/proot-ipc/` exista, crearlo si no
- **LOCK**: flock(registry.lock, LOCK_EX) para operaciones de escritura, LOCK_SH para lectura
- **OP**: Leer/escribir el archivo correspondiente
- **UNLOCK**: flock(registry.lock, LOCK_UN)
- **CLEANUP**: Al cerrar proot, el helper limpia entries huerfanas (procesos que ya no existen)

#### Implementación en helper (sin threads)

Todo se implementa en `sysvipc_shm_helper_main()` agregando nuevos opcodes:

```c
enum SysVIpcHelperOp {
    SHM_ALLOC_KEY,     // shmget key-based
    SHM_LOOKUP,        // shmget lookup
    SHM_RMID,          // shmctl IPC_RMID
    SEM_GET,           // semget
    SEM_OP,            // semop (puede ser múltiple)
    SEM_CTL,           // semctl
    MSG_GET,           // msgget
    MSG_SND,           // msgsnd
    MSG_RCV,           // msgrcv
    MSG_CTL,           // msgctl
};
```

#### Ventajas sobre implementación actual

| Aspecto | Actual (en memoria de proot) | Propuesta (archivos compartidos) |
|---------|------------------------------|----------------------------------|
| Persistencia | Solo mientras proot vive | Hasta reinicio del host |
| Cross-instance | ❌ No | ✅ flock() + archivos |
| Semáforos | Solo dentro de un proot | Cross-instance |
| Message queues | Solo dentro de un proot | Cross-instance |
| Threads | Depende del backend | 0 threads |
| Deadlock | Posible (libandroid) | Imposible |

#### Archivos a modificar

- `proot-source/src/extension/sysvipc/sysvipc_shm.c`: PSSR (ya documentado)
- `proot-source/src/extension/sysvipc/sysvipc_sem.c`: Agregar helper ops para semáforos persistentes
- `proot-source/src/extension/sysvipc/sysvipc_msg.c`: Agregar helper ops para colas persistentes
- `proot-source/src/extension/sysvipc/sysvipc.c`: Modificar dispatch para nuevo helper protocol
- `proot-source/src/extension/sysvipc/sysvipc_internal.h`: Definir estructura del registry, opcodes
- `packages/proot/build.sh`: Crear /data/local/tmp/proot-ipc/ en post-install

### Proot Shared Memory Registry (PSSR)

Implementar un registro compartido de segmentos SysV IPC shared memory que permita a múltiples instancias de proot compartir memoria por clave (key_t), recuperando la funcionalidad que libandroid-shmem intentaba proveer pero sin su deadlock interno.

#### Diseño: Registry basado en archivos compartidos

En vez de usar libandroid-shmem (que tenía threads + socket IPC interno → deadlock ABBA), usar un archivo en tmpfs accesible por todas las instancias de proot:

```
/data/local/tmp/proot-shm/
├── registry.lock        ← flock(2) para exclusión mutua entre instancias
├── registry.dat         ← binario: entries de [key:8, dev:8, ino:8, size:8, gen:4]
└── segments/
    └── {dev}_{ino}.shm  ← archivo respaldo del segmento (ftruncate al tamaño)
```

#### Flujo

1. **shmget(key, size, IPC_CREAT)**:
   - Helper recibe SHMHELPER_ALLOC con key
   - `flock(registry.lock, LOCK_EX)`
   - Lee `registry.dat`, busca key
   - Si existe: verifica size, retorna (dev, ino) = shmid
   - Si no existe: crea `segments/{dev}_{ino}.shm`, ftruncate(size), agrega a registry
   - `flock(registry.lock, LOCK_UN)`
   - Retorna shmid que codifica (dev, ino, generation)

2. **shmget(key, 0, 0)** (buscar existente):
   - Helper recibe SHMHELPER_LOOKUP con key
   - `flock(registry.lock, LOCK_SH)`
   - Lee `registry.dat`, busca key
   - Si existe: retorna shmid
   - Si no: retorna -ENOENT
   - `flock(registry.lock, LOCK_UN)`

3. **shmat(shmid)**:
   - Decodifica (dev, ino) del shmid
   - Helper abre `segments/{dev}_{ino}.shm` → fd
   - fd se pasa al tracee vía SCM_RIGHTS (DISTRIBUTE handler existente)
   - Tracee mmap(MAP_SHARED, fd, ...)

4. **shmctl(IPC_RMID)**:
   - Helper cierra fd local
   - `flock(registry.lock, LOCK_EX)`
   - Elimina entrada de `registry.dat`
   - Unlink `segments/{dev}_{ino}.shm`
   - `flock(registry.lock, LOCK_UN)`

#### Formato de shmid

Codificar (dev, ino, generation) en 32 bits para compatibilidad con SysV IPC:

```
bits 0-11:  inode & 0xFFF     (índice dentro del segmento)
bits 12-27: generation & 0xFFFF
bits 28-31: 0 (reservado)
```

El helper mantiene una tabla local (dev, ino) → fd para evitar re-abrir en cada shmat.

#### Ventajas sobre libandroid-shmem

| Aspecto | libandroid-shmem | PSSR (propuesto) |
|---------|-----------------|------------------|
| Threads | 1 listener thread | 0 threads |
| Sockets | socketpair interno | 0 sockets extra |
| Deadlock | ABBA clásico | Imposible (sin threads) |
| Cross-instance | ❌ No soportado | ✅ Archivo compartido |
| Persistencia | Solo mientras helper vive | Hasta reinicio del host |
| Concurrencia | Un solo proceso | flock() entre instancias |

#### Archivos a modificar

- `proot-source/src/extension/sysvipc/sysvipc_shm.c`: Agregar nuevos ops SHMHELPER_ALLOC_KEY, SHMHELPER_LOOKUP, SHMHELPER_RMID_KEY; modificar handlers en helper_main()
- `proot-source/src/extension/sysvipc/sysvipc_internal.h`: Agregar defines para registry path, formato de entry
- `packages/proot/build.sh`: Agregar creación de /data/local/tmp/proot-shm/ en termux_step_post_make_install()

### Merge bind (getdents64)

Implementar true overlay-style merge bind donde archivos tanto del host como del rootfs sean visibles simultáneamente, no solo copiados. Requiere interceptar `getdents64` para mergear directorios (similar a hidden_files extension).
