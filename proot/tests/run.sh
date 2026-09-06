#!/usr/bin/env bash
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

run_dir() {
    local dir=$1
    local test
    while IFS= read -r test; do
        echo "=== $test ==="
        bash "$test"
    done < <(find "$ROOT/tests/$dir" -type f -name 'test_*.sh' -print | sort)
}

run_control_api() {
    local tool
    for tool in python3 cargo bun; do
        command -v "$tool" >/dev/null || {
            echo "ERROR: control-api tests require $tool" >&2
            return 1
        }
    done
    PYTHONPATH="$ROOT/control-api/python${PYTHONPATH:+:$PYTHONPATH}" \
        python3 -m unittest discover -s "$ROOT/tests/control-api/python" -v
    CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$ROOT/control-api/rust/target}" \
        cargo test --offline --manifest-path "$ROOT/control-api/rust/Cargo.toml"
    bun test "$ROOT/control-api/bun/control_api.test.ts"
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
