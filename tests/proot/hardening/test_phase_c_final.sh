#!/bin/bash
# Stable entry point for the complete Phase C regression suite.
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$SCRIPT_DIR/test_phase_c.sh" "$@"
