#!/usr/bin/env bash
#
# Regresión: las cadenas de symlinks válidas no deben fallar con ELOOP.
#
# Causa raíz corregida: canonicalize() cortaba la recursión en
# `recursion_level > 3` en lugar de MAXSYMLINKS, así que cualquier cadena de
# 4 o más enlaces devolvía ELOOP aunque el kernel la resolviera sin problema.
# El límite ahora coincide con el del kernel (MAXSYMLINKS), y el caso negativo
# (ciclo real) debe seguir fallando con ELOOP.
#
# Modo de rutas: rootfs `-r /` (identidad host==guest), así que las rutas del
# fixture son válidas en ambos lados y la comparación nativo/proot es directa.
# Los fixtures viven bajo $TMPDIR y se eliminan al salir.

set -u

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)

if [ -d /data/data/com.termux/files/usr/tmp ]; then
    : "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
else
    : "${TMPDIR:=/tmp}"
fi
export TMPDIR

# Resolución del binario: override explícito, build del perfil host, build
# en el árbol, o instalación en $PREFIX (Termux).
find_proot() {
    if [ -n "${PROOT_BIN:-}" ] && [ -x "${PROOT_BIN}" ]; then
        printf '%s\n' "$PROOT_BIN"
        return 0
    fi
    for candidate in \
        "$REPO_ROOT/build-host/proot" \
        "$REPO_ROOT/src/proot" \
        "${PREFIX:-/nonexistent}/bin/proot"
    do
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

PROOT=$(find_proot) || {
    echo "SKIP: proot no disponible (compila con ./proot/scripts/build-native.sh)"
    exit 0
}

WORK="$TMPDIR/proot-symlink-chain.$$"
cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

mkdir -p "$WORK" || {
    echo "FAIL: no se pudo crear el fixture en $WORK" >&2
    exit 1
}

PASS=0
FAILED=0
ok()   { echo "  ok - $1"; PASS=$((PASS + 1)); }
fail() { echo "  not ok - $1" >&2; FAILED=$((FAILED + 1)); }

echo "=== fixtures: cadenas de symlinks y ciclo ==="
printf 'CADENA-OK\n' > "$WORK/target.txt" || exit 1

# Cadena de N enlaces: linkN -> link(N-1) -> ... -> target.txt
make_chain() {
    local depth=$1
    ln -sf target.txt "$WORK/link1"
    local i=2
    while [ "$i" -le "$depth" ]; do
        ln -sf "link$((i - 1))" "$WORK/link$i"
        i=$((i + 1))
    done
}
make_chain 6
make_chain 20

# Ciclo real: a -> b -> a (debe fallar con ELOOP en ambos lados).
ln -sf cycle_b "$WORK/cycle_a"
ln -sf cycle_a "$WORK/cycle_b"

echo "=== preflight: el kernel resuelve las cadenas ==="
native_ok=true
for depth in 6 20; do
    if [ "$(cat "$WORK/link$depth" 2>/dev/null)" = "CADENA-OK" ]; then
        ok "nativo resuelve la cadena de $depth enlaces"
    else
        fail "nativo no resuelve la cadena de $depth enlaces (fixture inválido)"
        native_ok=false
    fi
done

if [ "$native_ok" != "true" ]; then
    echo "FAIL: el fixture no es válido en este host; se aborta la comparación" >&2
    exit 1
fi

echo "=== proot: misma resolución que el kernel ==="
for depth in 6 20; do
    output=$("$PROOT" -r / /bin/cat "$WORK/link$depth" 2>&1)
    status=$?
    if [ "$status" -eq 0 ] && [ "$output" = "CADENA-OK" ]; then
        ok "proot resuelve la cadena de $depth enlaces"
    else
        fail "proot falló en la cadena de $depth enlaces (exit=$status, salida='$output')"
    fi
done

echo "=== proot: ruta relativa dentro de la cadena ==="
output=$(cd "$WORK" && "$PROOT" -r / /bin/cat ./link6 2>&1)
status=$?
if [ "$status" -eq 0 ] && [ "$output" = "CADENA-OK" ]; then
    ok "proot resuelve la cadena desde una ruta relativa"
else
    fail "proot falló con ruta relativa (exit=$status, salida='$output')"
fi

echo "=== control negativo: un ciclo real sigue dando ELOOP ==="
"$PROOT" -r / /bin/cat "$WORK/cycle_a" >/dev/null 2>&1
proot_status=$?
/bin/cat "$WORK/cycle_a" >/dev/null 2>&1
native_status=$?
if [ "$proot_status" -ne 0 ] && [ "$native_status" -ne 0 ]; then
    ok "el ciclo falla con error en nativo (exit=$native_status) y en proot (exit=$proot_status)"
else
    fail "el ciclo no falló como se esperaba (nativo=$native_status, proot=$proot_status)"
fi

# El error reportado no debe ser otro distinto de ELOOP.
proot_error=$("$PROOT" -r / /bin/cat "$WORK/cycle_a" 2>&1)
case "$proot_error" in
    *"Too many levels of symbolic links"*)
        ok "proot reporta ELOOP en el ciclo, no otro error" ;;
    *)
        fail "proot reportó un error inesperado en el ciclo: '$proot_error'" ;;
esac

echo
if [ "$FAILED" -ne 0 ]; then
    echo "FAIL: $FAILED comprobaciones fallaron ($PASS correctas)" >&2
    exit 1
fi

echo "PASS: cadenas de symlinks resueltas como el kernel ($PASS comprobaciones)"
exit 0
