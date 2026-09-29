# Pendientes de proot, Android, SELinux y kernel

## Alcance

Este documento conserva únicamente observaciones cuya causa no quedó atribuida a
la capa Hermes. Los resultados son indicios, no pruebas de escape ni de
escalada. Deben reproducirse con la versión exacta de proot-termux, la
configuración efectiva y una comparación directa sin Hermes.

**Re-verificado en device el 2026-09-28** (fork local, `--termux-isolated
--termux-paths`, binario construido con `proot/scripts/build-native.sh -i`).
Resultado de la re-verificación: los ítems 4 y 5 ya están cerrados con
regresiones en verde, el 2 no se reproduce, el 1 quedó codificado en una
regresión (que estaba desactivada por un SKIP mal puesto, ya corregido) y el 3
está respondido en el código de `net_policy`. No queda ningún pendiente abierto
en esta lista salvo la prueba de protocolo del ítem 3, que requiere autorización
explícita.

## 1. Rootfs y traducción de rutas

En una sesión Termux-paths se observaron rutas Android visibles, entre ellas:

```text
/data
/data/data
/data/data/com.termux/files/home
/data/data/com.termux/files/usr
/system
/vendor
/sdcard
/storage
```

También resolvieron rutas mediante la vista del root del proceso:

```text
/proc/self/root/data/data/com.termux/files/usr/bin/proot
/proc/self/root/system/etc/hosts
```

Esto puede ser la semántica esperada de un rootfs basado en rutas host, pero debe
confirmarse si proot promete una raíz guest real en este modo. La raíz host visible
en solo lectura no equivale a un namespace de filesystem ni a un rootfs aislado.

**Estado verificado (2026-09-28).** El contrato quedó fijado por
`proot/tests/termux-isolated/proc/test_termux_isolated_root_visibility.sh`:
con `--termux-paths` estricto, las rutas Termux son guest legítimas
(`/data/data/com.termux/files/usr/bin/proot` y su equivalente bajo
`/proc/self/root` se leen), `/home` está bloqueado, `/system/etc/hosts` sigue
legible y `/proc/self/root` es **ilegible**. Con rootfs real se invierte:
rutas Termux bloqueadas, rutas del rootfs legibles.

El test tenía un SKIP mal puesto: comprobaba el rootfs alpine en la cabecera y
salía antes de ejecutar los dos casos de `--termux-paths`, que no necesitan
rootfs. Corregido: sin rootfs corre los 2 casos y reporta `PASS=2 SKIP=2`;
con rootfs, `PASS=4 SKIP=0`. Los casos de rootfs no se ejecutaron en esta
re-verificación porque no hay contenedor alpine instalado en el device.

## 2. Interfaces de red e ioctl

Una ejecución mostró una interfaz `wlan0` con una dirección privada, pero el
resultado no fue estable. En ejecuciones posteriores se observó lo siguiente:

```text
ifconfig -a  -> Permission denied
ip addr      -> solo lo
ip route     -> sin rutas
/proc/net/*  -> no accesible
/sys/class/net -> bloqueado
AF_PACKET    -> Permission denied
/dev/net/tun -> no se pudo abrir
```

También se observó la normalización de un bind a una dirección LAN hacia
`127.0.0.1`. La fuga inicial podría proceder de un ioctl no interceptado, una
regresión del fork, una diferencia entre procesos o una interacción con Android.
No debe considerarse permanente sin reproducción estable.

**Estado verificado (2026-09-28): NO se reproduce.** Con
`--termux-paths --proc-isolated`:

```text
ifconfig -a    -> solo lo, 127.0.0.1 (avisa que /proc/net/dev no existe)
/proc/net      -> ENOENT
/sys/class/net -> EACCES (permisos/SELinux del host)
/dev/net/tun   -> el Open devuelve EACCES (existe como enlace, no se abre)
```

No hay `wlan0` ni dirección LAN visible, y `/proc/net` está bloqueado por ruta.
Se mantiene la conclusión de "no permanente": si reaparece, comparar contra proot
directo con los mismos argumentos antes de atribuirlo al fork.

## 3. Sockets Unix de servicios Android

Se enumeraron nombres bajo `/dev/socket` que parecen corresponder a servicios
Android, incluyendo:

```text
dnsproxyd
fwmarkd
netd
mdns
netdiag
wpa_wlan0
```

Sin enviar payload ni ejecutar protocolos, se pudo conectar en una ejecución a:

```text
/dev/socket/dnsproxyd -> CONNECT_OK
/dev/socket/fwmarkd   -> CONNECT_OK
```

`SO_PEERCRED` informó en ambos casos un peer con UID/GID 0. No se enviaron bytes,
no se hizo una consulta DNS y no se demostró ninguna operación privilegiada.
Otros sockets devolvieron `ENOENT` o `EACCES`.

**Estado verificado (2026-09-28).** La enumeración se confirma: 45 entradas
visibles desde el guest, incluidas `dnsproxyd`, `fwmarkd`, `mdns`, `netdiag` y
`wpa_wlan0`. Las dos preguntas sobre `--net-policy` quedan respondidas en el
código (`extension/net_policy/net_policy.c`, rama AF_UNIX de `check_operation`):

- `connect(AF_UNIX)` con pathname **y** con nombre abstracto siguen la misma
  rama; sólo se excluye el caso UNNAMED.
- La decisión devuelve `-EACCES` únicamente cuando el modo no es `off` **y** hay
  un `--proxy` activo sin control-fd. Es decir: `--net-policy deny` por sí solo
  no filtra AF_UNIX (es IPC local, no red), y por eso los connects a
  `/dev/socket` del guest se comportan como los de cualquier proceso.
- El peer UID/GID 0 es propiedad del socket Android (los daemons corren como
  sistema); no concede privilegio alguno a quien sólo conecta.
- `SO_PEERCRED` en un extremo AF_UNIX no habilita al peer a operar en nombre del
  otro: sin payload no hay impacto protocolario demostrado.

No se ha cambiado nada aquí: era un pendiente de atribución, y la atribución es
"permisos Android normales + AF_UNIX local no interceptado por diseño". Conectar
a un servicio del sistema y enviar payload sigue requiriendo autorización
explícita y una especificación del servicio.

## 4. Loopback y `accept()`

En una prueba de servidor TCP dentro del guest, el cliente pudo conectar pero
`accept()` falló con:

```text
EMSGSIZE - Message too long
```

**Estado verificado (2026-09-28): RESUELTO.** El commit `6bf31a1d4d`
(`fix(proot): preserve Android virtual accept peer addresses`) conservó el
metadata AF_UNIX del peer en Android y añadió
`proot/tests/proot/networking/test_accept_matrix.sh`, que hoy pasa completo:
`PASS=9 FAIL=0` cubriendo directo y con `--proxy`, IPv4 e IPv6, `accept` y
`accept4`, y el listener publicado por `-p`.

La causa del `EMSGSIZE` estaba en Bionic: una dirección peer `AF_INET` sintética
sobre un fd AF_UNIX real provoca `EMSGSIZE`; el listener publicado y el virtual
conservan ahora el peer AF_UNIX real y la familia virtual sólo se muestra por
`getsockname()`/`getpeername()`. Ver `proot/docs/operations/usage.md`.

## 5. Proot anidado y límites de sesión

Un intento de proot anidado pudo resolver rutas Android y ejecutar un shell
visible, pero terminó por timeout sin producir root Android real.

**Estado verificado (2026-09-28): no se reproduce límite alguno en el fork.**
`proot/tests/proot/proc/test_nesting_pthread.sh` pasa hoy:

```text
PASS: pthread/proc/fork/exec nesting depth 1
PASS: pthread/proc/fork/exec nesting depth 2
PASS: pthread/proc/fork/exec nesting depth 3
PASS: nested supervisor accepts an --exec client
```

Se confirma la lectura original: anidar proot es una reconfiguración de la vista
host, no una escalada, y el UID Android real (`u0_a379`) no cambia.

## 6. Matriz de atribución recomendada

Para cada caso, registrar sin secretos:

- versión y checksum del binario proot;
- arquitectura y versión del kernel;
- contexto SELinux del proceso;
- modo lightweight o PRCT;
- raíz, CWD, entorno filtrado y tabla final de binds;
- flags `--proc-*`, `--net-*`, `--proxy` y `--control-fd`.

Repetir cada prueba en esta matriz:

```text
Hermes -> proot
proot directo con los mismos argumentos
proot sin política de red
proot sin aislamiento de procesos
rootfs guest real
modo --termux-paths
```

Un problema que aparezca solo en Hermes apunta a la integración; uno que aparezca
también con proot directo apunta al fork, Android, SELinux o el kernel. Ningún
resultado de esta matriz demuestra por sí solo una escalada de privilegios.

## 7. Observación nueva, no atribuida (2026-09-28)

Con `--proc-isolated`, el propio PID del lector aparece de forma distinta según
la herramienta: `sh`/`ls`/`ps` ven su PID (y los de los tracees hermanos), pero
`python3` que hace `os.listdir("/proc")` no ve ningún PID —ni el suyo—, mientras
que un hijo suyo (`sleep`) sí aparece y es accesible por
`/proc/<pid>/status`. El contrato documentado ("solo `/proc/self` funciona para
el proceso actual") se cumple en ambos casos, así que no es un escape; se anota
como diferencia de comportamiento entre lectores que conviene revisar aparte,
por si en un runtime concreto (Python, Go) la enumeración de `/proc` queda vacía
cuando debería mostrar los tracees del guest.
