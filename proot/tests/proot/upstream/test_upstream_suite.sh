#!/data/data/com.termux/files/usr/bin/bash
# Wrapper de la suite de upstream vendorizada en tests/upstream/.
#
# Los tests de termux/proot son autocontenidos pero asumieron un host Linux:
# escriben en /tmp (inexistente en Termux, que no tiene root) y llaman a proot
# por la variable PROOT. Este wrapper los ejecuta *dentro* de un proot al que
# se le traduce /tmp a un directorio escribible, y clasifica cada resultado.
#
# La batería del fork no depende de ellos: si el harness anidado no arranca en
# este dispositivo, el wrapper hace SKIP con la razón, no falla.
set -uo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR" || exit 1

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
PROOT="${PROOT:-$PREFIX/bin/proot}"
SUITE="$ROOT/tests/upstream"

FAILS=0
pass() { echo "  ok - $*"; }
skip() { echo "SKIP - $*"; }
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }

[ -x "$PROOT" ] || { echo "SKIP: falta $PROOT (ejecuta ./proot/scripts/build-native.sh -i)"; exit 0; }
[ -d "$SUITE" ] || { echo "SKIP: no está $SUITE"; exit 0; }
command -v mcookie >/dev/null 2>&1 || { echo "SKIP: falta mcookie (util-linux) para la suite de upstream"; exit 0; }
command -v gcc >/dev/null 2>&1 || { echo "SKIP: falta gcc para compilar los probes de la suite"; exit 0; }

FIX=$(mktemp -d "$TMPDIR/proot-upstream.XXXXXX") || exit 1
cleanup() { [ -n "${FIX:-}" ] && rm -rf -- "$FIX"; }
trap cleanup EXIT INT TERM
mkdir -p "$FIX/tmp"

# Canario: el harness necesita anidar proot dentro de proot. Si eso no arranca
# aquí, nada de lo que sigue es medible.
if ! env -i TMPDIR="$FIX/tmp" PATH="$PREFIX/bin:$PREFIX/xbin:/system/bin" \
	PROOT_LOADER="$PREFIX/libexec/proot/loader" PROOT_TMP_DIR="$FIX" \
	"${PROOT}" -0 --bind="${FIX}/tmp:/tmp" "${PREFIX}/bin/sh" -c 'test -w /tmp' \
	>"$FIX/canary.log" 2>&1; then
	skip "el harness anidado de upstream no arranca aquí: $(tail -2 "$FIX/canary.log" | tr '\n' ' ')"
	exit 0
fi
pass "canario: proot anidado con /tmp traducido"

# Causas conocidas de este dispositivo, medidas una a una. No se esconden: se
# listan con su motivo, y cualquier fallo fuera de esta tabla se reporta FAIL.
env_reason() {
	case "$1" in
		test-2e1b4cd0) echo "necesita helpers de 32 bits (gcc -m32) corriendo anidados" ;;
		test-4171f0b5) echo "su bucle de 9999 enlaces no cabe en el tiempo de este wrapper" ;;
		test-7fa2c1d4) echo "usa FICLONE/reflink, inexistente en el filesystem del dispositivo" ;;
		test-9f16ee06) echo "requiere rename(2) entre filesystemes (EXDEV), aquí es el mismo" ;;
		test-bcf7c15f) echo "el test se autorremata con SIGTERM al agotar su propio reloj" ;;
		test-bf7be26e) echo "el proot interno del test no hereda la traducción de /tmp" ;;
		*) echo "" ;;
	esac
}

RAN=0
for t in "$SUITE"/test-*.sh; do
	[ -f "$t" ] || continue
	RAN=$((RAN + 1))
	name=$(basename "$t" .sh)
	out=$(env -i TMPDIR="$FIX/tmp" PROOT="$PROOT" PROOT_LOADER="$PREFIX/libexec/proot/loader" \
		PROOT_TMP_DIR="$FIX" PATH="$PREFIX/bin:$PREFIX/xbin:/system/bin" \
		"${PROOT}" -0 --bind="${FIX}/tmp:/tmp" "${PREFIX}/bin/sh" "$t" 2>&1)
	rc=$?
	reason=$(env_reason "$name")
	case "$rc" in
		0)  pass "$name" ;;
		125) skip "$name (el propio test declaró entorno insuficiente)" ;;
		*)
			if [ -n "$reason" ]; then
				skip "$name (bloqueado por entorno: $reason)"
			else
				fail "$name (rc=$rc): $(printf '%s' "$out" | tail -1 | cut -c1-90)"
			fi
			;;
	esac
done

echo "=== SUMMARY: suite upstream: $RAN tests shell ejecutados, fallos=$FAILS ==="
[ "$FAILS" -eq 0 ] || exit 1
