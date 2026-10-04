#!/data/data/com.termux/files/usr/bin/bash
# Regresión de resource_limit: --fd-limit (heredado por el guest) y --proc-limit
# (puerta guest-side sobre fork/clone/vfork con -EAGAIN).
#
# --proc-limit se mide con un probe C: bash reintenta EAGAIN internamente y un
# test en shell daria falso negativo.
set -uo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR" || exit 1

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
PROOT="${PROOT:-$PREFIX/bin/proot}"
EAGAIN=11

FAILS=0
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }
pass() { echo "  ok - $*"; }

[ -x "$PROOT" ] || { echo "SKIP: falta $PROOT (ejecuta ./proot/scripts/build-native.sh -i)"; exit 0; }
command -v clang >/dev/null 2>&1 || { echo "SKIP: falta clang para el probe de forks"; exit 0; }

FIX=$(mktemp -d "$TMPDIR/proot-rlimit.XXXXXX") || exit 1
cleanup() { [ -n "${FIX:-}" ] && rm -rf -- "$FIX"; }
trap cleanup EXIT INT TERM

clang -D_GNU_SOURCE -o "$FIX/fork_gate" "$ROOT/tests/proot/probes/fork_gate.c" || {
    fail "no se compilo fork_gate.c"
    exit 1
}

run() {
    env -i PATH="$PREFIX/bin" TMPDIR="$TMPDIR" HOME="$FIX" \
        "$PROOT" --kill-on-exit "$@" 2>"$FIX/stderr"
}

echo "=== resource_limit: --fd-limit y --proc-limit ==="

# --- --fd-limit: el guest percibe RLIMIT_NOFILE ---
got=$(run --fd-limit 64 "$PREFIX/bin/sh" -c 'ulimit -Sn; ulimit -Hn' | tr -d '[:space:]')
if [ "$got" = "6464" ]; then
    pass "--fd-limit 64 se hereda al guest (soft y hard = 64)"
else
    fail "--fd-limit 64: el guest reportó '$got' (esperado 6464)"
fi

if run --fd-limit 8 "$PREFIX/bin/true" 2>/dev/null; then
    fail "--fd-limit 8 fue aceptado (el mínimo es 32)"
else
    pass "--fd-limit 8 rechazado por la validación"
    grep -q "at least 32" "$FIX/stderr" \
        && pass "el rechazo explica el mínimo" \
        || fail "--fd-limit 8 fallo por otra razon: $(cat "$FIX/stderr")"
fi

# --- --proc-limit: control sin puerta ---
base=$(run "$FIX/fork_gate" 5)
case "$base" in
    created=5\ requested=5\ errno=0) pass "sin --proc-limit los 5 forks se crean" ;;
    *) fail "control de forks sin limite dio '$base' (esperado created=5 errno=0)" ;;
esac

# --- --proc-limit: la puerta responde EAGAIN ---
gated=$(run --proc-limit 3 "$FIX/fork_gate" 5)
created=${gated#created=}
created=${created%% *}
errno_seen=${gated##*errno=}
case "$gated" in
    created=[0-4]*)
        pass "--proc-limit 3 cerro la puerta tras $created proceso(s) (pidia 5)" ;;
    *)
        fail "--proc-limit 3 devolvió '$gated' (esperado menos de 5 forks)" ;;
esac
if [ "$errno_seen" = "$EAGAIN" ]; then
    pass "el fork rechazado recibe EAGAIN, la semantica natural de RLIMIT_NPROC"
else
    fail "el fork rechazado devolvió errno=$errno_seen (esperado $EAGAIN)"
fi
if [ "${created:-0}" -lt 5 ] && [ "${created:-0}" -ge 1 ]; then
    pass "la puerta no niega todo ni deja pasar todo"
else
    fail "conteo de forks fuera de rango: '$gated'"
fi

if run --proc-limit 0 "$PREFIX/bin/true" 2>/dev/null; then
    fail "--proc-limit 0 fue aceptado (el mínimo es 1)"
else
    pass "--proc-limit 0 rechazado por la validación"
fi

echo ""
if [ "$FAILS" -ne 0 ]; then
    echo "FAILURES: $FAILS"
    exit 1
fi
echo "PASS: --fd-limit y --proc-limit se ejercen como documenta AGENTS.md"
