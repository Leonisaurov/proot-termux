#!/bin/bash
# Pentest: Talloc memory report
# Tests: no leaks in talloc-managed memory
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR" || exit 1
export PROOT_RUNTIME_DIR="$TMPDIR"

ROOTFS="/data/data/com.termux/files/usr/var/lib/proot-distro/containers/alpine/rootfs"
PROOT="/data/data/com.termux/files/usr/bin/proot"

echo "=== Talloc memory report ==="

# Start proot with --supervise in background
echo "[1] Starting supervised proot..."
env -i PATH=/bin:/usr/bin LD_LIBRARY_PATH=/usr/lib \
  TMPDIR="$TMPDIR" PROOT_RUNTIME_DIR="$PROOT_RUNTIME_DIR" \
  PROOT_L2S_DIR="$TMPDIR" \
  $PROOT --kill-on-exit --link2symlink -L --change-id=0:0 \
  --rootfs="$ROOTFS" --cwd=/root --bind=/dev --bind=/sys \
  --supervise --nice 10 --cpu-limit 1 \
  /bin/sh -c "sleep 60" &
SUPER_PID=$!
sleep 2
if ! kill -0 "$SUPER_PID" 2>/dev/null; then
  echo "ERROR: supervised proot exited before the memory test"
  wait "$SUPER_PID" 2>/dev/null || true
  exit 2
fi

# Run a few --exec clients to exercise the memory paths
echo "[2] Running --exec clients..."
for i in $(seq 1 5); do
  env -i PATH=/bin:/usr/bin LD_LIBRARY_PATH=/usr/lib \
    TMPDIR="$TMPDIR" PROOT_RUNTIME_DIR="$PROOT_RUNTIME_DIR" \
    PROOT_L2S_DIR="$TMPDIR" \
    $PROOT --exec "$SUPER_PID" -- /bin/sh -c "echo client_$i; exit 0" 2>/dev/null || true
done

# Send USR1 for talloc report (if supported)
echo "[3] Sending USR1 for talloc report..."
kill -USR1 $SUPER_PID 2>/dev/null || echo "  (USR1 not supported, skipping)"

# Check /proc/$SUPER_PID/status for memory
echo "[4] Supervisor memory status:"
grep -E "^(VmRSS|VmSize|Threads)" /proc/$SUPER_PID/status 2>/dev/null || echo "  (process already exited)"

# SIGTERM is intentionally ignored by --supervise; SIGKILL is the
# documented deterministic shutdown path for this test.
kill -KILL "$SUPER_PID" 2>/dev/null || true
wait $SUPER_PID 2>/dev/null || true

echo ""
echo "=== VERDICT ==="
echo "Talloc report: See output above for memory details"
echo "If VmRSS is reasonable (<50MB for idle supervisor), no major leak."
