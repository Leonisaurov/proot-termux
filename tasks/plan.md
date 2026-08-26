# Implementation Plan: proot independiente y control explícito

## Objetivo

Restablecer una frontera clara: proot ofrece mecanismos generales (rootfs,
bindings, traducción, aislamiento y protocolos opt-in), mientras que
`control-api`, `termux-isolated` y cualquier otro harness decide qué rutas,
entorno, permisos y políticas desea pasar. Ningún comportamiento de proot debe
depender de rutas Android/Termux ni de presets de un consumidor.

## Decisiones de arquitectura

- Mantener `-R/-S` únicamente como compatibilidad explícita del CLI upstream;
  no se convierten en defaults y su documentación debe dejar claro que son
  aliases de conveniencia solicitados por el usuario.
- Eliminar rutas Termux compiladas en proot. Los directorios de estado se
  resolverán mediante una opción/configuración explícita y, como fallback
  genérico, `TMPDIR`.
- El protocolo PRCT seguirá siendo una capacidad opt-in, pero no impondrá una
  lista de excepciones de rutas Android. Las excepciones serán reglas explícitas
  del consumidor/controlador.
- `termux-isolated` conservará su configuración concreta de lanzamiento; no
  es el harness del protocolo. Un harness es el software que abre el control
  fd, recibe eventos y responde las decisiones.

## Fases y criterios de aceptación

### Fase 1: inventario y contrato

- [ ] Identificar rutas/presets/políticas específicas incrustadas en binario,
      wrappers y documentación.
- [ ] Actualizar AGENTS, README y control-api con la frontera de ownership.

### Fase 2: implementación raíz

- [ ] Sustituir directorios de estado Termux hardcodeados por resolución
      genérica/configurable.
- [ ] Retirar la excepción fija `/data/data/com.termux/files/usr` del núcleo
      PRCT y evitar que proot conozca rutas del consumidor.
- [ ] Conservar el fast-path sin flags y la semántica upstream de bindings
      explícitos.

### Fase 3: consumer/harness

- [ ] Hacer que `control-api` pase explícitamente sus reglas y contexto.
- [ ] Marcar `termux-isolated` como harness, no como API ni comportamiento
      predeterminado de proot.

### Checkpoint final

- [ ] `bash -n` en scripts afectados y búsqueda de rutas temporales
      accidentales.
- [ ] Preflight Termux sin compilar.
- [ ] Build/tests solo tras autorización escalada y con diagnóstico completo
      si fallan.

## Riesgos

| Riesgo | Mitigación |
|---|---|
| Romper consumidores que dependían de excepciones implícitas | Mover las reglas al launcher y probar PRCT con binds/rutas explícitos |
| Incompatibilidad de protocolo | No cambiar frames; cambiar únicamente ownership de reglas |
| Confundir compatibilidad `-R/-S` con defaults | Mantenerlas opt-in y documentarlas como aliases upstream |
