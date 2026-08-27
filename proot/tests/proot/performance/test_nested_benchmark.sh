#!/usr/bin/env bash
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
PROOT="${PROOT:-$PREFIX/bin/proot}"
HELPER="$ROOT/tests/proot/performance/nested_helper.sh"
export PREFIX ROOT PROOT
if [[ ! -x "$PROOT" ]]; then
	printf 'SKIP: proot is unavailable\n'
	exit 0
fi

now_ns() { date +%s%N; }
run_case() {
	local depth=$1 label=$2 start end
	local -a command
	case "$label" in
		true) command=(true) ;;
		sh) command=(sh -c true) ;;
		ls) command=(ls -d .) ;;
		readlink) command=(readlink /proc/self/cwd) ;;
		*) return 2 ;;
	esac
	start=$(now_ns)
	"$PROOT" --bind="$ROOT" --bind="$TMPDIR" --cwd="$ROOT" \
		"$PREFIX/bin/sh" "$HELPER" "$depth" "${command[@]}" >/dev/null
	end=$(now_ns)
	awk -v n="$((end - start))" 'BEGIN { printf "%.6f", n / 1000000000 }'
}

run_supervised_case() {
	local start end spid
	"$PROOT" --bind="$ROOT" --bind="$TMPDIR" --cwd="$ROOT" --supervise \
		"$PREFIX/bin/sh" -c 'sleep 30' >/dev/null 2>&1 &
	spid=$!
	trap 'kill -KILL "$spid" 2>/dev/null || true; wait "$spid" 2>/dev/null || true' RETURN
	sleep 1
	if ! kill -0 "$spid" 2>/dev/null; then
		return 1
	fi
	start=$(now_ns)
	"$PROOT" --bind="$ROOT" --bind="$TMPDIR" --cwd="$ROOT" \
		--exec "$spid" "$PREFIX/bin/sh" -c true >/dev/null
	end=$(now_ns)
	kill -KILL "$spid" 2>/dev/null || true
	wait "$spid" 2>/dev/null || true
	trap - RETURN
	awk -v n="$((end - start))" 'BEGIN { printf "%.6f", n / 1000000000 }'
}

printf 'depth command cold_s repeated_s\n'
if [[ "${NESTED_BENCH_FULL:-0}" == 1 ]]; then
	commands=(true sh ls readlink)
else
	# Keep the normal regression suite bounded.  The complete matrix remains
	# available for explicit performance runs with NESTED_BENCH_FULL=1.
	commands=(true)
fi
for depth in 1 2 3; do
	for command in "${commands[@]}"; do
		cold=$(run_case "$depth" "$command")
		repeated=$(run_case "$depth" "$command")
		printf '%d %s %s %s\n' "$depth" "$command" "$cold" "$repeated"
	done
done
if [[ "${NESTED_BENCH_FULL:-0}" == 1 ]]; then
	printf 'supervisor client_exec_s %s\n' "$(run_supervised_case)"
fi
printf '=== SUMMARY: nested benchmark completed ===\n'
