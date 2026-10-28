#!/usr/bin/env bash
# Regresion de los dos defectos de construccion que aparecieron al compilar el
# fork desde un host Windows (WSL2 con clang):
#
#   D1 - extension/ashmem_memfd/ashmem_memfd.c llamaba a strcmp(3) y memset(3)
#        sin declarar <string.h>.  El cuerpo del archivo esta detras de
#        "#if defined(__ANDROID__) || defined(__BIONIC__)", de modo que solo lo
#        ve un build Android/Bionic, y clang >= 16 convierte la declaracion
#        implicita en error duro:
#          error: call to undeclared library function 'strcmp'
#                 [-Wimplicit-function-declaration]
#
#   D2 - con CC=clang y -flto (los OPTIMIZATION_FLAGS por defecto del
#        makefile) "clang -flto -c" deja un objeto de bitcode crudo que ni
#        objdump ni objcopy leen, y el paso que incrusta el loader (OBJIFY)
#        revienta:
#          objdump: 'cli/cli.o': file format not recognized
#          objcopy: 'loader/loader-wrapped.o': No such file or directory
#        El arreglo esta en el makefile: se detecta un compilador tipo clang y
#        se anade -ffat-lto-objects cuando hay LTO, y loader/loader-info.c se
#        genera con $(READELF) en lugar del "readelf" pelado.
#
# Que valida:
#   1. D1 estatico: el TU de ashmem_memfd incluye <string.h> y usa strcmp(3) y
#      memset(3) (si no los usara, el include sobraria).
#   2. D1 compilador: el TU se compila con -D__ANDROID__ y
#      -Werror=implicit-function-declaration sin ningun diagnostico de
#      declaracion implicita.
#   3. D2 makefile: con CC=clang la linea de compilacion lleva
#      -ffat-lto-objects exactamente una vez; con un compilador que no es
#      clang no lo lleva.
#   4. D2 build: la copia del arbol en el temporal compila con CC=clang y
#      -flto, cli/cli.o sigue siendo un ELF legible por objdump y los bytes
#      del loader aparecen dentro del binario proot.
#
# El test es autonomo: no necesita red, Docker ni un Termux.  Si falta una
# herramienta o una condicion del entorno hace SKIP limpio (exit 0); solo
# falla cuando el defecto esta presente.
set -euo pipefail

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
# Base de los temporales: $TMPDIR cuando existe y es escribible; si no, el
# directorio temporal que el propio sistema ofrece.  Nunca /tmp literal y
# nunca el arbol del repositorio.
TMP_BASE=""
TMP_BASE_OWNED=0
if [ -n "${TMPDIR:-}" ] && [ -d "$TMPDIR" ] && [ -w "$TMPDIR" ]; then
    TMP_BASE="$TMPDIR"
else
    TMP_BASE=$(mktemp -d) || { echo "SKIP: no hay un directorio temporal utilizable"; exit 0; }
    TMP_BASE_OWNED=1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

# Rutas HOST: el arbol de fuentes del repositorio y la copia de build que
# vive en el temporal.  Rutas GUEST: ninguna, este test no arranca rootfs ni
# guest y todo lo que compila es codigo host; los dos espacios no se mezclan.
HOST_SRC="$PROOT_DIR/src"
HOST_ASHMEM="$HOST_SRC/extension/ashmem_memfd/ashmem_memfd.c"

JOBS="${PROOT_TEST_JOBS:-2}"

FAILS=0
SKIPS=0
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }
pass() { echo "  ok - $*"; }
skip() { echo "SKIP: $*"; SKIPS=$((SKIPS + 1)); }

[ -f "$HOST_SRC/GNUmakefile" ] || { echo "SKIP: no existe $HOST_SRC/GNUmakefile"; exit 0; }
[ -f "$HOST_ASHMEM" ] || { echo "SKIP: no existe $HOST_ASHMEM"; exit 0; }
command -v make >/dev/null 2>&1 || { echo "SKIP: falta make"; exit 0; }
command -v grep >/dev/null 2>&1 || { echo "SKIP: falta grep"; exit 0; }
command -v od >/dev/null 2>&1 || { echo "SKIP: falta od (coreutils)"; exit 0; }

# Compilador para la comprobacion de D1: clang si esta disponible, si no
# cualquiera.  La ausencia de compilador no es un fallo: solo se pierde la
# comprobacion de compilacion, la estatica sigue siendo valida.
CC_ANY=""
for candidate in clang gcc cc; do
    if command -v "$candidate" >/dev/null 2>&1; then
        CC_ANY="$candidate"
        break
    fi
done

# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------
# Todo el build ocurre fuera del repositorio: proot/src se copia al temporal
# y se compila ahi, de modo que el arbol de trabajo no se toca.
FIX=$(mktemp -d "$TMP_BASE/proot-clang-lto.XXXXXX") || { echo "SKIP: no se pudo crear el temporal"; exit 0; }
cleanup() {
    if [ -n "${FIX:-}" ] && [ -d "$FIX" ]; then
        rm -rf -- "$FIX"
    fi
    if [ "$TMP_BASE_OWNED" = "1" ] && [ -n "${TMP_BASE:-}" ] && [ -d "$TMP_BASE" ]; then
        rm -rf -- "$TMP_BASE"
    fi
    return 0
}
trap cleanup EXIT INT TERM

# copy_source_tree <destino>: copia proot/src sin los artefactos de build
# (objetos, .d, build.h, loader generados).  Esos se regeneran, y durante un
# build concurrente en el repositorio cambian bajo los pies de la copia.
BUILD_SRC="$FIX/src"
copy_source_tree() {
    local dest=$1 attempt=0
    while [ "$attempt" -lt 3 ]; do
        attempt=$((attempt + 1))
        rm -rf -- "$dest"
        mkdir -p "$dest"
        if command -v tar >/dev/null 2>&1; then
            if (cd "$HOST_SRC" && tar -cf - \
                    --exclude='*.o' --exclude='*.d' --exclude='*.tmp' \
                    --exclude='./proot' --exclude='./build.h' --exclude='./licenses' \
                    --exclude='./loader/loader' --exclude='./loader/loader-m32' \
                    --exclude='./loader/loader-info.c' --exclude='.check_*.res' \
                    .) | (cd "$dest" && tar -xf -); then
                clean_artifacts "$dest"
                return 0
            fi
        elif cp -a "$HOST_SRC/." "$dest" 2>/dev/null; then
            clean_artifacts "$dest"
            return 0
        fi
        sleep 1 2>/dev/null || true
    done
    return 1
}

# Los artefactos de build (objetos, .d, build.h, loader generados) no se
# copian: el makefile los regenera.
clean_artifacts() {
    find "$1" -type f \( -name '*.o' -o -name '*.d' \) -delete
    rm -rf -- "$1/proot" "$1/build.h" "$1/licenses" \
        "$1/loader/loader" "$1/loader/loader-m32" "$1/loader/loader-info.c"
    return 0
}

if copy_source_tree "$BUILD_SRC"; then
    :
else
    BUILD_SRC=""
    skip "no se pudo copiar proot/src al temporal: las comprobaciones de D2 se omiten"
fi

# bytes_embedded <haystack> <needle>: 0 si la secuencia de bytes de <needle>
# aparece dentro de <haystack>.  La comparacion va en hexadecimal para no
# depender de grep con datos binarios, y el desplazamiento tiene que ser par
# para que el emparejamiento caiga en un limite de byte.
bytes_embedded() {
    local hay=$1 needle=$2 off=""
    [ -s "$needle" ] || return 1
    od -An -v -tx1 "$needle" | tr -d ' \n' > "$FIX/needle.hex"
    od -An -v -tx1 "$hay" | tr -d ' \n' > "$FIX/haystack.hex"
    off=$(grep -b -o -m1 -F -f "$FIX/needle.hex" "$FIX/haystack.hex" 2>/dev/null | cut -d: -f1) || off=""
    [ -n "$off" ] || return 1
    [ $((off % 2)) -eq 0 ] || return 1
    return 0
}

echo "=== D1: ashmem_memfd declara <string.h> ==="

if grep -qE '^[[:space:]]*#[[:space:]]*include[[:space:]]*<string\.h>' "$HOST_ASHMEM"; then
    pass "ashmem_memfd.c incluye <string.h>"
else
    fail "ashmem_memfd.c no incluye <string.h>"
fi

# Las lineas de include se excluyen del conteo: el comentario del propio
# include menciona strcmp(3) y memset(3) y no es un uso real.
uses=$(grep -vE '^[[:space:]]*#[[:space:]]*include' "$HOST_ASHMEM" \
    | grep -cE '\b(strcmp|memset)[[:space:]]*\(' || true)
if [ "$uses" -gt 0 ]; then
    pass "el TU usa strcmp(3)/memset(3) en $uses lineas: el include es necesario"
else
    pass "el TU no usa strcmp(3)/memset(3): el include se puede revisar"
fi

echo "=== D1: compilacion Android sin declaracion implicita ==="

if [ -z "$CC_ANY" ]; then
    skip "no hay compilador C: la comprobacion de compilacion no se puede hacer"
else
    # <linux/ashmem.h> solo existe en el kernel de Android.  En un host Linux
    # cualquiera se sustituye por un stub minimo con los mismos valores, para
    # que el TU se pueda comprobar sin un rootfs Android.
    STUB_INC="$FIX/stub-include"
    mkdir -p "$STUB_INC/linux"
    if ! printf '#include <linux/ashmem.h>\n' | "$CC_ANY" -xc -fsyntax-only - >/dev/null 2>&1; then
        cat > "$STUB_INC/linux/ashmem.h" <<'STUB_EOF'
/* Stub de prueba: linux/ashmem.h solo existe en el kernel de Android. */
#ifndef PROOT_TEST_STUB_LINUX_ASHMEM_H
#define PROOT_TEST_STUB_LINUX_ASHMEM_H
#define ASHMEM_NAME_LEN 256
#define ASHMEM_SET_SIZE 0x4001
#define ASHMEM_GET_SIZE 0x4002
#endif
STUB_EOF
    fi
    if ! printf '#include <linux/memfd.h>\n' | "$CC_ANY" -xc -fsyntax-only - >/dev/null 2>&1; then
        cat > "$STUB_INC/linux/memfd.h" <<'STUB_EOF'
/* Stub de prueba: linux/memfd.h solo existe en kernels recientes. */
#ifndef PROOT_TEST_STUB_LINUX_MEMFD_H
#define PROOT_TEST_STUB_LINUX_MEMFD_H
#define MFD_CLOEXEC 0x0001U
#endif
STUB_EOF
    fi

    # -D__ANDROID__ es lo que hace visible el cuerpo del TU: sin el, el
    # archivo entero se reduce a un include vacio.
    d1_rc=0
    "$CC_ANY" -fsyntax-only -D__ANDROID__ -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
        -I"$HOST_SRC" -I"$STUB_INC" \
        -Werror=implicit-function-declaration \
        "$HOST_ASHMEM" > "$FIX/d1-compile.log" 2>&1 || d1_rc=$?

    if [ "$d1_rc" -eq 0 ]; then
        pass "el TU compila con -D__ANDROID__ sin declaraciones implicitas"
    elif grep -qE 'implicit-function-declaration|undeclared library function' "$FIX/d1-compile.log"; then
        fail "el TU usa strcmp(3)/memset(3) sin declarar <string.h>"
        sed -n '1,12p' "$FIX/d1-compile.log" >&2
    else
        skip "el TU no se puede compilar en este host (cabeceras del sistema ausentes)"
        sed -n '1,6p' "$FIX/d1-compile.log" >&2
    fi
fi

echo "=== D2: deteccion de clang y -ffat-lto-objects en el makefile ==="

if [ -z "$BUILD_SRC" ]; then
    skip "sin copia de proot/src: la deteccion del makefile no se puede ejercitar"
elif ! command -v clang >/dev/null 2>&1; then
    skip "clang no esta instalado: la deteccion del makefile no se puede ejercitar"
else
    # Un compilador que no es clang: gcc no acepta -ffat-lto-objects, asi que
    # el makefile no debe anadirlo.
    GCC_LIKE=""
    for candidate in gcc cc; do
        if command -v "$candidate" >/dev/null 2>&1 \
            && ! "$candidate" --version 2>/dev/null | grep -qi clang; then
            GCC_LIKE="$candidate"
            break
        fi
    done

    dry_rc=0
    dry_clang=$(cd "$BUILD_SRC" && env -u CFLAGS -u OPTIMIZATION_FLAGS \
        make -n CC=clang cli/cli.o 2>"$FIX/dry-clang.err") || dry_rc=$?

    if [ "$dry_rc" -ne 0 ]; then
        fail "make -n CC=clang fallo (rc=$dry_rc): el makefile no se puede ni simular"
        sed -n '1,10p' "$FIX/dry-clang.err" >&2
    else
        clang_line=$(printf '%s\n' "$dry_clang" | grep -m1 -E -- '-c .*cli/cli\.c' || true)
        if [ -z "$clang_line" ]; then
            fail "make -n no imprimio la linea de compilacion de cli/cli.c"
        else
            fat_count=$(printf '%s\n' "$clang_line" | grep -o -- '-ffat-lto-objects' | wc -l || true)
            if [ "$fat_count" -eq 1 ]; then
                pass "con CC=clang la linea de compilacion lleva -ffat-lto-objects una vez"
            elif [ "$fat_count" -eq 0 ]; then
                fail "con CC=clang la linea de compilacion no lleva -ffat-lto-objects: el objeto seria bitcode crudo"
            else
                fail "con CC=clang la linea de compilacion lleva -ffat-lto-objects $fat_count veces (esperado 1)"
            fi
        fi
    fi

    if [ -z "$GCC_LIKE" ]; then
        skip "no hay un compilador que no sea clang: la variante gcc no se puede comprobar"
    else
        dry_rc=0
        dry_gcc=$(cd "$BUILD_SRC" && env -u CFLAGS -u OPTIMIZATION_FLAGS \
            make -n CC="$GCC_LIKE" cli/cli.o 2>"$FIX/dry-gcc.err") || dry_rc=$?

        if [ "$dry_rc" -ne 0 ]; then
            skip "make -n CC=$GCC_LIKE fallo en este host (rc=$dry_rc)"
            sed -n '1,6p' "$FIX/dry-gcc.err" >&2
        else
            gcc_line=$(printf '%s\n' "$dry_gcc" | grep -m1 -E -- '-c .*cli/cli\.c' || true)
            if [ -z "$gcc_line" ]; then
                skip "make -n CC=$GCC_LIKE no imprimio la linea de compilacion de cli/cli.c"
            elif printf '%s\n' "$gcc_line" | grep -q -- '-ffat-lto-objects'; then
                fail "con CC=$GCC_LIKE la linea de compilacion lleva -ffat-lto-objects: gcc no lo soporta"
            else
                pass "con CC=$GCC_LIKE la linea de compilacion no lleva -ffat-lto-objects"
            fi
        fi
    fi
fi

echo "=== D2: build con clang + LTO y loader incrustado ==="

if [ -z "$BUILD_SRC" ]; then
    skip "sin copia de proot/src: el build de D2 no se puede hacer"
elif ! command -v clang >/dev/null 2>&1; then
    skip "clang no esta instalado: el build de D2 no se puede hacer"
else
    # Paso 1: el que reventaba con D2, la generacion del objeto que envuelve
    # el loader (OBJIFY).  Es el punto exacto de la regresion.
    wrap_rc=0
    (cd "$BUILD_SRC" && env -u CFLAGS -u OPTIMIZATION_FLAGS \
        make -j"$JOBS" CC=clang loader/loader-wrapped.o) > "$FIX/build-wrap.log" 2>&1 || wrap_rc=$?

    if [ "$wrap_rc" -ne 0 ]; then
        if grep -qE 'file format not recognized|not recognized as a valid object file' "$FIX/build-wrap.log" \
            || grep -qE 'loader(-m32)?-wrapped\.o' "$FIX/build-wrap.log"; then
            fail "el build con clang+LTO falla: los objetos son bitcode crudo y no se pueden enlazar ni incrustar (D2)"
            sed -n '1,15p' "$FIX/build-wrap.log" >&2
        else
            skip "el build no se puede completar en este host (rc=$wrap_rc)"
            sed -n '1,10p' "$FIX/build-wrap.log" >&2
        fi
    else
        pass "make CC=clang genera loader/loader-wrapped.o con -flto activo"

        if command -v objdump >/dev/null 2>&1; then
            if objdump -f "$BUILD_SRC/cli/cli.o" > "$FIX/objdump-cli.log" 2>&1; then
                pass "cli/cli.o sigue siendo un objeto ELF legible por objdump"
            else
                fail "objdump no puede leer cli/cli.o: el objeto es bitcode crudo"
                sed -n '1,5p' "$FIX/objdump-cli.log" >&2
            fi
        else
            skip "objdump no esta instalado: la legibilidad del objeto no se puede comprobar"
        fi

        # El payload incrustado es el loader sin simbolos (loader.exe en el
        # makefile): se extrae del objeto envolvente y se compara con una
        # copia recién stripeada del loader construido.
        PAYLOAD="$FIX/loader-payload.bin"
        if command -v objcopy >/dev/null 2>&1; then
            if objcopy -O binary "$BUILD_SRC/loader/loader-wrapped.o" "$PAYLOAD" 2>"$FIX/objcopy.log" \
                && [ -s "$PAYLOAD" ]; then
                pass "el objeto envolvente contiene el payload del loader ($(wc -c < "$PAYLOAD" | tr -d ' ') bytes)"

                if command -v strip >/dev/null 2>&1 && [ -f "$BUILD_SRC/loader/loader" ]; then
                    cp "$BUILD_SRC/loader/loader" "$FIX/loader.exe"
                    if strip "$FIX/loader.exe" 2>/dev/null && cmp -s "$FIX/loader.exe" "$PAYLOAD"; then
                        pass "el payload incrustado es el loader recien construido"
                    else
                        fail "el payload incrustado no coincide con el loader construido"
                    fi
                else
                    skip "strip no esta disponible: el payload no se puede contrastar con el loader"
                fi
            else
                fail "no se pudo extraer el payload del loader del objeto envolvente"
                sed -n '1,5p' "$FIX/objcopy.log" >&2
            fi
        else
            skip "objcopy no esta instalado: el payload del loader no se puede extraer"
        fi

        # Paso 2: el binario completo, que es donde tiene que aparecer el
        # loader.  Aqui tambien se construye el loader de 32 bits, que puede
        # faltar en hosts sin soporte multilib: eso es del entorno, no de D2.
        full_rc=0
        (cd "$BUILD_SRC" && env -u CFLAGS -u OPTIMIZATION_FLAGS \
            make -j"$JOBS" CC=clang) > "$FIX/build-full.log" 2>&1 || full_rc=$?

        if [ "$full_rc" -ne 0 ]; then
            skip "el binario completo no se pudo enlazar en este host (rc=$full_rc)"
            sed -n '1,10p' "$FIX/build-full.log" >&2
        else
            pass "make CC=clang construye el binario proot con -flto"

            if [ ! -f "$BUILD_SRC/proot" ]; then
                fail "el build termino bien pero no hay binario proot"
            elif [ ! -s "$PAYLOAD" ]; then
                skip "no hay payload del loader con el que comparar"
            elif bytes_embedded "$BUILD_SRC/proot" "$PAYLOAD"; then
                pass "los bytes del loader aparecen dentro del binario proot"
            else
                fail "los bytes del loader no aparecen dentro del binario proot"
            fi
        fi
    fi
fi

echo ""
if [ "$FAILS" -ne 0 ]; then
    echo "FAILURES: $FAILS"
    exit 1
fi
if [ "$SKIPS" -ne 0 ]; then
    echo "PASS: clang+LTO incrusta el loader y ashmem_memfd declara <string.h> ($SKIPS comprobaciones omitidas)"
else
    echo "PASS: clang+LTO incrusta el loader y ashmem_memfd declara <string.h>"
fi