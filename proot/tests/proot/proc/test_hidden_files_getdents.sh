#!/data/data/com.termux/files/usr/bin/bash
# Regresión de hidden_files sobre getdents(2) fallido (upstream 355f840ea5 y
# b8b3e6280b).
#
# hidden_files leía el resultado como unsigned int: un ENOTDIR (listar algo que
# no es directorio) o un EINVAL (buffer corto) se interpretaba como un listado
# de casi 4 GiB, y PRoot copiaba esa cantidad desde el tracee hacia un buffer de
# stack del tamaño pedido. El tracee moría con SIGSEGV del propio proot. Ahora
# el resultado es signed y un fallo se deja pasar tal cual.
set -uo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR" || exit 1

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
PROOT="${PROOT:-$PREFIX/bin/proot}"
PROBE_SRC="$ROOT/tests/proot/probes/getdents_notdir.c"

FAILS=0
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }
pass() { echo "  ok - $*"; }

[ -x "$PROOT" ] || { echo "SKIP: falta $PROOT (ejecuta ./proot/scripts/build-native.sh -i)"; exit 0; }
command -v clang >/dev/null 2>&1 || { echo "SKIP: falta clang para el probe de getdents"; exit 0; }

FIX=$(mktemp -d "$TMPDIR/proot-getdents.XXXXXX") || exit 1
cleanup() { [ -n "${FIX:-}" ] && rm -rf -- "$FIX"; }
trap cleanup EXIT INT TERM

clang -D_GNU_SOURCE -Wall -o "$FIX/getdents_notdir" "$PROBE_SRC" || {
	echo "FAIL: no se pudo compilar el probe" >&2
	exit 1
}
printf 'no soy un directorio\n' > "$FIX/regular"

# Caso mínimo: con la extensión cargada (-H), un listado que falla debe
# devolverse al tracee como error, sin matar PRoot.
for hidden in "" "--hide"; do
	if [ -n "$hidden" ]; then
		args=("$hidden" "$FIX/hide-me")
		label="con --hide"
		: > "$FIX/hide-me"
	else
		args=()
		label="sin --hide (control)"
	fi

	out=$(env -i TMPDIR="$TMPDIR" PROOT_TMP_DIR="$TMPDIR" \
		"$PROOT" "${args[@]}" "$FIX/getdents_notdir" "$FIX/regular" 2>&1)
	rc=$?

	if printf '%s' "$out" | grep -q 'signal 11'; then
		fail "$label: PRoot recibió SIGSEGV"
	elif printf '%s' "$out" | grep -q 'ENOTDIR-like'; then
		pass "$label: el fallo llega al tracee ($(printf '%s' "$out" | grep 'ENOTDIR-like' | head -1))"
	else
		fail "$label: salida inesperada (rc=$rc): $(printf '%s' "$out" | tail -2)"
	fi
done

# Un listado de un directorio real sigue funcionando con la extensión activa.
mkdir -p "$FIX/dir"
printf 'x\n' > "$FIX/dir/inside"
out=$(env -i TMPDIR="$TMPDIR" PROOT_TMP_DIR="$TMPDIR" \
	"$PROOT" --hide "$FIX/hide-me" "$FIX/getdents_notdir" "$FIX/dir" 2>&1)
if printf '%s' "$out" | grep -q 'OK listado'; then
	pass "listado normal del directorio intacto"
else
	fail "listado normal roto con --hide: $(printf '%s' "$out" | tail -2)"
fi

if [ "$FAILS" -gt 0 ]; then
	echo "=== FAILURES: $FAILS ==="
	exit 1
fi
echo "=== SUMMARY: hidden_files getdents PASS ==="
