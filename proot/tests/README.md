# Tests

Las pruebas están separadas por la capa que ejercitan:

| Área | Directorio | Qué valida |
|---|---|---|
| PRoot | [`proot/`](proot/) | syscalls, aislamiento, red y hardening |
| `termux-isolated` | [`termux-isolated/`](termux-isolated/) | cwd, shell, storage, PTY, proc y políticas |
| control-api | [`control-api/`](control-api/) | protocolo PRCT, launcher y harness |
| harness | [`harness/`](harness/) | integración PTY/PRCT y `proot-exec` |
| rootfs | [`rootfs/`](rootfs/) | wrappers y fixtures Alpine |

Desde la raíz del repositorio, primero revisa y después ejecuta el script:

```bash
bash -n proot/tests/termux-isolated/cwd/test_termux_isolated_cwd.sh
git diff --check
```

Las pruebas que ejecutan binarios, PRoot o Android deben ejecutarse en el
Termux local con autorización elevada. Los temporales deben estar bajo
`$TMPDIR`; las rutas `/tmp` que aparecen dentro de probes pertenecen al
rootfs guest o al contenedor CI y no deben copiarse al flujo Termux.

La batería completa puede ser costosa. Ejecuta primero el caso mínimo sin
protocolo, después `control-fd` y finalmente el harness que atiende eventos
durante toda la vida del proceso.
