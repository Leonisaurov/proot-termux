#!/data/data/com.termux/files/usr/bin/bash
# sync_upstream.sh — inventaría, comprueba y aplica commits de termux/proot sobre
# el árbol vendored proot/src/ (upstream publica su fuente en src/; el fork la
# mantiene en proot/src/).
#
# Este script nunca commitea, nunca pushea, nunca hace fetch y nunca toca
# proot/ci/. Sólo escribe bajo proot/src/ cuando se le invoca con --apply.
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
REPO=$(git -C "$PROOT_DIR" rev-parse --show-toplevel)
SRC_REL="proot/src"
PIN_DOC="$PROOT_DIR/docs/UPSTREAM.md"
TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/proot-sync"

usage() {
	cat >&2 <<'EOF'
uso: sync_upstream.sh <comando> [especificación] [opciones]

comandos:
  --status                    clasifica el rango completo (advisory, ver nota)
  --check    <sha>|<a>..<b>   dry-run por archivo (advisory, ver nota)
  --apply    <sha>|<a>..<b>   aplica en orden cronológico; es transaccional: si un
                              archivo queda con marcadores o .rej se revierte y se
                              marca NEEDS-PORT (los hunks van a $TMPDIR)
  --verify                    gates: sin .rej, sin marcadores, trabajo propio intacto
  --coverage <sha>|<a>..<b>   % de líneas añadidas por upstream que ya están aquí

NOTA: `git apply --check -3` no reproduce todos los conflictos del apply real,
así que CLEAN no es una garantía; --apply es el que decide. Los rangos son con
semántica de git (`A..B` EXCLUYE A): para incluir el primer commit usa `A^..B`.

opciones:
  --from <sha>       base upstream (por omisión: pin SYNC_FROM de docs/UPSTREAM.md)
  --to <ref>         punta upstream (por omisión: upstream/master)
  --baseline <ref>   commit del fork para medir el trabajo propio
                     (por omisión: pin SYNC_FORK_BASE)
EOF
	exit 2
}

CMD=""
SPEC=""
FROM=""
TO=""
BASELINE=""

while [ $# -gt 0 ]; do
	case "$1" in
	--status | --check | --apply | --verify | --coverage)
		CMD="${1#--}"
		if [ $# -gt 1 ] && [[ "$2" != --* ]]; then
			SPEC="$2"
			shift
		fi
		;;
	--from | --to | --baseline)
		[ $# -ge 2 ] || usage
		Opt="${1#--}"
		val="$2"
		case "$Opt" in
		from) FROM="$val" ;;
		to) TO="$val" ;;
		baseline) BASELINE="$val" ;;
		esac
		shift
		;;
	-h | --help) usage ;;
	*)
		echo "error: opción desconocida: $1" >&2
		usage
		;;
	esac
	shift
done

[ -n "$CMD" ] || usage

# ------------------------------------------------------------------ preflight

cd "$REPO"

test -d "$SRC_REL" || {
	echo "error: no existe $REPO/$SRC_REL" >&2
	exit 3
}

pin() { sed -n "s/^$1=\([0-9a-f]\{7,40\}\)\$/\1/p" "$PIN_DOC" 2>/dev/null | head -1; }

FROM="${FROM:-$(pin SYNC_FROM)}"
TO="${TO:-upstream/master}"
BASELINE="${BASELINE:-$(pin SYNC_FORK_BASE)}"

if [ -z "$FROM" ]; then
	echo "error: sin base: pasa --from o define SYNC_FROM en $PIN_DOC" >&2
	exit 3
fi

for ref in "$FROM" "$TO" "$BASELINE"; do
	git cat-file -e "${ref}^{commit}" 2>/dev/null || {
		echo "error: '$ref' no es un commit local; ¿falta 'git fetch upstream --prune'?" >&2
		exit 3
	}
done

git merge-base --is-ancestor "$FROM" "$TO" || {
	echo "error: $FROM no es ancestro de $TO" >&2
	exit 3
}

grafts=$(git replace -l)
if [ -n "$grafts" ]; then
	echo "error: hay refs de reemplazo activas ($(printf '%s' "$grafts" | tr '\n' ' ')); falsearían el 3-way" >&2
	exit 3
fi

FROM_SHA=$(git rev-parse "$FROM")
TO_SHA=$(git rev-parse "$TO")
BASE_SHA=$(git rev-parse "$BASELINE")
RUNDIR="$TMPBASE/$(date -u +%Y%m%dT%H%M%SZ)"
REJDIR="$RUNDIR/rej"
REV_CURRENT=$(sed -n 's/^TERMUX_PKG_REVISION=\([0-9][0-9]*\)$/\1/p' \
	"$PROOT_DIR/ci/termux/packages/proot/build.sh" | head -1)

# ------------------------------------------------------------------- helpers

# src/x.c -> proot/src/x.c ; vacío si el archivo no vive bajo src/
map_path() {
	case "$1" in
	src/*) printf '%s/%s\n' "$SRC_REL" "${1#src/}" ;;
	*) printf '\n' ;;
	esac
}

commit_files() {
	git show --format='' --name-only "$1" -- src | sed '/^$/d'
}

# líneas añadidas por un diff (sin cabeceras +++), una por línea
added_lines() {
	awk '/^@@/ {inf=1; next} inf && /^\+/ && !/^\+\+\+/ {sub(/^\+/, ""); print}'
}

# cuántas de las líneas del archivo $2 no aparecen ya en el archivo $1
missing_count() {
	local haystack=$1 needles=$2 n=0
	[ -s "$needles" ] || {
		echo 0
		return
	}
	n=$(grep -Fvxf "$haystack" "$needles" 2>/dev/null | wc -l | tr -d ' ')
	echo "${n:-0}"
}

# dry-run de un commit restringido a un archivo: 0 si aplica limpio (advisory)
check_file() {
	git show "$1" -- "$2" | git apply -3 --directory=proot -p1 --check - >/dev/null 2>&1
}

# apply transaccional de un commit en un archivo: revierte si deja marcadores y
# informa NEEDS-PORT para portar a mano. Devuelve 0 si quedó limpio.
apply_file() {
	local sha=$1 up=$2 dst=$3
	local short base snap existed=0
	short=$(printf '%.10s' "$sha")
	base=$(basename "$dst")
	snap="$RUNDIR/pre/$short.$base"
	mkdir -p "$RUNDIR/pre" "$REJDIR"
	[ -f "$dst" ] && existed=1 && cp -p "$dst" "$snap"
	git show "$sha" -- "$up" |
		git apply -3 --directory=proot -p1 --reject="$REJDIR/$short.$base.rej" - \
			>"$RUNDIR/apply.out" 2>&1
	if [ -f "$dst" ] && grep -qE '^(<<<<<<< |>>>>>>> )' "$dst"; then
		if [ "$existed" -eq 1 ]; then
			cp -p "$snap" "$dst"
		else
			rm -f "$dst"
		fi
		printf 'NEEDS-PORT\t%s\t%s\trej=%s\n' "$short" "$dst" "$REJDIR/$short.$base.rej"
		return 1
	fi
	if [ "$existed" -eq 0 ] && [ ! -f "$dst" ]; then
		printf 'DELETED\t%s\t%s\n' "$short" "$dst"
	else
		printf 'APPLIED\t%s\t%s\n' "$short" "$dst"
	fi
	return 0
}

commits_in() {
	local spec=${1:-}
	if [ -z "$spec" ]; then
		git rev-list --reverse "${FROM}..${TO}"
	elif [[ "$spec" == *..* ]]; then
		git rev-list --reverse "$spec"
	else
		git rev-list --reverse "${spec}^..${spec}"
	fi
}

header() {
	echo "SYNC_FROM=$FROM_SHA"
	echo "SYNC_TO=$TO_SHA"
	echo "SYNC_FORK_BASE=$BASE_SHA"
	echo "SYNC_RANGE_COMMITS=$(git rev-list --count "${FROM_SHA}..${TO_SHA}")"
	echo "REV_CURRENT=$REV_CURRENT"
}

# ------------------------------------------------------------------ comandos

case "$CMD" in
status)
	header
	clean=0
	conflict=0
	nosrc=0
	while read -r sha; do
		[ -n "$sha" ] || continue
		subject=$(git show -s --format='%s' "$sha")
		files=$(commit_files "$sha")
		if [ -z "$files" ]; then
			printf 'NO-SRC\t%s\t%s\n' "${sha:0:10}" "$subject"
			nosrc=$((nosrc + 1))
			continue
		fi
		c=0
		k=0
		conflicts=""
		while IFS= read -r up; do
			[ -n "$up" ] || continue
			if check_file "$sha" "$up"; then
				c=$((c + 1))
			else
				k=$((k + 1))
				conflicts+="$up"$'\n'
			fi
		done <<<"$files"
		clean=$((clean + c))
		conflict=$((conflict + k))
		printf 'COMMIT\t%s\tclean=%d\tconflict=%d\t%s\n' "${sha:0:10}" "$c" "$k" "$subject"
		while IFS= read -r up; do
			[ -n "$up" ] || continue
			printf 'CONFLICT\t%s\t%s\n' "${sha:0:10}" "$up"
		done <<<"$conflicts"
	done <<<"$(commits_in)"
	echo "FILES_CLEAN=$clean"
	echo "FILES_CONFLICT=$conflict"
	echo "COMMITS_NO_SRC=$nosrc"
	;;

check)
	[ -n "$SPEC" ] || usage
	header
	while read -r sha; do
		[ -n "$sha" ] || continue
		while IFS= read -r up; do
			[ -n "$up" ] || continue
			dst=$(map_path "$up")
			[ -n "$dst" ] || continue
			if check_file "$sha" "$up"; then
				printf 'CLEAN\t%s\t%s\t-> %s\n' "${sha:0:10}" "$up" "$dst"
			else
				printf 'CONFLICT\t%s\t%s\t-> %s\n' "${sha:0:10}" "$up" "$dst"
			fi
		done <<<"$(commit_files "$sha")"
	done <<<"$(commits_in "$SPEC")"
	;;

apply)
	header
	mkdir -p "$REJDIR"
	echo "SYNC_RUNDIR=$RUNDIR"
	applied=0
	needs=0
	while read -r sha; do
		[ -n "$sha" ] || continue
		files=$(commit_files "$sha")
		if [ -z "$files" ]; then
			printf 'SKIP-NO-SRC\t%s\t%s\n' "${sha:0:10}" "$(git show -s --format='%s' "$sha")"
			continue
		fi
		while IFS= read -r up; do
			[ -n "$up" ] || continue
			dst=$(map_path "$up")
			[ -n "$dst" ] || continue
			if apply_file "$sha" "$up" "$dst"; then
				applied=$((applied + 1))
			else
				needs=$((needs + 1))
			fi
		done <<<"$files"
	done <<<"$(commits_in "$SPEC")"
	echo "APPLIED_FILES=$applied"
	echo "NEEDS_PORT_FILES=$needs"
	;;

verify)
	header
	fails=0
	if found=$(find "$SRC_REL" -name '*.rej' -print -quit); [ -n "$found" ]; then
		echo "GATE=fail:hay .rej bajo $SRC_REL"
		find "$SRC_REL" -name '*.rej'
		fails=$((fails + 1))
	fi
	if grep -rlE '^(<<<<<<< |>>>>>>> )' "$SRC_REL" --include='*.c' --include='*.h' 2>/dev/null | grep -q .; then
		echo "GATE=fail:hay marcadores de conflicto"
		grep -rnE '^(<<<<<<< |>>>>>>> )' "$SRC_REL" --include='*.c' --include='*.h'
		fails=$((fails + 1))
	fi

	# Puerta principal: ninguna línea propia (lo que el fork añadió sobre la base
	# upstream) desapareció al re-aplicar el port.
	own_total=0
	own_missing=0
	lost_files=0
	mkdir -p "$RUNDIR"
	while IFS= read -r f; do
		[ -n "$f" ] || continue
		[ -f "$SRC_REL/$f" ] || continue
		git diff "${FROM_SHA}:src/$f" "${BASE_SHA}:proot/src/$f" 2>/dev/null |
			added_lines >"$RUNDIR/own.txt"
		n=$(wc -l <"$RUNDIR/own.txt" | tr -d ' ')
		[ "$n" -gt 0 ] || continue
		m=$(missing_count "$SRC_REL/$f" "$RUNDIR/own.txt")
		own_total=$((own_total + n))
		if [ "$m" -gt 0 ]; then
			own_missing=$((own_missing + m))
			lost_files=$((lost_files + 1))
			printf 'OWN-LOST\t%s\t%s/%d\n' "$f" "$m" "$n"
			git diff "${FROM_SHA}:src/$f" "${BASE_SHA}:proot/src/$f" 2>/dev/null |
				added_lines >"$RUNDIR/own.txt"
			grep -Fvxf "$SRC_REL/$f" "$RUNDIR/own.txt" 2>/dev/null |
				sed "s/^/\t\t| /" | head -10
		fi
	done <<<"$(git diff --name-only "${FROM_SHA}..${TO_SHA}" -- src | sed '/^$/d; s|^src/||')"
	rm -f "$RUNDIR/own.txt"
	echo "OWN_LINES=$own_total"
	echo "OWN_LINES_MISSING=$own_missing"
	echo "OWN_LOST_FILES=$lost_files"
	if [ "$own_missing" -gt 0 ]; then
		echo "GATE=fail:trabajo propio perdido"
		fails=$((fails + 1))
	fi
	if [ "$fails" -eq 0 ]; then
		echo "GATE=ok"
	else
		echo "GATE=fail:$fails"
		exit 1
	fi
	;;

coverage)
	[ -n "$SPEC" ] || usage
	header
	total=0
	present=0
	mkdir -p "$RUNDIR"
	while read -r sha; do
		[ -n "$sha" ] || continue
		while IFS= read -r up; do
			[ -n "$up" ] || continue
			dst=$(map_path "$up")
			[ -n "$dst" ] && [ -f "$dst" ] || continue
			git show "$sha" -- "$up" | added_lines >"$RUNDIR/up.txt"
			n=$(wc -l <"$RUNDIR/up.txt" | tr -d ' ')
			[ "$n" -gt 0 ] || continue
			m=$(missing_count "$dst" "$RUNDIR/up.txt")
			total=$((total + n))
			present=$((present + n - m))
			if [ "$m" -gt 0 ]; then
				printf 'ABSENT\t%s\t%s\t%s/%d\n' "${sha:0:10}" "$up" "$m" "$n"
			fi
		done <<<"$(commit_files "$sha")"
	done <<<"$(commits_in "$SPEC")"
	rm -f "$RUNDIR/up.txt"
	echo "COVERAGE_ADDED=$total"
	echo "COVERAGE_PRESENT=$present"
	if [ "$total" -gt 0 ]; then
		echo "COVERAGE_PCT=$((100 * present / total))"
	else
		echo "COVERAGE_PCT=n/a"
	fi
	;;
esac
