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

case "${1:-all}" in
    proot) run_dir proot ;;
    termux-isolated) run_dir termux-isolated ;;
    harness) run_dir harness ;;
    control-api)
        PYTHONPATH="$ROOT/control-api/python${PYTHONPATH:+:$PYTHONPATH}" \
            python3 -m unittest discover -s "$ROOT/tests/control-api/python" -v
        ;;
    all)
        run_dir proot
        run_dir termux-isolated
        run_dir harness
        PYTHONPATH="$ROOT/control-api/python${PYTHONPATH:+:$PYTHONPATH}" \
            python3 -m unittest discover -s "$ROOT/tests/control-api/python" -v
        ;;
    *)
        echo "usage: $0 {all|proot|termux-isolated|harness|control-api}" >&2
        exit 2
        ;;
esac
