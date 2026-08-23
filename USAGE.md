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
./termux-isolated --termux-paths -- sh -c 'ls /proc; readlink /proc/self/exe'
./termux-isolated -- sh -c 'head -3 /proc/self/mountinfo; head -3 /proc/self/maps'
./termux-isolated --termux-paths --no-proc-isolated -- sh -c 'head -1 /proc/self/mountinfo'
```

En `--termux-paths`, `/data/data/com.termux/...` es una ruta guest válida y
debe aparecer. Sin esa opción, el rootfs debe mostrar rutas como `/usr` y
`/home`, nunca rutas Android del host. `--no-proc-isolated` es un opt-out
explícito que restaura la vista procfs anterior, incluida la posibilidad de
ver datos procfs host; se usa solo para comparar compatibilidad. El wrapper
`./termux-isolated` activa la vista estricta por defecto.

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

## Resource Limits

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
$PREFIX/usr/tmp/proot-exit-<PID>.log
```

Contenido: `process 'x' exited with status N / killed by signal N`

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

## Fake Permissions

```bash
# Emula permisos sin chmod real (no-op en override_permissions)
proot --fake-permissions --rootfs=... /bin/sh
```

Útil para apps que verifican permisos via `access()` pero no necesitan `chmod` real.

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
- **`-q` expone todo el host** en `/host-rootfs` — solo usar con guests confiables.
- **PID 0 en `get_tracee()`** se cambia a `getpid()` — el hash table usa `tracee_hash_update()` para re-indexar. No manually set `tracee->pid` sin actualizar el hash.

## Política de red estática

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
  --port 127.0.0.1:8080 /bin/sh
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
`--net-ask FD`
permite un harness externo fail-closed: usa mensajes nativos de tamaño fijo,
versión 1, request ID y timeout de 1000 ms; una respuesta incompleta, inválida,
EOF o timeout deniega la operación.
