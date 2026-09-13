#!/usr/bin/env bash
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# Keep running the whole battery: report every failure instead of aborting on
# the first one (set -e would otherwise hide the remaining tests).
FAILED_TESTS=()

run_step() {
    local label=$1
    shift
    local rc=0
    echo "=== ${label} ==="
    "$@" || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "!!! FAILED (exit ${rc}): ${label}" >&2
        FAILED_TESTS+=("$label")
    fi
}

run_dir() {
    local dir=$1
    local test
    while IFS= read -r test; do
        run_step "$test" bash "$test"
    done < <(find "$ROOT/tests/$dir" -type f -name 'test_*.sh' -print | sort)
}

run_control_api() {
    local tool
    for tool in python3 cargo bun; do
        command -v "$tool" >/dev/null || {
            echo "ERROR: control-api tests require $tool" >&2
            FAILED_TESTS+=("control-api (missing $tool)")
            return 0
        }
    done
    run_step "control-api/python" env \
        PYTHONPATH="$ROOT/control-api/python${PYTHONPATH:+:$PYTHONPATH}" \
        python3 -m unittest discover -s "$ROOT/tests/control-api/python" -v
    run_step "control-api/rust" env \
        CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$ROOT/control-api/rust/target}" \
        cargo test --offline --manifest-path "$ROOT/control-api/rust/Cargo.toml"
    run_step "control-api/bun" bun test "$ROOT/control-api/bun/control_api.test.ts"
}

case "${1:-all}" in
    proot) run_dir proot ;;
    termux-isolated) run_dir termux-isolated ;;
    harness) run_dir harness ;;
    control-api) run_control_api ;;
    all)
        run_dir proot
        run_dir termux-isolated
        run_dir harness
        run_control_api
        ;;
    *)
        echo "usage: $0 {all|proot|termux-isolated|harness|control-api}" >&2
        exit 2
        ;;
esac

if [ "${#FAILED_TESTS[@]}" -ne 0 ]; then
    echo "" >&2
    echo "=== FAILURES (${#FAILED_TESTS[@]}) ===" >&2
    printf '  - %s\n' "${FAILED_TESTS[@]}" >&2
    exit 1
fi
