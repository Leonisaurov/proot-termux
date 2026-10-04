#!/data/data/com.termux/files/usr/bin/bash
# Contrato de los meta files de fake_id0 y de su alcance real.
#
# Contexto verificado: el subsistema de meta files ("<dir>/.proot-meta-file.<x>",
# helper_functions.c + open/mk/chmod/chown/stat/rename/unlink/exec/access/
# utimensat) está entero bajo #ifdef USERLAND, y USERLAND no se define en ninguna
# config de build de este repo (GNUmakefile, scripts/build-native.sh,
# ci/termux/packages/proot/build.sh).  Por eso el binario no lo contiene y el
# vector de symlink es LATENTE: solo sería explotable en un build con USERLAND.
#
# Qué valida:
#   1. Alcance: el binario instalado sigue sin el subsistema (si alguien
#      activa USERLAND, este test se vuelve rojo y obliga a revalidar el punto 2
#      con proot real, no con grep).
#   2. Contrato del código: las rutas de meta se abren con O_NOFOLLOW y con
#      S_ISREG en lectura, fscanf valida su retorno, y get_meta_path mide antes
#      de concatenar.  Un fopen() pelado sobre la ruta del meta reabre el escape
#      de integridad en cuanto USERLAND exista.
#   3. Contrato observable de `-0` en el build vigente: el guest se ve root,
#      chmod/chown no son denegados, stat sirve dueño 0, y NO se crean meta
#      files en el territorio del guest.
set -uo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR" || exit 1

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
PROOT="${PROOT:-$PREFIX/bin/proot}"
HELPERS="$ROOT/src/extension/fake_id0/helper_functions.c"

FAILS=0
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }
pass() { echo "  ok - $*"; }

[ -x "$PROOT" ] || { echo "SKIP: falta $PROOT (ejecuta ./proot/scripts/build-native.sh -i)"; exit 0; }
[ -f "$HELPERS" ] || { echo "SKIP: no existe $HELPERS"; exit 0; }

echo "=== fake_id0: meta files (USERLAND) y contrato de -0 ==="

# --- 1. alcance en el build vigente ---
if command -v strings >/dev/null 2>&1; then
    if [ "$(strings "$PROOT" | grep -c 'proot-meta-file')" = "0" ]; then
        pass "el binario no incluye el subsistema de meta files (USERLAND apagado)"
    else
        fail "el binario AHORA contiene meta files: USERLAND se activó y hay que revalidar el symlink ejecutando proot"
    fi
else
    echo "  skip - strings no disponible; no se comprueba el alcance"
fi

# --- 2. contrato del código de las rutas de meta ---
if grep -qE '\bfopen[[:space:]]*\(' "$HELPERS"; then
    fail "quedó un fopen() sobre rutas de meta (reabre el symlink-follow)"
else
    pass "ningún fopen() en helper_functions.c: las metas se abren por fd"
fi

for pat in 'O_NOFOLLOW' 'O_CLOEXEC' 'S_ISREG'; do
    if grep -q "$pat" "$HELPERS"; then
        pass "meta_open exige $pat"
    else
        fail "falta $pat en meta_open()"
    fi
done

if grep -qE 'if \(fscanf\(fp, "%d %d %d "[^)]*\) != 3\)' "$HELPERS"; then
    pass "read_meta_file descarta un meta que no declara tres enteros"
else
    fail "read_meta_file no valida el retorno de fscanf (modo sin inicializar)"
fi

check_line=$(grep -n 'return -ENAMETOOLONG' "$HELPERS" | cut -d: -f1 | head -1)
strcat_line=$(grep -n 'strcat(meta_path' "$HELPERS" | cut -d: -f1 | head -1)
if [ -n "$check_line" ] && [ -n "$strcat_line" ] && [ "$check_line" -lt "$strcat_line" ]; then
    pass "get_meta_path mide la longitud antes de concatenar"
else
    fail "get_meta_path concatena antes de medir (overflow de char[PATH_MAX])"
fi

# --- 3. contrato observable de -0 con el build vigente ---
FIX=$(mktemp -d "$TMPDIR/proot-fake-id0.XXXXXX") || exit 1
cleanup() { [ -n "${FIX:-}" ] && rm -rf -- "$FIX"; }
trap cleanup EXIT INT TERM
mkdir -p "$FIX/bind"

guest() {
    env -i PATH="$PREFIX/bin" TMPDIR="$TMPDIR" HOME="$FIX" \
        "$PROOT" --change-id=0:0 --kill-on-exit --bind="$FIX/bind:/mnt" "$@" 2>"$FIX/stderr"
}

uid=$(guest "$PREFIX/bin/id" -u | tr -d '[:space:]')
if [ "$uid" = "0" ]; then
    pass "-0: el guest se ve uid 0"
else
    fail "-0: el guest reportó uid '$uid' (esperado 0) $(cat "$FIX/stderr")"
fi

if guest "$PREFIX/bin/sh" -c 'touch /mnt/plain && chmod 640 /mnt/plain && chown 123:123 /mnt/plain'; then
    pass "-0: create/chmod/chown emulados no son denegados"
else
    fail "-0: la secuencia create/chmod/chown falló: $(cat "$FIX/stderr")"
fi

owner=$(guest "$PREFIX/bin/stat" -c '%u %g' /mnt/plain | tr -d '[:space:]')
if [ "$owner" = "00" ]; then
    pass "-0: stat sirve dueño root al guest"
else
    fail "-0: stat sirvió dueño '$owner' (esperado 0 0)"
fi

leftovers=$(find "$FIX/bind" -name '.proot-meta-file.*' -print | wc -l | tr -d '[:space:]')
if [ "$leftovers" = "0" ]; then
    pass "-0: el build vigente no escribe meta files en el territorio del guest"
else
    fail "-0: aparecieron $leftovers meta files: USERLAND está activo y el symlink-follow debe revalidarse ejecutando proot"
fi

echo ""
if [ "$FAILS" -ne 0 ]; then
    echo "FAILURES: $FAILS"
    exit 1
fi
echo "PASS: contrato de meta files y de --change-id=0:0 intactos"
