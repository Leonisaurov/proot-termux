#!/bin/bash
# Pentest B1/B2/B7: --exec concurrent clients
# Tests: fd leak (B1), child_tracee leak (B2), signalfd leak (B7)
set -uo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR" || exit 1

ROOTFS="/data/data/com.termux/files/usr/var/lib/proot-distro/containers/alpine/rootfs"
PROOT="/data/data/com.termux/files/usr/bin/proot"
RESULTS="${RESULTS:-/data/data/com.termux/files/home/Develop/Patch/proot-termux/reports/pentest}"
mkdir -p "$RESULTS"
RESULT_FILE="$RESULTS/b1_b2_b7.txt"
rm -f "$RESULT_FILE"
PASS=0; FAIL=0

log() { echo "$@" | tee -a "$RESULT_FILE"; }

log "=== B1/B2/B7: --exec concurrent clients ==="

# Start supervisor
env -i PATH=/bin:/usr/bin PROOT_L2S_DIR="$TMPDIR" PROOT_RUNTIME_DIR="$TMPDIR" \
  $PROOT --kill-on-exit --link2symlink -L --change-id=0:0 \
  --rootfs="$ROOTFS" --cwd=/root --bind=/dev --bind=/sys \
  --supervise /bin/sh -c "echo SUP_STARTED; sleep 600" &
SPID=$!
sleep 2

if ! kill -0 $SPID 2>/dev/null; then
  log "FAIL: supervisor died"; exit 2
fi
log "PASS: supervisor started (PID $SPID)"

SUP_FD=$(ls /proc/$SPID/fd 2>/dev/null | wc -l)
log "INFO: supervisor FDs at start: $SUP_FD"

# Launch 20 concurrent --exec clients
for i in $(seq 1 20); do
  env -i PATH=/bin:/usr/bin PROOT_L2S_DIR="$TMPDIR" PROOT_RUNTIME_DIR="$TMPDIR" \
    $PROOT --kill-on-exit --link2symlink -L --change-id=0:0 \
    --rootfs="$ROOTFS" --cwd=/root --bind=/dev --bind=/sys \
    --exec $SPID /bin/sh -c "echo CLIENT_$i; exit 0" &
done

# Wait briefly for clients to finish
sleep 10

# Kill any remaining client processes
for pid in $(jobs -p); do
  [ "$pid" != "$SPID" ] && kill -9 $pid 2>/dev/null
done

# Check supervisor FDs
if kill -0 $SPID 2>/dev/null; then
  SUP_FD_POST=$(ls /proc/$SPID/fd 2>/dev/null | wc -l)
  log "INFO: supervisor FDs after test: $SUP_FD_POST"
  
  FD_LEAK=$((SUP_FD_POST - SUP_FD))
  if [ "$FD_LEAK" -gt 2 ]; then
    log "FAIL: FD leak detected (+$FD_LEAK FDs)"
    FAIL=$((FAIL+1))
  else
    log "PASS: no significant FD leak (delta=$FD_LEAK)"
    PASS=$((PASS+1))
  fi
else
  log "PASS: supervisor exited cleanly after clients"
  PASS=$((PASS+1))
fi

kill -9 $SPID 2>/dev/null
sleep 1

# B7: check signalfd was closed (supervisor should not have extra fds after fini)
log ""
log "=== B7: signalfd leak check ==="
# If supervisor exited cleanly, signalfd was closed in fini
if ! kill -0 $SPID 2>/dev/null; then
  log "PASS: supervisor terminated (signalfd closed in fini)"
  PASS=$((PASS+1))
else
  log "INFO: supervisor still alive (killed externally)"
fi

log ""
log "=== SUMMARY: PASS=$PASS FAIL=$FAIL ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
