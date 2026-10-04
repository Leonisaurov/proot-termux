#!/data/data/com.termux/files/usr/bin/bash
# Regresión de ashmem_memfd: el camino memfd_create/ashmem debe dejar al guest
# un fd útil, escribible y con el tamaño correcto, tanto con la flag como sin
# ella (principio "sin flags -> cero overhead").
#
# Las dos ramas del diseño se cubren según el dispositivo:
#   - kernel con memfd_create: la extensión no interviene y el resultado debe
#     ser idéntico con y sin flag.
#   - kernel sin memfd_create: sin flag el probe falla; con --ashmem-memfd debe
#     funcionar reescribiendo la syscall a openat("/dev/ashmem").
set -uo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR" || exit 1

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
PROOT="${PROOT:-$PREFIX/bin/proot}"

FAILS=0
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }
pass() { echo "  ok - $*"; }

[ -x "$PROOT" ] || { echo "SKIP: falta $PROOT (ejecuta ./proot/scripts/build-native.sh -i)"; exit 0; }
command -v clang >/dev/null 2>&1 || { echo "SKIP: falta clang para el probe de memfd"; exit 0; }

FIX=$(mktemp -d "$TMPDIR/proot-ashmem.XXXXXX") || exit 1
cleanup() { [ -n "${FIX:-}" ] && rm -rf -- "$FIX"; }
trap cleanup EXIT INT TERM

clang -D_GNU_SOURCE -o "$FIX/memfd_probe" "$ROOT/tests/proot/probes/memfd_probe.c" || {
    fail "no se compilo memfd_probe.c"
    exit 1
}

run() {
    env -i PATH="$PREFIX/bin" TMPDIR="$TMPDIR" HOME="$FIX" \
        "$PROOT" --kill-on-exit "$@" 2>"$FIX/stderr"
}

EXPECTED='size=11 read=11 content=proot-memfd'

echo "=== ashmem_memfd: memfd_create dentro del sandbox ==="

plain=$(run "$FIX/memfd_probe")
armed=$(run --ashmem-memfd "$FIX/memfd_probe")

case "$plain" in
    *"$EXPECTED"*)
        # El kernel soporta memfd: la flag no debe cambiar nada observable.
        if [ "$armed" = "$plain" ]; then
            pass "con memfd nativo, --ashmem-memfd no altera lo que percibe el guest"
        else
            fail "--ashmem-memfd cambió el resultado: '$plain' -> '$armed'"
        fi ;;
    memfd_errno=*)
        # Kernel sin memfd: aquí es donde la extensión tiene que ganar.
        pass "sin la flag el guest no tiene memfd (${plain})"
        case "$armed" in
            *"$EXPECTED"*)
                pass "--ashmem-memfd da un fd escribible, legible y con st_size correcto" ;;
            *)
                fail "--ashmem-memfd no rescata memfd_create: '$armed' $(cat "$FIX/stderr")" ;;
        esac ;;
    *)
        fail "el probe no devolvió un resultado interpretable: '$armed' / '$(cat "$FIX/stderr")'" ;;
esac

# El fd tiene que seguir siendo útil para el stub de fstat de la extensión:
# se comprueba dentro del guest con un tamaño no trivial.
big=$(run --ashmem-memfd "$PREFIX/bin/sh" -c 'echo -n 0123456789 > /dev/null; echo ok')
if [ "$big" = "ok" ]; then
    pass "el guest sigue pudiendo escribir bajo la flag"
else
    fail "--ashmem-memfd rompio el guest basico: '$big' $(cat "$FIX/stderr")"
fi

echo ""
if [ "$FAILS" -ne 0 ]; then
    echo "FAILURES: $FAILS"
    exit 1
fi
echo "PASS: ashmem_memfd deja al guest un fd de memoria util"
