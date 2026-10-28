# USAGE.md — proot-termux: Guía de Uso Práctico

## Quick Start

```bash
# Alpine rootfs (mínimo)
proot -0 -r /path/to/alpine-rootfs -w /root /bin/sh

# Ubuntu rootfs con binds estándar
proot -0 -r /path/to/ubuntu-rootfs -b /dev -b /sys -b /proc -w /root /bin/sh

# Con alias -R (recomendado: rootfs + binds automáticos de /etc)
proot -R /path/to/alpine-rootfs
```

**NUNCA** ejecutes proot sin `env -i` en rootfs Alpine — el entorno heredado causa execve failures.

## Aislamiento (como termux-isolated)

termux-isolated usa proot para crear sandboxes con aislamiento de procesos. Los flags de proot-termux permiten hacer esto directamente:

### Nivel 1: Básico (solo rootfs aislado)

```bash
# Sandbox básico: rootfs aislado, sin ver procesos del host
proot --kill-on-exit --link2symlink -L \
  --change-id=0:0 \
  --rootfs=/path/to/alpine \
  --cwd=/root \
  --bind=/dev --bind=/sys --bind=/proc \
  /bin/sh
```

### Nivel 2: Aislamiento de procesos (--proc-isolation)

```bash
# Oculta procesos del host en /proc, bloquea ptrace y kill a PIDs host
proot --kill-on-exit --link2symlink -L \
  --change-id=0:0 \
  --proc-isolation \
  --rootfs=/path/to/alpine \
  --cwd=/root \
  --bind=/dev --bind=/sys --bind=/proc \
  /bin/sh
```

Esto equivale a `--proc-isolated --ptrace-isolated`. El guest solo ve sus propios procesos en `/proc`.

La vista es estricta: el listado usa una whitelist y los archivos globales y
por proceso soportados (`status`, `stat`, `limits`, `maps`, `mountinfo`,
`attr/current`, `io`, `sched`, `pagemap`, `fdinfo`, entre otros) se sintetizan
sin copiar estadísticas ni topología del host. También se cubren
`/proc/self`, `/proc/thread-self`, enlaces `exe`/`cwd`, rutas no canónicas,
`openat` relativo y el estado de FDs duplicados. Las entradas sensibles como
`/proc/net`, `/proc/sys`, `/proc/kcore`, `/proc/keys` y `/proc/kmsg` no se
publican.

### Verificación local y rutas guest

El build e instalación local deben hacerse con el script oficial:

```bash
./scripts/build-native.sh -i
```

Pruebas mínimas mediante el sandbox del repositorio:

```bash
./bin/termux-isolated --termux-paths -- sh -c 'ls /proc; readlink /proc/self/exe'
./bin/termux-isolated -- sh -c 'head -3 /proc/self/mountinfo; head -3 /proc/self/maps'
./bin/termux-isolated --termux-paths --no-proc-isolated -- sh -c 'head -1 /proc/self/mountinfo'
```

En `--termux-paths`, `/data/data/com.termux/...` es una ruta guest válida y
debe aparecer. Sin esa opción, el rootfs muestra rutas guest como `/usr` y
`/home`, bloquea las rutas Termux, almacenamiento y externas, y puede conservar
rutas permitidas del runtime Android, como `/system/etc/hosts`. Esto
proporciona aislamiento de la raíz guest y de los enlaces procfs, no un namespace
completo del filesystem Android. La vista estricta es **opt-in**: hay que pasar
`--proc-isolated` explícitamente a `./bin/termux-isolated` para activarla.
`--no-proc-isolated` es un opt-out explícito que restaura la vista procfs
anterior, incluida la posibilidad de ver datos procfs host; se usa solo para
comparar compatibilidad y es el estado por defecto del launcher.

### Nivel 3: Aislamiento granular

```bash
# Solo ocultar procesos en /proc (ptrace funciona dentro del sandbox)
proot --kill-on-exit --link2symlink -L \
  --change-id=0:0 \
  --proc-isolated \
  --rootfs=/path/to/alpine \
  --cwd=/root \
  --bind=/dev --bind=/sys --bind=/proc \
  /bin/sh

# Solo bloquear ptrace al host (proc visible pero no hijackeable)
proot --kill-on-exit --link2symlink -L \
  --change-id=0:0 \
  --ptrace-isolated \
  --rootfs=/path/to/alpine \
  --cwd=/root \
  --bind=/dev --bind=/sys --bind=/proc \
  /bin/sh
```

### Nivel 4: Aislamiento máximo (todos los flags)

```bash
proot --kill-on-exit --link2symlink -L \
  --change-id=0:0 \
  --proc-isolation \
  --bpf-isolated --perf-isolated --handle-isolated \
  --kexec-isolated --swap-isolated --ioport-isolated \
  --rootfs=/path/to/alpine \
  --cwd=/root \
  --bind=/dev --bind=/sys --bind=/proc \
  /bin/sh
```

### Flags de aislamiento disponibles

| Flag | Qué bloquea | Error retornado |
|------|-------------|-----------------|
| `--proc-isolation` | /proc + ptrace + kill + pidfd_open | ENOENT / ESRCH |
| `--proc-isolated` | /proc + kill a PIDs host | ENOENT / ESRCH |
| `--ptrace-isolated` | ptrace + process_vm a PIDs host | ESRCH |
| `--bpf-isolated` | bpf() syscall | ENOSYS |
| `--perf-isolated` | perf_event_open() | ENOENT |
| `--handle-isolated` | open_by_handle_at() | EOPNOTSUPP |
| `--kexec-isolated` | kexec_load() | 0 (void) |
| `--swap-isolated` | swapon()/swapoff() | ENOSYS |
| `--ioport-isolated` | iopl()/ioperm() | 0 (void) |
| `--reboot-isolated` | reboot() | mata todo + re-exec |

## Virtual Networking (--proxy)

La red virtual usa Abstract Unix Domain Sockets — sin TCP/IP real, sin permisos root.

### Concepto

```
Guest app → socket(AF_INET) → proot traduce → @proot-vnet-{name}-{port}-{token}
                                                         ↕
                                              Registry compartido (flock)
                                                         ↕
                                              Otro proot con mismo --proxy
```

### Ejemplo básico: dos apps que se comunican

```bash
# App A: servidor web en el puerto 8080
proot --kill-on-exit --link2symlink -L \
  --change-id=0:0 \
  --proxy myserv \
  --rootfs=/path/to/alpine \
  --cwd=/root --bind=/dev --bind=/sys --bind=/proc \
  /bin/sh -c "python -m http.server 8080 &"

# App B: cliente que conecta al servidor (mismo --proxy NAME)
proot --kill-on-exit --link2symlink -L \
  --change-id=0:0 \
  --proxy myserv \
  --rootfs=/path/to/alpine \
  --cwd=/root --bind=/dev --bind=/sys --bind=/proc \
  /bin/sh -c "curl localhost:8080"
```

**Regla clave**: ambos proot DEBEN usar el mismo `--proxy NAME` para comunicarse.

### Con bridge TCP real (-p)

```bash
# Bridge: traduce tráfico TCP real a la red virtual
proot --kill-on-exit --link2symlink -L \
  --change-id=0:0 \
  --proxy myserv \
  -p 8080:8080 \
  --rootfs=/path/to/alpine \
  --cwd=/root --bind=/dev --bind=/sys --bind=/proc \
  /bin/sh -c "python -m http.server 8080"

# Ahora localhost:8080 desde el host llega al servidor en el guest
curl http://localhost:8080
```

### Aislamiento de red

```bash
# Diferente --proxy NAME = red separada (no se ven entre sí)
proot --proxy red_a ... /bin/sh -c "python -m http.server 8080" &
proot --proxy red_b ... /bin/sh -c "curl localhost:8080"  # FALLA: red separada

# Sin --proxy = no participa en red virtual
proot ... /bin/sh -c "curl localhost:8080"  # FALLA: no hay red virtual
```

### DNS a través de proxy

```bash
# Servidor DNS en red "web"
proot --proxy web --rootfs=... /bin/sh -c "dnsmasq &"

# Cliente en misma red resuelve
proot --proxy web --rootfs=... /bin/sh -c "curl example.com"

# Cliente en red diferente NO resuelve
proot --proxy web2 --rootfs=... /bin/sh -c "curl example.com"  # FALLA
```

### Alcance de `--net-policy`

La política estática y el protocolo PRCT v1 clasifican destinos `AF_INET` y
`AF_INET6`. También reconocen `AF_UNIX` pathname y abstracto como endpoints
separados: el tipo, la longitud exacta y los bytes del nombre viajan en
`NET_ACCESS_REQUEST`; los paths host nunca se exponen.

Con `--net-policy deny` y un proxy, los endpoints Unix del guest se deniegan
salvo que exista un `--control-fd` válido y el harness devuelva una decisión de
allow. En ese modo, `bind`, `connect` y las operaciones address-bearing
soportadas por el fork pueden llegar a PRCT; `socketcall` usa la misma política.
Sin proxy o con `--net-policy off`, se conserva el comportamiento directo
compatible. Un wildcard de `--net-allow` en PRCT sólo entrega el evento al
harness: no autoriza por sí mismo.

Los sockets anónimos y los sockets internos del control plane no deben
confundirse con autorización para el guest. `SO_PEERCRED` sólo identifica al
endpoint y no concede privilegios. La cobertura real está en:

- `tests/proot/networking/test_unix_socket_scope.sh`
- `tests/proot/networking/test_control_fd.sh`
- `tests/proot/networking/test_accept_matrix.sh`

En una red virtual, los sockets guest se implementan sobre `AF_UNIX`. En
Android, `accept()` y `accept4()` conservan la dirección peer `AF_UNIX` real:
la envoltura Bionic valida que la familia devuelta coincida con la familia del
FD aceptado y rechaza una dirección `AF_INET` sintética con `EMSGSIZE`. Los
resultados de `getsockname()` y `getpeername()` siguen usando la representación
virtual de la familia guest. Los listeners publicados con `-p` conservan la misma
semántica del peer `AF_UNIX` en Android.

## Resource Limits

Los límites se aplican en dos lados distintos del tracer y no todos los flags
tocan los dos:

| Flag | Lado | Qué hace | Rango válido |
|---|---|---|---|
| `--cpu-limit N` | host + guest | `sched_setaffinity()` sobre el proceso proot (primeras N CPUs) y el guest percibe N cores porque se intercepta `sched_getaffinity()` | 1..número real de CPUs del host |
| `--single-core` | host + guest | alias de `--cpu-limit 1` | — |
| `--nice N` | host | `setpriority()` sobre el proceso proot; los tracees lo heredan por fork/exec | 0..19 (subir prioridad exige root) |
| `--fd-limit N` | host | `prlimit64(RLIMIT_NOFILE)` sobre el proceso proot; los tracees lo heredan | >= 32 |
| `--mem-limit N[KMG]` | guest | `prlimit64(RLIMIT_AS)` sobre cada tracee **después** de su `execve` | >= 16 MiB |
| `--proc-limit N` | guest | cuenta los tracees vivos de esta instancia y responde `-EAGAIN` a `fork`/`clone`/`vfork` al alcanzar N | >= 1 |
| `--resource-isolated` | host + guest | combinación de `--cpu-limit 1` y `--nice 10`; no impone límites de memoria, FDs ni procesos | — |

Notas de implementación:

- `--mem-limit` **no** se aplica al proceso proot: su espacio virtual ronda los
  10 GiB en Android y un `RLIMIT_AS` lo mataría. Se aplica a cada tracee tras
  su `execve`, cuando el VSZ todavía es pequeño; es el guest quien recibe
  `ENOMEM` si crece de más. El sufijo acepta `K`, `M` y `G` (potencias de
  1024).
- `--proc-limit` **no** usa `RLIMIT_NPROC` del host (afectaría a todo el UID de
  Termux): el gate vive en la extensión `resource_limit`, que intercepta
  `fork`/`clone`/`vfork` en el flanco de entrada de la syscall y cuenta sólo
  los procesos y hilos de esta instancia.
- Si se combinan `--resource-isolated` con un `--cpu-limit` o un `--nice`
  explícitos, **gana la última opción** de la línea de comandos.

### Límite de CPU

```bash
# Limitar a 2 cores
proot --cpu-limit 2 --rootfs=... /bin/sh

# Un solo core (alias)
proot --single-core --rootfs=... /bin/sh

# Modo resourceId: 1 core + nice 10
proot --resource-isolated --rootfs=... /bin/sh
```

### Límite de memoria (guest-side)

```bash
# Cada tracee máximo 256 MB de VSZ
proot --mem-limit 256M --rootfs=... /bin/sh

# Mínimo 16 MiB (por debajo se rechaza)
proot --mem-limit 8M --rootfs=...  # ERROR: mínimo 16 MiB
```

**Nota**: `--mem-limit` aplica RLIMIT_AS al guest después de execve, NO al proceso proot (que tiene VSZ ~10 GiB en Android).

### Límite de procesos

```bash
# Máximo 50 procesos en el sandbox
proot --proc-limit 50 --rootfs=... /bin/sh

# Fork más allá del límite retorna EAGAIN (error natural)
```

### Límite de FDs

```bash
# Máximo 256 file descriptors
proot --fd-limit 256 --rootfs=... /bin/sh
```

### Nice (prioridad)

```bash
# Prioridad baja (0=normal, 19=mínima)
proot --nice 10 --rootfs=... /bin/sh
```

### Combinación típica

```bash
# Sandbox contenido: 1 core, nice 10, 512M RAM, 50 procesos
proot --single-core --nice 10 --mem-limit 512M --proc-limit 50 \
  --rootfs=... /bin/sh
```

## Supervise & Exec

`--supervise` mantiene el event loop vivo después de que el root tracee sale. `--exec` ejecuta comandos dentro del mismo contexto.

Con `--supervise` el event loop deja de usar el `waitpid()` bloqueante de
upstream y pasa a `signalfd` + `poll()`: un descriptor para `SIGCHLD` y otro
para el socket de control. El supervisor escucha en un socket Unix **abstracto**
(el primer byte de `sun_path` es cero, así que no existe entrada en el
filesystem) con el nombre `@proot-exec-<PID>`, donde `<PID>` es el PID del
propio supervisor.

`--exec <PID> <cmd>` conecta a ese socket, envía el comando junto con
stdin/stdout/stderr mediante `SCM_RIGHTS` y espera al tracee hijo. El
supervisor reutiliza su rootfs, sus binds, su proxy y sus extensiones; el
`cwd` explícito del cliente (`-w`/`--cwd`/`--pwd`) se propaga al hijo y se
resuelve contra el rootfs del supervisor, no contra el del cliente. El hijo
hereda `seccomp = DISABLED` porque no pasó por el filtro BPF del kernel: sin
eso, `restart_tracee()` esperaría eventos de seccomp que nunca llegan y las
syscalls no se interceptarían.

Sin `--supervise` el loop es idéntico al de upstream: cero overhead.

### Patron daemon + clientes

```bash
# Terminal 1: iniciar supervisor (se queda vivo)
proot --kill-on-exit --link2symlink -L \
  --change-id=0:0 \
  --supervise \
  --rootfs=/path/to/alpine \
  --cwd=/root --bind=/dev --bind=/sys --bind=/proc \
  /bin/sh -c "echo 'Supervisor listo'; sleep infinity" &
SPID=$!

# Terminal 2: ejecutar comandos en el mismo contexto
proot --kill-on-exit --link2symlink -L \
  --change-id=0:0 \
  --rootfs=/path/to/alpine \
  --cwd=/root --bind=/dev --bind=/sys --bind=/proc \
  --exec $SPID /bin/sh -c "echo 'Hello from client'; ls /"

# Terminal 3: otro cliente
proot ... --exec $SPID /bin/sh -c "ps aux"

# Matar el supervisor
kill $SPID
```

### Con proxy + supervise

```bash
# Supervisor con red virtual
proot --kill-on-exit --link2symlink -L \
  --change-id=0:0 \
  --proxy mynet \
  --supervise \
  --rootfs=/path/to/alpine \
  --cwd=/root --bind=/dev --bind=/sys --bind=/proc \
  /bin/sh -c "python -m http.server 8080" &

# Cliente en la misma red
proot --kill-on-exit --link2symlink -L \
  --change-id=0:0 \
  --proxy mynet \
  --rootfs=/path/to/alpine \
  --cwd=/root --bind=/dev --bind=/sys --bind=/proc \
  --exec $SPID /bin/sh -c "curl localhost:8080"
```

### Logs

Los logs del supervisor se guardan en:
```
${PROOT_RUNTIME_DIR:-$TMPDIR}/proot-exit-<PID>.log
```

`PROOT_RUNTIME_DIR` debe ser un directorio host escribible; si no está
definido, proot usa `TMPDIR`. El launcher `termux-isolated` configura ambos en
`$PREFIX/tmp` para sus procesos.

Contenido: `process 'x' exited with status N (started Ns)` o
`process 'x' killed by signal N (started Ns)`.

### Comportamiento conocido

- Cuando el root tracee sale, el supervisor registra el motivo, cierra el
  socket de control y termina en cuanto no quedan clientes `--exec` pendientes
  ni tracees vivos. **No queda vivo en background**: con `--supervise` y sin
  `--exec` se apaga al salir el root tracee.
- `SIGTERM` e `SIGINT` se ignoran (el event loop los instala como `SIG_IGN`).
  Sólo `SIGKILL` —que no se puede capturar— o la salida natural del root tracee
  detienen el supervisor; para terminarlo hay que usar `kill -9`.
- El cliente se autentica por UID: `SO_PEERCRED` debe coincidir con el UID del
  supervisor, y un cliente de otro UID se descarta sin ejecutar nada.

### Límites conocidos

- La creación inicial de tracees del supervisor **no pasa** por el gate de
  `--proc-limit`: ese gate cuenta los tracees vivos de la instancia y se
  evalúa en el flanco de entrada de `fork`/`clone`/`vfork`, un camino que los
  tracees creados por el propio supervisor no atraviesan. Los clientes
  `--exec` sí se comprueban antes de crear su tracee: si el límite está
  alcanzado, el supervisor rechaza el cliente con `EAGAIN` y no se crea el
  proceso.
- Los flags del cliente `--exec` **no** se propagan al supervisor. El cliente
  sólo transmite el comando y, si lo pidió de forma explícita, el `cwd`. El
  contexto (rootfs, binds, proxy, extensiones y `--mem-limit`) lo decide el
  supervisor.

## Bind Permissions

```bash
# Solo lectura
proot -b /data/data/com.termux/files/home:/data:ro --rootfs=... /bin/sh

# Solo escritura
proot -b /data/data/com.termux/files/home:/data:wo --rootfs=... /bin/sh

# Lectura + escritura (default)
proot -b /data/data/com.termux/files/home:/data:rw --rootfs=... /bin/sh

# Default: /etc/ es :ro (con --recommended-etc-rw se restaura RW legacy)
proot --recommended-etc-rw --rootfs=... /bin/sh
```

## Visibilidad de rutas y emulación de permisos

### Permisos emulados (`--fake-permissions`)

```bash
# Emula permisos sin chmod real (no-op en override_permissions)
proot --fake-permissions --rootfs=... /bin/sh
```

Útil para apps que verifican permisos via `access()` pero no necesitan `chmod` real.

Con `--fake-permissions` la extensión `fake_id0` no llama a `chmod()` sobre los
ficheros del host: los permisos se emulan en las respuestas de `stat` y de
`access`/`faccessat`/`faccessat2`, que devuelven éxito. Así no quedan cambios
de permisos persistidos tras un `SIGKILL` y no se tocan los binds `:ro`. El
contrapartida es que `open()` puede seguir fallando con `EACCES` si el usuario
del host no puede leer realmente el fichero.

### `/etc` en modo lectura-escritura (`--recommended-etc-rw`)

```bash
# Por defecto los binds recomendados de /etc/* son :ro
proot -R /path/to/alpine --rootfs=... /bin/sh

# Restaura el comportamiento legacy (lectura y escritura)
proot --recommended-etc-rw -R /path/to/alpine --rootfs=... /bin/sh
```

`--recommended-etc-rw` sólo cambia el modo de los binds recomendados que
empiezan por `/etc/`; el resto conserva el suyo. Úsalo con cuidado: el guest
puede sobrescribir `/etc/passwd` o `/etc/hosts` del host.

### Ocultar rutas (`--hide`)

```bash
# Instala un shadow oculto recursivo sobre una ruta guest
proot --hide /ruta/secreta --rootfs=... /bin/sh
```

La ruta y sus descendientes dejan de aparecer en el lookup ni en el enumerado
de directorios. El valor debe ser una ruta guest absoluta: un valor relativo
o que contenga `:` se rechaza.

### Nodo visible pero bloqueado (`:mask`)

```bash
# El nodo existe, pero su contenido no se puede leer, escribir, abrir ni atravesar
proot -b /data/data/com.termux/files/home:/data:mask --rootfs=... /bin/sh
```

A diferencia de `:ro`, el modo `mask` conserva la presencia y la metadata
mínima del nodo, pero bloquea la lectura, la escritura, la apertura y el
travesado del contenido. Es el modo que usa `bin/termux-isolated` para tapar el
almacenamiento interno sin borrarlo del árbol guest. Con un `--control-fd`
activo, un shadow puede revelarse con `REVEAL_SHADOW` (alcance `node` o
`recursive`) y volver a ocultarse con `RESTORE_SHADOW`.

## System V IPC (`--sysvipc`)

```bash
# Emula shm/msg/sem de System V dentro del sandbox
proot --sysvipc --rootfs=... /bin/sh
```

La extensión `sysvipc` intercepta y emula dentro de proot las syscalls de
System V: `msgget`/`msgsnd`/`msgrcv`/`msgctl`, `semget`/`semop`/
`semtimedop`/`semctl` y `shmget`/`shmat`/`shmdt`/`shmctl`. Cada instancia de
proot tiene **su propio namespace de IPC**: dos proot distintos no comparten
colas de mensajes, semáforos ni segmentos de memoria compartida.

La memoria compartida se materializa con un proceso helper: proot hace doble
`fork()` y re-ejecuta `/proc/self/exe` con `--shm-helper`. El helper escucha
en un socket Unix temporal (`SOCK_SEQPACKET`), cierra todos los descriptores
heredados salvo stdin/stdout/stderr y sirve las peticiones de asignación y
liberación. En Android el segmento se reserva en `/dev/ashmem`
(`ASHMEM_SET_NAME`/`ASHMEM_SET_SIZE`); fuera de Android, en un fichero
temporal. El guest lo mapea después con un `mmap` encadenado de tipo
`MAP_SHARED` sobre el descriptor que entrega el helper.

Existe además un backend alternativo basado en `libandroid-shmem`, que se
activa compilando con `PROOT_WITH_LIBANDROID_SHMEM` (la macro de C es
`WITH_LIBANDROID_SHMEM`). **La build oficial no lo define**, así que el
comportamiento efectivo es el backend genérico de `mmap`. En tiempo de
ejecución, la variable de entorno `PROOT_DONT_SHARE_LIBANDROID_SHMEM` desactiva
el camino de `libandroid-shmem` si algún día se compila con él.

El guest ve además una vista propia de `/proc/sysvipc/shm`, generada a partir
del namespace de la instancia.

## Compatibilidad memfd (`--ashmem-memfd`)

```bash
# Expone un memfd usable sobre ashmem
proot --ashmem-memfd --rootfs=... /bin/sh
```

`--ashmem-memfd` existe **sólo en Android/Bionic**: todo el código de la
extensión está bajo la guarda `#if defined(__ANDROID__) || defined(__BIONIC__)`,
y la propia opción CLI no se declara fuera de esa plataforma, así que en otro
sistema el flag ni siquiera aparece en la ayuda.

Al arrancar, la extensión comprueba si el kernel soporta `memfd_create`
(`fork()` más una llamada de prueba). Si no lo soporta, reescribe
`memfd_create` como un `openat(AT_FDCWD, "/dev/ashmem", O_RDWR [| O_CLOEXEC],
0)`: el guest recibe un descriptor real y utilizable. Además, `ftruncate` sobre
un descriptor de ashmem se traduce a `ioctl(fd, ASHMEM_SET_SIZE, size)`, y
`fstat`/`fstatat64` sobre un descriptor de ashmem con `st_size` a cero encadena
un `ioctl(fd, ASHMEM_GET_SIZE, 0)` para rellenar el tamaño real. La variable
`PROOT_ASSUME_MEMFD_UNSUPPORTED=1` fuerza el camino de ashmem y `=0` fuerza el
de `memfd_create` nativo, para poder probar los dos en un mismo dispositivo.

## Port Mapping (-p)

```bash
# Mapear host:8080 → guest:80
proot -p 8080:80 --rootfs=... /bin/sh -c "python -m http.server 80"

# Auto-puerto libre si el host está ocupado
proot -p 8080:80 --rootfs=... /bin/sh  # si 8080 ocupado, usa 8081

# Puertos privilegiados: auto-redirect +2000
proot --protect-privileged-ports --rootfs=... /bin/sh -c "python -m http.server 80"
# bind(80) → bind(2080) automáticamente
```

`--port` exige la forma con `=`: `--port=127.0.0.1:8080`. La forma con
espacio (`--port 127.0.0.1:8080`) se rechaza con el error *"option '--port'
and its value must be separated by '='"*, porque la opción está declarada con
separador `=`. La forma corta `-p 8080:80` sí acepta el valor separado por un
espacio.

## Ejemplo Completo: termux-isolated-style

Script que crea un sandbox aislado con red virtual:

```bash
#!/bin/bash
# isolated-server.sh — sandbox con proc isolation + red virtual

ROOTFS="/data/data/com.termux/files/usr/var/lib/proot-distro/containers/alpine/rootfs"
PROOT="/data/data/com.termux/files/usr/bin/proot"

env -i PATH=/bin:/usr/bin \
  $PROOT --kill-on-exit --link2symlink -L \
  --change-id=0:0 \
  --proc-isolation \
  --proxy myapp \
  --mem-limit 256M \
  --proc-limit 50 \
  --rootfs="$ROOTFS" \
  --cwd=/root \
  --bind=/dev --bind=/sys --bind=/proc \
  /bin/sh -c "
    echo '=== Sandbox aislado iniciado ==='
    echo 'PID: \$\$'
    echo 'Proxy: myapp'
    echo 'Proc isolation: activo'
    echo 'Mem limit: 256M'
    echo 'Proc limit: 50'
    echo '==============================='
    python -m http.server 8080
  "
```

Cliente que conecta al servidor:

```bash
#!/bin/bash
# isolated-client.sh — cliente en la misma red virtual

ROOTFS="/data/data/com.termux/files/usr/var/lib/proot-distro/containers/alpine/rootfs"
PROOT="/data/data/com.termux/files/usr/bin/proot"

env -i PATH=/bin:/usr/bin \
  $PROOT --kill-on-exit --link2symlink -L \
  --change-id=0:0 \
  --proxy myapp \
  --rootfs="$ROOTFS" \
  --cwd=/root \
  --bind=/dev --bind=/sys --bind=/proc \
  /bin/sh -c "curl http://localhost:8080"
```

## Pitfalls

- **`env -i` obligatorio** en rootfs Alpine — el entorno heredado causa execve failures.
- **`--proxy` sin `-p`** = red virtual pura (sin acceso TCP real desde el host).
- **`--mem-limit` mínimo 16 MiB** — valores menores se rechazan.
- **`--proc-limit` solo cuenta tracees de ESTE proot** — no afecta al resto del UID.
- **`--supervise` sin `--exec`** = el supervisor se apaga al salir el root tracee.
- **`--sysvipc` crea un namespace de IPC por instancia** — dos proot distintos no comparten shm, colas ni semáforos.
- **`--ashmem-memfd` sólo existe en Android/Bionic** — fuera de esa plataforma el flag no se declara.
- **`--proc-isolated` es opt-in** en `bin/termux-isolated`; sin el flag se conserva la vista procfs anterior.
- **`-q` expone todo el host** en `/host-rootfs` — solo usar con guests confiables.
- **PID 0 en `get_tracee()`** se cambia a `getpid()` — el hash table usa `tracee_hash_update()` para re-indexar. No manually set `tracee->pid` sin actualizar el hash.

## Política de red estática

| Flag | Valor | Efecto |
|---|---|---|
| `--net-policy off\|deny\|allow` | modo base | `off` (el valor por defecto) no media nada; `deny` bloquea por defecto; `allow` permite por defecto |
| `--net-allow <destino>` | IP, CIDR, dominio o `dominio:puerto` | permite el destino |
| `--net-deny <destino>` | ídem | deniega el destino |
| `--net-allow-bind <puerto>` | número de puerto o `*` | permite publicar un listener en ese puerto |
| `--net-deny-bind <puerto>` | ídem | deniega publicar un listener en ese puerto |
| `--control-fd <FD>` | descriptor ya abierto | abre el canal PRCT fail-closed para decisiones de red, de rutas y de shadows |

Precedencia: **una regla `deny` siempre gana**, tanto sobre los destinos como
sobre los puertos de publicación. En modo `deny` sólo pasa lo que coincide con
una regla `allow`; en modo `allow` sólo se bloquea lo que coincide con una
regla `deny`. La publicación de un puerto se valida antes de configurar
`port_switch` o de arrancar el helper.

Orden de evaluación: la política se aplica **antes** de las traducciones de
`--proxy` (red virtual) y de `-p`/`--port` (`port_switch`). Un destino denegado
no deja estado en el registry de la red virtual ni en el helper del bridge.

La red conserva su comportamiento actual por defecto. Para activar la
mediación, usa `--net-policy deny` (bloqueo por defecto) o `--net-policy
allow` (permitido por defecto). Las reglas deny siempre ganan. También se
median `sendto`/`recvfrom` cuando llevan una dirección explícita:

```bash
proot --net-policy deny \
  --net-allow 127.0.0.1:8080 \
  --net-allow-bind 8080 /bin/sh

proot --net-policy allow --net-deny 10.0.0.0/8 /bin/sh

# Publicación: se valida antes de configurar port_switch o arrancar el helper
proot --proxy demo --net-policy deny --net-allow-bind 8080 \
  --port=127.0.0.1:8080 /bin/sh
```

Los destinos aceptan IPv4, IPv6 entre corchetes, CIDR, dominios y un puerto
opcional (`203.0.113.0/24`, `[::1]:8080`, `127.0.0.1:8080`,
`google.com:443`). Los dominios se validan y resuelven con el resolver del
tracer, antes de iniciar el guest; todas sus respuestas A y AAAA se guardan
como IPs fijas durante toda la sesión. El DNS, `/etc/hosts` y cualquier otra
respuesta del guest no puede ampliar ese conjunto. Si el host no puede
resolver el dominio, el arranque falla de forma fail-closed. Un punto final
(`example.com.`) es aceptado. `--net-policy off` no resuelve dominios y sigue
siendo la ruta compatible sin mediación. La mediación cubre `bind`, `listen`,
`connect`, `sendto` y `recvfrom` antes de las traducciones de `--proxy` o `-p`;
la autorización en tiempo de conexión es únicamente por IP fijada.
Para resolver DNS UDP se interceptan además las operaciones auxiliares
`recvmsg`, `ppoll` y `read` únicamente cuando pertenecen a una consulta
sintética pendiente.
Con una política activa, las consultas DNS UDP del guest se contestan de forma
sintética usando únicamente ese snapshot A/AAAA; no se envían al servidor DNS
del guest. Las consultas de dominios no fijados reciben NXDOMAIN y el resultado
solo puede usarse si la IP también satisface las reglas numéricas. Esto evita que
`/etc/hosts`, un DNS alternativo o una respuesta manipulada amplíen el permiso.
`--control-fd FD`
instala un canal bidireccional para un harness externo fail-closed. Usa frames
con magic `PRCT`, versión 1, tipo, tamaño acotado a 4096 bytes y `request_id`.
El encabezado es `u32 magic, u16 version, u16 type, u32 size, u64 request_id`;
los enteros se envían en el endian nativo de la instancia. El harness debe
entregar un `SOCK_STREAM` full-duplex (normalmente un `socketpair(AF_UNIX,
SOCK_STREAM)`). `HELLO` se envía al instalar el FD, incluso antes de arrancar
el guest, para que las decisiones de publicación de `--port` usen el mismo
canal.

Los tiempos del canal: la escritura de un frame y la lectura de su encabezado
y de su payload usan un timeout de 1 s (`CONTROL_TIMEOUT_MS = 1000`). La lectura
de la **decisión** en sí es deliberadamente ilimitada
(`CONTROL_DECISION_TIMEOUT_MS = -1`) para que un harness humano pueda responder
sin que el tracee caduque; EOF, error o una respuesta malformada siguen
denegando la operación. Los consumidores de referencia aplican un deadline de
1 s por frame completo por defecto.

Proot envía `NET_ACCESS_REQUEST`, `PATH_ACCESS_REQUEST`, `SHADOW_EVENT` y
`COMMAND_RESULT`. Las operaciones de path son `READ`, `WRITE`, `CREATE`,
`DELETE`, `RENAME` y `METADATA`; las peticiones incluyen la ruta guest
canónica y, para rename/link, el segundo operando guest cuando corresponde.
Las respuestas a una petición deben conservar su `request_id` y pueden usar
`ALLOW_ONCE`, `ALLOW_ALWAYS`, `DENY_ONCE`, `DENY_ALWAYS` o
`COMMAND_RESULT`. Una respuesta incompleta, inválida, EOF o timeout deniega la
operación. Tras una desincronización el canal queda inutilizado hasta instalar
explícitamente otro FD. El descriptor es `FD_CLOEXEC` y no se hereda al guest.

Los tipos de decisión son `ALLOW_ONCE`, `ALLOW_ALWAYS`, `DENY_ONCE`,
`DENY_ALWAYS` y `FORGET`. Los comandos espontáneos `SET_RULE` permiten
instalar reglas proactivas antes del primer acceso; `GET_STATE` responde con
`COMMAND_RESULT`. `FORGET` elimina únicamente la regla dinámica indicada y
restaura la política estática. Los tipos de shadows son `REVEAL_SHADOW` y
`RESTORE_SHADOW`, con alcance `node` o `recursive`; el alcance por defecto es
`node`. Las decisiones futuras usan rutas guest canónicas y nunca aceptan una
ruta host suministrada por el harness.

`--hide /ruta` instala un shadow oculto recursivo. Para un nodo visible pero
bloqueado puede usarse `--bind SRC:/ruta:mask`; a diferencia de `:ro`, el modo
`mask` conserva la presencia y metadata mínima del nodo, pero bloquea lectura,
escritura, apertura y traversal del contenido. `REVEAL_SHADOW` con alcance
`node` revela solo el nodo; sus shadows descendientes siguen activos. El
alcance `recursive` revela el subárbol y `RESTORE_SHADOW` vuelve al estado
declarado por CLI.
