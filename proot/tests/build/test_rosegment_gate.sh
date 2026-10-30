#!/usr/bin/env bash
# Regression del enlace del loader: '--rosegment' solo puede aplicarse cuando el
# toolchain lo necesita (android/aarch64).
#
# Causa raiz verificada (2026-10-29): GNUmakefile probeaba si $(LD) acepta
# -Wl,--rosegment y lo aplicaba a LOADER_LDFLAGS para TODA arquitectura. En
# x86_64/glibc con BFD ld, el loader se enlaza con -Ttext=0x600000000000
# (arch.h) y --rosegment deja .rodata aun mas lejos de .text, asi que el enlace
# moria antes de producir binario:
#   relocation truncated to fit: R_X86_64_PC32 against `.rodata'
#   collect2: error: ld returned 1 exit status
# El workaround nacio para el lld del NDK r29+, que es donde si corresponde.
#
# Que valida:
#   1. La receta del loader aplica --rosegment si y solo si el triple de CC es
#      linux-android o aarch64 (expectativa derivada del triple, no hardcodeada,
#      para que el test valga igual en el host y en Termux).
#   2. El triple android con codegen x86_64 ENLAZA FALLO (control de sensibilidad:
#      prueba que el flag es la causa, no un factor indiferente).
#   3. Con el gate vigente el loader del enlace nativo se construye de verdad.
#   4. El escape hatch 'make ROSEGMENT=' sigue forzando la omision.
set -uo pipefail

# En Termux TMPDIR apunta al tmp privado de la app; fuera de Termux se usa el
# temporal del sistema (la regla "nunca /tmp" aplica al guest Termux).
if [ -d /data/data/com.termux/files/usr/tmp ]; then
    : "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
else
    : "${TMPDIR:=/tmp}"
fi
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
SRC="$ROOT/src"

FAILS=0
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }
pass() { echo "  ok - $*"; }

for cmd in make gcc sed grep; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "SKIP: falta $cmd"; exit 0; }
done
[ -f "$SRC/GNUmakefile" ] || { echo "SKIP: no existe $SRC/GNUmakefile"; exit 0; }
[ -f "$SRC/loader/loader.c" ] || { echo "SKIP: no existe $SRC/loader/loader.c"; exit 0; }

WORK=$(mktemp -d "$TMPDIR/proot-rosegment.XXXXXX") || exit 1
cleanup() { [ -n "${WORK:-}" ] && [ -d "$WORK" ] && rm -rf -- "$WORK"; }
trap cleanup EXIT INT TERM

# Arbol aislado: sin .o/.d generados, para que la receta del loader se resuelva
# desde cero y el grafo de dependencias del arbol real no interfiera.
cp -a "$SRC" "$WORK/src"
find "$WORK/src" -type f \( -name '*.o' -o -name '*.d' \) -delete
cd "$WORK/src" || exit 1

# Wrapper de CC: reporta un triple falso para -dumpmachine y delega el trabajo
# real en gcc. Permite ejercitar el gate sin tener el toolchain cruzado.
mkdir -p "$WORK/bin"
make_wrapper() {
    local triple=$1 path="$WORK/bin/cc-$2"
    cat > "$path" << WRAP
#!/bin/sh
if [ "\$1" = "-dumpmachine" ]; then echo "$triple"; exit 0; fi
exec gcc "\$@"
WRAP
    chmod +x "$path"
    echo "$path"
}

# Expectativa derivada del triple: el flag debe aparecer en android o aarch64.
expect_rosegment() {
    if printf '%s' "$1" | grep -Eq 'linux-android|aarch64'; then
        echo yes
    else
        echo no
    fi
}

# Receta del loader (solo el enlace) para un CC dado.
loader_recipe() {
    make -n loader/loader CC="$1" V=1 2>/dev/null | grep -E 'o loader/loader .*loader\.o'
}

NATIVE_TRIPLE=$(gcc -dumpmachine 2>/dev/null || echo desconocido)
echo "=== triple nativo: $NATIVE_TRIPLE ==="

for spec in \
    "android-aarch64:aarch64-linux-android" \
    "android-arm:arm-linux-androideabi" \
    "gnu-aarch64:aarch64-linux-gnu" \
    "gnu-x86_64:x86_64-unknown-linux-gnu" \
    "gnu-i386:i686-pc-linux-gnu"; do
    name=${spec%%:*}
    triple=${spec#*:}
    cc=$(make_wrapper "$triple" "$name")
    recipe=$(loader_recipe "$cc")
    want=$(expect_rosegment "$triple")

    if [ -z "$recipe" ]; then
        fail "$name: no se obtuvo la receta de enlace del loader"
        continue
    fi

    if printf '%s' "$recipe" | grep -q -- '--rosegment'; then have=yes; else have=no; fi
    if [ "$have" = "$want" ]; then
        pass "$name ($triple): --rosegment $([ "$want" = yes ] && echo aplicado || echo omitido)"
    else
        fail "$name ($triple): esperaba --rosegment $want, la receta dice $have"
    fi

    # invariante comun a toda receta: el loader sigue enlazandose en su direccion
    # fija, si no el gate estaria enmascarando otra regresion
    if printf '%s' "$recipe" | grep -q -- '-Ttext=0x'; then
        pass "$name: la receta conserva -Ttext=<LOADER_ADDRESS>"
    else
        fail "$name: la receta perdio -Ttext=<LOADER_ADDRESS>"
    fi
done

echo ""
echo "=== sensibilidad: el flag rompe el enlace donde no aplica ==="
# Codegen real x86_64 + triple android falso: si --rosegment fuera inocuo, este
# enlace pasaria. Debe fallar con "relocation truncated to fit".
if [ "$(printf '%s' "$NATIVE_TRIPLE" | grep -c aarch64)" -eq 0 ]; then
    # Triple android NO-aarch64: activa la rama android del gate sin arrastrar
    # -march=armv8-a+crc (que gcc x86_64 rechazaria antes de llegar al enlace).
    cc_android=$(make_wrapper "arm-linux-androideabi" "sens")
    if make loader/loader CC="$cc_android" >"$WORK/rose.log" 2>&1; then
        fail "el enlace con --rosegment forzado paso: el test perdio sensibilidad"
    elif grep -q "relocation truncated to fit" "$WORK/rose.log"; then
        pass "--rosegment sobre codegen x86_64 trunca la relocacion (causa confirmada)"
    else
        fail "el enlace fallo por otra razon:"
        sed -n '1,8p' "$WORK/rose.log" >&2
    fi
else
    echo "  skip - control de truncamiento no aplicable en aarch64 nativo"
fi

echo ""
echo "=== enlace nativo del loader con el gate vigente ==="
rm -f loader/loader loader/loader.o loader/assembly.o
if make loader/loader CC=gcc >"$WORK/native.log" 2>&1 && [ -f loader/loader ]; then
    pass "make loader/loader produce el binario sin overrides"
else
    fail "el enlace nativo del loader fallo (rc=$?)"
    sed -n '1,15p' "$WORK/native.log" >&2
fi

echo ""
echo "=== escape hatch: ROSEGMENT= explicito ==="
if [ "$(expect_rosegment "$NATIVE_TRIPLE")" = yes ]; then
    # En android/aarch64 el gate aplica el flag; vaciarlo por linea de comandos
    # debe seguir enlazando (override documentado).
    rm -f loader/loader loader/loader.o loader/assembly.o
    if make loader/loader CC=gcc ROSEGMENT= >"$WORK/hatch.log" 2>&1 && [ -f loader/loader ]; then
        pass "make ROSEGMENT= fuerza la omision y enlaza"
    else
        fail "el override ROSEGMENT= no enlazo"
        sed -n '1,15p' "$WORK/hatch.log" >&2
    fi
else
    rm -f loader/loader loader/loader.o loader/assembly.o
    if make loader/loader CC=gcc ROSEGMENT= >"$WORK/hatch.log" 2>&1 && [ -f loader/loader ]; then
        pass "make ROSEGMENT= enlaza igual que el default del gate"
    else
        fail "el override ROSEGMENT= no enlazo"
        sed -n '1,15p' "$WORK/hatch.log" >&2
    fi
fi

echo ""
if [ "$FAILS" -ne 0 ]; then
    echo "FAILURES: $FAILS"
    exit 1
fi
echo "PASS: --rosegment reservado al toolchain android/aarch64"
