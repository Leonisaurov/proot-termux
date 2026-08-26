# Solicitud de correcciones para `proot --control-fd`

Documento de coordinación para el agente responsable de `../proot-termux`.
Hermes no debe integrar todavía un adaptador local: primero debe existir una
API oficial funcional y coherente con el protocolo implementado por proot.

## Bloqueador actual: `HELLO`

Proot envía un frame `HELLO` inmediatamente después de instalar el FD de
control, incluso antes de arrancar el guest. Sin embargo, los consumidores
oficiales actuales (`python`, `rust` y `bun`) no exponen `HELLO` como handshake
consumible:

- `receive()` clasifica `HELLO` como un frame que no es una petición.
- `serve()` llama a `receive()` como primera operación.
- Por tanto, el ejemplo oficial `ControlChannel.from_fd(3).serve(...)` falla
  antes de recibir la primera petición de ruta o red.

### Comportamiento solicitado

La API debe consumir el handshake automáticamente al crear o iniciar el
canal. Como mínimo:

```python
channel = ControlChannel.from_fd(3)
channel.handshake()  # consume HELLO y valida payload/request_id
channel.serve(handler)
```

También debe ser válido que `serve()` haga el handshake implícitamente, para
que los ejemplos oficiales funcionen sin pasos ocultos adicionales. Un
`HELLO` recibido fuera del estado esperado debe producir un error de protocolo
fail-closed.

## Correcciones de robustez solicitadas

### 1. Timeout total por frame

El timeout actual se aplica a cada llamada parcial de `recv`, por lo que un
peer que envía un byte periódicamente puede prolongar indefinidamente la
lectura de un frame. La API debe aplicar un deadline total a:

- header completo;
- payload completo;
- respuesta completa.

El timeout debe ser configurable, conservar un valor predeterminado de 1000
ms y devolver un error distinguible de EOF y de frame inválido. Una vez
agotado el deadline, el canal debe quedar inutilizable y las decisiones deben
fallar cerradas.

### 2. Estado explícito del canal

El consumidor debe distinguir al menos estos estados:

- creado, sin handshake;
- listo;
- fallido/desincronizado;
- cerrado.

Después de un error de framing, timeout, EOF inesperado o request-id inválido,
no debe continuar leyendo ni reutilizar el socket como si estuviera sano.

### 3. API consistente para respuestas

Los tres lenguajes deben ofrecer la misma capacidad:

- `allow_once(request_id, reason=..., reason_code=...)`;
- `allow_always(...)`;
- `deny_once(...)`;
- `deny_always(...)`;
- validación de request-id y tamaño de payload;
- codificación del motivo en los 96 bytes definidos por el protocolo.

Rust y Bun no deben perder el motivo de decisión que sí admite el layout
oficial. Las variantes simplificadas pueden mantenerse como atajos, pero no
como única interfaz.

### 4. Soporte seguro para comandos espontáneos

Durante una petición de ruta o red, proot puede recibir comandos del harness
con otro `request_id` (`SET_RULE`, `FORGET`, `REVEAL_SHADOW`,
`RESTORE_SHADOW`, `GET_STATE`). La API debe:

- exponerlos como eventos/comandos diferenciados de las peticiones de acceso;
- aplicar o rechazar payloads con validación estricta;
- devolver `COMMAND_RESULT` con el mismo `request_id`;
- permitir que el handler procese comandos sin romper la respuesta pendiente;
- documentar que no se debe responder un comando espontáneo como si fuera una
  decisión de acceso.

### 5. Validación de layouts y rutas guest

Todos los consumidores deben validar de forma equivalente:

- magic, versión, tipo y tamaño máximo;
- tamaños exactos de cada payload conocido;
- endian little-endian de la implementación Termux;
- rutas absolutas guest, sin NUL interno y con máximo de 1023 bytes;
- que nunca se acepten ni se devuelvan rutas host.

Las diferencias actuales entre Python/Rust/Bun deben cubrirse con vectores
compartidos y tests de frames fragmentados, truncados, inválidos y con paths
no guest.

## Requisitos de integración documentados

La documentación oficial debe incluir un ejemplo completo de lanzamiento del
proceso consumidor:

1. Crear `socketpair(AF_UNIX, SOCK_STREAM)`.
2. Pasar un extremo heredable a `proot --control-fd FD`.
3. Consumir `HELLO` antes de esperar peticiones.
4. Mantener el otro extremo en un harness separado del lector de stdout/stderr
   del guest.
5. Cerrar ambos extremos y terminar el harness cuando proot finalice.

Debe aclararse que la API solo implementa framing/codec y despacho; no decide
políticas automáticamente ni convierte rutas guest en rutas host. La política
de aprobación pertenece al proyecto consumidor.

## Criterios de aceptación

- Los ejemplos oficiales Python, Rust y Bun completan el handshake y reciben
  al menos una `PATH_ACCESS_REQUEST` real.
- Un harness puede permitir y denegar dos peticiones consecutivas sin perder
  sincronización.
- Un `HELLO` malformado, timeout total, EOF o request-id incorrecto termina el
  canal y deniega la operación.
- Los comandos espontáneos producen `COMMAND_RESULT` correlacionado.
- Existe una suite común o vectores equivalentes para fragmentación,
  truncamiento, payload inválido, path host y timeout.
- La documentación describe el ciclo de vida del FD y el cierre seguro.

## Compatibilidad requerida para Hermes

Cuando estas correcciones estén publicadas, Hermes debe consumir la API
oficial, preferiblemente mediante el módulo Python oficial versionado junto
con el paquete/binario de proot. Hermes no debe conservar un segundo codec
PRCT ni depender de rutas relativas como `../proot-termux/control-api` en
producción.
