# AGENTS.md — proot-termux (proot-only fork)

## Project Overview

Fork proot-only: cross-compila proot para Android aarch64 (NDK r29 vía Docker + CI GitHub Actions). La fuente vive en `proot-source/` — sin parches, sin downloads. Rama `master`. Security hardening: fases A-E completadas (ver FIXES.md).

## Build & CI

### Comandos clave

```bash
# Build CI (Docker, termux-packages pipeline)
./scripts/run-docker.sh ./build-package.sh -I -a aarch64 --format pacman proot

# Build local en Termux (clang, sin Docker)
./scripts/build-native.sh            # compila + empaqueta
./scripts/build-native.sh -i         # compila + instala + empaqueta
./scripts/build-native.sh -c -i      # clean build + instala
./scripts/build-native.sh --skip-build   # solo empaqueta (binarios existentes)
./scripts/build-native.sh -j4        # paralelismo (default -j2, OOM-safe; -j1 en <4GB RAM)
```

Output local: `$PREFIX/bin/proot`, loaders en `$PREFIX/libexec/proot/{loader,loader32}`, script `$PREFIX/bin/termux-chroot`, paquete `proot-<ver>-<rev>-aarch64.pkg.tar.xz` en la raíz. Dependencias: `libandroid-shmem`, `libtalloc` (se construyen solas con `-I`).

### Workflows GitHub Actions

| Workflow | Estado | Trigger |
|----------|--------|---------|
| `build-proot.yml` | ✅ ACTIVO | Push a `packages/proot/**` o `proot-source/**` + `workflow_dispatch` |
| `docker_image.yml` | ⛔ DESHABILITADO (`if: false` en el job) | — |

`build-proot.yml` steps: clone → zram → restore cache → prepare → build → collect → release → save cache → artifact. Caché: `~/.termux-build` montada en Docker vía `TERMUX_DOCKER_RUN_EXTRA_ARGS` (cache key: hash de build.sh de proot/libtalloc/libandroid-shmem). Primera corrida ~5 min (seeds cache), subsecuentes ~20-30s. Download de release previa: `gh release download proot-latest -R Leonisaurov/proot-termux -p "*.pkg.tar.xz"`.

### Monitoreo CI (gita)

```bash
gita notify build-proot.yml 2>/dev/null | grep -E '(error|##\[error\]|mbind|Success)'
```

- Exit: 0=éxito, 1=falló, 2=cancelado. `gita notify` bloquea hasta que el workflow termina (o retorna al instante si ya terminó). NO usar `timeout`, NO streaming.

### ⚠️ Inconsistencia de versión (documentada, no resuelta)

| Fuente | VERSION | REVISION |
|--------|---------|----------|
| `packages/proot/build.sh` | `5.1.107.89` | `27` |
| `scripts/build-native.sh` | `5.1.107.87` | `16` |

Discrepancia verificada entre ambos. NO asumas cuál es canónica ni la sincronices unilateralmente; usa la del archivo que edites.

## Critical Rules

### ⚠️ SIEMPRE bump TERMUX_PKG_REVISION antes de commit

Cada vez que toques `proot-source/src/` o `packages/proot/`, incrementa `TERMUX_PKG_REVISION` en `packages/proot/build.sh` ANTES de commit. Sin esto el CI no se dispara bien y los usuarios no reciben la actualización. Es el error más común. REVISION actual: **27**.

### Orden de commit (secuencial, no omitir pasos)

1. Editas código (`proot-source/src/` o `packages/proot/`)
2. Bump `TERMUX_PKG_REVISION` en `packages/proot/build.sh`
3. `git add -A && git commit -m "<type>(<scope>): <summary>"`
4. `git push origin master`
5. `gita notify build-proot.yml 2>/dev/null | grep -E '(error|##\[error\]|mbind|Success)'`

Convención: `<type>(<scope>): <summary>` — types: `fix`, `enhance`, `chore`, `ci`, `test`.

### Repositorios

| Remote | URL | Estado |
|--------|-----|--------|
| `origin` | `https://github.com/Leonisaurov/proot-termux.git` | ✅ preferido, único para push |
| `upstream` | `https://github.com/termux/proot.git` | ⛔ deprecado, no usar |

### Gotchas del entorno (Termux)

- Usa `$TMPDIR`, NUNCA `/tmp`.
- Builds locales: OOM-safe por defecto (`-j2`); usa `-j1` en dispositivos <4GB RAM.
- `gita` es la herramienta de monitoreo CI (ver Monitoreo CI); no uses `timeout` con ella.
- **proot necesita `env -i`** para ejecutar binarios en rootfs Alpine — el entorno heredado causa fallos en execve. Siempre limpiar env al invocar proot directamente.

## Security Hardening (Fases A-E)

Resumen en `FIXES.md`. Commits por fase:

| Fase | Estado | Commits | REV |
|------|--------|---------|-----|
| A — Aislamiento P0 | ✅ | `573f4cb8d9`, `3b98197d8a` | 19-20 |
| B — Leaks y fds | ✅ | `ec308f5425` | 21 |
| C — Aislamiento P1 | ✅ | `6d3a556007`, `e141f86c56` | 22 |
| D — Rendimiento | ✅ | `e30bdb5b43`, D5 hash | 25 |
| E — Resto | ✅ | `89e2598828`, E2-E8 | 27 |

### Nuevos CLI flags (Fase C)

- `--recommended-etc-rw` — restores legacy RW for /etc/ binds (default: :ro)
- `--fake-permissions` — emulate permissions without real chmod (no-op in override_permissions + access() emulated)

### Nuevas funciones (Fase C)

- `insort_binding3_with_mode()` — like `insort_binding3()` but accepts `BindingAccess` parameter (wrapper, no signature change)

### Pentest (Fase B+C+D+E)

```bash
# Smoke tests
pentest/test_b1_b2_b7.sh      # B1/B2/B7: 20 concurrent --exec clients
pentest/test_b3.sh             # B3: fd_map stale sweep
pentest/test_b5.sh             # B5: SP integrity bind/connect loop
pentest/test_b6.sh             # B6: bridge children killed on exit
pentest/test_b8.sh             # B8: talloc leak verification
pentest/test_phase_c.sh        # C2-C7: MS_RDONLY, /etc :ro, proc, PEERCRED, fake-perms
pentest/test_d4_e1_e3_e6.sh   # D4 socket, E1 renameat2, E3 uname, E6 mknod (39 tests)
```

Resultados: B=14/14 PASS, C=6/6 PASS, D4/E1/E3/E6=39/39 PASS. Reportes en `pentest/results/`.

## Arquitectura del fork (no obvia desde los nombres de archivo)

### Extensiones reales (12) en `proot-source/src/extension/`

`virtual_net`, `resource_limit`, `proc_isolation`, `fake_id0`, `kompat`, `port_switch`, `sysvipc`, `link2symlink`, `hidden_files`, `mountinfo`, `fix_symlink_size`, `ashmem_memfd`.

### Virtual Networking (`--proxy NAME`)

Red virtual con **Abstract Unix Domain Sockets** (sin TCP/IP real): `socket(AF_INET/AF_INET6)`→`AF_UNIX`, `bind/connect` traducidos a `@proot-vnet-{name}-{port}-{token}`; `getsockname/getpeername` emulan loopback (`127.0.0.1`/`::1`); `setsockopt(IPPROTO_TCP)` voided. Registry compartido para multi-instancia: `$PREFIX/usr/tmp/proot-net/{name}/registry.lock` (flock; magic `0x50524F4E` = **"PRON"**; entradas 512 × 116 B, ~59 KB). `-p HOST:VIRTUAL` lanza helper (`--vnp-helper NAME`, `virtual_net_helper.c` 422 líneas) que abre TCP real y hace bridge TCP→Unix.

| Escenario | Resultado |
|-----------|-----------|
| Mismo `--proxy NAME` | ✅ conecta (socket abstracto) |
| Diferente NAME / sin `--proxy` / host nativo | ❌ bloqueado |

### Supervise & Exec (`--supervise`, `--exec <PID> <cmd>`)

`--supervise` cambia el event loop a `signalfd`+`poll()` y escucha en `@proot-exec-<PID>`. `--exec` (invocación proot separada) conecta y ejecuta un comando dentro del mismo contexto (rootfs, binds, proxy). Logs: `$PREFIX/usr/tmp/proot-exit-<PID>.log` (`process 'x' exited with status N / killed by signal N`). Sin `--supervise` el loop es 100% idéntico al upstream (cero overhead).

**Comportamiento real (verificado)**: al salir el root tracee el supervisor cierra `ctl_fd` y se apaga salvo clients `--exec` pendientes — no queda vivo en background. SIGTERM/SIGINT son SIG_IGN; solo SIGKILL o la salida natural del root tracee lo termina.

### Resource Limits (`--cpu-limit` y familia) — diseño híbrido

| Flag | Efecto | Lado |
|------|--------|------|
| `--cpu-limit N` | `sched_setaffinity` a N cores (host) + guest percibe N cores | host + guest |
| `--single-core` | Alias de `--cpu-limit 1` | host + guest |
| `--mem-limit N[KMG]` | RLIMIT_AS AL GUEST (post-exec) — el host NO se limita | guest |
| `--nice N` | `setpriority` (0..19) | host |
| `--fd-limit N` | `prlimit64(RLIMIT_NOFILE)` (mín 32) | host |
| `--proc-limit N` | Gate de fork/clone/vfork: `-EAGAIN` al superar N procesos de ESTE proot | guest |
| `--resource-isolated` | Combo: 1 core + nice 10 | host + guest |

- **Host-side**: `resource_config_apply()` (llamado desde `main()`) aplica affinity → nice → prlimits en orden; los tracees heredan vía fork/exec.
- **Guest-side** (extensión `resource_limit`, callback `rlimit_callback`): `--mem-limit` aplica `prlimit64(pid, RLIMIT_AS)` a CADA tracee tras su execve (`apply_mem_limit_to_tracee()` en `execve/exit.c`) — bionic proot tiene VSZ ~10 GiB y un RLIMIT_AS host-side lo crashearía. `--proc-limit` intercepta `PR_clone`/`PR_clone3`/`PR_fork`/`PR_vfork` en ENTER: cuenta tracees vivos (`!tracee->terminated`) y si count ≥ N responde `-EAGAIN` (semántica RLIMIT_NPROC; por-proceso, no por uid). Guest percibe N cores reescribiendo `sched_getaffinity` en EXIT.
- Validaciones: `--mem-limit` mín 16 MiB; `--cpu-limit` ≤ cores reales; `--nice` 0..19; `--fd-limit` ≥ 32; `--proc-limit` ≥ 1. "Last option wins" con `--resource-isolated`. Fallo de la extensión guest = no fatal (solo avisa).
- `--supervise`/`--exec`: los tracees nuevos reciben `--mem-limit`; los flags del cliente `--exec` NO se propagan (gana el supervisor). Gap conocido: la creación inicial de tracees del supervisor no pasa el gate de `--proc-limit`.

### proc_isolation (extensión `hpc_callback`)

Filtra /proc (solo pids propios vía getdents, cpuinfo/meminfo/mountinfo/environ/version/uptime/stat/loadavg/kallsyms/slabinfo/zoneinfo/iomem/interrupts/modules/cmdline/misc→ENOENT, maps guest-pure con paths host→guest), ptrace/process_vm_readv/writev/kill a PIDs host→ESRCH, `pidfd_open` a PIDs host→ESRCH (ISOLATE_PROC), `socket(AF_NETLINK)`→AF_UNIX fake, unshare(CLONE_NEWNS)/mount→0 emulados. Lazy maps_fd detection en read handler.

### Filosofía: Emulate, Never Deny

NUNCA EPERM/ENOSYS cuando se puede emular un resultado natural. Un guest con `--change-id=0:0` cree que es root: solo debe ver errores naturales (ESRCH = "no existe") o éxito emulado (0) sin efecto real. Vectores clave: ptrace→ESRCH, unshare→0, mount(tmpfs)→0, bpf()→ENOSYS, perf_event_open→ENOENT, open_by_handle_at→EOPNOTSUPP, kexec_load→0 (void), io_uring_setup→ENOSYS, chroot→ENOENT.

### ARM64 Limitations

- **Chained syscall mechanism** (socket→connect→dup3→close) NO funciona en ARM64: el kernel no re-lee x0-x5 al rebobinar el PC vía ptrace; solo x8 (número) es cambiable (NT_ARM_SYSTEM_CALL). Razón de la eliminación de `--allow-internet` (f516c4b87f).
- **reboot()** bloqueado por el seccomp del kernel Android antes de que proot lo intercepte (solo Linux estándar; afecta a `--reboot-isolated` y combinaciones).
- `process_vm_writev` entre procesos proot queda permitido (intra-sandbox, no host escape).

### Overhead: cero overhead sin flags — commit f7b618772a

Principio rector: **sin flags → cero overhead**.
- `pipe_shadow.c`: latch estático `shadow_active` (early-returns) — elimina readlink por close y el scan de slots sin pipes. Latch permanente por diseño.
- `tracee/tracee.h` + `syscall/enter.c`: `fake_netlink_reply` pasó de `uint8_t[8192]` a puntero talloc perezoso (8 KB × N tracees). OJO: usar la macro `MAX_FAKE_NETLINK_REPLY`, NUNCA `sizeof(tracee->fake_netlink_reply)` (daría 8).
- vnet registry cache: `dirs_ready` cacheado (name-aware) + fast-path por `generation`+`count` leyendo solo el header (~12 B); re-lee ~59 KB solo si cambian. Clamp de `count > 512`. Cache estático por-proceso → riesgo documentado si un día un proceso manejara múltiples proxies.
- **D1 BPF**: sysnums sorted copy para construction (qsort + talloc, preparado para binary search futuro).
- **D2 ioctl**: FILTER_SYSEXIT removido del filtro base; FICLONE detectado dinámicamente en enter.c (`tracee->sysexit_pending = true`).
- **D3 faccessat2**: FILTER_SYSEXIT removido (no hay exit handler que lo necesite).
- **E2 registry fd cache**: fd para LOCK_SH reutilizado across operaciones (evita open+flock+close cycle). LOCK_EX re-abre para writes.
- **E8 fake_netlink fast-path**: `is_fake_netlink_fd()` retorna false inmediatamente si `fake_netlink_fds_count == 0` (skip scan).

### Fix de accept ARM64 — commit b1b775073b

Kernel Android/ARM64 NO implementa `accept()` (202, deprecada) → ENOSYS. Fix en 3 partes:
1. `syscall/sysnums-arm64.h:206`: `[ 202 ] = PR_accept` (sin él no entra al filtro BPF).
2. `syscall/exit.c:139-147`: retry `accept→accept4(242)` con flags=0 en ENOSYS (`fix_and_restart_enosys_syscall`); el rewrite ya existía en `tracee/seccomp.c:195-196`.
3. `virtual_net.c` (SYSCALL_EXIT_END): skip del 1er EXIT del retry (`ORIGINAL==PR_accept` + `restore_original_regs_after_seccomp_event`) para no escribir fake sockaddr con un newfd incorrecto.

**NOTAS**: el flags de accept4 es **SYSARG_4** (x3); NUNCA tocar SYSARG_3 (x2 = puntero addrlen). El EXIT despacha con ORIGINAL → tras el retry ve `PR_accept4`, sin loop. accept4 flags=0 ≡ accept. kompat nunca dispara en Android (host > 2.6.28).

### Original Features (pre-virtual_net)

Port mapping (`-p host:container`, máx 64, auto-puerto libre), auto-redirect de puertos <1024 (+2000, `--protect-privileged-ports`), bind permissions (`-b` con `:ro/:wo/:rw`), merge bind (`-m`/`--mbind`).

## Modified Source Files (referencia)

| Archivo | Qué se cambió |
|---------|---------------|
| `extension/virtual_net/` (5: .c/h/internal.h/helper.c/helper.h) | Red virtual, registry fd cache (E2), helper `--vnp-helper` |
| `extension/resource_limit/` (3: .c/h/internal.h) | sched_getaffinity fake + gate fork/clone |
| `extension/proc_isolation/` (2: .c/h) | `hpc_callback`: /proc, ptrace, kill, netlink, maps. E5 pidfd_open→ISOLATE_PROC, E7 lazy maps_fd |
| `extension/fake_id0/fake_id0.c` | `--fake-permissions` (override_permissions no-op + access emulated) |
| `supervise/` (2: .c/h) | `--supervise`/`--exec`, signalfd+poll, socket abstracto, SO_PEERCRED |
| `cli/proot.c` (1004+) | handlers `--proxy`/`-p`, `resource_config`, `--recommended-etc-rw`, `--fake-permissions`, E4 -q host-rootfs doc |
| `cli/proot.h` (614+) | opciones CLI propias, declaraciones |
| `cli/cli.c` (694+) | dispatch `--vnp-helper`/`--exec`, hook `resource_config_apply()`, D5 `tracee_hash_update` |
| `extension/extension.h` | `vnp_callback`, `rlimit_callback`, `hpc_callback`, `fake_id0_*` |
| `path/binding.c` | `insort_binding3_with_mode()` wrapper |
| `path/binding.h` | `insort_binding3_with_mode()` declaration |
| `GNUmakefile` (318) | objs virtual_net, virtual_net_helper, resource_limit, proc_isolation |
| `tracee/event.c` (976) | event loop poll para supervise (idéntico sin flag) |
| `tracee/tracee.c` | D5 hash table (insert/remove/update), `get_tracee` O(1), `free_terminated_tracees` hash cleanup |
| `tracee/tracee.h` (399+) | campo `supervise`, `fake_netlink_reply` como puntero, D5 `hash_next` |
| `syscall/seccomp.c` | D1 sorted sysnums, D2 ioctl no-sysexit, D3 faccessat2 no-sysexit |
| `syscall/enter.c` | `fake_netlink_reply` lazy talloc, D2 FICLONE dynamic detection, E8 fake_netlink fast-path |
| `syscall/sysnums-arm64.h` | `[ 202 ] = PR_accept` |
| `syscall/exit.c` | retry accept→accept4 en ENOSYS |
| `syscall/pipe_shadow.c` | latch `shadow_active` |
| `execve/exit.c` | `apply_mem_limit_to_tracee()` post-exec |
| `tracee/seccomp.c` | rewrite accept→accept4 + `SYSARG_4`=0 |

## How the Build Works (CI)

`TERMUX_PKG_SKIP_SRC_EXTRACT=true` salta descarga → `termux_step_pre_configure()` rsync de `proot-source/` al build dir → `make` compila con todas las features built-in (sin parches) → package step crea `.pkg.tar.xz`.

## Known Issues / Gotchas

- **`repo.json` declara `pkg_format: debian` pero el workflow usa `--format pacman`** — inconsistencia verificada, no "arreglar" (el pipeline CI manda).
- **`buildorder.py`**: parcheado para saltar deps ausentes (libllvm, python declaran deps removidas).
- **`/data` mount**: no usar en CI (`-m` en run-docker.sh causa permisos en runners GHA); la caché se monta vía `TERMUX_DOCKER_RUN_EXTRA_ARGS`.
- **Registry cleanup**: entradas stale de `registry.lock` no se limpian solas (no afectan). Limpieza manual: borrar `$PREFIX/usr/tmp/proot-net/`.
- **Tamaños reales** (para estimar diffs): `virtual_net.c`=1144, `virtual_net_helper.c`=422, `cli/proot.c`=1004, `cli/proot.h`=614, `cli/cli.c`=694, `GNUmakefile`=318, `tracee/event.c`=976, `tracee/tracee.h`=399.
- **D5 hash table**: ✅ implementada. Hash estático 256 buckets + `tracee_hash_update()` para PID change en cli.c. Sin talloc lifecycle issues.
- **D6 binding cache**: SKIP — riesgo de dangling pointers supera ganancia (3-10 bindings típicas, scan lineal es efectivamente O(1)).
- **E items**: E1-E8 todos RESUELTO (REV 24-27). Ver FIXES.md §7 para detalles.
- **proot necesita `env -i`** al ejecutar en rootfs Alpine — el entorno heredado causa execve failures. El wrapper `alpine_rootfs` ya lo hace correctamente.

## Pentest / Hardening Testing

`pentest/` (6 programas C: p_fs, p_sys, p_proc, p_net, p_kernel, memtest.c) — resultados en `pentest/results/*.txt` (10 archivos A/B + B fixes + C fixes). `vulneration-report.md` (raíz del repo, refiere `pentest/results/`). `REPORT-FASE-B-PENTEST.md` (reporte detallado Fase B). Wrappers (NO commitear): `alpine_rootfs` y `alpine_rootfs_hardened` (usan el rootfs de proot-distro y bindean `pentest/`→`/pentest`). Uso: `./alpine_rootfs /pentest/p_sys`. OJO: los wrappers usan `env -i` que borra `PROOT_VERBOSE` — inyectarla dentro del env del wrapper para debug.

### Scripts de pentest (Fase B+C+D+E)

```bash
pentest/test_b1_b2_b7.sh      # 20 clientes --exec concurrentes
pentest/test_b3.sh             # fd_map stale sweep
pentest/test_b5.sh             # SP integrity bind/connect loop
pentest/test_b6.sh             # bridge children killed on exit
pentest/test_b8.sh             # talloc leak verification
pentest/test_phase_c.sh        # C2-C7: MS_RDONLY, /etc :ro, proc, PEERCRED, fake-perms
pentest/test_d4_e1_e3_e6.sh   # D4 socket, E1 renameat2, E3 uname, E6 mknod (39 tests)
```

## Agent Configuration

Orquestador: `~/.config/opencode/agent/orquestador.md`. No cerrar la sesión tras completar tareas salvo petición explícita; mantener el estado de trabajo; ante duda preguntar "¿Algo más?" en vez de asumir finalización.
