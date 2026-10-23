#!/data/data/com.termux/files/usr/bin/bash
# Regresión de las direcciones fijas de carga en ARM64 (upstream 37c09de3b1).
#
# LOADER/EXEC_PIC/INTERP_PIC son mapeos fijos que parten el espacio de
# direcciones del tracee. Con la colocación antigua (0x20/0x30/0x3f GiB) un
# kernel Android de 39 bits dejaba un hueco de ~260 GiB y una reserva de
# 256 GiB fallaba con ENOMEM sólo dentro de proot. Ahora quedan agrupadas por
# debajo de donde el kernel carga los PIE, así que [0, 336 GiB) sigue libre.
set -uo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR" || exit 1

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
LAUNCHER="${LAUNCHER:-$ROOT/bin/termux-isolated}"
PROBE_SRC="$ROOT/tests/proot/probes/big_mmap.c"

FAILS=0
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }
pass() { echo "  ok - $*"; }

[ -x "$LAUNCHER" ] || { echo "SKIP: falta $LAUNCHER"; exit 0; }
[ -f "$PROBE_SRC" ] || { echo "SKIP: falta $PROBE_SRC"; exit 0; }
command -v clang >/dev/null 2>&1 || { echo "SKIP: falta clang para el probe de mmap"; exit 0; }

FIX=$(mktemp -d "$TMPDIR/proot-addrs.XXXXXX") || exit 1
cleanup() { [ -n "${FIX:-}" ] && rm -rf -- "$FIX"; }
trap cleanup EXIT INT TERM

clang -D_GNU_SOURCE -Wall -o "$FIX/big_mmap" "$PROBE_SRC" || {
	echo "FAIL: no se pudo compilar el probe" >&2
	exit 1
}

# El probe debe funcionar en el host: si el dispositivo ya no puede reservar
# 256 GiB por su cuenta, la comparación no prueba nada.
if ! "$FIX/big_mmap" >"$FIX/native.txt" 2>&1; then
	echo "SKIP: el host tampoco reserva 256 GiB ($(tail -1 "$FIX/native.txt"))"
	exit 0
fi
pass "host reserva 256 GiB"

out=$("$LAUNCHER" --termux-paths --rw-dir "$TMPDIR" -- "$FIX/big_mmap" 2>&1)
rc=$?
if [ "$rc" -ne 0 ] || ! printf '%s' "$out" | grep -q 'OK mmap 256 GiB'; then
	fail "guest no pudo reservar 256 GiB (rc=$rc): $(printf '%s' "$out" | tail -2)"
else
	pass "guest reserva 256 GiB bajo proot"
fi

# La dirección del loader debe ser la nueva: 0x5400000000 en arch.h.
if grep -q 'LOADER_ADDRESS     0x5400000000' "$ROOT/src/arch.h"; then
	pass "arch.h fija LOADER_ADDRESS en 0x5400000000"
else
	fail "arch.h no tiene la colocación nueva de LOADER_ADDRESS"
fi

if [ "$FAILS" -gt 0 ]; then
	echo "=== FAILURES: $FAILS ==="
	exit 1
fi
echo "=== SUMMARY: arm64 load addresses PASS ==="
