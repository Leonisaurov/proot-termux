#!/usr/bin/env bash
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
PROOT="${PROOT:-$PREFIX/bin/proot}"
CC="${CC:-$PREFIX/bin/clang}"
export PREFIX ROOT
fixture="$TMPDIR/proot-pthread-fixture-$$"
trap 'rm -f "$fixture"' EXIT

if [[ ! -x "$PROOT" || ! -x "$CC" ]]; then
	printf 'SKIP: proot or clang is unavailable\n'
	exit 0
fi

"$CC" -O2 -pthread "$SCRIPT_DIR/pthread_proc_fixture.c" -o "$fixture"

run_nested() {
	local depth=$1
	local command="$fixture"
	local i
	for ((i=1; i<depth; i++)); do
		command="exec $PROOT --bind=$ROOT --cwd=$ROOT $PREFIX/bin/sh -c '$command'"
	done
	"$PROOT" --proc-isolated --ptrace-isolated --bind="$ROOT" --bind="$TMPDIR" --cwd="$ROOT" \
		"$PREFIX/bin/sh" -c "test -n \"\$(readlink /proc/self/cwd)\"; test -n \"\$(readlink /proc/self/exe)\"; $command"
}

for depth in 1 2 3; do
	output=$(run_nested "$depth")
	[[ "$output" == PTHREAD_PROC_OK ]]
	printf 'PASS: pthread/proc/fork/exec nesting depth %d\n' "$depth"
done

output=$("$PROOT" --proc-isolated --ptrace-isolated --bind="$ROOT" --bind="$TMPDIR" \
	--cwd="$ROOT" "$PREFIX/bin/sh" -c '
	set -eu
	inner="$PREFIX/bin/proot"
	"$inner" --bind="$ROOT" --bind="$TMPDIR" --cwd="$ROOT" --supervise \
		"$PREFIX/bin/sh" -c "sleep 5" &
	spid=$!
	sleep 1
	"$inner" --bind="$ROOT" --bind="$TMPDIR" --cwd="$ROOT" --exec "$spid" \
		"$PREFIX/bin/sh" -c "test -f src/path/proc.c; printf SUPERVISED_EXEC_OK"
	wait "$spid" || true
')
[[ "$output" == SUPERVISED_EXEC_OK ]]
printf 'PASS: nested supervisor accepts an --exec client\n'

printf '=== SUMMARY: nesting pthread coverage PASS ===\n'
