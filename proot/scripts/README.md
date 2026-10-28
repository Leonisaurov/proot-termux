# Scripts de build y utilidades

Este directorio concentra los scripts que el repositorio usa para compilar
proot y para lanzar instancias declarativas. Ninguno de ellos sustituye al
`makefile` del producto (`proot/src/GNUmakefile`): son los entrypoints oficiales
que aplican las variables, flags y rutas que cada flujo necesita.

| Script | Qué hace | Dónde se ejecuta |
|---|---|---|
| [`build-native.sh`](build-native.sh) | Compila proot de forma nativa, lo instala en `$PREFIX` y genera el paquete `.pkg.tar.xz` | Un dispositivo o emulador con Termux |
| [`build-windows.ps1`](build-windows.ps1) | Compila desde un host Windows: binario Linux nativo y/o Android aarch64, con verificación y reporte | Windows 11 con WSL2 |
| [`tools/proot_exec.py`](tools/proot_exec.py) | Launcher declarativo detrás de `proot/bin/proot-exec`: convierte un TOML en un `argv` de proot | Cualquier host con Python 3.11+ |

## `build-native.sh`

Compila el árbol de `proot/src` con el compilador nativo de Termux, instala
`proot` y los loaders en `$PREFIX` (`bin/proot`, `libexec/proot/loader`,
`libexec/proot/loader32`) y empaqueta el resultado.

```bash
./proot/scripts/build-native.sh -i          # compila, instala y empaqueta
./proot/scripts/build-native.sh -j 4        # más paralelismo
./proot/scripts/build-native.sh --skip-build # solo empaqueta
```

Requisitos: Termux con `make`, `gcc` (o `clang`), `bsdtar`, `xz` y
`libtalloc`. La opción de paralelismo recibe el argumento separado (`-j 2` o
`--jobs 2`; `-j2` no existe).

**Desfase de versión conocido.** `build-native.sh` fija su propia versión
(`5.1.107.87-16`) mientras que la definición del paquete de CI
(`proot/ci/termux/packages/proot/build.sh`) fija `5.1.107.89-106`. Son dos
fuentes de verdad distintas: no se sincronizan a mano, usa la del archivo que
estés editando.

## `build-windows.ps1`

Es el camino de build desde Windows. Windows no puede *ejecutar* proot (ptrace
y seccomp son de Linux), pero sí puede ser un *host de compilación* perfectamente
válido: el script conduce WSL2 y produce dos artefactos.

```powershell
.\proot\scripts\build-windows.ps1 -Target linux     # binario Linux x86_64
.\proot\scripts\build-windows.ps1 -Target android   # PIE Android aarch64 (NDK r30)
.\proot\scripts\build-windows.ps1 -Target all -Verbose
```

El script hace preflight (WSL, distribución, RAM, disco, fuentes), instala las
dependencias con `apt-get`, compila, verifica el resultado con pruebas de
humo y escribe los artefactos y un reporte en `proot/artifacts/windows/`. La
guía completa, con los comandos exactos y la evidencia, está en
[`../docs/operations/windows-build.md`](../docs/operations/windows-build.md).

## `tools/proot_exec.py`

Implementa el launcher declarativo: lee un archivo TOML (`rootfs`, `binds`,
`proxy`, política de red, `control_fd`, entorno y comando) y lo traduce a los
argumentos de proot sin invocar un shell. Lo usa `proot/bin/proot-exec`:

```bash
./proot/bin/proot-exec --config ./proot/examples/proot-exec/proot-exec.conf.example --dry-run
```

La referencia completa está en
[`../docs/tools/proot-exec.md`](../docs/tools/proot-exec.md).

## Reglas de uso

1. **Usa siempre el script oficial del flujo**, no `make` a mano. Cada script
   fija las variables que el makefile espera (`CC`, `CPPFLAGS`, `CFLAGS`,
   `LDFLAGS`, `PROOT_UNBUNDLE_LOADER`, `OPTIMIZATION_FLAGS`) y con eso evita
   builds a medias o artefactos que no reproducen el del CI.
2. **Distingue "compilar para el host" de "compilar para el objetivo
   Android"**. El build para el host (Linux x86_64) genera un binario que
   corre en la máquina y sirve para las pruebas de humo. El cross-build para
   Android aarch64 usa el NDK, produce un PIE que necesita un dispositivo (o una
   imagen de Android) y empaqueta el mismo código con otra toolchain. No son
   intercambiables: el `file` del resultado y el `interpreter` de la ELF lo
   muestran.
3. **No mezcles objetos entre toolchains.** El cross-build copia el árbol de
   fuentes a una ruta propia dentro de WSL; el build nativo trabaja en
   `proot/src`. Mezclarlos deja objetos compilados con otro compilador en el
   mismo enlace.
4. **El directorio de salida está en `.gitignore`.** `proot/artifacts/windows/`
   está ignorado junto con `packages/` y `audit/`, así que los artefactos y el
   log del build de Windows no aparecen en `git status`.