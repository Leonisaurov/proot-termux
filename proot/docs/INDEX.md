# Índice de documentación

## Primeros pasos

- [Mapa del proyecto](../README.md)
- [Uso operativo de PRoot y `termux-isolated`](operations/usage.md)
- [Launcher declarativo `proot-exec`](tools/proot-exec.md)
- [Build de Termux y CI](../ci/termux/README.md)
- [Compilar proot desde Windows](operations/windows-build.md)
- [Matriz de pruebas](../tests/README.md)
- [Wrappers de rootfs para tests](../tests/rootfs/README.md)

## Integraciones

- [Control API y harness](../control-api/README.md)
- [Referencia Python y cookbook](../control-api/docs/python-api.md)
- [Guía de la API de control en Python](../control-api/docs/python.md)
- [Contrato binario PRCT](../control-api/PROTOCOL.md)
- [Vectores de prueba del protocolo](../control-api/vectors/README.md)
- [Ejemplo de configuración](../examples/proot-exec/proot-exec.conf.example)

## Arquitectura, protocolo y seguridad

- [Origen del código](architecture/proot-source.md)
- [Detalles de PRoot](architecture/proot-details.md)
- [Arquitectura vigente del sandbox](architecture/proot-sandbox-impl.md)
- [Vista proc aislada](architecture/proc-leaks.md)
- [Contrato binario PRCT](../control-api/PROTOCOL.md)
- [Hardening y regresiones](security/FIXES.md)
- [Reporte de fase B](security/REPORT-FASE-B.md)
- [Reporte de vulnerabilidades](security/vulneration-report.md)

## Rendimiento

- [Benchmarks](performance/benchmarks.md)
- [Benchmark de `--net-policy`](performance/net-policy-benchmark.md)

## Legal

- [Licencia upstream (GPL-2.0) y créditos originales](legal/proot-COPYING)

## Mapa de capas y proprietarios

| Capa | Qué hace | Qué no asume |
|---|---|---|
| proot (`src/`) | Traducción de rutas, bindings explícitos, aislamiento y protocolo opt-in | Rutas de Android/Termux, binds del consumidor ni decisiones del harness |
| `termux-isolated` (`bin/termux-isolated`) | Lanzamiento concreto de proot en Termux | Controlar el `control-fd` o exponer almacenamiento por su cuenta |
| `control-api` (`control-api/`) | Codec, launcher y contrato del control fd | Presets internos de proot |
| harness | Lee cada evento y responde mientras proot vive | Que proot decida por él |

La dependencia es unidireccional: `control-api` puede exigir una versión de
proot compatible con su protocolo, pero proot no depende de `control-api` ni
contiene sus presets.

## Reportes de pendientes

- [Pendientes de proot, Android, SELinux y kernel](../../proot-sandbox-fix.md)
- [FIX — `/proc/meminfo` devuelve ceros](../../proc-meminfo-fix.md)

## Histórico / archivado

Los documentos de `archive/` conservan el estado de revisiones anteriores. Sus
rutas (`proot-source/`, `packages/`, `pentest/`) y sus expectativas **no son
normativos** y no describen el layout actual, que es `proot/`.

- [Histórico de control-fd](archive/PROGRESS.md)
- [Solicitud de correcciones inicial](archive/api-fix.md)
- [Segunda revisión de `control-api`](archive/api-fix-2.md)
- [Revisión de integración real](archive/api-fix-3.md)
- [Limpieza de recursos en `control-api`](archive/api-fix-4.md)
- [Hand-off PRCT para puertos virtuales cross-proxy](archive/api-fix-5.md)
- [PRCT debe interceptar lecturas externas](archive/api-fix-6.md)

## Validación y mantenimiento

- Los scripts de pruebas viven en `../tests/<tema>/`; la matriz de red está en
  `../tests/proot/networking/`.
- Los resultados persistentes de las regresiones viven en `../tests/reports/`.
- Los planes de trabajo viven en `../tasks/` y no son contratos operativos.
- Los artefactos generados viven en `../artifacts/` y no son código fuente.
