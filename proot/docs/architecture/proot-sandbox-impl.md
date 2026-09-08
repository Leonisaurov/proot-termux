# Hermes sandbox integration for proot-termux

## Alcance

Hermes usa este fork como backend Termux con binds de filesystem, aislamiento de
procesos y red virtual opcional. Este documento describe la arquitectura
implementada; no es una propuesta de una CLI futura.

La frontera sigue siendo la de PRoot: reduce exposición y media operaciones,
pero no equivale a una VM ni a un namespace completo del kernel. El consumidor
debe probar el binario real, el kernel y los permisos Android de la plataforma.

## Red virtual

`--proxy NAME` traduce sockets guest a endpoints Unix abstractos. Dos procesos
con el mismo nombre comparten la red virtual; nombres distintos quedan
separados. Sin `-p` no se abre un puerto TCP real. `-p HOST:GUEST` es una
exposición explícita hacia la red host y se valida antes de iniciar el bridge.

Sin proxy, el comportamiento de red es directo y compatible. No debe
presentarse como aislamiento.

## Política y control PRCT

La política estática usa `--net-policy`, `--net-allow`, `--net-deny`,
`--net-allow-bind` y `--net-deny-bind`. Las reglas deny prevalecen. Para
mediación dinámica, `--control-fd FD` instala el protocolo PRCT v1 sobre un
socketpair Unix full-duplex. El canal es independiente del proxy: puede mediar
filesystem sin red virtual; con proxy también recibe solicitudes de red.

El canal envía `HELLO` antes del guest y puede producir:

- `PATH_ACCESS_REQUEST` para accesos externos al filesystem;
- `NET_ACCESS_REQUEST` para sockets, bind, connect y publicación;
- `SHADOW_EVENT` para cambios de shadows;
- `COMMAND_RESULT` para comandos del controlador.

El harness responde por `request_id`. Frames incompletos, inválidos, EOF,
timeout o desincronización fallan cerrados y dejan el canal inutilizable hasta
instalar explícitamente otro FD.

Los wildcards de red que Hermes añade en PRCT son sólo hand-off: permiten que
un destino desconocido llegue al harness antes del bloqueo estático. No son
aprobaciones y nunca sustituyen una respuesta `ALLOW`.

## Endpoints Unix

La política reconoce `AF_UNIX` pathname y abstract. El frame PRCT conserva el
tipo, la longitud exacta y los bytes del nombre; los nombres abstractos no se
tratan como strings ni como paths, y los paths host nunca se exponen. Las
operaciones address-bearing soportadas incluyen `bind`, `connect`, `sendto`,
`recvfrom` y las variantes `socketcall` disponibles en la arquitectura.

En modo directo o con `--net-policy off`, el comportamiento Unix conserva la
compatibilidad del kernel. En modo proxy seguro, un endpoint Unix requiere un
control fd válido y una decisión del harness; sin él se deniega. Los sockets
anónimos y los endpoints internos del control plane no deben confundirse con
autorización del guest. La especificación binaria está en
[`control-api/PROTOCOL.md`](../../control-api/PROTOCOL.md).

## Contrato de integración Hermes

Hermes debe:

1. usar `--proxy NAME` sólo para el perfil de red seguro o una red virtual
   solicitada explícitamente;
2. mantener `direct` como compatibilidad y no como aislamiento;
3. activar `--control-fd` sólo cuando el harness esté listo para responder;
4. separar la política de filesystem de la política de red;
5. reiniciar sesiones persistentes cuando cambie la tabla de binds;
6. no añadir `-p` implícitamente.

La guía operativa de Hermes documenta `compat`/`restricted`, `direct`/`secure`
y los límites de PRCT. Las regresiones viven en:

- `tests/proot/networking/test_unix_socket_scope.sh`
- `tests/proot/networking/test_control_fd.sh`
- `tests/proot/networking/test_net_policy.sh`
- `tests/proot/networking/test_accept_matrix.sh`

## Trabajo futuro separado

Las siguientes mejoras no forman parte del contrato actual y requieren diseño y
pruebas independientes:

- completar cobertura de interfaces Android específicas por kernel;
- ampliar diagnósticos de bridges y registros de red virtual;
- medir dependencias para sustituir gradualmente el bind raíz en un rootfs
  mínimo;
- añadir nuevas superficies de mediación sólo con cambios de protocolo y
  pruebas reales.
