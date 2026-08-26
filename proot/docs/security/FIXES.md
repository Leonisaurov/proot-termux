# FIXES.md — Historial de hardening post-auditoría

> Este documento se conserva como referencia técnica de las fases de hardening.
> Sus rutas relativas parten de `proot/`: `src/`, `ci/termux/packages/`,
> `tests/` y `reports/`. El índice vigente está en [`../INDEX.md`](../INDEX.md).

## Estado de avance

| Fase | Estado | Commits | Notas |
|------|--------|---------|-------|
| A — Aislamiento P0 | **Completada** ✅ (en producción) | `573f4cb8d9` (REV 19), `3b98197d8a` (REV 20) | A2 + C1 + V4 (573f4cb8d9); cierre de los 4 MINORs + kill(0)/kill(-pgid) confinados (3b98197d8a). Pentest ampliado con baselines `*_2` y verificaciones `*_3`. |
| B — Leaks y fds | **Completada** ✅ | `ec308f5425` (REV 21) | 8 fixes B1-B8. Pentest: 14/14 tests PASS (B1-B2-B7 concurrent, B3 fd_map sweep, B5 SP integrity, B6 bridge children, B7 signalfd, B8 talloc). Scripts en `tests/proot/hardening/`, reporte en `reports/pentest/REPORT-FASE-B-PENTEST.md`. |
| C — Aislamiento P1 | **Completada** ✅ | `6d3a556007` (REV 22), `e141f86c56` | C2 MS_RDONLY emulation (insort_binding3_with_mode), C3 /etc :ro default (--recommended-etc-rw), C4 proc blocklist expandido (12 paths), C6 SO_PEERCRED auth, C7 --fake-permissions. Pentest: 7/7 PASS. C3 fix: defer -R bindings + guest path check. |
| D — Rendimiento P0/P1 | **Completada** ✅ | `e30bdb5b43` (REV 23), `89e2598828` (REV 24), D5 hash (REV 25) | D1 BPF sorted copy, D2 ioctl dinámico FICLONE, D3 faccessat2 sin sysexit, D4 socket no-sysexit. D5 hash table O(1) get_tracee: implementada con `tracee_hash_update()` para PID change en cli.c. D6 (binding cache) y D7 (canonicalize cache) SKIP — riesgo supera ganancia. |
| E — Resto P2/P3 | **Completada** ✅ | `89e2598828` (REV 24), REV 27 | E1 renameat2 no-sysexit, E3 uname conditional, E6 mknod phantom fix. E5 pidfd_open → ISOLATE_PROC. E7 lazy maps_fd detection. E4 doc -q host-rootfs. E2 registry fd cache. E8 fake_netlink fast-path. |

## G — Limpieza de warnings y temporales seguros (REV 57)

Completada en `25ebffc459`. El build oficial ARM64 con `-Wall -Wextra` quedó
sin warnings C; la validación completa de objetos con `-Werror` también pasa.
Los loaders ARM64 y ARM32 se reconstruyen sin los avisos de clobbers
reservados: los saltos no retornables conservan la restauración de `sp`, la
limpieza de `x0`/`r0` y el salto al entrypoint, pero no declaran `sp`/`pc` como
clobbers que los toolchains reservan.

Los nombres de sockets temporales se reservan con `mkstemp`, se cierran y se
desvinculan antes de `bind`; los errores de reserva, cierre, `unlink`, `bind` y
`listen` conservan cleanup explícito. El contexto talloc de proceso reemplaza
`talloc_autofree_context` y se libera durante las salidas normales.

Verificación local: todos los scripts bajo `tests/` existentes, 11/11 scripts
con código 0; D4/E1/E3/E6 reportó 39/39, net policy 9/9 y Phase C 6/6.

## H — Seguimiento de upstream (v5.1.107.92)

`upstream/master` fue revisado hasta `7266fb3e85` (`v5.1.107.92`). Los cuatro
commits posteriores a la base común son `7ff389a181`, `894e5789cd`,
`61681c6481` y `7266fb3e85`. El fork conserva deliberadamente su árbol
`src/`: el upstream actualizado usa `src/` y una fusión normal
eliminaría componentes propios del fork.

La rama registra esa referencia upstream mediante una fusión de historial sin
cambiar el árbol de trabajo. Los cambios funcionales de link2symlink y la
corrección sysenter/sysexit para kernels Android antiguos quedan identificados
para portarlos al árbol compilado del fork, con pruebas específicas antes de
activarlos.

## F — Vista `/proc` guest estricta (REV 30)

La implementación actual amplía `ISOLATE_PROC` a una vista procfs coherente y
aislada por tracee. El listado de `/proc` usa una whitelist y solo conserva
PIDs vivos de la instancia, `self`, `thread-self` y archivos explícitamente
soportados. Los archivos globales y por proceso se generan con formato Linux
compatible, incluyendo `status`, `stat`, `limits`, `maps`, `mountinfo`,
`attr/current`, `io`, `sched`, `pagemap`, `fdinfo` y enlaces `exe`/`cwd`.

El estado se conserva por FD para aperturas normales y relativas (`openat`),
lecturas parciales, `dup`, `dup3`, `fcntl`, `pread` y `close`. Se cubren rutas
no canónicas y `/proc/self`/`/proc/thread-self`. `maps` y los enlaces se
sanitizan para no revelar rutas Android, dispositivos, inodos o direcciones
host. `mountinfo` se genera desde la vista guest y no desde la tabla de
montajes host. Sin `ISOLATE_PROC`, estas transformaciones no se activan.

La revisión 30 también elimina de `maps` las líneas del loader temporal
`<PROOT_TMP_DIR>/prooted-<pid>-XXXXXX`, cuyo nombre podía revelar el PID del
tracer aunque el acceso a ese PID ya estuviera bloqueado.

La semántica de rutas depende del entorno: con `--termux-paths`,
`/data/data/com.termux/...` es guest y debe conservarse; con rootfs, las rutas
guest esperadas son `/usr`, `/home` y similares, sin rutas Android del host.

Verificación local realizada:

- `./scripts/build-native.sh -i` (flujo oficial de compilación e instalación).
- `./bin/termux-isolated --termux-paths`: rutas Termux, PID host oculto, listado y
  `mountinfo` sintéticos.
- `./bin/termux-isolated --`: rootfs con rutas `/usr`/`/home`, sin rutas host en
  `maps`, PID host oculto y `mountinfo` sintético.
- `./bin/termux-isolated --termux-paths --no-proc-isolated`: opt-out explícito que
  conserva la vista procfs anterior, incluyendo datos host, para confirmar la
  compatibilidad. `./bin/termux-isolated` sin esa opción usa la vista estricta.
- `bash tests/proot/hardening/test_phase_c.sh`: **6/6 PASS**.

Documento de referencia INMUTABLE durante la implementación. Resultado de 3 auditorías profundas (rendimiento, leaks/fds, aislamiento). Cada fix especifica archivo:línea, cambio concreto, riesgo y verificación. No re-abrir ítems marcados en §0.

## 0. Estado actual (lo ya arreglado — NO re-abrir)

| ítem | estado |
|------|--------|
| Bug `--exec`/usr (cwd_raw/cwd_explicit, copy_binding, `-w` relativo, canonicalización chdir) | ✅ fixes 3109a2ee2e, 3d307720df, b8b47375f3 |
| Leak talloc en shutdown supervise (`free_terminated_tracees`, FU-1..FU-4, `supervise_handle_exited_tracee`, guard `ctl_fd>=0`) | ✅ fixes 5ad187e929 + 414053fc04 |
| **FASE A COMPLETADA — A2 stat/readlink oracle + C1 kill(-1) broadcast + V4 netlink topology** | ✅ commit `573f4cb8d9` 'fix(isolation): block /proc host stat/readlink oracle, kill(-1) broadcast, netlink topology' (REVISION 19) — pentest ampliado con baselines `*_2` y verificaciones `*_3` |
| **FASE A COMPLETADA — cierre de los 4 MINORs + hardening señales** | ✅ commit `3b98197d8a` 'fix(isolation): deliver kill broadcasts to guest tracees, block statx on SIGSYS, harden signal validation' (REVISION 20) — kill(-1) entrega real a tracees; statx cubierto en SIGSYS legacy; pentest EMULADO-OK; buffers PATH_MAX; extra kill(0)/kill(-pgid) confinados al guest (ESRCH pgid vacío, EINVAL señal inválida) |
|| REVISION actual en `ci/termux/packages/proot/build.sh` | **69** — bump SIEMPRE antes de commit si se toca `src/` o `ci/termux/packages/proot/` |

## 1. Resumen ejecutivo de las 3 auditorías

| Auditoría | Nº hallazgos | P0 | P1 | P2 | P3 | Archivos principales |
|-----------|-------------|----|----|----|----|----------------------|
| Aislamiento | 12 (A1-A2, C1-C7, E5-E7) | 2 | 7 | 1 | 2 | proc_isolation.c, enter.c, cli/proot.h, fake_id0.c, supervise.c |
| Leaks y fds | 8 (B1-B8) + 8 NO-leak | 0 | 3 | 3 | 2 | supervise.c, virtual_net.c, binding.c, virtual_net_helper.c, ldso.c |
| Rendimiento | 8 (D1-D8) + E8 | 2 | 5 | 1 | 0 | seccomp.c, tracee.c, binding.c, canon.c, event.c, syscall.c |

## 2. Reglas de oro de implementación (del repo)

1. **Emulate, Never Deny** — nunca EPERM/ENOSYS cuando se puede emular un resultado natural (ESRCH, 0, ENOENT).
2. **Sin flags → cero overhead** — los cambios deben mantener early-returns/latches en el path sin flags.
3. Orden de fases: **A → B → C → D → E** (A es PRIMERA; D va AL FINAL porque la semántica debe estar congelada).
4. Commits atómicos por archivo; una fase = no mezclar con otra.
5. ARM64: chained-syscall mechanism roto; `SYSARG_4` para flags de accept4; no tocar `SYSARG_3`.
6. No cambiar firmas de funciones compartidas (ej. `insort_binding3`): wrapper nuevo si hace falta.

## 3. FASE A — Aislamiento P0 (← PRIMERA)

> **Estado: ✅ COMPLETADA** (commits `573f4cb8d9` REV 19 + `3b98197d8a` REV 20, en producción). Nota: **C1 y V4 de la antigua Fase C se implementaron en la Fase A** por ser los escapes reales detectados por el pentest ampliado (ver §5).

### A1. Bypass del filtro /proc por rutas no canónicas [P0] — ✅ IMPLEMENTADO (matizado)
- **Hallazgo**: `proc_isolation.c:154-186` solo matchea el path canónico `"/proc/"` (strncmp exacto); el filtro corre sobre el path SIN canonicalizar. Un guest puede: `openat(dirfd=open("/proc"), "1/status")`, `open("/proc//1/status")`, `open("/proc/./1/status")`, `open("/proc//net/tcp")`, getdents vía dirfd relativo → lectura COMPLETA del /proc host incluso en modo B. Autodocumentado en `proc_isolation.c:108-114`.
- **Fix (debatir al implementar)**: (a) filtrar sobre el path ya traducido/canonicalizado, o (b) trackear fds que apuntan a /proc en el hook de open/openat EXIT (como ya hace con `maps_fd`) y filtrar por fd en read/getdents/stat. **El crítico recomienda (b)** por riesgo de falsos ENOENT en rutas legítimas del guest con (a).
- **Riesgo**: falsos ENOENT en rutas legítimas (mitigar con (b)); bypass inverso si el punto de hook es incorrecto.
- **Verificación**: pentest ampliado V1 (`openat` relativo dirfd=/proc, `//`, `/./`, `/proc//net/tcp`, getdents de `/proc/1`).
- **Resultado**: el baseline (`*_2`) demostró que **V1 NO escapaba** — proot canonicaliza las rutas ANTES del filtro, así que las rutas no canónicas ya llegaban normalizadas. Implementado por prevención/defensa en profundidad; el escape real estaba en V2 (A2) y V4 (netlink).

### A2. Oracle de pids + leak de rutas host vía stat/readlink [P0] — ✅ IMPLEMENTADO
- **Hallazgo**: `flag_sysnum_map` (`proc_isolation.c:58-70`) solo cubre open/openat/openat2/getdents/read/kill; stat/newfstatat/statx/readlink NO están → pasan al kernel host. `path/proc.c:111-113` da DEFAULT para pids no-tracee (readlink `/proc/<hostpid>/{exe,cwd,root,fd/N}` filtra rutas host). Oracle de existencia vía `stat("/proc/1")`.
- **Fix**: añadir stat/newfstatat/statx/readlink/readlinkat a `filtered_sysnums` con la misma lógica → ENOENT para pids host (filosofía Emulate-Never-Deny, NO EPERM).
- **Verificación**: pentest ampliado V2 (`stat`/`statx`/`readlink` de `/proc/1/{exe,cwd,root,fd/0,maps}`).
- **Resultado**: implementado en `573f4cb8d9` (stat/readlink → ENOENT); `statx` sobre SIGSYS legacy cubierto en `3b98197d8a`. Pentest p_proc V2 con baselines `*_2` y verificaciones `*_3`.

## 4. FASE B — Leaks y fds (verificados por debugger con archivo:línea)

> **Estado: ✅ COMPLETADA** (REV 21, Fase B — 8 fixes B1-B8, incluye fix B5 de SP restore en error paths). Verificación mediante smoke tests `--exec`/`--proxy` + talloc report (ver §8). Cualquier fix que toque `free_terminated_tracees` usa la infraestructura creada en la Fase A (ver R9 en §9).

| ID | Sev | Hallazgo | Fix | Riesgo | Verificación |
|----|-----|----------|-----|--------|--------------|
| B1 | P1 | `supervise.c:251` `accept(ctl_fd)` SIN SOCK_CLOEXEC → fd del socket de control filtrado a cada guest `--exec`; si el supervisor muere el cliente cuelga (el guest mantiene el fd) | `accept4(..., SOCK_CLOEXEC)` o close en el hijo | bajo | smoke `--exec` múltiple + check /proc/self/fd del guest |
| B2 | P1 | `supervise.c:530-536` `add_client()` falla (SUPERVISE_MAX_CLIENTS=16) → kill+waitpid pero `child_tracee` (`get_tracee`:440) queda en la lista global sin terminated → fuga Tracee+fs+heap+bindings en supervisor longevo | marcar `terminated=true` + `TALLOC_FREE` tras kill/waitpid | medio | smoke 16+ clients `--exec` concurrentes; talloc report |
| B3 | P1 | `virtual_net.c` `fd_map[256]` con entradas stale de tracees muertos (sin hook de salida por-pid; REMOVED solo con última ref talloc) → `vnp_add_fd` retorna NULL SILENCIOSO (`virtual_net.c:564,734,1026`) → getsockname/accept fake dejan de funcionar | sweep por pid en `free_terminated_tracees()` o hook de exit de tracee | medio | ciclo crear/cerrar sockets vnet con `--proxy` hasta agotar fd_map |
| B4 | P2 | `binding.c:351-354+782` mbind duplicado idéntico → `mbind_cleanup` del binding reemplazado borra los archivos host del binding activo (data loss patológico) | `talloc_set_destructor(NULL)` en el descarte o registrar destructor al final del insort | bajo | caso mbind duplicado + verificar archivos host |
| B5 | P2 | `virtual_net.c:604,750` `alloc_mem` baja SP del guest ~128B por bind/connect sin restaurar (~65k llamadas para agotar 8MiB) | restaurar SP o reusar área de scratch | bajo | loop bind/connect + medir SP antes/después |
| B6 | P2 | `virtual_net_helper.c:235-254` bridge children huérfanos (2 fds c/u) hasta que peer TCP cierre; no se matan al salir el helper | trackear pids de bridge + kill al salir | bajo | helper + conexiones, matar helper, contar fds |
| B7 | P3 | `supervise.c:155-161` `sig_fd` (signalfd) nunca se cierra en `supervise_fini()` (solo cierra `ctl_fd_global`) | guardarlo global + cerrarlo en fini | muy bajo | valgrind/fd count en ciclo supervise |
| B8 | P3 | `execve/ldso.c:488` `initial_ldso_paths` `strdup()` (malloc) sin liberar, 1x por proceso | talloc o static-no-free documentado | muy bajo | valgrind |

**Verificados NO-leak (NO tocar)**: BPF seccomp free en end:; `fake_netlink_reply` bajo tracee; loader/glue/temp bajo contexto talloc; registry stale correcto; bindings bajo ctx via talloc_reference; copy_binding+mbind_files; extensiones compartidas REMOVED sin UAF; `cwd_raw` bajo tracee.

## 5. FASE C — Aislamiento P1

| ID | Sev | Hallazgo | Fix | Riesgo | Verificación |
|----|-----|----------|-----|--------|--------------|
| C1 | P1 | `proc_isolation.c:224` `kill(-1, SIGKILL)` no bloqueado (solo cubre pid>0) → DoS de TODOS los procesos host mismo-uid | interceptar `pid<=0` con sig>0 → 0 emulado o ESRCH | bajo | pentest V3 (SOLO `kill(-1,0)` como oráculo, ver §8) |
| C2 | P1 | `enter.c:238-265` `emulate_mount` ignora MS_RDONLY/MS_BIND → `mount --bind` de path `:ro` crea binding RW (bypass de ro); pivot_root (`enter.c:2163-2168`) igual (oldroot RW) | wrapper `insort_binding3_with_mode` (NO cambiar firma: 5 call sites: mount, pivot_root, `socket.c:136`, `glue.c:184`, `execve/exit.c:131`) + aplicar `access_mode`. OJO MS_REMOUNT ignorado (`enter.c:235`) → completar | medio | pentest V5 (bypass `:ro` vía mount, wrapper con bind `:ro`) |
| C3 | P1 | `cli/proot.h:12-49` binds recomendados -R/-S RW sobre archivos críticos host (`/etc/passwd`, `/etc/group`, `/etc/hosts`, `/etc/resolv.conf`, `/etc/localtime`, `/run`, `/tmp`, `$HOME`) | `:ro` por defecto DETRÁS DE FLAG (p.ej. `--isolate-binds`) — NO default (rompería proot-distro) | alto | smoke proot-distro completo con flag ON y OFF |
| C4 | P1 | `proc_isolation.c:586-594` blocklist solo 4 paths; faltan: `/proc/{version,uptime,stat,loadavg,sys/kernel/*,kallsyms,iomem,interrupts,modules,cmdline,misc}`, `sysinfo()`, getcpu, sched_getaffinity, uname | ampliar blocklist con ENOENT emulado | bajo | pentest p_proc ampliado |
| C5 | P1 | `enter.c:958-1119` fake netlink filtra topología host (getifaddrs/relay_route_dump: ifaces, MAC, MTU, IPs) | solo loopback sintético + hook de salida para `fake_netlink_fds` (un tracee muerto con fd fake-netlink → colisión con fd nuevo → respuesta sintética a fd real) | medio | pentest V4 (topología) |
| C6 | P1 | `supervise.c:164-187+246-540` supervisor `@proot-exec-<pid>` sin auth de peers (guest vía ppid; host mismo-uid inyecta comandos) | `SO_PEERCRED == pid esperado` + token opcional (versionar protocolo, actualizar doc en mismo commit) | medio | pentest inyección + smoke --exec |
| C7 | P1 | `fake_id0.c:404-486` `override_permissions` MUTA ficheros host (chmod real) — persistente tras SIGKILL, toca binds `:ro` | emular permisos en stat/access sin chmod real — DETRÁS DE FLAG, fase propia con smoke test `-0`/`-S` | alto | smoke `-0`/`-S` + verificar no-mutación host |

## 6. FASE D — Rendimiento P0/P1 (va AL FINAL: semántica congelada)

| ID | Sev | Hallazgo | Fix | Riesgo | Verificación |
|----|-----|----------|-----|--------|--------------|
| D1 | P0 | `syscall/seccomp.c:97-122` cadena lineal BPF_JEQ ~180 instr; TODO syscall del guest la recorre | BPF búsqueda binaria (log2(90)≈7 niveles) o reordenar `proot_sysnums` por frecuencia. **Hacerlo genérico** (binaria sobre cualquier lista, no hardcodear orden) para no chocar con C4/A2 que añaden syscalls | CRÍTICO multi-ABI (offsets, fallback ALLOW por sección) | CI + stress syscall (apt update); comparar ptrace stops |
| D2 | P0 | `syscall/seccomp.c:368` `PR_ioctl` FILTER_SYSEXIT estático → quitar; `sysexit_pending` solo si `cmd==FICLONE` (`enter.c:2481`) | 1 parada menos por ioctl | bajo | benchmark ioctl |
| D3 | P1 | `syscall/seccomp.c:356` `PR_faccessat2` FILTER_SYSEXIT sin case en exit.c → quitar (`fake_id0` lo ORa si activo) | quitar | bajo | benchmark access |
| D4 | P1 | `syscall/seccomp.c:352` `PR_socket` FILTER_SYSEXIT → quitar (enter ya auto-solicita sysexit para netlink, `enter.c:1790-1791`) | ✅ RESUELTO (REV 24) quitar — enter.c:1554 ya pide sysexit dinámicamente para AF_NETLINK; ahorra 1 ptrace stop por cada socket() no-netlink | pentest D4: 8/8 PASS (socket create, close cycle, multi-fd, /proc/self/fd) |
| D5 | P1 | `tracee.c:339-340` `get_tracee` hace flush de ctx (talloc_free+new) en CADA parada ENTER/EXIT (`event.c:432,532,579`) | hash pid→tracee O(1) + no flushear en EXIT | ALTO: el flush es memory collector deliberado — auditar allocs bajo ctx primero (ASan/valgrind) | ASan + benchmark loop stat/open |
| D6 | P1 | `binding.c:128-173` `get_binding` O(n_bindings) por path, binding `/` (rootfs) al final → walk completo | fast-path `/` + MRU cache 1-4 binds, INVALIDAR en `insort_binding`/remove/pivot_root (el plan original lo omitía) | alto | benchmark `find / -type f` |
| D7 | P1 | `canon.c:159,265` lstat por componente en canonicalize | cache de canonicalización keyed `(cwd,path)` con invalidación en rename/rmdir/unlink/mkdir — ÚNICA con riesgo de coherencia real → DETRÁS DE FLAG o descartar si D1-D6 no bastan | alto coherencia | stress rename/mkdir + benchmark |

**OJO CONFLICTO**: D1 asume lista estática; A2/C4 la cambian → hacer D1 genérico o acordar el set final de syscalls ANTES de implementar D1.

## 7. FASE E — Resto P2/P3

| ID | Sev | Hallazgo | Fix | Verificación |
|----|-----|----------|-----|--------------|
| E1 | P2 | `syscall/seccomp.c:405` `renameat2` FILTER_SYSEXIT sin case → quitar (`link2symlink` lo ORa) | ✅ RESUELTO (REV 24) quitar — link2symlink ya añade FILTER_SYSEXIT en su filtered_sysnums cuando está activo; sin link2symlink no hay handler → ptrace stop inútil eliminado | pentest E1: 8/8 PASS (rename básico, overwrite, cross-dir, nonexistent) |
| E2 | P2 | `virtual_net.c:279-318` registry flock por bind/connect (4-6 syscalls) | ✅ RESUELTO (REV 27) fd cache para LOCK_SH — reusa fd abierto, solo re-abre con LOCK_EX para writes | pentest E2: vnet stress 5/5 PASS |
| E3 | P2 | `syscall/seccomp.c:423` `uname` FILTER_SYSEXIT solo x86_64 (`exit.c:466` #ifdef) | ✅ RESUELTO (REV 24) `#ifdef ARCH_X86_64`-arlo — en ARM64 el handler está compilado out, ptrace stop inútil; en x86_64 se mantiene (necesario para 32-bit compat) | pentest E3: 8/8 PASS (uname -a/-s/-m/-r) + build aarch64 limpio |
| E4 | P2 | `cli/proot.c:194` `-q` expone TODO el host en `/host-rootfs` | ✅ RESUELTO (REV 26) doc comment con security note — `new_binding("/", HOST_ROOTFS)` es intencional pero documenta el riesgo | doc comment in-code |
| E5 | P1 | `proc_isolation.c:560-570` `pidfd_open` escape solo en A (ISOLATE_PTRACE) → mover a ISOLATE_PROC o default-on | ✅ RESUELTO (REV 26) movido a ISOLATE_PROC — `flag_sysnum_map` + handler check; previene pidfd-based /proc bypass en todas las configs con proc isolation | pentest E5: smoke con pidfd_open |
| E6 | P2 | `fake_id0.c:1026-1039` mknod phantom success (EPERM→0; nodo no existe en host) | ✅ RESUELTO (REV 24) ENOENT emulado para S_ISBLK/S_ISCHR — mknod/mknodat verifica mode antes de forzar éxito; S_ISREG/S_ISFIFO no afectados | pentest E6: 10/10 PASS (block, char, regular, FIFO, mknodat) |
| E7 | P1 | proc_isolation bypass getdents/maps vía no-canónico (`maps_fd` no se registra) | ✅ RESUELTO (REV 26) lazy maps_fd detection en `hpc_handle_maps_read_exit` — si maps_fd no registrado, verifica fd via readlink_proc_pid_fd y registra | pentest E7: smoke con rutas no canónicas |
| E8 | P3 | micro-opts: `event.c:855-858` check seccomp antes de GETEVENTMSG; `syscall.c:139-141` save_current_regs doble; `enter.c:573-582` latch `fake_netlink_active`; `path.c:51-111` `join_paths` memcpy | ✅ RESUELTO (REV 27) fast-path en `is_fake_netlink_fd` (skip scan si count==0); seccomp check ya óptimo; save_current_regs ambos necesarios | pentest E8: stress OK |

## 8. Verificación empírica por fase (pentest ampliado + benchmarks + smoke proot-distro)

- **Fase A**: pentest ampliado V1+V2 (p_proc) + V4 (p_net) en modos A y B con baselines `*_2` y verificaciones `*_3` → **ESCAPE hoy, BLOQUEADO tras fix**. Resultados en `reports/pentest/`: `p_proc_A3/B3/A4/B4/B5` y `p_net_A3/B3/A4/B4`. Punto clave: `kill(-1,SIGTERM)` → **EMULADO-OK** (verificado en `p_proc_B4` y `p_proc_B5`): el broadcast se entrega a los tracees del guest y NO llega al host (el hijo del guest sigue vivo). V1 (rutas no canónicas) demostró en baseline que NO escapaba (proot canonicaliza antes del filtro).
- **Fase B** (en curso): smoke 16+ clients `--exec` concurrentes; ciclo crear/cerrar sockets vnet con `--proxy` para agotar `fd_map`; smoke `--exec`/`--proxy` + **talloc report** (`kill -USR1/USR2`).
- **Fase C**: pentest V3 (`kill(-1,0)` con guard de seguridad: **NO ejecutar `kill(-1,SIGTERM/SIGKILL)` real** porque el broadcast al uid host destruiría la sesión — solo `kill(-1,0)` como oráculo) + V4 (topología) + V5 (bypass `:ro` vía mount, requiere wrapper con bind `:ro`); smoke proot-distro completo para C3/C7 (detrás de flag).
- **Fase D**: benchmark antes/después: `find / -type f` dentro del guest + `strace -c` loop de stat/open + conteo de ptrace stops (`proot -v 9`); CI + stress syscall (`apt update`) para BPF.
- **Fase E**: pruebas puntuales por fix.

## 9. Riesgos top y mitigaciones (del crítico)

| ID | Riesgo | Sev | Mitigación |
|----|--------|-----|------------|
| R1 | BPF binario multi-ABI | BLOQUEANTE si va antes de Fase C → va al final | verificar CI+stress; fallback ALLOW por sección |
| R2 | Quitar flush de ctx (`get_tracee`) | ALTO | auditar allocs bajo ctx; flag opt-in primero |
| R3 | Cache get_binding sin invalidación | ALTO | invalidar en insort/remove/pivot |
| R4 | Binds `:ro` rompe proot-distro | ALTO | flag opt-in, smoke proot-distro |
| R5 | Emular permisos fake_id0 | ALTO | fase propia + flag + test `-0`/`-S` |
| R6 | Cambiar `insort_binding3` | MEDIO | wrapper nuevo, no cambiar firma (5 call sites) |
| R7 | Falsos ENOENT en /proc | MEDIO | trackear fds en vez de re-canonicalizar |
| R8 | Protocolo `--exec` | MEDIO | token opcional + versionado + doc mismo commit |
| R9 | B2/B3 tocan `free_terminated_tracees` dos veces | MEDIO | ✅ RESUELTO (parcial) — la Fase A creó el hook de ciclo de vida de tracees primero (commits `573f4cb8d9`/`3b98197d8a`: entrega de kill broadcasts a tracees del guest y saneamiento del ciclo de salida); la Fase B reutiliza esa infraestructura. Si al implementar B2/B3 surge conflicto, el hook ya existe y solo hay que extenderlo, no crearlo. |
| R10 | mknod phantom | ✅ RESUELTO (REV 24) ENOENT emulado para S_ISBLK/S_ISCHR |
| R11 | pidfd default-on | MEDIO | test apps del guest |

## 10. Checklist de commit (reglas AGENTS.md)

1. Editar código (`src/` o `ci/termux/packages/proot/`).
2. **Bump `TERMUX_PKG_REVISION` en `ci/termux/packages/proot/build.sh` ANTES del commit**. La revisión actual es 69; las referencias a revisiones anteriores en las fases históricas son deliberadas.
3. `git add -A && git commit -m "<type>(<scope>): <summary>"`.
4. `git push origin master` (SOLO `origin`).
5. `gita notify build-proot.yml 2>/dev/null | grep -E '(error|##\[error\]|mbind|Success)'` — exit 0=éxito, 1=falló, 2=cancelado. **NO timeout, NO streaming.**

- Convención commits: `fix`/`enhance`/`chore`/`ci`(`<scope>`): `<summary>`.
- Flujo de calidad: fixer → code-reviewer (barrera sin CRITICAL/MAJOR) → git-workflow.
- NO commitear AGENTS.md ni wrappers ni pentest binarios (los `.c` SÍ se commitearían).
- Cada fase = commits atómicos por archivo, no mezclar fases.
