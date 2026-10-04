#!/data/data/com.termux/files/usr/bin/bash
# Regresion de la extension mountinfo: /proc/<pid>/mountinfo que lee el guest
# debe listar los bindings activos (para que helpers como bubblewrap encuentren
# el "mount" que creen haber hecho) y conservarse como tabla valida.
#
# Sin rootfs la raiz del guest es la del host, asi que el camino ejercitado es
# "tabla real + lineas de bindings", que es determinista.  El camino de
# termux/proot#294 (raiz bajo /data, columna root reescrita a "/") pertenece a
# termux-isolated y lo cubre tests/termux-isolated/proc/.
#
# Con --proc-isolated el callback legacy esta expluitamente fuera (hace
# early-return): la vista la sintetiza proc_isolation y aqui solo se verifica
# que el archivo siga siendo una tabla mountinfo parseable.
set -uo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR" || exit 1

PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
PROOT="${PROOT:-$PREFIX/bin/proot}"

FAILS=0
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }
pass() { echo "  ok - $*"; }

[ -x "$PROOT" ] || { echo "SKIP: falta $PROOT (ejecuta ./proot/scripts/build-native.sh -i)"; exit 0; }

FIX=$(mktemp -d "$TMPDIR/proot-mountinfo.XXXXXX") || exit 1
cleanup() { [ -n "${FIX:-}" ] && rm -rf -- "$FIX"; }
trap cleanup EXIT INT TERM

mkdir -p "$FIX/bind" "$FIX/second"

run() {
    env -i PATH="$PREFIX/bin" TMPDIR="$TMPDIR" HOME="$FIX" \
        "$PROOT" --kill-on-exit "$@" 2>"$FIX/stderr"
}

echo "=== mountinfo: bindings visibles para el guest ==="

out=$(run --bind="$FIX/bind:/mnt" --bind="$FIX/second:/srv" \
         "$PREFIX/bin/sh" -c 'cat /proc/self/mountinfo') || {
    fail "no se pudo leer /proc/self/mountinfo en el guest: $(cat "$FIX/stderr")"
    echo ""
    echo "FAILURES: $FAILS"
    exit 1
}

if [ -n "$out" ]; then
    pass "el guest obtiene una tabla de mounts"
else
    fail "el guest recibio mountinfo vacio"
fi

binds=$(printf '%s\n' "$out" | grep ' - bind ')

if printf '%s\n' "$binds" | grep -qE '^[0-9]+ [0-9]+ 0:1 / /mnt rw,relatime - bind '"$FIX"'/bind rw,relatime$'; then
    pass "el binding /mnt aparece como mount bind"
else
    fail "falta la linea del binding /mnt; lineas de bind obtenidas:"
    printf '%s\n' "$binds" >&2
fi

if printf '%s\n' "$binds" | grep -qE '^[0-9]+ [0-9]+ 0:1 / /srv rw,relatime - bind '"$FIX"'/second rw,relatime$'; then
    pass "el segundo binding aparece como mount bind"
else
    fail "falta la linea del binding /srv"
fi

# insort_binding3 ordena la lista, asi que no se asume que /mnt sea el primero:
# lo que importa es que cada mount reciba su propio id.
ids=$(printf '%s\n' "$binds" | awk '{print $1}' | sort -u | wc -l | tr -d '[:space:]')
if [ "$ids" = "2" ]; then
    pass "los dos mounts sintetizados tienen ids distintos"
else
    fail "los bind sintéticos comparten id ($ids lineas unicas)"
fi

if printf '%s\n' "$out" | grep -qE '^[0-9]+ [0-9]+ 0:1 / / rw,relatime - bind '; then
    fail "la raiz '/' se emite como bind (debe estar cubierta por la tabla del kernel)"
else
    pass "el binding raiz no se duplica en la tabla"
fi

if printf '%s\n' "$out" | grep -vq ' - bind '; then
    pass "las lineas reales del kernel se conservan"
else
    fail "la tabla sintetizada perdio las lineas del kernel"
fi

# --- --proc-isolated: la sintesis de proc_isolation sigue siendo parseable ---
iso=$(run --proc-isolated --bind="$FIX/bind:/mnt" \
         "$PREFIX/bin/sh" -c 'cat /proc/self/mountinfo') || {
    fail "--proc-isolated no sirvio mountinfo: $(cat "$FIX/stderr")"
    iso=""
}
if [ -n "$iso" ]; then
    bad=$(printf '%s\n' "$iso" | awk 'NF > 0 && index($0, " - ") == 0 {c++} END {print c+0}')
    if [ "$bad" = "0" ]; then
        pass "--proc-isolated sirve una tabla mountinfo validamente separada"
    else
        fail "--proc-isolated sirvio $bad linea(s) sin separador ' - '"
    fi
fi

echo ""
if [ "$FAILS" -ne 0 ]; then
    echo "FAILURES: $FAILS"
    exit 1
fi
echo "PASS: mountinfo expone los bindings del guest como mounts"
