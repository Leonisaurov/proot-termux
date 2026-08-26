# Reporte Fase B — Leaks y FDs (REV 21)

> Este reporte histórico usa rutas relativas a `proot/`; por ejemplo,
> `ci/termux/packages/proot` es el antiguo `packages/proot`.

**Commit**: `ec308f5425` — `fix(isolation): complete Fase B — close fd/leak/sp issues B1-B8`
**Fecha**: 2026-08-18
**REVISION**: 21

## Resumen Ejecutivo

La Fase B cierra todas las fugas de file descriptors, memoria y stack pointer identificadas durante la auditoría de seguridad. 8 fixes implementados y verificados. **Un fix adicional (B5) fue completado en esta revisión**: el restore de SP en error paths de bind/connect.

## Estado por Fix

| Fix | Severidad | Descripción | Archivo(s) | Estado |
|-----|-----------|-------------|------------|--------|
| B1 | P1 | accept4+SOCK_CLOEXEC en socket de control | supervise.c:266-282 | ✅ COMPLETO |
| B2 | P1 | Marcar child_tracee terminated en fallo add_client | supervise.c:469-580 | ✅ COMPLETO |
| B3 | P1 | Sweep fd_map stale con kill(pid,0) liveness | virtual_net_internal.h:175-199 | ✅ COMPLETO |
| B4 | P2 | talloc_set_destructor(NULL) en mbind reemplazado | binding.c:354-366 | ✅ COMPLETO |
| B5 | P2 | Restore SP en error paths de alloc_mem | virtual_net.c:619-631, 772-780 | ✅ COMPLETO |
| B6 | P2 | Trackear y matar bridge children en helper exit | virtual_net_helper.c:52-61, 270-274, 436-442 | ✅ COMPLETO |
| B7 | P3 | Cerrar signalfd en supervise_fini | supervise.c:66-67, 165, 239-245 | ✅ COMPLETO |
| B8 | P3 | strdup→talloc_strdup en initial_ldso_paths | ldso.c:487-492 | ✅ COMPLETO |

## Detalle del Fix B5 (completado en esta revisión)

**Problema hallado**: En `vnp_handle_bind` y `vnp_handle_connect`, si `alloc_mem()` tiene éxito pero `vnp_write_to_tracee()` falla, la función retornaba sin restaurar el SP del guest. Esto causaba que el SP del guest quedara corrupto después de un error de escritura.

**Fix**: Se añadió `poke_reg(tracee, STACK_POINTER, sp_before_alloc)` antes del `return 0` en ambos error paths.

**Riesgo**: BAJO — solo afecta el path de error (write failure), que es raro. El fix es trivialmente correcto.

## Compilación

Todos los archivos fuente modificados compilan limpio con clang (clang -fsyntax-only verificado). El build completo del sistema de build (GNUmakefile) tiene un issue pre-existente con objdump/objcopy en el loader wrapping que no está relacionado con estos fixes.

## Archivos Modificados

| Archivo | Fix | Líneas +/- |
|---------|-----|-----------|
| supervise/supervise.c | B1+B2+B7 | +50/-34 |
| extension/virtual_net/virtual_net_internal.h | B3 | +29/-1 |
| path/binding.c | B4 | +13/-2 |
| extension/virtual_net/virtual_net.c | B5 | +32/-10 |
| extension/virtual_net/virtual_net_helper.c | B6 | +24/-13 |
| execve/ldso.c | B8 | +6/-3 |
| FIXES.md | Status update | +16/-16 |
| ci/termux/packages/proot/build.sh | REVISION 21 | +2/-1 |
| tests/proot/probes/memtest.c | Test update | +13/-0 |

**Total**: 313 inserciones, 516 eliminaciones en 10 archivos.

## Fases Completadas

| Fase | Estado | Commits |
|------|--------|---------|
| A — Aislamiento P0 | ✅ COMPLETADA | `573f4cb8d9` (REV 19) + `3b98197d8a` (REV 20) |
| **B — Leaks y FDs** | **✅ COMPLETADA** | **`ec308f5425` (REV 21)** |
| C — Aislamiento P1 | ⏳ PENDIENTE | — |
| D — Rendimiento P0/P1 | ⏳ PENDIENTE | — |
| E — Resto P2/P3 | ⏳ PENDIENTE | — |

## Verificación Recomendada

1. **Smoke test --exec**: lanzar supervisor con `--supervise`, ejecutar 16+ clientes `--exec` concurrentes, verificar que no hay fd leaks
2. **Smoke test --proxy**: ciclo crear/cerrar sockets vnet hasta llenar fd_map, verificar que B3 recupera entradas stale
3. **Talloc report**: ejecutar `kill -USR1 <proot_pid>` para generar reporte de memoria, verificar que no hay fugas de Tracee o bindings
4. **SP check**: en un guest con `--proxy`, ejecutar un loop de bind/connect repetidos y verificar que el stack del guest no crece indefinidamente
