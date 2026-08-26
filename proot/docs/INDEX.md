# Índice de documentación

## Primeros pasos

- [Mapa del proyecto](../README.md)
- [Uso operativo de PRoot y `termux-isolated`](operations/usage.md)
- [Launcher declarativo `proot-exec`](tools/proot-exec.md)
- [Build de Termux y CI](../ci/termux/README.md)
- [Wrappers de rootfs para tests](../tests/rootfs/README.md)

## Integraciones

- [Control API y harness](../control-api/README.md)
- [Referencia Python y cookbook](../control-api/docs/python-api.md)
- [Contrato binario PRCT](../control-api/PROTOCOL.md)
- [Ejemplo de configuración](../examples/proot-exec/proot-exec.conf.example)

## Arquitectura y seguridad

- [Detalles de PRoot](architecture/proot-details.md)
- [Arquitectura del sandbox](architecture/proot-sandbox-impl.md)
- [Vista proc aislada](architecture/proc-leaks.md)
- [Hardening y regresiones](security/FIXES.md)
- [Reporte de vulnerabilidades](security/vulneration-report.md)

## Validación y mantenimiento

- Los scripts de pruebas viven en `../tests/<tema>/`.
- Los resultados persistentes viven en `../reports/`.
- Los planes de trabajo viven en `../tasks/`.
- Los artefactos generados viven en `../artifacts/` y no son código fuente.
- Los documentos bajo [`archive/`](archive/) conservan referencias históricas;
  sus rutas antiguas no describen el layout actual.
