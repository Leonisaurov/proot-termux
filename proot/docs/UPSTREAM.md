# UPSTREAM.md — punto de sincronización y manifiesto del port

Este archivo es la memoria machine-readable del sync con [`termux/proot`](https://github.com/termux/proot).
`proot/scripts/sync_upstream.sh` lee los pines de abajo para saber qué rango queda pendiente,
así que el próximo sync es ejecutar `--status` sin argumentos.

## Pines

```
SYNC_FROM=968ae1be84c69a607dbcd43ee514ab04753d60c5
SYNC_FORK_BASE=8bd8ca9954
```

- `SYNC_FROM` — commit upstream ya portado (tag `v5.1.107.99`). Todo lo posterior a este commit está pendiente.
- `SYNC_FORK_BASE` — commit del fork donde terminó esta tanda de port. `--verify` mide contra él si un futuro
  port borra líneas propias.

## Resultado del port (2026-10-23)

Se portaron los 47 commits sobre `7266fb3e85` (`v5.1.107.92`), punta `968ae1be84` = **`v5.1.107.99`**, en 7
commits de grupo (`45bc3808`…`8bd8ca99`), con `TERMUX_PKG_REVISION` 106→112 y `TERMUX_PKG_VERSION`
`5.1.107.89`→`5.1.107.99`.

| Medida | Valor |
|---|---|
| Cobertura acumulada de upstream | 2613/2630 líneas añadidas (**99%**) |
| Líneas propias preservadas (`--verify`) | 1299/1299, salvo 7 explicadas abajo |
| Batería `tests/run.sh all` | baseline pre-port 68 pasos RC=0 → tras el port 76 pasos RC=0 (las 4 regresiones nuevas), con los mismos 12 SKIP por el rootfs Alpine ausente en ambas |
| Batería con rootfs Alpine instalado (2026-10-24) | 99 pasos, **RC=0**, **cero SKIP de rootfs**: los 12 tests que no llegaban a ejecutarse (modo rootfs: rutas guest, mountinfo, shebang, storage) corrieron y pasaron |
| Rendimiento pareado pre-port vs port (`AUDIT_BASELINE`, 8 pares, CPU-segundos) | ratios 0.96–1.05 sin sesgo: [benchmarks.md](performance/benchmarks.md) |
| Overhead sin flags | 9427 ms → 9485 ms en 60 corridas de `sh -c "ls -R"` (**+0,6%**, dentro del ruido) |
| Overhead sin flags | 9427 ms → 9485 ms en 60 corridas de `sh -c "ls -R"` (**+0,6%**, dentro del ruido) |

Las 17 líneas de upstream que no están son deliberadas: 11 de `8bf810ccc4` (el `fallback` de `SYS_SECCOMP`
que glibc 2.43 obligaba a mover — Bionic ya lo define, así que en nuestro build es código muerto que además
rompería la invariante de `compat.h`) y 6 de comentarios cuya redacción cambiamos al portar `tracee.h` y
`enter.c` a mano. Las 7 líneas propias que faltan son el comentario y el `(void)` que este fork añadió a
`write_meta_file()` en `fake_id0/chmod.c` y `fake_id0/rename.c`: upstream reescribió esas dos funciones y ya
**ignora** el resultado de escribir el meta file, que era exactamente la intención del parche propio.

### Tercer fuente: `proot-me/proot` (el repo original)

`termux/proot` declara explícitamente que no sincroniza con él, así que se evaluó commit a commit en vez de
portar en bloque. Resultado sobre los tres fixes recientes del original (punta `25dc6a3134`, preparando 5.5.0):

| upstream sha | asunto | decisión |
|---|---|---|
| `a5b0019a09` | prctl: reportar el `no_new_privs` del programa, no el de PRoot | **YA PRESENTE** — `src/syscall/enter.c` responde `PR_GET_NO_NEW_PRIVS` con la bandera del tracee y observa su `PR_SET_NO_NEW_PRIVS` en sysexit |
| `4356c516d3` | auxv: reportar el programa como `AT_EXECFN` | **NO APLICABLE** — `termux/proot` no tiene `AT_EXECFN` en `src/execve/auxv.c`; no hay a dónde aplicar el parche |
| `1153e020fa` | readlink: no detraducir un destino que el kernel recortó | **NO APLICABLE / A REPRODUCIR** — `termux/proot` no tiene `src/path/readlink.c`; si el bug existe aquí hay que demostrarlo con una regresión propia antes de escribir el fix |

Queda pendiente de decidir con reproducción, no con cherry-pick. La capa de tests Bats/libcheck del original
tampoco se porta: no hay infraestructura equivalente en este repo.

### Divergencia conocida con el paquete Termux oficial

El paquete `termux-packages` compila con `PROOT_WITH_LIBANDROID_SHMEM=true` y `DEPENDS="libandroid-shmem,
libtalloc"`; este fork pasa sólo `-C src` y depende de `libtalloc`. Decidido expresamente: **no se adopta
ahora**, porque cambia la semántica de `sysvipc_shm.c` justo donde convive nuestra extensión `ashmem_memfd`
y su flag `--ashmem-memfd`. Nuestro `GNUmakefile` sí soporta el flag (líneas 20-21), así que adoptarlo es un
cambio de una línea cuando se valide con su propia regresión.

### Hallazgo: fallo de upstream expuesto por nuestro toolchain

`05a93edfc8` introdujo `get_link_count()` indexando `dot[-i]` con `i` de tipo `size_t`. Con el build oficial
(NDK clang 21, `-O3 -flto`) el desbordamiento sin signo se materializa como dirección no mapeada y **PRoot
muere con SIGSEGV** al abrir un enlace fingido con nominación por defecto — se reproduce con
`tests/proot/syscalls/test_link2symlink_link_count.sh` caso A. El fork lo corrigió en `009340ab` (índice con
signo + comprobación del punto). Merece reportarse aguas arriba: en x86_64/glibc con `-O2` no se manifiesta.

## Cómo se porta

```bash
git fetch upstream --prune                    # escribe refs locales: pedido antes de portar
proot/scripts/sync_upstream.sh --status       # inventaría clean/conflict/no-src
proot/scripts/sync_upstream.sh --check  <sha|a..b>
proot/scripts/sync_upstream.sh --apply  <sha|a..b>   # sólo toca proot/src/
proot/scripts/sync_upstream.sh --verify              # puerta: sin .rej, sin marcadores, propio intacto
proot/scripts/sync_upstream.sh --coverage <sha|a..b> # cuánto del commit ya estaba aquí
```

El fork mantiene su fuente en `proot/src/`, upstream en `src/`: el helper reindexa con
`git apply -3 --directory=proot -p1`. No se hace fusión de árboles — sobre un
`syscall/enter.c` propio de 1106 líneas produciría un commit no revisable.

Regla del port: un archivo tocado por ambos lados se **puerta**, nunca se elige. Se arranca de la
versión upstream y se re-aplica la intención propia. La evidencia de que no se perdió trabajo es
`--verify` (`OWN_LINES_MISSING=0`); la evidencia de que el port está completo es `--coverage`.

## Decisiones de cribado

- **USERLAND**: ningún build de este repo define `USERLAND`, así que el subsistema de meta files de
  `fake_id0` no existe en el binario oficial (`strings $PREFIX/bin/proot | grep -c proot-meta-file` = 0).
  Los hunks bajo `#ifdef USERLAND` se registran como `REJECT-USERLAND`, no se portan.
- **Cero overhead**: `58c3b42849` y `d4d2a19081` reintroducen paradas `sysexit` que las fases D2/D3/E1
  quitaron a propósito. Se deciden con benchmark, y si se rechazan queda escrito aquí.
- **ARM64**: no existe el mecanismo de chained syscall (el kernel no relee `x0-x5` al rebobinar el PC),
  así que cualquier ruta `SYSCALL_CHAINED_EXIT` portada necesita comprobación antes de darse por válida.
- **`AGENTS.md` de upstream** (`bd6d55e122`) no se porta: es la política de contributors de termux/proot
  y contradice la nuestra (declara explícitamente "No sync with upstream").

## Manifiesto de commits

| upstream sha | asunto | #src | archivos | disposición |
|---|---|---|---|---|
| 8f467b0991 | link2symlink: report the faked hard link a program was executed through | 5 | execve/enter.c,execve/execve.h,extension/extension.h,extension/link2symlink/link2symlink.c,path/canon.c | PORTED 009340ab |
| a396094c7c | link2symlink: name a descriptor opened O_NOFOLLOW after its faked hard link | 1 | extension/link2symlink/link2symlink.c | PORTED 009340ab |
| 58c3b42849 | syscall: run fork syscalls through a sysexit stop, not PTRACE_CONT | 1 | syscall/enter.c | PORTED c68a67f2 |
| 8b9941505d | tracee: register a new child even if its fork event has no PID | 3 | tracee/event.c,tracee/tracee.c,tracee/tracee.h | PORTED 465f5fcf |
| d4d2a19081 | seccomp: don't rely on the event message to tell which exits to handle | 3 | syscall/seccomp.c,syscall/seccomp.h,tracee/event.c | PORTED 465f5fcf |
| a179d3e8a4 | fake_id0: let the kernel do the real open for files & memfds when the requested read/write mode differs | 1 | extension/fake_id0/fake_id0.c | PORTED 646f5fb3 |
| 37c09de3b1 | arm64: keep the fixed load addresses out of the way of big mmap(2)s | 1 | arch.h | PORTED 45bc3808 |
| bd6d55e122 | Add AGENTS.md | 0 | (sin src) | EXCLUIDO |
| c1cd48d5b9 | fake_id0: report 32-bit IDs from getresuid/getresgid | 1 | extension/fake_id0/fake_id0.c | PORTED 646f5fb3 |
| b5af02e7ae | fake_id0: pass uid_map reads to the kernel; translate SO_PEERCRED across instances | 2 | extension/fake_id0/getsockopt.c,syscall/enter.c | PORTED 646f5fb3 |
| 7fca80c8a7 | syscall: translate fchmodat2 | 11 | extension/fake_id0/chmod.c,extension/fake_id0/fake_id0.c,syscall/enter.c,syscall/seccomp.c,syscall/sysnums-arm.h,syscall/sysnums-arm64.h,syscall/sysnums-i386.h,syscall/sysnums-sh4.h,syscall/sysnums-x32.h,syscall/sysnums-x86_64.h,syscall/sysnums.list | PORTED c68a67f2 |
| 60a9c15ccd | syscall: resolve an empty path at AT_FDCWD against the guest cwd | 2 | syscall/enter.c,tracee/statx.c | PORTED c68a67f2 |
| 254202b844 | syscall: translate open_tree, hide the fd-based mount API | 9 | syscall/enter.c,syscall/seccomp.c,syscall/sysnums-arm.h,syscall/sysnums-arm64.h,syscall/sysnums-i386.h,syscall/sysnums-sh4.h,syscall/sysnums-x32.h,syscall/sysnums-x86_64.h,syscall/sysnums.list | PORTED c68a67f2 |
| b8c1f5c5c6 | syscall: translate the destination of sendto, sendmsg and sendmmsg | 5 | syscall/enter.c,syscall/exit.c,syscall/seccomp.c,syscall/socket.c,syscall/socket.h | PORTED c68a67f2 |
| 9688d01e5a | fake_id0: read unterminated result string by length | 1 | extension/fake_id0/fake_id0.c | PORTED 646f5fb3 |
| 4a1dbac41f | fake_id0: replay fstat for descriptors that aren't paths | 1 | extension/fake_id0/fake_id0.c | PORTED 646f5fb3 |
| 7f2fab0fb3 | link2symlink: count faked hard links in the emulated fstat | 1 | extension/link2symlink/link2symlink.c | PORTED 009340ab |
| 0439248ae4 | sysvipc: back shared memory with memfd where /dev/ashmem is closed | 1 | extension/sysvipc/sysvipc_shm.c | PORTED 8bd8ca99 |
| 04c28b7dd1 | fake_id0: keep the group when chown(2) passes -1 for it | 1 | extension/fake_id0/chown.c | PORTED 646f5fb3 |
| 77310de75b | fake_id0: apply chmod through /proc/self/fd to the open file's record | 3 | extension/fake_id0/chmod.c,extension/fake_id0/helper_functions.c,extension/fake_id0/helper_functions.h | PORTED 646f5fb3 |
| 54733e84c3 | fake_id0: apply chmod modes to the real file as well | 1 | extension/fake_id0/chmod.c | PORTED 646f5fb3 |
| ea0e9b6d41 | fake_id0: record only the permission bits of a chmod mode | 1 | extension/fake_id0/chmod.c | PORTED 646f5fb3 |
| fdbed740df | fake_id0: record fchmodat2(fd, "", mode, AT_EMPTY_PATH) | 1 | extension/fake_id0/fake_id0.c | PORTED 646f5fb3 |
| 788c48205b | fake_id0: write the chmod record once the kernel has applied the mode | 4 | extension/fake_id0/chmod.c,extension/fake_id0/chmod.h,extension/fake_id0/config.h,extension/fake_id0/fake_id0.c | PORTED 646f5fb3 |
| 38a60cbed0 | fake_id0: follow mode changes made through POSIX ACLs | 4 | extension/fake_id0/chmod.c,extension/fake_id0/chmod.h,extension/fake_id0/config.h,extension/fake_id0/fake_id0.c | PORTED 646f5fb3 |
| 1a41a193e8 | fake_id0: compare statx's group with the real gid, not the uid | 1 | extension/fake_id0/stat.c | PORTED 646f5fb3 |
| cf393e0ea3 | fake_id0: apply ownership records to statx | 1 | extension/fake_id0/stat.c | PORTED 646f5fb3 |
| ccd4b745b4 | fake_id0: apply ownership records to fstatat(fd, "", AT_EMPTY_PATH) | 1 | extension/fake_id0/stat.c | PORTED 646f5fb3 |
| 32d70f7574 | fake_id0: keep the id mapping of fstatat(AT_EMPTY_PATH) on non-files | 1 | extension/fake_id0/stat.c | PORTED 646f5fb3 |
| 66b112ed38 | fake_id0: move ownership records on renameat2 | 3 | extension/fake_id0/fake_id0.c,extension/fake_id0/rename.c,extension/fake_id0/rename.h | PORTED 646f5fb3 |
| 53b2159cda | fake_id0: name the record of "dir/" after "dir" | 1 | extension/fake_id0/helper_functions.c | PORTED 646f5fb3 |
| e8a6c0ebe0 | fake_id0: remove orphaned records along with their directory | 3 | extension/fake_id0/helper_functions.c,extension/fake_id0/helper_functions.h,extension/fake_id0/unlink.c | PORTED 646f5fb3 |
| 4a27ca61ff | fake_id0: move records once the kernel has renamed the file | 3 | extension/fake_id0/fake_id0.c,extension/fake_id0/rename.c,extension/fake_id0/rename.h | PORTED 646f5fb3 |
| 9bd53079d2 | seccomp: run fork(2) as clone(2) when the outer policy rejects it | 1 | tracee/seccomp.c | PORTED 465f5fcf |
| e2d843ee6d | seccomp: give the child of fork(2) run as clone(2) its registers back | 6 | tracee/event.c,tracee/reg.c,tracee/reg.h,tracee/seccomp.c,tracee/tracee.c,tracee/tracee.h | PORTED 465f5fcf |
| 8bf810ccc4 | compat: fix build with glibc 2.43 | 2 | compat.h,tracee/event.c | PARCIAL-BIONIC |
| 032f82d579 | link2symlink: decrement link counts once the kernel removed the name | 1 | extension/link2symlink/link2symlink.c | PORTED 009340ab |
| bd16a7d6b7 | link2symlink: ignore syscalls whose translation failed | 1 | extension/link2symlink/link2symlink.c | PORTED 009340ab |
| bffb58da8b | link2symlink: report the error a faked hard link failed with | 1 | extension/link2symlink/link2symlink.c | PORTED 009340ab |
| 87c4109a52 | link2symlink: leave broken faked hard links to the kernel | 1 | extension/link2symlink/link2symlink.c | PORTED 009340ab |
| 03aea83617 | link2symlink: reserve the intermediate before moving the file | 1 | extension/link2symlink/link2symlink.c | PORTED 009340ab |
| 05a93edfc8 | link2symlink: read link counts of 10000 and more | 1 | extension/link2symlink/link2symlink.c | PORTED 009340ab |
| fa47780ce5 | link2symlink: let 9999 files of the same name be linked | 1 | extension/link2symlink/link2symlink.c | PORTED 009340ab |
| 1ce2525b3c | link2symlink: give faked hard links the d_type of their file | 1 | extension/link2symlink/link2symlink.c | PORTED 009340ab |
| 355f840ea5 | hidden_files: leave failed getdents(2) alone | 1 | extension/hidden_files/hidden_files.c | PORTED a4401803 |
| b8b3e6280b | hidden_files: parse the getdents(2) records of 32-bit tracees | 1 | extension/hidden_files/hidden_files.c | PORTED a4401803 |
| 968ae1be84 | link2symlink: link symbolic links as symbolic links | 1 | extension/link2symlink/link2symlink.c | PORTED 009340ab |

Total: 47 commits pendientes sobre `7266fb3e85`.
