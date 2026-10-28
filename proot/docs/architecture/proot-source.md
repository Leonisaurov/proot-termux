# Origen del código

## Qué hay en `proot/src/`

`proot/src/` es un **fork de PRoot**: el código fuente de
[PRoot](https://github.com/termux/proot), el trazador de procesos sin
privilegios creado originalmente por STMicroelectronics, cuyo sitio del
proyecto es <https://proot-me.github.io/>. El fork existe para compilar y
publicar el paquete de proot para **Android aarch64** dentro del pipeline de
[termux-packages](https://github.com/termux/termux-packages).

El ejecutable sigue siendo una herramienta independiente y multipropósito: el
rootfs, los binds, el entorno y la política los decide quien lo invoca. Proot
no conoce presets de Termux ni políticas de ningún consumidor concreto.

## Fork "proot-only"

El fork no aplica parches al upstream ni descarga fuentes externas:

- **Sin parches**: `proot/src/` se copia tal cual al directorio de build con
  `rsync -ac --exclude=.git` durante `termux_step_pre_configure()`, en
  `proot/ci/termux/packages/proot/build.sh`.
- **Sin descarga de fuentes**: el paquete declara
  `TERMUX_PKG_SKIP_SRC_EXTRACT=true`, de modo que el pipeline no extrae ni
  descarga ningún tarball upstream.
- **Sin parches de build**: el `makefile` que se usa es el del propio árbol
  (`proot/src/GNUmakefile`), con las extensiones del fork añadidas a
  `OBJECTS`.

En otras palabras: las diferencias respecto al upstream son código propio,
versionado en este repositorio, y no parches aplicados encima del upstream.

## Créditos y licencia

El código upstream es GPL-2.0 (así lo declara el paquete:
`TERMUX_PKG_LICENSE="GPL-2.0"`). La lista completa de los autores originales
—Cédric Vincent (autor y mantenedor), Rémi Duraffort, Christophe Guillon,
Yves Janin, Antoine Moynault, Claire Robine, Clément Bazin, Christian Bertin
y otros— está en [`../legal/proot-COPYING`](../legal/proot-COPYING), que
reproduce además el texto de la licencia del upstream. Los ficheros concretos
mandan sobre cualquier resumen: la infraestructura de build que no pertenece a
`packages/` se publica bajo Apache-2.0 (ver el `LICENSE.md` de la raíz).

## Extensiones propias del fork

El fork añade trece extensiones bajo `proot/src/extension/`:

| Directorio | Qué aporta |
|---|---|
| `net_policy/` | política estática de red y canal de control PRCT |
| `virtual_net/` | red virtual sobre sockets Unix abstractos |
| `port_switch/` | mapeo y publicación de puertos |
| `resource_limit/` | límites de CPU, memoria, descriptores y procesos |
| `proc_isolation/` | vista guest estricta de `/proc` |
| `fake_id0/` | emulación de identidad de usuario y de permisos |
| `kompat/` | compatibilidad con kernels antiguos |
| `sysvipc/` | emulación de System V IPC (shm, colas de mensajes, semáforos) |
| `link2symlink/` | hard links emulados como enlaces simbólicos |
| `hidden_files/` | ocultación de ficheros y directorios |
| `mountinfo/` | vista guest de `mountinfo` |
| `fix_symlink_size/` | tamaño correcto en `lstat` de enlaces simbólicos |
| `ashmem_memfd/` | `memfd_create` sobre ashmem en Android/Bionic |

Además, `proot/src/supervise/` es una capa propia —no una extensión del
sistema de extensiones— que implementa `--supervise` y `--exec`.

## CI actual

La build del paquete se automatiza con GitHub Actions, en
[`.github/workflows/build-proot.yml`](../../../.github/workflows/build-proot.yml).
El workflow se dispara con un push sobre `proot/src/**`,
`proot/ci/termux/packages/proot/**`, `proot/ci/termux/scripts/**` o
`ndk-patches/**`, y también de forma manual (`workflow_dispatch`). El pipeline
compila con el NDK r30 dentro del contenedor del package builder.

No hay ninguna integración con Travis CI: ese servicio ya no participa en el
build y su insignia anterior se ha retirado de esta página.

## Documentación relacionada

- [Uso operativo de PRoot y `termux-isolated`](../operations/usage.md)
- [Vista proc aislada](proc-leaks.md)
- [Hardening y regresiones](../security/FIXES.md)
- [Build de Termux y CI](../../ci/termux/README.md)
- [Índice de documentación](../INDEX.md)
