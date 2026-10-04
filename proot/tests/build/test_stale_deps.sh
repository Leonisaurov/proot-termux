#!/data/data/com.termux/files/usr/bin/bash
# Regression del sistema de construccion local: los archivos de dependencias
# (.d) que genera GNUmakefile deben sobrevivir a la desaparicion de headers
# del sistema.
#
# Causa raiz verificada: ndk-sysroot 29-3 -> 30-0 (2026-09-24) elimino
# $PREFIX/include/android/legacy_stdlib_inlines.h y $PREFIX/include/bits/stdlib_inlines.h.
# Los .d ya generados (sin -MP) seguian declarandolos como prerequisitos, asi
# que make abortaba antes de compilar nada:
#   make: *** No rule to make target '.../legacy_stdlib_inlines.h',
#   needed by '.check_process_vm.o'.  Stop.
#
# Que valida (usa make -n, no necesita el binario proot):
#   1. make compila un objeto real del arbol con los headers vigentes.
#   2. Simulacion de header eliminado: un prerequisito inexistente que conserva
#      su stub phony (lo que emite -MP) no detiene el grafo.
#   3. Control negativo: el mismo prerequisito sin stub debe fallar con
#      "No rule to make target" (el test detecta la regresion real, no un flag).
set -uo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
SRC="$ROOT/src"

OBJECT="cli/note.o"
DEP="cli/note.d"

FAILS=0
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }
pass() { echo "  ok - $*"; }

for cmd in make gcc sed; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "SKIP: falta $cmd"; exit 0; }
done
[ -f "$SRC/GNUmakefile" ] || { echo "SKIP: no existe $SRC/GNUmakefile"; exit 0; }
[ -f "$SRC/cli/note.c" ] || { echo "SKIP: no existe $SRC/cli/note.c"; exit 0; }

WORK=$(mktemp -d "$TMPDIR/proot-stale-deps.XXXXXX") || exit 1
cleanup() { [ -n "${WORK:-}" ] && [ -d "$WORK" ] && rm -rf -- "$WORK"; }
trap cleanup EXIT INT TERM

# Fixture: copia fiel del arbol de fuentes sin dependencias ni objetos
# generados, para forzar la regeneracion de los .d.
cp -a "$SRC" "$WORK/src"
find "$WORK/src" -type f \( -name '*.o' -o -name '*.d' \) -delete

cd "$WORK/src" || exit 1

# Variables por linea de comandos: aisladas del entorno de la sesion y del
# sistema de flags de build-native.sh (el recipe COMPILE conserva -MD/-MP).
MAKE=(make "$OBJECT" CC=gcc
      CPPFLAGS="-D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -I. -DARG_MAX=131072"
      CFLAGS="-O2")

echo "=== RED/GREEN: generacion de dependencias -MD -MP ==="
if "${MAKE[@]}" >"$WORK/build.log" 2>&1; then
    pass "make $OBJECT compila con los headers vigentes"
else
    fail "make $OBJECT fallo (rc=$?)"
    sed -n '1,20p' "$WORK/build.log" >&2
fi

if [ ! -f "$DEP" ]; then
    fail "no se genero $DEP (el compile no produjo dependencias)"
    echo ""
    echo "FAILURES: $FAILS"
    exit 1
fi

# Header de sistema tomado del propio .d: se reescribe a una ruta inexistente
# tanto en la lista de prerequisitos como en su stub, que es exactamente la
# forma que deja -MP cuando ndk-sysroot elimina un header.
HDR=$(tr ' ' '\n' < "$DEP" | sed -n 's|^\(/.*\.h\)$|\1|p' | head -1)
if [ -z "$HDR" ]; then
    fail "el .d no declara headers de sistema; no se puede simular la remocion"
else
    GONE="$PREFIX/include/proot-stale-deps-gone-$$.h"
    if [ -e "$GONE" ]; then
        fail "$GONE existe; el control negativo no seria concluyente"
    else
        sed -i "s|$HDR|$GONE|g" "$DEP"

        if grep -qE "^[[:space:]]*${GONE}:" "$DEP"; then
            pass "$DEP conserva el stub phony del header eliminado"
        else
            fail "$DEP no tiene stub phony para $HDR (falta -MP en COMPILE)"
        fi

        # Positivo: prerequisito inexistente CON stub -> el grafo se resuelve.
        if make -n "$OBJECT" >"$WORK/positive.log" 2>&1; then
            pass "make -n resuelve un prerequisito eliminado que conserva su stub"
        else
            fail "make -n aborto con un header eliminado pero con stub"
            sed -n '1,10p' "$WORK/positive.log" >&2
        fi

        # Negativo: sin el stub, make debe reportar la regla faltante. Esto es
        # el error reportado y garantiza que el test sea sensible a la regresion.
        sed -i "\#^[[:space:]]*${GONE}:[[:space:]]*\$#d" "$DEP"
        if grep -qE "^[[:space:]]*${GONE}:" "$DEP"; then
            fail "no se pudo eliminar el stub de ${GONE} (control negativo no aplicable)"
        elif make -n "$OBJECT" >"$WORK/negative.log" 2>&1; then
            fail "make -n no detecto la falta de regla para ${GONE} (test ciego)"
        elif grep -q "No rule to make target" "$WORK/negative.log"; then
            pass "control negativo: sin stub, make falla con 'No rule to make target'"
        else
            fail "make -n fallo por otra razon, no por la regla faltante"
            sed -n '1,10p' "$WORK/negative.log" >&2
        fi
    fi
fi

echo ""
if [ "$FAILS" -ne 0 ]; then
    echo "FAILURES: $FAILS"
    exit 1
fi
echo "PASS: dependencias de build tolerantes a headers eliminados"
