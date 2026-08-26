# Auditoría de rendimiento persistente de PRoot

## Alcance de esta iteración

Reducir búsquedas lineales de extensiones en rutas calientes sin cambiar
defaults de seguridad, protocolo ni semántica de bindings. El cache pertenece
al encabezado de extensiones, que ya es lazy, por lo que un tracee sin
extensiones no gana memoria.

## Trabajo

1. Medir y registrar el estado inicial del árbol y del toolchain.
2. Cachear los callbacks consultados desde paths/syscalls: net-policy,
   proc-isolation, virtual-net, resource-limit y fake-id0.
3. Invalidar los punteros durante `REMOVED` y reconstruirlos mediante la ruta
   normal de `new_extension()` para herencia y clones.
4. Compilar con el flujo local oficial y ejecutar regresiones proporcionales.

## Criterios de aceptación

- La compilación oficial termina correctamente.
- `get_extension()` conserva la búsqueda lineal para callbacks no cacheados.
- La extensión cacheada se limpia antes de notificar `REMOVED`.
- Las pruebas de control-fd, net-policy, proc isolation y workloads persistentes
  siguen pasando.
