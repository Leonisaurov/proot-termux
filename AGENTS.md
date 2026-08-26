# AGENTS.md — proot-termux (proot-only fork)

## Layout vigente

La organización actual usa `bin/` para entrypoints, `tests/<tema>/` para
regresiones y wrappers de rootfs, `reports/` para resultados y `docs/` para documentación. Ejecuta
los launchers desde `./bin/` y la batería desde `./tests/run.sh`; cualquier
referencia histórica a `pentest/` en las secciones de fases antiguas no es una
ruta operativa vigente.

## Project Overview

Fork proot-only: cross-compila proot para Android aarch64 (NDK r29 vía Docker + CI GitHub Actions). La fuente vive en `proot-source/src/` — sin parches, sin downloads. Rama `master`. Security hardening: fases A-F completadas (ver `docs/security/FIXES.md`).

## Build & CI

### Flujo obligatorio de trabajo

1. Identifica la capa: proot, `termux-isolated`, `control-api` o harness.
2. Elige el modo de rutas antes de escribir comandos: `--termux-paths`
   conserva `$PREFIX` como ruta guest; el modo rootfs usa `/usr`, `/bin`,
   `/etc` y `/home`.
3. Separa rutas host y guest. `PROOT_TMP_DIR` y `PROOT_RUNTIME_DIR` deben ser
   rutas host escribibles; `TMPDIR` dentro del guest puede ser otra ruta.
4. Valida primero el caso mínimo sin protocolo, después con `control-fd`, y
   finalmente con un harness que atienda eventos durante toda la vida del
   proceso.
5. Ante un fallo, reproduce el caso mínimo y localiza la capa responsable
   antes de editar otra capa.
6. No declares terminado mientras fallen la build o las regresiones relevantes.

### Ownership y rutas

| Capa | Hace | No asume |
|---|---|---|
| proot | Traducción, bindings explícitos, aislamiento y protocolo opt-in | Rutas Android/Termux, binds del consumidor o decisiones del harness |
| `termux-isolated` | Lanzamiento concreto de proot en Termux | Controlar el `control-fd` o exponer almacenamiento |
| `control-api` | Codec, launcher y contrato del control fd | Presets internos de proot |
| harness | Lee cada evento y responde mientras proot vive | Que proot decida por él |

Con `--termux-paths`, usa `$PREFIX/bin/sh` y `$PREFIX/etc`; con rootfs, usa
`/bin/sh` y `/etc`. No mezcles ambos espacios en un mismo test. `/data` y
`/storage` no son rutas guest genéricas; `termux-isolated` no expone
almacenamiento.

### Comandos clave

```bash
# Build CI (Docker, termux-packages pipeline)
./ci/termux/scripts/run-docker.sh ./ci/termux/build-package.sh -I -a aarch64 --format pacman proot

# Build local en Termux (clang, sin Docker)
./scripts/build-native.sh            # compila + empaqueta
./scripts/build-native.sh -i         # compila + instala + empaqueta
./scripts/build-native.sh -c -i      # clean build + instala
./scripts/build-native.sh --skip-build   # solo empaqueta (binarios existentes)
./scripts/build-native.sh -j 4      # paralelismo (default 2; -j 1 en <4GB RAM)
```

Para builds locales siempre se usa `scripts/build-native.sh`; no se ejecuta
`make` directamente. La opción de paralelismo recibe un argumento separado:
`-j 2` o `--jobs 2` (no existe la forma `-j2`).
`--skip-package` todavía compila e instala; únicamente omite el paquete.

Output local: `$PREFIX/bin/proot`, loaders en `$PREFIX/libexec/proot/{loader,loader32}`, script `$PREFIX/bin/termux-chroot`, paquete en `artifacts/packages/proot-<ver>-<rev>-aarch64.pkg.tar.xz`. Dependencias: `libandroid-shmem`, `libtalloc` (se construyen solas con `-I`).

Para compilar e instalar localmente se debe usar el flujo oficial, sin
reemplazar manualmente `$PREFIX/bin/proot`:

```bash
./scripts/build-native.sh -i
```

Las pruebas del modo aislado se ejecutan mediante `./bin/termux-isolated`. Con
`--termux-paths`, las rutas `/data/data/com.termux/...` son rutas guest
intencionales; sin esa opción, un rootfs debe mostrar sus rutas guest (`/usr`,
`/home`, etc.) y no rutas Android del host.

### Workflows GitHub Actions

| Workflow | Estado | Trigger |
|----------|--------|---------|
| `build-proot.yml` | ✅ ACTIVO | Push a `ci/termux/packages/proot/**` o `proot-source/**` + `workflow_dispatch` |
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
| `ci/termux/packages/proot/build.sh` | `5.1.107.89` | `46` |
| `scripts/build-native.sh` | `5.1.107.87` | `16` |

Discrepancia verificada entre ambos. NO asumas cuál es canónica ni la sincronices unilateralmente; usa la del archivo que edites.

## Critical Rules

### Preflight y ejecución local obligatorios

- Antes de compilar, empaquetar, instalar o ejecutar una batería de tests, analiza el objetivo, el flujo exacto, los scripts, workflows, parches, toolchain y documentación aplicables.
- Ejecuta un preflight sin compilación: `uname -m`, API/NDK, herramientas disponibles, espacio libre, RAM, red/caché, pins de versión, `TMPDIR` y permisos de salida.
- Cualquier build, test, empaquetado, instalación o validación que ejecute binarios debe lanzarse con `sandbox_permissions: "require_escalated"`, para ejecutarse en el entorno local de Termux y no dentro del sandbox del agente.
- No inicies una compilación larga por intuición. Si el preflight falla, corrige o reporta la causa antes de consumir tiempo de build.
- Tras un fallo, captura el error completo, clasifícalo como fuente, toolchain, entorno, red, memoria o CI, y determina la causa raíz antes de cambiar código o recompilar.
- No uses ciclos apresurados de compilar–fallar–parchar. El cambio debe ser el mínimo que resuelva la causa demostrada.
- No declares una tarea terminada mientras existan errores conocidos. Cuando una
  validación falle, vuelve a analizar el diseño y resuelve la causa raíz antes
  de continuar con otra iteración o presentar el trabajo como completo.
- Distingue siempre la ruta guest de la ruta host. Con `--termux-paths` el
  guest usa el prefijo real de Termux (`$PREFIX`, normalmente
  `/data/data/com.termux/files/usr`); `/etc`, `/bin` y `/home` no se pueden
  asumir como rutas guest. En una jerarquía rootfs sí deben usarse las rutas
  Linux del rootfs. Los tests deben construir sus objetivos según el modo que
  realmente ejecutan.
- Toda regresión nueva debe convertirse primero en un archivo revisable
  `tests/<tema>/test_*.sh` (o en el harness equivalente), con preflight, fixtures
  temporales bajo `$TMPDIR`, separación explícita host/guest y cleanup. No se
  valida una feature nueva improvisando comandos sueltos directamente en la
  terminal: se revisa el script y luego se ejecuta ese archivo con escalada.
- Si el comportamiento pertenece a `termux-isolated`, el test debe invocar
  `./bin/termux-isolated` y ejercer sus opciones reales (`--termux-paths`, rootfs,
  `--with-storage`, `--rw-dir`, etc.). Los casos que comparan vistas deben
  cubrir cada modo relevante; no se sustituye el launcher por una invocación
  directa de proot.
- El modo interactivo sin comando debe lanzar la shell ejecutable indicada por
  `$SHELL` dentro del prefijo Termux, traduciendo `$PREFIX/...` a `/usr/...`
  en rootfs; bash es únicamente el fallback. Las pruebas de esta ruta deben
  usar un pseudo-terminal, no solo stdin por pipe.
- Conserva `target/`, cachés y `sccache` compatibles; no uses `cargo clean` ni borres cachés sin una causa comprobada.

### Propósito e independencia de proot

- Proot es una herramienta independiente y multipropósito para ejecutar procesos sin privilegios con traducción de filesystem, bindings, compatibilidad y aislamiento explícito.
- Las capacidades de proot deben ser genéricas, simples y útiles sin depender de un consumidor concreto.
- No añadas a proot presets, políticas, rutas, permisos, autorizaciones, defaults ni configuraciones específicas de `control-api` u otra aplicación.
- `control-api` puede depender de una versión de proot compatible con su protocolo; la dependencia es de compatibilidad entre consumidor y proveedor, no una razón para que proot dependa de `control-api`.
- La implementación del protocolo en proot debe permanecer como capacidad explícita y documentada del ejecutable, sin imponer políticas de autorización ni configuración de la integración.
- Launchers, modalidades como `load termux`, presets y facilidades de configuración pertenecen a la integración que los ofrece. Esa integración debe pasar explícitamente sus opciones a proot y conservar la responsabilidad de sus efectos.
- Antes de diseñar una extensión, explica qué problema general de proot resuelve, cómo funciona sin integraciones y por qué no debe vivir en un consumidor externo.

### ⚠️ SIEMPRE bump TERMUX_PKG_REVISION antes de commit

Cada vez que toques `proot-source/src/` o `ci/termux/packages/proot/`, incrementa `TERMUX_PKG_REVISION` en `ci/termux/packages/proot/build.sh` ANTES de commit. Sin esto el CI no se dispara bien y los usuarios no reciben la actualización. Es el error más común. La revisión vigente debe leerse siempre de `ci/termux/packages/proot/build.sh`; no mantengas un número duplicado aquí.

### Orden de commit (secuencial, no omitir pasos)

1. Editas código (`proot-source/src/` o `ci/termux/packages/proot/`)
2. Bump `TERMUX_PKG_REVISION` en `ci/termux/packages/proot/build.sh`
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
- Builds locales: OOM-safe por defecto (2 jobs); usa `-j 1` en dispositivos <4GB RAM.
- `gita` es la herramienta de monitoreo CI (ver Monitoreo CI); no uses `timeout` con ella.
- Los tests que invoquen proot con `env -i` deben pasar explícitamente
  `TMPDIR` y `PROOT_RUNTIME_DIR` a una ruta host escribible; no pueden depender
  del entorno heredado ni usar `/tmp` como sustituto en Termux.
- **proot necesita `env -i`** para ejecutar binarios en rootfs Alpine — el entorno heredado causa fallos en execve. Siempre limpiar env al invocar proot directamente.

## Security Hardening (Fases A-F)

Resumen en `docs/security/FIXES.md`. Commits por fase:

| Fase | Estado | Commits | REV |
|------|--------|---------|-----|
| A — Aislamiento P0 | ✅ | `573f4cb8d9`, `3b98197d8a` | 19-20 |
| B — Leaks y fds | ✅ | `ec308f5425` | 21 |
| C — Aislamiento P1 | ✅ | `6d3a556007`, `e141f86c56` | 22 |
| D — Rendimiento | ✅ | `e30bdb5b43`, D5 hash | 25 |
| E — Resto | ✅ | `89e2598828`, E2-E8 | 27 |
| F — Vista `/proc` guest estricta | ✅ | implementación actual | 30 |

### Nuevos CLI flags (Fase C)

- `--recommended-etc-rw` — restores legacy RW for /etc/ binds (default: :ro)
- `--fake-permissions` — emulate permissions without real chmod (no-op in override_permissions + access() emulated)

### Nuevas funciones (Fase C)

- `insort_binding3_with_mode()` — like `insort_binding3()` but accepts `BindingAccess` parameter (wrapper, no signature change)

### Tests organizados (Fase B+C+D+E)

```bash
# Smoke tests
tests/proot/hardening/test_b1_b2_b7.sh      # B1/B2/B7: 20 concurrent --exec clients
tests/proot/hardening/test_b3.sh             # B3: fd_map stale sweep
tests/proot/hardening/test_b5.sh             # B5: SP integrity bind/connect loop
tests/proot/hardening/test_b6.sh             # B6: bridge children killed on exit
tests/proot/hardening/test_b8.sh             # B8: talloc leak verification
tests/proot/hardening/test_phase_c.sh        # C2-C7: MS_RDONLY, /etc :ro, proc, PEERCRED, fake-perms
tests/proot/syscalls/test_d4_e1_e3_e6.sh     # D4 socket, E1 renameat2, E3 uname, E6 mknod (39 tests)
tests/proot/syscalls/test_upstream_link2symlink.sh  # regresiones portadas de upstream
tests/termux-isolated/storage/test_termux_isolated_storage.sh # storage opt-in y binds :mask
tests/termux-isolated/shell/test_termux_isolated_shebang.sh # termux-exec y shebangs en ambos modos
tests/termux-isolated/shell/test_termux_isolated_default_shell.sh # shell $SHELL en modo interactivo
```

Para una regresión nueva, crea primero el script, ejecútalo con `bash -n` y
`git diff --check`, y después lanza el archivo mediante `require_escalated`.
La batería completa se ejecuta con `./tests/run.sh all`; no se considera
suficiente una prueba manual equivalente.

Resultados: B=14/14 PASS, C=6/6 PASS, D4/E1/E3/E6=39/39 PASS. Reportes en `reports/pentest/`.

## Arquitectura del fork (no obvia desde los nombres de archivo)

### Extensiones reales (12) en `proot-source/src/extension/`

`virtual_net`, `resource_limit`, `proc_isolation`, `fake_id0`, `kompat`, `port_switch`, `sysvipc`, `link2symlink`, `hidden_files`, `mountinfo`, `fix_symlink_size`, `ashmem_memfd`.

### Virtual Networking (`--proxy NAME`)

El estado temporal de estas extensiones no usa una ruta Termux compilada:
proot usa `PROOT_RUNTIME_DIR` y, si no existe, `TMPDIR`. El consumidor debe
proporcionar un directorio válido; proot no inventa binds ni rutas de Termux.

Red virtual con **Abstract Unix Domain Sockets** (sin TCP/IP real): `socket(AF_INET/AF_INET6)`→`AF_UNIX`, `bind/connect` traducidos a `@proot-vnet-{name}-{port}-{token}`; `getsockname/getpeername` emulan loopback (`127.0.0.1`/`::1`); `setsockopt(IPPROTO_TCP)` voided. Registry compartido para multi-instancia: `<PROOT_RUNTIME_DIR o TMPDIR>/proot-net/{name}/registry.lock` (flock; magic `0x50524F4E` = **"PRON"**; entradas 512 × 116 B, ~59 KB). `-p HOST:VIRTUAL` lanza helper (`--vnp-helper NAME`, `virtual_net_helper.c` 422 líneas) que abre TCP real y hace bridge TCP→Unix.

| Escenario | Resultado |
|-----------|-----------|
| Mismo `--proxy NAME` | ✅ conecta (socket abstracto) |
| Diferente NAME / sin `--proxy` / host nativo | ❌ bloqueado |

### Supervise & Exec (`--supervise`, `--exec <PID> <cmd>`)

Los logs se escriben en `PROOT_RUNTIME_DIR` o `TMPDIR`, según el entorno que
proporcione quien lanza proot.

`--supervise` cambia el event loop a `signalfd`+`poll()` y escucha en `@proot-exec-<PID>`. `--exec` (invocación proot separada) conecta y ejecuta un comando dentro del mismo contexto (rootfs, binds, proxy). Logs: `<PROOT_RUNTIME_DIR o TMPDIR>/proot-exit-<PID>.log` (`process 'x' exited with status N / killed by signal N`). Sin `--supervise` el loop es 100% idéntico al upstream (cero overhead).

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

`--proc-isolated` proporciona una vista guest estricta de procfs, no solo un
filtro de nombres:

- `/proc` conserva únicamente `self`, `thread-self`, archivos globales
  soportados y PIDs de tracees vivos; también cubre `getdents{,64}`, `dup`,
  `fcntl`, `fdopendir`, lecturas parciales y aperturas relativas con `dirfd`.
- Los archivos globales y por proceso soportados se sintetizan con formato
  Linux válido. `stat`, `meminfo`, `uptime`, `mountinfo`, `status`, `limits`,
  `maps`, `attr/current`, `io`, `sched`, `pagemap`, `fdinfo` y similares no
  copian estadísticas ni topología del procfs host.
- `maps`, `exe`, `cwd` y enlaces `/proc/*` se sanitizan y conservan la vista
  guest. En modo `--termux-paths`, las rutas Termux son guest válidas; con un
  rootfs, se conservan sus rutas (`/usr`, `/home`) y se ocultan rutas Android
  o externas al rootfs.
- `/proc/net`, `/proc/sys`, `/proc/kcore`, `/proc/keys`, `/proc/kmsg` y
  equivalentes sensibles no aparecen en el listado. PIDs que no pertenecen a
  la instancia devuelven `ENOENT`/`ESRCH` según la operación.
- El estado sintético por FD se mantiene entre `openat`, `read`, `pread`,
  `dup`, `fcntl` y `close`; se limpia al cerrar, hacer `execve` o terminar el
  tracee.

La extensión también confina `ptrace`, `process_vm_readv/writev`, `kill` y
`pidfd_open` hacia PIDs host (`ESRCH`), emula la red netlink aislada y conserva
early-returns cuando `ISOLATE_PROC` está desactivado.

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

`TERMUX_PKG_SKIP_SRC_EXTRACT=true` salta descarga → `termux_step_pre_configure()` rsync de `proot-source/src/` al build dir → `make` compila las capacidades del fork (sin parches) → package step crea `.pkg.tar.xz`.

## Known Issues / Gotchas

- **`ci/termux/repo.json` declara `pkg_format: debian` pero el workflow usa `--format pacman`** — inconsistencia verificada, no "arreglar" (el pipeline CI manda).
- **`buildorder.py`**: parcheado para saltar deps ausentes (libllvm, python declaran deps removidas).
- **`/data` mount**: no usar en CI (`-m` en run-docker.sh causa permisos en runners GHA); la caché se monta vía `TERMUX_DOCKER_RUN_EXTRA_ARGS`.
- **Registry cleanup**: entradas stale de `registry.lock` no se limpian solas (no afectan). Limpieza manual: borrar el directorio `proot-net/` dentro de `PROOT_RUNTIME_DIR` o `TMPDIR` del proceso correspondiente.
- **Tamaños reales** (para estimar diffs): `virtual_net.c`=1144, `virtual_net_helper.c`=422, `cli/proot.c`=1004, `cli/proot.h`=614, `cli/cli.c`=694, `GNUmakefile`=318, `tracee/event.c`=976, `tracee/tracee.h`=399.
- **D5 hash table**: ✅ implementada. Hash estático 256 buckets + `tracee_hash_update()` para PID change en cli.c. Sin talloc lifecycle issues.
- **D6 binding cache**: SKIP — riesgo de dangling pointers supera ganancia (3-10 bindings típicas, scan lineal es efectivamente O(1)).
- **E items**: E1-E8 todos RESUELTO (REV 24-27). Ver `docs/security/FIXES.md` §7 para detalles.
- **proot necesita `env -i`** al ejecutar en rootfs Alpine — el entorno heredado causa execve failures. Los wrappers están en `tests/rootfs/`.

## Pentest / Hardening Testing

`tests/proot/probes/` contiene los programas C (`p_fs`, `p_sys`, `p_proc`,
`p_net`, `p_kernel`, `memtest.c`); los resultados históricos viven en
`reports/pentest/`. Los wrappers `tests/rootfs/alpine_rootfs` y
`tests/rootfs/alpine_rootfs_hardened` son auxiliares locales.

### Scripts de regresión (Fase B+C+D+E)

```bash
tests/proot/hardening/test_b1_b2_b7.sh      # 20 clientes --exec concurrentes
tests/proot/hardening/test_b3.sh             # fd_map stale sweep
tests/proot/hardening/test_b5.sh             # SP integrity bind/connect loop
tests/proot/hardening/test_b6.sh             # bridge children killed on exit
tests/proot/hardening/test_b8.sh             # talloc leak verification
tests/proot/hardening/test_phase_c.sh        # C2-C7: MS_RDONLY, /etc :ro, proc, PEERCRED, fake-perms
tests/proot/syscalls/test_d4_e1_e3_e6.sh     # D4 socket, E1 renameat2, E3 uname, E6 mknod (39 tests)
```

## Configuración del agente

Estas instrucciones son autosuficientes y no requieren archivos de configuración fuera del repositorio.
