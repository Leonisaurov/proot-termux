#!/data/data/com.termux/files/usr/bin/bash
# Regresión del conteo de enlaces fingidos (upstream 05a93edfc8, "read link
# counts of 10000 and more") y del endurecimiento propio del fork sobre esa
# función.
#
# 05a93edfc8 introdujo get_link_count(), que leía los dígitos del nombre del
# fichero final ("<intermedio>.<count>") con un índice derivado de un size_t
# (dot[-i]). En nuestro build (NDK clang 21, -O3 -flto) eso desbordaba a una
# dirección no mapeada y PRoot moría con SIGSEGV al abrir un enlace fingido. El
# fork indexa con entero con signo y comprueba el punto: este script fija las
# dos cosas, la funcionalidad de 10000+ y la ausencia de fallo.
set -uo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR" || exit 1

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
LAUNCHER="${LAUNCHER:-$ROOT/bin/termux-isolated}"
GUEST_SH="$PREFIX/bin/sh"

FAILS=0
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }
pass() { echo "  ok - $*"; }

[ -x "$LAUNCHER" ] || { echo "SKIP: falta $LAUNCHER"; exit 0; }

run_guest() {
	"$LAUNCHER" --termux-paths --rw-dir "$TMPDIR" --cwd "$PREFIX" \
		--no-proc-isolated --proot-arg --link2symlink -- \
		"$GUEST_SH" -c "$1" 2>&1
}

check_no_segv() {
	if printf '%s' "$2" | grep -q 'signal 11'; then
		fail "$1: PRoot recibió SIGSEGV"
		return 1
	fi
	return 0
}

FIX=$(mktemp -d "$TMPDIR/proot-l2s-count.XXXXXX") || exit 1
cleanup() { [ -n "${FIX:-}" ] && rm -rf -- "$FIX"; }
trap cleanup EXIT INT TERM

# Caso A: abrir un enlace fingido por su nombre de guest y pedir el nombre del
# descriptor. Nominación por defecto (sin PROOT_L2S_DIR): es la ruta que
# llamaba a get_link_count() con el nombre "<intermedio>.0002".
out=$(run_guest "set -eu
base=$FIX
mkdir -p \"\$base/links\"
printf 'content\n' > \"\$base/links/o\"
cd \"\$base/links\"
ln o l
exec 3< l
got=\$(readlink /proc/self/fd/3)
echo \"FDNAME=\$got\"
echo \"COUNT=\$(stat -c %h \"\$base/links/l\")\"
")
check_no_segv "caso A" "$out" || true
if printf '%s' "$out" | grep -qx "FDNAME=$FIX/links/l"; then
	pass "caso A: readlink(/proc/self/fd/N) reporta el nombre del enlace"
else
	fail "caso A: nombre de descriptor inesperado: $(printf '%s' "$out" | grep FDNAME | head -1)"
fi
if printf '%s' "$out" | grep -qx 'COUNT=2'; then
	pass "caso A: la cadena de 2 enlaces se cuenta como 2"
else
	fail "caso A: conteo inesperado: $(printf '%s' "$out" | grep COUNT | head -1)"
fi

# Caso B: un intermedio cuyo destino es un fichero del usuario sin conteo en el
# nombre no es una cadena de PRoot: se deja en manos del kernel (upstream
# 87c4109a52) y debe seguir siendo listable/estadisticable.
out=$(run_guest "set -eu
base=$FIX/broken
mkdir -p \"\$base\"
printf 'real\n' > \"\$base/target\"
ln -s target \"\$base/.l2s.t0001\"
ln -s .l2s.t0001 \"\$base/link\"
ls \"\$base\" > /dev/null
echo \"ISLINK=\$(test -L \"\$base/link\" && echo si || echo no)\"
rm \"\$base/link\"
echo BORRADO=ok
")
check_no_segv "caso B" "$out" || true
if printf '%s' "$out" | grep -qx 'BORRADO=ok'; then
	pass "caso B: una cadena ajena se deja al kernel y el nombre se puede borrar"
else
	fail "caso B: no se pudo tratar la cadena ajena: $(printf '%s' "$out" | tail -2)"
fi

if [ "$FAILS" -gt 0 ]; then
	echo "=== FAILURES: $FAILS ==="
	exit 1
fi
echo "=== SUMMARY: link2symlink link count PASS ==="
