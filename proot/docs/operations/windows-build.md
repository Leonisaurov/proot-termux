# Compilar proot-termux desde Windows

**Idea clave**: proot es una herramienta de **Linux**. Intercepta llamadas al
sistema con `ptrace` e instala filtros `seccomp`, así que **no se puede
ejecutar de forma nativa en Windows**. Windows sí puede ser, sin embargo, un
**host de compilación perfectamente válido**: el código se compila en Linux
(WSL2) o se cross-compila con el NDK de Android, y el resultado se copia al
repositorio de Windows.

Esta página documenta las tres rutas de build: **A** y **B** se verificaron en
la máquina de referencia (Windows 11 Home, WSL 3.0.3, `Ubuntu-24.04`,
2026-10-20/21) y **C** es la ruta canónica del CI, documentada pero no
ejecutada aquí. También recoge la evidencia medida de por qué **no existe un
port nativo de Windows**.

| Ruta | Qué produce | Herramientas | Estado medido |
|---|---|---|---|
| [A](#ruta-a--binario-linux-nativo-wsl2) | Binario Linux x86_64 para el host | WSL2 + gcc | Verificada (exit 0) |
| [B](#ruta-b--paquete-android-aarch64-ndk-r30-desde-wsl2) | PIE Android aarch64 | WSL2 + NDK r30 | Verificada (exit 0) |
| [C](#ruta-c--la-ruta-exacta-del-ci-docker-desktop) | Paquete `.pkg.tar.xz` (lo que publica el CI) | Docker Desktop + `ghcr.io/termux/package-builder` | Documentada, **no ejecutada** aquí |

El script [`proot/scripts/build-windows.ps1`](../../scripts/build-windows.ps1)
automatiza las rutas A y B (preflight, dependencias, compilación, pruebas de
humo, artefactos y reporte).

```powershell
.\proot\scripts\build-windows.ps1 -Target linux     # ruta A
.\proot\scripts\build-windows.ps1 -Target android   # ruta B
.\proot\scripts\build-windows.ps1 -Target all       # ambas
```

## Requisitos

| Recurso | Mínimo | Nota |
|---|---|---|
| WSL2 | 3.0.3 (medido) | Con una distribución Linux instalada (`wsl -l -q`) |
| Distribución | `Ubuntu-24.04` (por defecto en el script) | Cualquier distribución glibc con `apt-get` |
| RAM libre | 4 GB recomendados con `-Jobs 1` | La máquina de referencia tenía 7,2 GB totales y a menudo menos de 1 GB libres: el build funcionó igual con `-j1`; con `-Jobs` mayor que 1 y menos de 4 GB libres el script avisa |
| Disco libre | ~3 GB | 738 633 529 B del NDK comprimido y 2,3 GB descomprimidos, más el árbol de fuentes |
| Red | Solo para la ruta B | Descarga del NDK (~704 MiB) y de talloc (684 092 B) |
| Herramientas en la distro | `build-essential`, `libtalloc-dev`, `gcc-multilib`, `g++-multilib`, `unzip`, `file`, `curl` | `python3` es opcional: sin él la prueba del registro de red virtual se omite |

El script instala esas dependencias con `apt-get` (paso `-SkipDeps` para
omitirlo) y usa `root` dentro de la distribución por defecto, que es lo que
permitió verificar la ruta en esta máquina.

## Ruta A — binario Linux nativo (WSL2)

Compila el árbol de `proot/src` tal cual, con el compilador de la
distribución. Es la forma más rápida de comprobar que el código compila y de
ejecutar las pruebas de humo.

### Comandos

```bash
# preflight
wsl -l -q
wsl -d Ubuntu-24.04 -u root -- uname -srm

# dependencias
wsl -d Ubuntu-24.04 -u root -- apt-get update
wsl -d Ubuntu-24.04 -u root -- apt-get install -y build-essential libtalloc-dev gcc-multilib g++-multilib unzip file curl

# compilación
wsl -d Ubuntu-24.04 -u root -- bash -lc 'cd /mnt/c/Users/leoni/Desktop/Develop/C/proot-termux/proot/src && make -j1 V=1'
```

`gcc-multilib` y `g++-multilib` no son opcionales: el objetivo x86_64 también
compila el loader de 32 bits (`loader/loader-m32`).

### Resultado medido

```text
$ time make -j1 V=1
...
real    0m28.984s
```

El makefile devuelve exit 0 y deja el binario en `proot/src/proot`.

El enlace termina con los dos avisos preexistentes de
`-Wstringop-overread` en `path/binding.c:462` (no son errores y no los
introduce este fork):

```text
./path/binding.c:462:16: warning: 'insort_binding3_with_mode' reading 4096 bytes from a region of size 2 [-Wstringop-overread]
./path/binding.c:462:16: warning: 'insort_binding3_with_mode' reading 4096 bytes from a region of size 4095 [-Wstringop-overread]
```

### Verificación

```bash
$ file proot
proot: ELF 64-bit LSB pie executable, x86-64, version 1 (SYSV), dynamically linked, interpreter /lib64/ld-linux-x86-64.so.2, for GNU/Linux 3.2.0, not stripped

$ ./proot --version
proot-5.1.107.89-105-dirty
built-in accelerators: process_vm = yes, seccomp_filter = yes

$ ./proot --about
...
Copyright (C) 2015 STMicroelectronics, licensed under GPL v2 or later.
proot-termux fork: Copyright (C) 2025-2026 Leonisaurov, GPL v2 or later.
https://github.com/Leonisaurov/proot-termux
```

La cadena `proot-termux fork:` en `--about` es la prueba de que el binario
viene de este fork y no del upstream pelado.

### Pruebas de humo

Se ejecutan con el entorno limpio (`env -i`) y con `PROOT_RUNTIME_DIR` y
`TMPDIR` apuntando a una ruta host escribible:

```bash
env -i PATH=/bin:/usr/bin PROOT_RUNTIME_DIR=/tmp/proot-probe TMPDIR=/tmp/proot-probe \
  ./proot -r / -b /proc -b /dev -b /sys /bin/echo SMOKE_OK
```

| Prueba | Resultado medido |
|---|---|
| `proot -r / -b /proc -b /dev -b /sys /bin/echo SMOKE_OK` | `SMOKE_OK`, exit 0 |
| `... --net-policy deny --net-deny-bind 443 /bin/echo NETPOLICY_OK` | `NETPOLICY_OK`, exit 0 |
| `... --proxy WINPROBE` con un `bind()` AF_INET dentro del guest | Registro `proot-net/WINPROBE/registry.lock` de **59 404 B** con magic `0x50524F4E` ("PRON") |
| `... --proc-isolated /bin/sh -c 'ls /proc'` | Solo los PIDs del propio tracee (**1** PID numérico frente a **37** del host en la última ejecución) y siguen visibles `self`, `thread-self`, `cpuinfo`, `meminfo`, `loadavg`, `uptime` y `stat` |

La comprobación del registro necesita un programa que cree un socket: el
registro de la red virtual se escribe cuando el guest hace `bind()` o
`connect()`, no al arrancar proot.

## Ruta B — paquete Android aarch64 (NDK r30 desde WSL2)

Produce el mismo tipo de binario que publica el CI, pero sin Docker: el NDK
y talloc se descargan y se compilan dentro de WSL.

### 1. NDK r30

```bash
curl -fL -o android-ndk-r30-linux.zip \
  https://dl.google.com/android/repository/android-ndk-r30-linux.zip
echo "753611f410d002cfcd3f3dc2ef49aad532089d3180b436c060a90bf0fcb64df2  android-ndk-r30-linux.zip" | sha256sum -c -
unzip -q android-ndk-r30-linux.zip
```

| Dato | Valor medido |
|---|---|
| Tamaño del zip | 738 633 529 B |
| SHA-256 | `753611f410d002cfcd3f3dc2ef49aad532089d3180b436c060a90bf0fcb64df2` (el mismo pin de `proot/ci/termux/scripts/setup-android-sdk.sh`) |
| Descarga | 52 s a ~13,4 MB/s (la medición de referencia dio 62 s a ~11 MB/s) |
| `Pkg.Revision` | `30.0.16248370` |

El compilador se detecta automáticamente: el NDK puede usar `toolchain/llvm/...`
o `toolchains/llvm/...` y el script prueba las dos formas más `prebuilt/`.

### 2. talloc estático para aarch64

```bash
curl -fL -o talloc-2.4.3.tar.gz https://www.samba.org/ftp/talloc/talloc-2.4.3.tar.gz
echo "dc46c40b9f46bb34dd97fe41f548b0e8b247b77a918576733c528e83abd854dd  talloc-2.4.3.tar.gz" | sha256sum -c -
tar -xzf talloc-2.4.3.tar.gz
```

El SHA-256 es el mismo pin del CI
(`proot/ci/termux/packages/libtalloc/build.sh`). Las respuestas del
`cross-compile` **no se escriben a mano**: el script las extrae del heredoc
del propio paquete del CI (`build.sh:27-53`):

```bash
sed -n '/^[[:space:]]*cat <<EOF > cross-answers.txt/,/^EOF$/p' \
  proot/ci/termux/packages/libtalloc/build.sh | sed '1d;$d' > cross-answers.txt
```

Y la configuración usa el mismo juego de flags que el CI, más el host triple:

```bash
CC="<ndk>/bin/aarch64-linux-android24-clang" \
AR="<ndk>/bin/llvm-ar" RANLIB="<ndk>/bin/llvm-ranlib" \
./configure --prefix="$PREFIX" --host=aarch64-linux-android \
  --disable-rpath --disable-python --cross-compile --cross-answers=cross-answers.txt
make
"<ndk>/bin/llvm-ar" rcu "$PREFIX/lib/libtalloc.a" bin/default/talloc*.6.o
"<ndk>/bin/llvm-ranlib" "$PREFIX/lib/libtalloc.a"
cp talloc.h "$PREFIX/include/talloc.h"
```

### 3. proot con el clang del NDK

El árbol de fuentes se copia a una ruta propia dentro de WSL para no mezclar
objetos con el build nativo:

```bash
export CC="<ndk>/bin/aarch64-linux-android24-clang"
export STRIP="<ndk>/bin/llvm-strip"
export OBJCOPY="<ndk>/bin/llvm-objcopy"
export OBJDUMP="<ndk>/bin/llvm-objdump"
export CPPFLAGS="-DARG_MAX=131072 -DVERSION=\"5.1.107.89\" -I<talloc>/include"
export LDFLAGS="-L<talloc>/lib"
make -j1 V=1 READELF="<ndk>/bin/llvm-readelf"
```

La versión sale de la definición del paquete del CI
(`TERMUX_PKG_VERSION` en `proot/ci/termux/packages/proot/build.sh`), igual que
en el empaquetado oficial.

### Características del artefacto

| Propiedad | Valor medido |
|---|---|
| Tamaño | 460 392 B |
| SHA-256 | `b58f3b3d279f430b9e0e2ec8943117dc8c3757fa391392e4444f15a02d6249b7` |
| Formato | `Type: DYN`, `Machine: AArch64`, intérprete `/system/bin/linker64` |
| `NEEDED` | `libdl.so`, `libc.so` |
| talloc | Estático: los símbolos con `talloc` en el nombre (80) quedan resueltos dentro del binario, no en una biblioteca aparte |
| Loaders | Los dos blobs van embebidos (`_binary_loader_exe_size` y `_binary_loader_m32_exe_size`): loader AArch64 `EXEC` en `0x2000000000` y loader ELF32 ARM `EXEC` en `0x20000000` |

El `libtalloc.a` que produce el script son 50 574 B, armados con
`llvm-ar` a partir de `bin/default/talloc.c.6.o`; la `talloc.h` instalada
ocupa 65 526 B.

### Limitación: el binario no se puede ejecutar en el host

`qemu-aarch64-static` no puede cargarlo porque el linker dinámico de bionic
no forma parte del sysroot del NDK:

```text
Could not open '/system/bin/linker64'
```

Hacen falta un dispositivo real o una imagen de Android.

## Ruta C — la ruta exacta del CI (Docker Desktop)

Es la ruta canónica: la que ejecuta
[`.github/workflows/build-proot.yml`](../../../.github/workflows/build-proot.yml)
en `ubuntu-26.04`.

```bash
./proot/ci/termux/scripts/run-docker.sh ./proot/ci/termux/build-package.sh -I -a aarch64 --format pacman proot
```

Avisos específicos de Windows:

| Punto | Detalle |
|---|---|
| Daemon | Docker Desktop 4.95.0 está instalado por usuario, pero su daemon **tenía que estar arrancado**. Durante las mediciones de esta página el daemon no corría y la ruta no se ejecutó |
| `readlink -f` | `run-docker.sh` lo usa para resolver su propio directorio; WSL2 y Git Bash lo tienen, una consola `cmd.exe` no |
| AppArmor | Docker Desktop no lo ofrece: el script degrada a un aviso (`WARNING: apparmor_parser not found`) y continúa |
| Montajes | **No uses `-m/--mount-termux-dirs`**: monta `/data`, que en Windows no existe. La caché se monta con `TERMUX_DOCKER_RUN_EXTRA_ARGS`, como hace el CI |
| Formato | `run-docker.sh` declara `pkg_format: debian` en `repo.json` pero el workflow usa `--format pacman`; manda el pipeline |

Esta ruta produce el `.pkg.tar.xz` publicable; las rutas A y B producen el
binario, no el paquete.

## Por qué no existe un binario nativo de Windows

No es una decisión de estilo: se intentó y se midió.

### MSVC

Compilación de solo sintaxis de los 74 ficheros `.c` con MSVC 19.44.35229
(VS 2022 BuildTools) + Windows SDK 10.0.26100.0: **72 fallan**, y los 2 que
"terminan" solo porque son unidades de traducción vacías
(`proot/src/proot.c` es solo cabeceras y
`extension/ashmem_memfd/ashmem_memfd.c` está entero dentro de
`#if defined(__ANDROID__) || defined(__BIONIC__)`). Primer error:

```text
fatal error C1083: No se puede abrir el archivo incluir: 'linux/limits.h'
```

Censo de las 644 líneas `#include <...>` del árbol: **61 cabeceras
Linux/POSIX** no existen en Windows, con 240 instancias de uso:

| Cabecera | Instancias |
|---|---|
| `unistd.h` | 43 |
| `linux/limits.h` | 24 |
| `sys/socket.h` | 13 |
| `sys/queue.h` | 9 |
| `sys/un.h` | 8 |
| `sys/wait.h` | 8 |
| `linux/net.h` | 7 |
| `sched.h` | 6 |
| `sys/param.h` | 6 |
| `netinet/in.h` | 6 |
| `sys/uio.h` | 6 |

### MinGW-w64

Con `x86_64-w64-mingw32-gcc` 13.2.0 (cross-compilado dentro de WSL2): 71 de
74 fallan en crudo. Con un *shim* POSIX + UAPI de Linux hecho a mano, 21
unidades de traducción compilan, pero `nm -u` sigue pidiendo símbolos que
Windows no tiene:

```text
prctl  fchmodat  mkdtemp  mkstemp  openat  unlinkat  readlinkat
S_ISLNK  WEXITSTATUS  compare_paths  translate_path
detranslate_sysnum  tracee_lookup_readonly
```

Los últimos cuatro ni siquiera son de la libc: son internos del propio proot.

### Muros del lenguaje que ni una capa POSIX resuelve

Añadir las cabeceras que faltan no basta. Hay construcciones del propio
lenguaje que el compilador de Microsoft no acepta, y están en el núcleo del
proyecto, no en la periferia:

| Construcción | Dónde | Qué pasa con MSVC |
|---|---|---|
| `asm volatile` (ensamblador en línea de GNU) | `loader/assembly-x86_64.h:32,81`, `loader/assembly-x86.h:32`, `loader/assembly-arm64.h:31,86`, `loader/assembly-arm.h:31,80,96` | MSVC sobre x64 no tiene ensamblador en línea, no hay equivalente que reescribir. Y el loader es justo el fragmento que proot mete dentro del tracee. |
| `__attribute__((packed))` | `extension/net_policy/net_policy.c:127,135,150,159,166,174,182`, `extension/virtual_net/virtual_net_internal.h:71,76` | MSVC no conoce `__attribute__`; su equivalente es `#pragma pack`. Son las estructuras del protocolo PRCT que viajan por el control fd: cambiar el empaquetado rompe el contrato de bytes con el harness. |
| `__builtin_expect`, `__builtin_unreachable`, `__builtin_types_compatible_p` | `loader/loader.c:48,51`, `loader/assembly-*.h`, `extension/fake_id0/fake_id0.c:925-926`, `extension/fake_id0/stat.c:29-30,136-137` | Son extensiones de GCC/Clang; cada una hay que reescribirla a mano. |
| `word_t = unsigned long` | `arch.h:31` | En LLP64 `long` es de 32 bits: el tamaño de palabra con el que el código calcula direcciones y desplazamientos cambia de golpe. Ver el error de ensamblado de abajo. |

### El loader no se puede ensamblar

Ni siquiera el sub-build del loader funciona en Windows:

```text
x86_64-w64-mingw32-gcc -ffreestanding -fPIC -nostdlib -static loader/loader.c
Error: operand type mismatch for 'movq'
```

La causa está en `proot/src/arch.h:31`:

```c
typedef unsigned long word_t;
```

En Windows `long` es de 32 bits (LLP64) y en Linux de 64 (LP64). La misma
macro emite `movq %rcx,%rsp` en Linux y `movq %ecx,%rsp` en Windows: el
registro de 64 bits se trunca y el ensamblador lo rechaza.

### El núcleo depende del kernel de Linux

| Mecanismo | Evidencia medida |
|---|---|
| `ptrace` | 288 referencias a `PTRACE_*` en 24 ficheros |
| `seccomp-BPF` | Filtro construido con `BPF_STMT`/`BPF_JUMP` e instalado con `prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER)` (`syscall/seccomp.c:109-136`, `:360`) |
| `/proc` | 150 referencias en 21 ficheros |
| Números de syscall | 3179 números Linux repartidos en 6 tablas por arquitectura más `syscall/sysnums.list` |
| Registros | `struct user_regs_struct` es la ABI del kernel de Linux |
| Loader ELF embebido | `execve/elf.c`, `execve/ldso.c`, `execve/auxv.c` y `loader/loader.c` con syscalls crudos (`CLOSE`, `OPEN`, `OPENAT`, `MMAP`, `MPROTECT`, `PRCTL`, `EXECVE`, `EXIT`), enlazados en una dirección fija de Linux (`-Ttext=0x600000000000` en x86_64, `0x2000000000` en aarch64) |

Las 13 extensiones del fork están atadas a ese mismo núcleo: `proc_isolation`
intercepta `PR_ptrace` y `PR_process_vm_*`, `sysvipc` emula SysV IPC sobre los
tracees, `virtual_net` lee y escribe la memoria del tracee con
`PTRACE_PEEKDATA`, `net_policy` habla el protocolo del control fd y
`ashmem_memfd` llama a `syscall(__NR_memfd_create)`. No son una capa
desacoplable: son el mismo tracer.

## Defectos reales encontrados al compilar

### D1 — `ashmem_memfd` sin `<string.h>`

`extension/ashmem_memfd/ashmem_memfd.c` usaba `strcmp` y `memset` sin incluir
`<string.h>`. Con clang ≥ 16 eso es un error duro, y solo en Android (el
fichero entero está dentro de `#if defined(__ANDROID__) || defined(__BIONIC__)`):

```text
error: call to undeclared function 'strcmp'
```

**Corrección**: añadir el `#include <string.h>`.

### D2 — LTO con clang y el loader embebido

El build por defecto (loader embebido) con `CC=clang` y `-flto` fallaba al
incrustar el loader: clang emite objetos de bitcode LLVM que `objdump` y
`objcopy` no saben leer.

```text
llvm-objdump: error: 'cli/cli.o': The file was not recognized as a valid object file
```

**Corrección**: en el makefile, detectar un compilador tipo clang y añadir
`-ffat-lto-objects` cuando el LTO está activo, más una variable `READELF ?=`
para que los cross-builds usen `llvm-readelf`. El CI de Termux no se vio
afectado porque fija `PROOT_UNBUNDLE_LOADER` (no incrusta el loader).

Con esa corrección el makefile ya añade el flag por su cuenta (la ruta B
muestra `The makefile already handles the clang LTO objects`). Por si se
compila contra un makefile anterior, el script de Windows comprueba el
makefile y, si no encuentra `ffat-lto-objects`, añade el flag él mismo. No
hace falta tocar nada a mano en ningún caso.

### Nota: `loader/loader-info.c`

`proot/src/loader/loader-info.c` está versionado **y** listado en
`.gitignore`: el makefile lo regenera siempre a partir de `loader/loader`, y
su contenido versionado está desfasado (el build calcula `1296`). No afecta al
resultado porque el fichero se rehace en cada compilación.

## Verificación

Para comprobar cualquier binario de proot:

```bash
file proot                                  # ELF 64-bit LSB pie, x86-64 o ARM aarch64
./proot --version                           # proot-<version> y los aceleradores
./proot --about                             # debe incluir "proot-termux fork:"
```

Prueba de humo dentro de la distribución (el `env -i` es importante: el
entorno heredado rompe `execve` en rootfs):

```bash
env -i PATH=/bin:/usr/bin PROOT_RUNTIME_DIR=/tmp/proot-probe TMPDIR=/tmp/proot-probe \
  ./proot -r / -b /proc -b /dev -b /sys /bin/echo SMOKE_OK
```

Vista de `/proc` aislada (el guest solo debe ver sus propios tracees):

```bash
env -i PATH=/bin:/usr/bin PROOT_RUNTIME_DIR=/tmp/proot-probe TMPDIR=/tmp/proot-probe \
  ./proot -r / -b /proc -b /dev -b /sys --proc-isolated /bin/sh -c 'ls /proc'
```

Registro de la red virtual (con un `bind()` AF_INET dentro del guest):

```bash
ls -l /tmp/proot-probe/proot-net/WINPROBE/registry.lock
od -An -tx1 -N4 /tmp/proot-probe/proot-net/WINPROBE/registry.lock
# 4e 4f 52 50  -> 0x50524F4E, "PRON"
```

Para el binario de Android, la comprobación es estática porque no se puede
ejecutar en el host:

```bash
"<ndk>/bin/llvm-readelf" -h proot      # Machine: AArch64, Type: DYN (PIE)
"<ndk>/bin/llvm-readelf" -d proot      # NEEDED libdl.so, libc.so
```

## Limitaciones conocidas

- **El binario de Android no se puede ejecutar en el host**: falta
  `/system/bin/linker64` (el loader de bionic no está en el sysroot del NDK).
  Necesita un dispositivo o una imagen de Android.
- **La batería de pruebas no es agnóstica del host**: `proot/tests/run.sh` y
  los scripts de `proot/tests/proot/` necesitan un host Linux o Termux (y
  `env -i` con `TMPDIR`/`PROOT_RUNTIME_DIR` apuntando a rutas host
  escribibles). La única rebanada que no necesita el binario de proot es la de
  la API de control (`proot/tests/control-api/python`, `python3 -m unittest
  discover`), que prueba el protocolo PRCT por sí solo.
- **La ruta C no se ejecutó en esta máquina**: el daemon de Docker Desktop
  estaba apagado y la máquina tenía menos de 1 GB de RAM libre. La ruta está
  documentada, no medida.
- **La ruta B copia el árbol de fuentes** dentro de WSL
  (`$HOME/proot-build/android/src`). El cross-build nunca debe reutilizar los
  objetos del build nativo.
- **`proot/artifacts/windows/` no está en `.gitignore`**: los artefactos
  aparecen en `git status` hasta que se añada la ruta.

## Relacionado

- [Uso operativo de PRoot y `termux-isolated`](usage.md)
- [Origen del código](../architecture/proot-source.md)
- [Índice de la documentación](../INDEX.md)
- [Scripts de build](../../scripts/README.md)
- [Definición del paquete de CI](../../ci/termux/packages/proot/build.sh)
- [Preparación del SDK Android en el CI](../../ci/termux/scripts/setup-android-sdk.sh)
- [Workflow de build](../../../.github/workflows/build-proot.yml)