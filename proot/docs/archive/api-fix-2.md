# Segunda revisión de `control-api`: correcciones aún necesarias

La API oficial ya incorporó `HELLO`, estados de canal, razones de decisión,
launchers con `socketpair` y validación básica. Aun así, todavía no debe
considerarse completamente lista para que Hermes dependa de ella.

## Correcciones necesarias

### 1. Deadline total en Rust y Bun

La documentación promete un único deadline para header y payload. Python ya
lo conserva, pero Rust y Bun crean un nuevo deadline al leer el payload después
del header. Un peer que envíe fragmentos lentamente puede superar el timeout
total anunciado.

La corrección debe calcular el deadline una sola vez en `recv_frame()` y
pasarlo a las lecturas de header y payload. Añadir una prueba de frame
fragmentado que verifique el límite total.

### 2. `serve()` no debe responder comandos como decisiones

Los tres `serve()` entregan todos los objetos al handler y pueden convertir
cualquier decisión devuelta por el handler en `ALLOW_*`/`DENY_*`, incluso para
`COMMAND_RESULT` o `SHADOW_EVENT`. Eso contradice la documentación: esos
mensajes son eventos y no esperan una decisión de acceso.

`serve()` debe responder automáticamente solo `NET_ACCESS_REQUEST` y
`PATH_ACCESS_REQUEST`. Los eventos y resultados deben entregarse al handler,
pero nunca generar una decisión automáticamente. Añadir una prueba para cada
tipo de mensaje.

### 3. `HELLO` debe ser obligatorio en el flujo normal

Python, Rust y Bun aceptan una primera petición sin `HELLO` por compatibilidad.
El protocolo documenta `HELLO` como handshake obligatorio y proot siempre lo
envía. Esto deja dos contratos distintos y permite usar el canal sin validar
su identidad/protocolo inicial.

La API normal (`from_fd`, launchers y `serve`) debe exigir `HELLO`. Si se
mantiene compatibilidad legacy, debe ser una opción explícita separada y no el
comportamiento predeterminado.

### 4. Todo error de framing debe dejar el canal en estado fallido

En Rust, un tipo de mensaje desconocido puede salir directamente por el error
de conversión sin pasar por `fail()` y sin dejar el canal en
`ChannelState::Failed`. Esto contradice el requisito de fallo cerrado y canal
no reutilizable después de desincronización.

Todos los errores de framing —incluido tipo desconocido— deben usar la misma
ruta de fallo. Añadir una prueba que consulte el estado después de un frame
inválido.

## Cobertura mínima antes de Hermes

La carpeta contiene solo dos tests Python básicos y no una suite equivalente
para Rust/Bun. Antes de integrar Hermes, deben existir pruebas para:

- handshake válido e inválido;
- fragmentación, truncamiento y EOF;
- timeout total header+payload;
- magic, versión, tipo y tamaño inválidos;
- paths no absolutos, demasiado largos y UTF-8 inválido;
- decisiones consecutivas con request-id distintos;
- comandos espontáneos sin respuesta de decisión;
- estado terminal después de cada error;
- vectores hex compartidos por los tres consumidores.

## Decisión para Hermes

Hermes debe esperar a que se corrijan los cuatro puntos anteriores y exista
cobertura mínima común. Después debe consumir el módulo Python oficial
versionado; no debe conservar un segundo codec PRCT ni activar una integración
basada en el contrato actual.
