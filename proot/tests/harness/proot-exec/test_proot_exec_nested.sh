#!/usr/bin/env bash
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"
fixture="$TMPDIR/nested-proc-fd-fixture"
trap 'rm -f "$fixture"' EXIT

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
PROOT_EXEC="${PROOT_EXEC:-$ROOT/bin/proot-exec}"
CONFIG="$ROOT/../proot-exec.conf"

if [[ ! -x "$PROOT_EXEC" ]]; then
	printf 'SKIP: proot-exec not found at %s\n' "$PROOT_EXEC"
	exit 0
fi

run_nested() {
    local output
    output=$("$PROOT_EXEC" --config "$CONFIG" -- "$@" 2>&1)
    if grep -Eq "can't sanitize binding|can't chdir|not found" <<< "$output"; then
        printf '%s\n' "$output" >&2
        return 1
    fi
    printf '%s\n' "$output"
}

output=$(run_nested "$PREFIX/bin/proot" --bind=. --cwd=. \
    "$PREFIX/bin/sh" -c '
        set -eu
        test -d .
        exec 9>"$TMPDIR/nested-proc-fd-fixture"
        fd_target=$(readlink /proc/self/fd/9)
        test -n "$fd_target"
        test "$fd_target" != /dev/null
        case "$(readlink /proc/self/cwd)" in /*) ;; *) exit 10 ;; esac
        case "$(readlink /proc/self/exe)" in /*) ;; *) exit 11 ;; esac
        grep -q "^TracerPid:[[:space:]]*0" /proc/self/status
        (exit 7) & child=$!
        wait "$child" || test "$?" -eq 7
        exec "$PREFIX/bin/sh" -c "printf NESTED_PROOT_OK"
')
[[ "$output" == NESTED_PROOT_OK ]]
printf 'PASS: nested PRoot resolves relative bindings and guest proc links\n'

output=$(run_nested "$PREFIX/bin/proot" --bind="$ROOT" --cwd="$ROOT" \
    "$PREFIX/bin/sh" -c 'test -f src/path/binding.c && printf ABSOLUTE_BIND_OK')
[[ "$output" == ABSOLUTE_BIND_OK ]]
printf 'PASS: nested PRoot resolves absolute workspace bindings\n'

if command -v pd >/dev/null 2>&1 && pd list 2>/dev/null | grep -Fq 'alpine'; then
	output=$("$PROOT_EXEC" --config "$CONFIG" -- \
		pd login alpine -- /bin/true 2>&1)
	if grep -q "can't sanitize binding" <<< "$output"; then
		echo 'FAIL: proot-distro emitted binding sanitation warnings' >&2
		exit 1
	fi
	printf 'PASS: proot-distro login works inside the Codex profile\n'
else
	printf 'SKIP: alpine container is not installed\n'
fi

printf '=== SUMMARY: nested proot PASS ===\n'
