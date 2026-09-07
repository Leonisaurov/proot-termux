# Pendientes de proot, Android, SELinux y kernel

## Alcance

Este documento conserva únicamente observaciones cuya causa no quedó atribuida a
la capa Hermes. Los resultados son indicios, no pruebas de escape ni de escalada.
Deben reproducirse con la versión exacta de proot-termux, la configuración efectiva
y una comparación directa sin Hermes.

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
Pendientes:

- comparar rootfs guest real frente a `--termux-paths`;
- comprobar si `/proc/self/root` debe quedar limitado al rootfs guest;
- ejecutar el binario proot directamente con la misma raíz y binds;
- documentar el contrato de visibilidad de `/data`, `/system`, `/vendor`, `/sdcard`
  y `/storage` para cada modo.

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

Pendientes:

- repetir `ifconfig`, `ip`, netlink, ioctl y sysfs en varias sesiones idénticas;
- comparar lightweight, PRCT y proot directo;
- separar AF_INET, AF_INET6, AF_NETLINK, AF_PACKET y TUN;
- confirmar si el kernel/SELinux permite alguna operación después de que proot la
  clasifique como denegada.

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

El resultado puede ser una combinación de permisos Android normales y filtrado
incompleto de proot. El fork usa el namespace `/dev/socket` para mecanismos de
control/proxy, por lo que ocultar indiscriminadamente ese directorio puede romper
el handshake. Queda por resolver en la capa de proot/Android:

- si `--net-policy deny` intercepta `connect(AF_UNIX)` con pathname;
- si intercepta sockets Unix abstractos equivalentes;
- si SELinux aplica una autorización adicional después del connect;
- si el peer UID observado representa solo al endpoint y no concede privilegios;
- si existe algún impacto protocolario sin enviar payload no autorizado.

Las pruebas futuras deben limitarse inicialmente a enumeración, connect sin datos
y captura de errno/credenciales. Cualquier prueba de protocolo requiere autorización
explícita y una especificación del servicio.

## 4. Loopback y `accept()`

En una prueba de servidor TCP dentro del guest, el cliente pudo conectar pero
`accept()` falló con:

```text
EMSGSIZE - Message too long
```

Esto podría ser una regresión de traducción de `accept`/`accept4`, una longitud de
sockaddr incorrecta en ARM64 o una interacción entre la virtualización de red y
el kernel Android. No constituye por sí mismo un escape.

Pendientes:

- reproducir con IPv4 e IPv6 y con `accept` y `accept4`;
- comparar sockets loopback directos, sockets bajo el mismo proxy y proot directo;
- registrar familia, longitud de sockaddr y errno en ambos lados;
- comprobar versiones del fork y del kernel donde cambia el comportamiento.

## 5. Proot anidado y límites de sesión

Un intento de proot anidado pudo resolver rutas Android y ejecutar un shell visible,
pero terminó por timeout sin producir root Android real. Podría ser solo una
reconfiguración adicional de la vista host, no una escalada.

Pendientes:

- repetirlo con una raíz guest real y con Termux-paths por separado;
- capturar la línea exacta de ambos procesos proot y sus binds;
- determinar si el timeout procede de proot, del control de procesos o del kernel;
- no interpretar UID guest 0 como UID Android real: la ejecución observada mantuvo
  UID Android 10379 (`u0_a379`) y no mostró capabilities reales.

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
