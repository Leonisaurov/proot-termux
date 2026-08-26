#!/usr/bin/env bash
# Verify conventional Termux shebangs remain usable without a global preload.
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
TERMUX_ISOLATED="${TERMUX_ISOLATED:-$REPO_ROOT/bin/termux-isolated}"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
FIXTURE="$PREFIX/tmp/termux-isolated-shebang-$$"

if [[ ! -x "$TERMUX_ISOLATED" ]]; then
	echo "SKIP: termux-isolated wrapper not found at $TERMUX_ISOLATED"
	exit 0
fi

cleanup() { rm -f "$FIXTURE"; }
trap cleanup EXIT
printf '#!/usr/bin/env bash\nprintf SHEBANG_OK' > "$FIXTURE"
chmod 700 "$FIXTURE"

run_case() {
	local mode=$1
	shift
	local guest_fixture="$FIXTURE"
	if [[ "$mode" = rootfs ]]; then
		guest_fixture="/tmp/$(basename "$FIXTURE")"
	fi
	local output
	output=$("$TERMUX_ISOLATED" "$@" -- fish -c "exec '$guest_fixture'")
	test "$output" = SHEBANG_OK
	echo "PASS: /usr/bin/env bash shebang ($mode)"
}

run_case termux-paths --termux-paths --cwd "$PREFIX"
run_case rootfs --cwd /
echo "=== SUMMARY: shebang PASS=2 FAIL=0 ==="
