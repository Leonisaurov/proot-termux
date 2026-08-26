# Solicitud de corrección: hand-off PRCT para puertos virtuales cross-proxy

Documento de coordinación para `proot-termux`. Esta incidencia bloquea el
último E2E de integración de Hermes: dos procesos proot con el mismo nombre
de proxy no pueden conectarse entre sí cuando Hermes mantiene
`--net-policy deny` y controla toda la red mediante `--control-fd`.

## Resumen

El fork crea correctamente el registro compartido del proxy y publica el
puerto virtual. Sin embargo, el proceso cliente clasifica el destino como
`VNP_NET_CLASS_UNKNOWN` durante la comprobación estática de `connect(2)`.
`destination_matches()` rechaza cualquier regla cuando la clase es
`UNKNOWN`, por lo que `--net-policy deny` devuelve `EACCES` antes de que
`ask_harness()` emita `NET_ACCESS_REQUEST` a PRCT.

El resultado es que Hermes no puede mostrar la aprobación de red para el
connect cross-proxy aunque el proxy y el registro compartido sean válidos.

## Reproducción local

Entorno Termux:

```text
proot: /data/data/com.termux/files/usr/bin/proot
proxy: un nombre compartido por ambos procesos
flags: --net-policy deny --net-allow-bind '*' --net-allow '*'
       --control-fd FD --proxy NAME
```

1. Lanzar un `ProotPersistentEnvironment` servidor con `--proxy NAME`.
2. Ejecutar dentro del servidor `python -m http.server 18082 --bind 127.0.0.1`.
3. Lanzar un segundo proceso proot con el mismo `NAME`.
4. Aprobar las solicitudes PRCT `BIND` y `CONNECT` con `ALLOW_ALWAYS`.
5. Ejecutar en el cliente `socket.connect(('127.0.0.1', 18082))`.

Observado:

```text
PermissionError: [Errno 13] Permission denied
```

El cliente recibe `CONNECT` para algunos pasos, pero el connect al puerto
virtual compartido se bloquea antes de completar la traducción. Hermes marca
este caso como `xfail` hasta que el fork lo corrija. El caso entre procesos
con el mismo proxy debe terminar en `peer-ok`; un nombre de proxy distinto
debe continuar fallando.

## Causa en el código

En `proot-source/src/extension/net_policy/net_policy.c`:

```c
static int destination_matches(..., VnpNetworkClass net_class)
{
    if (net_class == VNP_NET_CLASS_UNKNOWN)
        return 0;
    ...
}
```

`check_operation()` aplica esta política estática antes de llamar a
`ask_harness()`. A su vez, `vnp_classify_destination()` evita consultar el
registro compartido durante la clasificación y solo puede reconocer un
puerto cross-proxy si la caché ya fue actualizada. La consulta del registro
que realiza la traducción virtual ocurre después, en el camino que nunca se
alcanza cuando la política estática devuelve `EACCES`.

## Corrección solicitada

Mantener el orden fail-closed y hacer que un destino todavía desconocido
pueda llegar al harness PRCT sin convertirse en una autorización implícita.
La solución debe cumplir todas estas condiciones:

1. Con `--control-fd` válido y handshake completado, las reglas wildcard de
   hand-off deben permitir que `UNKNOWN` llegue a `ask_harness()`.
2. `ask_harness()` debe emitir `NET_ACCESS_REQUEST` y la decisión
   `ALLOW_ONCE`/`ALLOW_ALWAYS`/`DENY_*` debe seguir siendo autoritativa.
3. Si el control FD no está listo, falla, expira o se desincroniza, un destino
   `UNKNOWN` debe continuar siendo denegado; no se puede resolver usando
   `--net-policy off` ni habilitando red sin PRCT.
4. Una regla específica de allow/deny no debe tratar un destino desconocido
   como perteneciente a una clase concreta. Si se adopta el hand-off por
   wildcard, debe limitarse al wildcard y al estado de control listo.
5. La solución no debe convertir una dirección externa en `VIRTUAL` solo por
   ser loopback ni consultar rutas host en mensajes del harness.
6. El camino de `vnp_handle_connect()` debe seguir usando el registro
   compartido y verificar que el puerto pertenece al mismo proxy antes de
   traducirlo.

Una alternativa válida es refrescar de forma segura la caché del registro
antes de la decisión estática y clasificar el puerto como `VIRTUAL`; si se
elige esa opción, documentar el coste y la sincronización de locks. No debe
introducir una consulta que pueda bloquear indefinidamente el syscall.

## Regresiones requeridas

Añadir pruebas con el proot real y el directorio temporal de Termux (`$TMPDIR`):

- servidor y cliente con el mismo proxy: publicación, aprobación PRCT y
  conexión exitosa;
- proxy distinto: connect denegado;
- mismo proxy sin respuesta del harness: denegado y proceso terminado;
- destino externo clasificado como `UNKNOWN`: llega a PRCT, pero requiere
  aprobación y nunca se permite automáticamente;
- canal PRCT roto durante un connect: el guest falla cerrado;
- la solución no cambia el comportamiento de `--net-policy deny` cuando no
  hay `--control-fd` o cuando el handshake no termina.

Ejecutar también los tests de `control-api` y comprobar que no aparecen
`ResourceWarning`, sockets abiertos ni procesos proot huérfanos.

## Criterios de aceptación

- `test_virtual_proxy_connects_between_proot_instances` de Hermes deja de ser
  `xfail` y pasa con una aprobación explícita.
- El proxy con nombre distinto sigue aislado.
- Toda red continúa requiriendo una respuesta PRCT.
- Un fallo del canal nunca abre una ruta de red de fallback.
- La documentación de `--net-policy` explica la interacción entre reglas
  estáticas, clases `UNKNOWN` y PRCT.

Hermes ya está preparado para consumir la corrección: no requiere un segundo
codec ni comandos PRCT desde hilos externos. El cambio necesario está en la
clasificación/política del fork `proot-termux`.
