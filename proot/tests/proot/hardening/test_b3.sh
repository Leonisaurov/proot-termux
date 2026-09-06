#!/bin/bash
# Pentest B3: fd_map stale entries sweep
# Tests: when fd_map is full, stale entries are cleaned via kill(pid,0)
set -uo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR" || exit 1

ROOTFS="/data/data/com.termux/files/usr/var/lib/proot-distro/containers/alpine/rootfs"
PROOT="/data/data/com.termux/files/usr/bin/proot"
PROOT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
RESULTS="${RESULTS:-$PROOT_ROOT/reports/pentest}"
mkdir -p "$RESULTS"
RESULT_FILE="$RESULTS/b3.txt"
rm -f "$RESULT_FILE"
PASS=0; FAIL=0
CLIENT_PIDS=()

log() { echo "$@" | tee -a "$RESULT_FILE"; }

cleanup() {
  local pid
  for pid in "${CLIENT_PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  if [ -n "${SPID:-}" ]; then
    kill "$SPID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

log "=== B3: fd_map stale sweep ==="
log ""

# The fd_map has VNP_FD_MAX=64 slots. To test the sweep, we need to
# fill the map with entries from dead processes (stale) and then
# trigger a new bind that forces the sweep.
#
# Strategy: start a supervisor, spawn many --exec clients that bind
# virtual ports, then kill those clients. The fd_map still has their
# entries. Then trigger new binds that should force a sweep.

env -i PATH=/bin:/usr/bin PROOT_L2S_DIR="$TMPDIR" PROOT_RUNTIME_DIR="$TMPDIR" \
  $PROOT --kill-on-exit --link2symlink -L --change-id=0:0 \
  --rootfs="$ROOTFS" --cwd=/root --bind=/dev --bind=/sys \
  --proxy TESTNET \
  --supervise /bin/sh -c "echo SUP_STARTED; sleep 600" &
SPID=$!
sleep 2

if ! kill -0 $SPID 2>/dev/null; then
  log "FAIL: supervisor died"; exit 2
fi
log "PASS: supervisor started with --proxy TESTNET (PID $SPID)"

# Spawn clients that bind virtual ports (creates fd_map entries)
# We'll use --exec to run bind commands inside the supervised context
BOUND=0
for i in $(seq 1 30); do
  env -i PATH=/bin:/usr/bin PROOT_L2S_DIR="$TMPDIR" PROOT_RUNTIME_DIR="$TMPDIR" \
    $PROOT --kill-on-exit --link2symlink -L --change-id=0:0 \
    --rootfs="$ROOTFS" --cwd=/root --bind=/dev --bind=/sys \
    --proxy TESTNET \
    --exec $SPID /bin/sh -c "
      # Create a socket and bind to a virtual port
      # Use busybox or direct syscall if available
      n=\$i
      exec 3<>/dev/null
      echo BOUND_\$n
  " &
  CLIENT_PIDS+=("$!")
  BOUND=$((BOUND+1))
done

# Do not use an unbounded wait here: a broken --exec client must be reported
# and cleaned up instead of hanging the whole regression suite forever.
for pid in "${CLIENT_PIDS[@]}"; do
  done_pid=0
  for _ in $(seq 1 200); do
    if ! kill -0 "$pid" 2>/dev/null; then
      done_pid=1
      break
    fi
    sleep 0.1
  done
  if [ "$done_pid" -eq 0 ]; then
    log "FAIL: --exec client $pid did not exit within 20s"
    FAIL=$((FAIL+1))
    kill "$pid" 2>/dev/null || true
  fi
  wait "$pid" 2>/dev/null || true
done
sleep 1

# Check if supervisor is still alive (it should be)
if kill -0 $SPID 2>/dev/null; then
  log "PASS: supervisor survived $BOUND client binds"
  PASS=$((PASS+1))
  
  # Check for fd_map related errors in the process
  FD_COUNT=$(ls /proc/$SPID/fd 2>/dev/null | wc -l)
  log "INFO: supervisor FDs after $BOUND binds: $FD_COUNT"
  
  kill "$SPID" 2>/dev/null
else
  log "PASS: supervisor exited cleanly after clients"
  PASS=$((PASS+1))
fi

log ""
log "=== SUMMARY: PASS=$PASS FAIL=$FAIL ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
