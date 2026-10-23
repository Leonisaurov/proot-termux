# UPSTREAM.md — punto de sincronización y manifiesto del port

Este archivo es la memoria machine-readable del sync con [`termux/proot`](https://github.com/termux/proot).
`proot/scripts/sync_upstream.sh` lee los pines de abajo para saber qué rango queda pendiente,
así que el próximo sync es ejecutar `--status` sin argumentos.

## Pines

```
SYNC_FROM=7266fb3e8516535682f5a9c8f3a7e70f6506eddb
SYNC_FORK_BASE=1f6ef25e5ca8919171cd2ac582fbbd85f03fb027
```

- `SYNC_FROM` — commit upstream ya portado (tag `v5.1.107.92`). Todo lo posterior a este commit está pendiente.
- `SYNC_FORK_BASE` — commit del fork donde empezó esta tanda de port. `--verify` mide contra él si el port
  ha borrado líneas propias; al cerrar el sync se actualiza a la punta nueva del fork.

Punta upstream objetivo: `968ae1be84c69a607dbcd43ee514ab04753d60c5` = `v5.1.107.99` (2026-10-22).

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
| 8f467b0991 | link2symlink: report the faked hard link a program was executed through | 5 | execve/enter.c,execve/execve.h,extension/extension.h,extension/link2symlink/link2symlink.c,path/canon.c | PENDIENTE |
| a396094c7c | link2symlink: name a descriptor opened O_NOFOLLOW after its faked hard link | 1 | extension/link2symlink/link2symlink.c | PENDIENTE |
| 58c3b42849 | syscall: run fork syscalls through a sysexit stop, not PTRACE_CONT | 1 | syscall/enter.c | PENDIENTE |
| 8b9941505d | tracee: register a new child even if its fork event has no PID | 3 | tracee/event.c,tracee/tracee.c,tracee/tracee.h | PENDIENTE |
| d4d2a19081 | seccomp: don't rely on the event message to tell which exits to handle | 3 | syscall/seccomp.c,syscall/seccomp.h,tracee/event.c | PENDIENTE |
| a179d3e8a4 | fake_id0: let the kernel do the real open for files & memfds when the requested read/write mode differs | 1 | extension/fake_id0/fake_id0.c | PENDIENTE |
| 37c09de3b1 | arm64: keep the fixed load addresses out of the way of big mmap(2)s | 1 | arch.h | PENDIENTE |
| bd6d55e122 | Add AGENTS.md | 0 | (sin src) | PENDIENTE |
| c1cd48d5b9 | fake_id0: report 32-bit IDs from getresuid/getresgid | 1 | extension/fake_id0/fake_id0.c | PENDIENTE |
| b5af02e7ae | fake_id0: pass uid_map reads to the kernel; translate SO_PEERCRED across instances | 2 | extension/fake_id0/getsockopt.c,syscall/enter.c | PENDIENTE |
| 7fca80c8a7 | syscall: translate fchmodat2 | 11 | extension/fake_id0/chmod.c,extension/fake_id0/fake_id0.c,syscall/enter.c,syscall/seccomp.c,syscall/sysnums-arm.h,syscall/sysnums-arm64.h,syscall/sysnums-i386.h,syscall/sysnums-sh4.h,syscall/sysnums-x32.h,syscall/sysnums-x86_64.h,syscall/sysnums.list | PENDIENTE |
| 60a9c15ccd | syscall: resolve an empty path at AT_FDCWD against the guest cwd | 2 | syscall/enter.c,tracee/statx.c | PENDIENTE |
| 254202b844 | syscall: translate open_tree, hide the fd-based mount API | 9 | syscall/enter.c,syscall/seccomp.c,syscall/sysnums-arm.h,syscall/sysnums-arm64.h,syscall/sysnums-i386.h,syscall/sysnums-sh4.h,syscall/sysnums-x32.h,syscall/sysnums-x86_64.h,syscall/sysnums.list | PENDIENTE |
| b8c1f5c5c6 | syscall: translate the destination of sendto, sendmsg and sendmmsg | 5 | syscall/enter.c,syscall/exit.c,syscall/seccomp.c,syscall/socket.c,syscall/socket.h | PENDIENTE |
| 9688d01e5a | fake_id0: read unterminated result string by length | 1 | extension/fake_id0/fake_id0.c | PENDIENTE |
| 4a1dbac41f | fake_id0: replay fstat for descriptors that aren't paths | 1 | extension/fake_id0/fake_id0.c | PENDIENTE |
| 7f2fab0fb3 | link2symlink: count faked hard links in the emulated fstat | 1 | extension/link2symlink/link2symlink.c | PENDIENTE |
| 0439248ae4 | sysvipc: back shared memory with memfd where /dev/ashmem is closed | 1 | extension/sysvipc/sysvipc_shm.c | PENDIENTE |
| 04c28b7dd1 | fake_id0: keep the group when chown(2) passes -1 for it | 1 | extension/fake_id0/chown.c | PENDIENTE |
| 77310de75b | fake_id0: apply chmod through /proc/self/fd to the open file's record | 3 | extension/fake_id0/chmod.c,extension/fake_id0/helper_functions.c,extension/fake_id0/helper_functions.h | PENDIENTE |
| 54733e84c3 | fake_id0: apply chmod modes to the real file as well | 1 | extension/fake_id0/chmod.c | PENDIENTE |
| ea0e9b6d41 | fake_id0: record only the permission bits of a chmod mode | 1 | extension/fake_id0/chmod.c | PENDIENTE |
| fdbed740df | fake_id0: record fchmodat2(fd, "", mode, AT_EMPTY_PATH) | 1 | extension/fake_id0/fake_id0.c | PENDIENTE |
| 788c48205b | fake_id0: write the chmod record once the kernel has applied the mode | 4 | extension/fake_id0/chmod.c,extension/fake_id0/chmod.h,extension/fake_id0/config.h,extension/fake_id0/fake_id0.c | PENDIENTE |
| 38a60cbed0 | fake_id0: follow mode changes made through POSIX ACLs | 4 | extension/fake_id0/chmod.c,extension/fake_id0/chmod.h,extension/fake_id0/config.h,extension/fake_id0/fake_id0.c | PENDIENTE |
| 1a41a193e8 | fake_id0: compare statx's group with the real gid, not the uid | 1 | extension/fake_id0/stat.c | PENDIENTE |
| cf393e0ea3 | fake_id0: apply ownership records to statx | 1 | extension/fake_id0/stat.c | PENDIENTE |
| ccd4b745b4 | fake_id0: apply ownership records to fstatat(fd, "", AT_EMPTY_PATH) | 1 | extension/fake_id0/stat.c | PENDIENTE |
| 32d70f7574 | fake_id0: keep the id mapping of fstatat(AT_EMPTY_PATH) on non-files | 1 | extension/fake_id0/stat.c | PENDIENTE |
| 66b112ed38 | fake_id0: move ownership records on renameat2 | 3 | extension/fake_id0/fake_id0.c,extension/fake_id0/rename.c,extension/fake_id0/rename.h | PENDIENTE |
| 53b2159cda | fake_id0: name the record of "dir/" after "dir" | 1 | extension/fake_id0/helper_functions.c | PENDIENTE |
| e8a6c0ebe0 | fake_id0: remove orphaned records along with their directory | 3 | extension/fake_id0/helper_functions.c,extension/fake_id0/helper_functions.h,extension/fake_id0/unlink.c | PENDIENTE |
| 4a27ca61ff | fake_id0: move records once the kernel has renamed the file | 3 | extension/fake_id0/fake_id0.c,extension/fake_id0/rename.c,extension/fake_id0/rename.h | PENDIENTE |
| 9bd53079d2 | seccomp: run fork(2) as clone(2) when the outer policy rejects it | 1 | tracee/seccomp.c | PENDIENTE |
| e2d843ee6d | seccomp: give the child of fork(2) run as clone(2) its registers back | 6 | tracee/event.c,tracee/reg.c,tracee/reg.h,tracee/seccomp.c,tracee/tracee.c,tracee/tracee.h | PENDIENTE |
| 8bf810ccc4 | compat: fix build with glibc 2.43 | 2 | compat.h,tracee/event.c | PENDIENTE |
| 032f82d579 | link2symlink: decrement link counts once the kernel removed the name | 1 | extension/link2symlink/link2symlink.c | PENDIENTE |
| bd16a7d6b7 | link2symlink: ignore syscalls whose translation failed | 1 | extension/link2symlink/link2symlink.c | PENDIENTE |
| bffb58da8b | link2symlink: report the error a faked hard link failed with | 1 | extension/link2symlink/link2symlink.c | PENDIENTE |
| 87c4109a52 | link2symlink: leave broken faked hard links to the kernel | 1 | extension/link2symlink/link2symlink.c | PENDIENTE |
| 03aea83617 | link2symlink: reserve the intermediate before moving the file | 1 | extension/link2symlink/link2symlink.c | PENDIENTE |
| 05a93edfc8 | link2symlink: read link counts of 10000 and more | 1 | extension/link2symlink/link2symlink.c | PENDIENTE |
| fa47780ce5 | link2symlink: let 9999 files of the same name be linked | 1 | extension/link2symlink/link2symlink.c | PENDIENTE |
| 1ce2525b3c | link2symlink: give faked hard links the d_type of their file | 1 | extension/link2symlink/link2symlink.c | PENDIENTE |
| 355f840ea5 | hidden_files: leave failed getdents(2) alone | 1 | extension/hidden_files/hidden_files.c | PENDIENTE |
| b8b3e6280b | hidden_files: parse the getdents(2) records of 32-bit tracees | 1 | extension/hidden_files/hidden_files.c | PENDIENTE |
| 968ae1be84 | link2symlink: link symbolic links as symbolic links | 1 | extension/link2symlink/link2symlink.c | PENDIENTE |

Total: 47 commits pendientes sobre `7266fb3e85`.
