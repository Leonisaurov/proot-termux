# Proot: código, herramientas y validación

Este directorio contiene el producto completo. Desde la raíz del repositorio
se entra aquí con `cd proot`; los comandos de esta página que empiezan con
`./` están escritos para ejecutarse desde `proot/`.

## Índice rápido

| Necesidad | Ubicación | Entrada recomendada |
|---|---|---|
| Código nativo de PRoot | [`src/`](src/) | [`src/GNUmakefile`](src/GNUmakefile) |
| Launchers de usuario | [`bin/`](bin/) | [`bin/termux-isolated`](bin/termux-isolated), [`bin/proot-exec`](bin/proot-exec) |
| API PRCT y harness | [`control-api/`](control-api/) | [`control-api/README.md`](control-api/README.md) |
| Tests y regresiones | [`tests/`](tests/) | [`tests/rootfs/README.md`](tests/rootfs/README.md) |
| Build Termux/CI | [`ci/termux/`](ci/termux/) | [`ci/termux/README.md`](ci/termux/README.md) |
| Build nativa local | [`scripts/build-native.sh`](scripts/build-native.sh) | `./scripts/build-native.sh --help` |
| Documentación técnica | [`docs/`](docs/) | [`docs/README.md`](docs/README.md) |
| Ejemplos de configuración | [`examples/`](examples/) | [`examples/proot-exec/proot-exec.conf.example`](examples/proot-exec/proot-exec.conf.example) |
| Artefactos locales | [`artifacts/`](artifacts/) | paquetes generados, no código fuente |
| Reportes | [`reports/`](reports/) | resultados de pentest y diagnósticos |

## Flujo recomendado

Desde la raíz del repositorio:

```bash
./proot/bin/termux-isolated --termux-paths --cwd "$PWD" -- nvim
./proot/bin/proot-exec --config ./proot/examples/proot-exec/proot-exec.conf.example --dry-run
```

Para una build local en Termux:

```bash
./proot/scripts/build-native.sh -i
```

Para la build reproducible del paquete:

```bash
./proot/ci/termux/scripts/run-docker.sh \
  ./proot/ci/termux/build-package.sh -I -a aarch64 --format pacman proot
```

Las pruebas que ejecutan PRoot deben lanzarse en el entorno local de Termux
con la autorización elevada correspondiente. Los scripts nuevos se organizan
por tema dentro de `tests/` y usan `$TMPDIR` como temporal de Termux.

## Fronteras de responsabilidad

- `src/` implementa PRoot genérico y no conoce presets Termux.
- `control-api/` implementa el framing PRCT, launchers y consumidores; la
  política de autorización pertenece al harness.
- `bin/termux-isolated` define el lanzamiento concreto para Termux.
- `tests/` valida cada capa sin sustituir el launcher real por comandos ad hoc.
- `.github/` permanece fuera de este directorio porque GitHub Actions exige
  esa ubicación en la raíz del repositorio.

Para el contrato del protocolo, consulta
[`control-api/PROTOCOL.md`](control-api/PROTOCOL.md). Para la guía completa de
Python y el harness interactivo, consulta
[`control-api/docs/python-api.md`](control-api/docs/python-api.md).
