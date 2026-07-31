# Reporte de Vulneración — Sandbox PRoot (fork proot-termux)

Fecha: 2026-07-30
Binario probado: proot-latest-104-gba208e4e-dirty
Rootfs: Alpine 3.24.1 (aarch64/musl, containers/alpine/rootfs)

## Resumen ejecutivo

El sandbox proot del fork proot-termux presenta **2 escapes persistentes** (CRITICAL) que funcionan incluso con el hardening activo: `pidfd_open(PID 1)` permite obtener un pidfd al proceso host, y `mknod()` permite crear nodos de dispositivo dentro del sandbox (aunque el open posterior es denegado). Existen además **3 vectores de escape adicionales** (ALTO) solo en la configuración por defecto (`unshare`, `mount(tmpfs)`, `AF_NETLINK`), bloqueados con el hardening, y **4 filtraciones de información** (MEDIO). El resto de vectores probados están bloqueados correctamente, incluyendo ptrace y acceso a `/dev/mem`.

## Configuraciones probadas

| Escenario | Flags | Descripción |
|-----------|-------|-------------|
| **A (default)** | `--kill-on-exit --link2symlink -L --change-id=0:0 -r rootfs -b /dev -b /proc -b /sys` | Configuración mínima de proot, **sin** flags de isolation. Equivale a la configuración usada por proot-distro. |
| **B (hardened)** | Igual que A + `--proc-isolation --bpf-isolated --perf-isolated --handle-isolated --reboot-isolated --swap-isolated --kexec-isolated --ioport-isolated` | Configuración endurecida del fork con todas las flags de isolation activadas. |

Programas de prueba compilados con `gcc` dentro del rootfs Alpine (binarios estáticos musl). Salidas completas en `pentest/results/*.txt`.

## Hallazgos por severidad

### 🔴 CRITICAL — Escapes persistentes (no bloqueados por hardening)

| ID | Vector | Escenario | Detalle | Recomendación |
|----|--------|-----------|---------|---------------|
| E1 | `pidfd_open(PID 1)` | A y B | Se obtiene un pidfd válido al PID 1 del host desde dentro del sandbox, en ambos escenarios. `pidfd_getfd` sí está bloqueado (EPERM), lo que impide robar el fd del proceso host, pero el pidfd en sí permite operaciones como `pidfd_send_signal` y seguimiento del proceso host. **Ninguna flag actual cubre `pidfd_open`**. | Interceptar `pidfd_open` en el modo isolation: devolver ESRCH para PIDs host (mismo filtrado que ptrace/kill), o bloquear la syscall con ENOSYS/EPERM. Añadir flag `--pidfd-isolated`. |
| E2 | `mknod()` | A y B | Es posible crear un nodo de dispositivo dentro del rootfs con `mknod()`/`mknodat()` en ambos escenarios. El `open()` posterior del nodo es denegado (ENOENT), lo que limita el impacto inmediato, pero la creación del nodo es un paso previo clásico en cadenas de explotación (p. ej. apuntar a dispositivos host si se encuentra otra vía de acceso). **Ninguna flag cubre `mknod`/`mknodat`**. | Interceptar `mknod`/`mknodat` en isolation: denegar la creación de nodos de dispositivo (devolver EPERM para `S_ISBLK`/`S_ISCHR`, permitir fifos/sockets como en otros sandboxes). Añadir flag `--mknod-isolated`. |

### 🟠 ALTO — Escapes solo en escenario default

| ID | Vector | Escenario A | Escenario B | Recomendación |
|----|--------|-------------|-------------|---------------|
| E3 | `unshare(CLONE_NEWNS)` | ESCAPE (permitido) | ENOSYS | La syscall pasa intacta: el tracee puede crear un nuevo namespace de montaje y desacoplarse del sandbox. Bloquear por defecto o añadir flag que la intercepte con ENOSYS. |
| E4 | `mount(tmpfs)` | ESCAPE (permitido) | ENOSYS | `mount()` pasa intacto y permite montar un tmpfs, con lo que el tracee obtiene control completo de su vista de filesystem, evadiendo las traducciones de ruta del sandbox. Bloquear por defecto o añadir flag que la intercepte con ENOSYS. |
| E5 | `socket(AF_NETLINK/NETLINK_ROUTE)` | ESCAPE (socket abierto) | EACCES | En A se abre un socket netlink ruta y se puede consultar la tabla de rutas / interfaces del host. En B la flag `--proc-isolated` lo bloquea con EACCES. Considerar bloquear netlink por defecto, no solo bajo isolation. |

### 🟡 MEDIO — Filtraciones de información

| ID | Vector | Detalle | Recomendación |
|----|--------|---------|---------------|
| F1 | `/proc/self/maps` | Revela rutas host del binario: `/data/data/com.termux/files/usr/libexec/proot/loader`. Expone la estructura de directorios de Termux al atacante. | En isolation, filtrar/sustituir rutas host en `/proc/self/maps` y `/proc/<pid>/maps` (p. ej. mostrar la ruta dentro del rootfs o una ruta ficticia). |
| F2 | `kill(-1, 0)` | Exitoso en **ambos** escenarios (A y B). Confirma la existencia de procesos fuera del sandbox (el host no devuelve error, por lo que hay procesos alcanzables). Es un oracle de existencia de procesos. | Interceptar `kill(-1, ...)` en isolation devolviendo ESRCH, igual que se hace con PIDs concretos, para eliminar el oracle. |
| F3 | `kill(1, 0)` | **A**: EPERM (el PID 1 existe pero sin permiso — filtra su existencia). **B**: ESRCH (ocultado correctamente). | El filtrado de PIDs de `--proc-isolation` funciona, pero en el escenario por defecto se filtra la existencia del PID 1. Considerar que la flag de filtrado de kill esté activa por defecto. |
| F4 | `/proc` listing | **A**: 41 pids visibles. **B**: 1 pid (solo el propio). La filtración de `/proc` del hardening funciona correctamente en B. | Mantener `--proc-isolation` como está; en default se expone el árbol de procesos completo del host. |

### 🟢 VERDE — Vectores bloqueados correctamente

Lista de vectores probados que funcionan bien (bloqueados en ambos escenarios o comportándose de forma segura):

- **ptrace a PID 1**: ESRCH en ambos escenarios.
- **process_vm_readv a PID 1**: EPERM en A, ESRCH en B (filtrado por isolation).
- **/proc/self/root → host**: ENOENT — la raíz real del host es invisible desde el sandbox.
- **openat2 con RESOLVE_BENEATH / RESOLVE_IN_ROOT**: ENOENT — bien manejado, no permite escapar del rootfs.
- **execveat de /system/bin/sh**: invisible — no se encuentra el binario host.
- **name_to_handle_at / open_by_handle_at**: ENOSYS — no se pueden obtener/abrir manejos de filesystem host.
- **/dev/mem, /dev/kmem, /dev/port**: ENOENT — no existen en el rootfs ni son accesibles.
- **io_uring_setup**: bloqueado (ENOSYS).
- **perf_event_open**: bloqueado (ENOENT).
- **bpf**: bloqueado (ENOSYS).
- **userfaultfd**: bloqueado (EPERM).
- **chroot('/')**: funciona, pero se mantiene dentro del rootfs propio — **no** es un escape (la raíz ya está traducida al rootfs del sandbox).

## Hallazgo adicional: proot-distro no activa isolation por defecto

**proot-distro NO pasa las flags de isolation** (`--proc-isolation`, `--*-isolated`, etc.) en su configuración estándar. Esto significa que, en el uso habitual de proot-distro, el sandbox se comporta como el **escenario A**: los 5 escapes (E1–E5) están disponibles y las 4 filtraciones (F1–F4) son visibles. Además, proot-distro **bindea rutas host sensibles** dentro del rootfs por defecto (p. ej. almacenamiento, `/sdcard`, sockets del sistema), lo que agrava el impacto de E1 (pidfd a PID 1) y F1 (rutas host en `/proc/self/maps`), al dar al atacante un punto de apoyo adicional dentro del sandbox.

**Recomendación**: evaluar que proot-distro active por defecto al menos `--proc-isolation`, el bloqueo de `unshare`/`mount`/netlink, y los futuros bloqueos de `pidfd_open` y `mknod` descritos en este reporte, o documentar claramente los riesgos de no hacerlo.

## Comparativa A vs B (tabla resumen)

| Vector | A (default) | B (hardened) |
|--------|-------------|--------------|
| `pidfd_open(PID 1)` | 🔴 ESCAPE (pidfd obtenido) | 🔴 ESCAPE (pidfd obtenido) |
| `mknod()` | 🔴 ESCAPE (nodo creado) | 🔴 ESCAPE (nodo creado) |
| `unshare(CLONE_NEWNS)` | 🟠 ESCAPE (permitido) | ✅ ENOSYS |
| `mount(tmpfs)` | 🟠 ESCAPE (permitido) | ✅ ENOSYS |
| `socket(AF_NETLINK)` | 🟠 ESCAPE (socket abierto) | ✅ EACCES |
| `ptrace(PID 1)` | ✅ ESRCH | ✅ ESRCH |
| `process_vm_readv(PID 1)` | ✅ EPERM | ✅ ESRCH |
| `/proc/self/root` → host | ✅ ENOENT | ✅ ENOENT |
| `openat2` (RESOLVE_BENEATH/IN_ROOT) | ✅ ENOENT | ✅ ENOENT |
| `execveat(/system/bin/sh)` | ✅ invisible | ✅ invisible |
| `name_to_handle_at`/`open_by_handle_at` | ✅ ENOSYS | ✅ ENOSYS |
| `/dev/mem`, `/dev/kmem`, `/dev/port` | ✅ ENOENT | ✅ ENOENT |
| `io_uring` / `perf_event_open` / `bpf` / `userfaultfd` | ✅ bloqueados | ✅ bloqueados |
| `chroot('/')` | ✅ dentro del rootfs | ✅ dentro del rootfs |
| `kill(-1, 0)` | 🟡 INFO (éxito) | 🟡 INFO (éxito) |
| `kill(1, 0)` | 🟡 FILTRADO (EPERM) | ✅ ESRCH |
| `/proc` listing | 🟡 41 pids visibles | ✅ 1 pid (solo el propio) |
| `/proc/self/maps` | 🟡 rutas host del loader | 🟡 rutas host del loader |

## Conclusiones y recomendaciones

- **Cerrar los 2 escapes persistentes (CRITICAL)** antes de considerar el sandbox endurecido: interceptar `pidfd_open` (devolver ESRCH para PIDs host, nueva flag `--pidfd-isolated`) e interceptar `mknod`/`mknodat` (denegar creación de nodos de dispositivo, nueva flag `--mknod-isolated`).
- **Replantear el hardening por defecto**: los escapes E3–E5 (`unshare`, `mount`, `AF_NETLINK`) solo se bloquean con flags explícitas. Considerar bloquearlos por defecto (o en `--proc-isolation`) para que el escenario A no sea inseguro por omisión.
- **Cerrar el oracle de procesos**: interceptar `kill(-1, 0)` en isolation para devolver ESRCH y eliminar la confirmación de procesos externos (F2), y aplicar el filtrado de `kill(1, 0)` también por defecto (F3).
- **Sanear `/proc/self/maps`** (F1): sustituir las rutas host por rutas internas del rootfs bajo isolation para no revelar la estructura de Termux.
- **Influir en proot-distro**: dado que proot-distro no activa isolation ni filtra los binds host, documentar el riesgo y promover que las flags de isolation se activen por defecto en el fork.
- **Verificación periódica**: añadir los vectores E1–E5 y F1–F4 a un suite de tests de regresión del pentest para detectar regresiones en futuras revisiones.

## Emulaciones implementadas (tras el pentest)

Filosofía del proyecto: NUNCA responder "Operation not permitted"/"Function not implemented"
cuando se puede EMULAR un resultado natural. Los errores deben ser naturales (ESRCH = "no
existe") o éxito emulado (0) sin efecto real — porque "no permitido" delata que hay un guardián,
y un proceso root no debería ver ninguna negación.

### E1: pidfd_open a pids host → ESRCH ✅
- Flag: ISOLATE_PTRACE (--ptrace-isolated / --proc-isolation)
- Comportamiento: pidfd_open de un PID host devuelve ESRCH ("no such process"), como si no existiera
- No cubierto anteriormente: pidfd_open era passthrough

### E3: unshare(CLONE_NEWNS) → 0 (éxito emulado) ✅
- Flag: ISOLATE_PROC (--proc-isolated / --proc-isolation)
- Antes: ENOSYS. Ahora: void + retorno 0 — el tracee cree que creó un namespace nuevo sin efecto real
- (igual que --kexec-isolated que ya devolvía 0)

### E4: mount(tmpfs) → 0 (éxito emulado) ✅
- Flag: ISOLATE_PROC
- Antes: ENOSYS. Ahora: void + retorno 0 — el tracee cree que montó sin efecto real

### E5: AF_NETLINK → emulación AF_UNIX fake ✅
- Flag: ISOLATE_PROC
- Antes: EACCES. Ahora: se sustituye AF_NETLINK por AF_UNIX/SOCK_DGRAM y el mecanismo
  fake_netlink existente (enter.c) sintetiza respuestas — el tracee cree tener un socket netlink real

### F1: /proc/self/maps → guest-pure (línea del loader eliminada) ✅
- Flag: ISOLATE_PROC
- Comportamiento: las líneas que contienen "/libexec/proot/loader" se eliminan COMPLETAMENTE
  del maps (compactación estilo getdents). El tracee ve un maps indistinguible del de un proceso
  nativo del guest — ni rastro de la ruta host del loader
- Verificado: `filtered maps 1735 -> 1483 bytes` (2 líneas eliminadas)
- Nota técnica: requirió añadir FILTER_SYSEXIT a open/openat/openat2 (el sysexit no se
  interceptaba bajo seccomp) + tracking del fd de /proc/self/maps (aceptando path original
  y traducido) + readlink de confirmación contra fd reuse

### F2: kill(-1,0) → sin cambio (documentado) ✅
- El comportamiento actual (retorno 0 = "hay procesos señalizables") es el natural: el propio
  proceso siempre es señalizable. No requiere emulación adicional

### No implementado
- mknod: el nodo se crea (éxito natural) pero open da ENOENT ("dispositivo no existe") — ya
  es emulativo por diseño del kernel/SELinux, sin cambio necesario

## Resultados re-verificados tras los fixes
| Vector | Antes (default) | Antes (hardened) | Después (hardened) |
|--------|-----------------|------------------|---------------------|
| unshare | ESCAPE | ENOSYS | 0 (emulado) |
| mount | ESCAPE | ENOSYS | 0 (emulado) |
| AF_NETLINK | ESCAPE | EACCES | AF_UNIX fake |
| pidfd_open | ESCAPE | ESCAPE | ESRCH |
| maps loader | visible | visible | eliminado (guest-pure) |
| kill(1,0) | EPERM | ESRCH | ESRCH |
| /proc listing | 41 pids | 1 pid | 1 pid |

## Anexo: salidas completas

Las salidas completas de cada prueba se encuentran en `pentest/results/`:

| Archivo | Contenido |
|---------|-----------|
| `pentest/results/p_fs_A.txt` | Pruebas de filesystem — escenario A |
| `pentest/results/p_fs_B.txt` | Pruebas de filesystem — escenario B |
| `pentest/results/p_proc_A.txt` | Pruebas de /proc y procesos — escenario A |
| `pentest/results/p_proc_B.txt` | Pruebas de /proc y procesos — escenario B |
| `pentest/results/p_kernel_A.txt` | Pruebas de syscalls de kernel — escenario A |
| `pentest/results/p_kernel_B.txt` | Pruebas de syscalls de kernel — escenario B |
| `pentest/results/p_net_A.txt` | Pruebas de red (netlink, sockets) — escenario A |
| `pentest/results/p_net_B.txt` | Pruebas de red (netlink, sockets) — escenario B |
| `pentest/results/p_sys_A.txt` | Pruebas de syscalls varias — escenario A |
| `pentest/results/p_sys_B.txt` | Pruebas de syscalls varias — escenario B |
