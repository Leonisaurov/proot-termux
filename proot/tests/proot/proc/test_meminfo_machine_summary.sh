#!/usr/bin/env bash
# Regression: /proc/meminfo is a machine summary and must describe the device
# (real host totals) instead of hardcoded zeros, while /proc/uptime, loadavg and
# the global stat counters stay synthetic and per-PID procfs stays isolated.
#
# Android denies /proc/stat, /proc/uptime and /proc/loadavg to the tracee
# (EACCES even outside proot), so those keep their synthesized view: reading the
# real file would turn a synthetic success into a failing open(2).
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
ISOLATED="${TERMUX_ISOLATED:-$ROOT/bin/termux-isolated}"
FIXTURE="$TMPDIR/meminfo-summary-$$.py"

if [ ! -x "$ISOLATED" ]; then
    echo "SKIP: termux-isolated not found at $ISOLATED"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 is required"
    exit 0
fi

cleanup() { rm -f "$FIXTURE"; }
trap cleanup EXIT

host_memtotal=$(awk '/^MemTotal:/{print $2}' /proc/meminfo)
host_swaptotal=$(awk '/^SwapTotal:/{print $2}' /proc/meminfo)
if [ -z "$host_memtotal" ] || [ -z "$host_swaptotal" ]; then
    echo "SKIP: host /proc/meminfo is not readable from the test"
    exit 0
fi

cat > "$FIXTURE" <<'PY'
import os
import re
import subprocess
import sys
import time

failures = []


def check(ok, label, detail=""):
    print(("ok   " if ok else "FAIL ") + label + (": " + detail if detail else ""))
    if not ok:
        failures.append(label)


def read(path):
    with open(path, "r", errors="replace") as handle:
        return handle.read()


# 1. Machine summary: real totals, not hardcoded zeros.
meminfo = read("/proc/meminfo")
fields = dict(re.findall(r"^(\w+):\s+(\d+) kB$", meminfo, re.M))
for name in ("MemTotal", "MemFree", "MemAvailable"):
    check(int(fields.get(name, "0")) > 0, "%s > 0" % name,
          "%s kB" % fields.get(name, "0"))
# Constant totals must match the host exactly; volatile ones are only required
# to be non-zero (the guest reads a few seconds after the host sample).
check(int(fields.get("MemTotal", "0")) == int(sys.argv[1]),
      "MemTotal equals the host value",
      "%s vs host %s" % (fields.get("MemTotal", "0"), sys.argv[1]))
check(int(fields.get("SwapTotal", "0")) == int(sys.argv[2]),
      "SwapTotal equals the host value",
      "%s vs host %s" % (fields.get("SwapTotal", "0"), sys.argv[2]))
check(re.search(r"^Active\(anon\):\s+\d+ kB$", meminfo, re.M) is not None,
      "meminfo carries the full host field set")

# 2. uptime/loadavg/stat: documented synthetic contract (Android denies the
#    real files to the tracee, so the guest gets a stable guest-safe view).
uptime = read("/proc/uptime").split()
check(len(uptime) >= 2 and float(uptime[0]) == 0.0 and float(uptime[1]) == 0.0,
      "uptime stays synthetic", " ".join(uptime))
loadavg = read("/proc/loadavg").strip()
check(re.match(r"^0\.00 0\.00 0\.00 \d+/\d+ \d+$", loadavg) is not None,
      "loadavg stays synthetic", loadavg)
stat_first = read("/proc/stat").splitlines()[0].split()
check(stat_first and stat_first[0] == "cpu"
      and set(stat_first[1:]) == {"0"}, "stat counters stay synthetic",
      " ".join(stat_first))

# 3. Isolation intact: guest tracees are reachable, host pids are not.
try:
    os.stat("/proc/1/status")
    check(False, "host pid 1 unreachable")
except (FileNotFoundError, ProcessLookupError):
    check(True, "host pid 1 unreachable")

child = subprocess.Popen(["sleep", "3"])
time.sleep(0.4)
try:
    entries = sorted(int(name) for name in os.listdir("/proc") if name.isdigit())
    check(child.pid in entries, "guest tracee pid is listed", str(entries))
    check(1 not in entries, "host pid is not listed", str(entries))
    try:
        os.stat("/proc/%d/status" % child.pid)
        check(True, "guest tracee status is reachable")
    except OSError as exc:
        check(False, "guest tracee status is reachable", "errno=%s" % exc.errno)

    status = read("/proc/self/status")
    check(re.search(r"^Pid:\t%d$" % os.getpid(), status, re.M) is not None,
          "/proc/self/status reports the guest pid")
    check("TracerPid:\t0" in status, "TracerPid stays hidden")
finally:
    child.terminate()
    child.wait()

if failures:
    print("MEMINFO_SUMMARY_FAILED:%s" % ",".join(failures))
    sys.exit(1)
print("MEMINFO_SUMMARY_OK")
PY

output=$("$ISOLATED" --termux-paths --proc-isolated -- python3 "$FIXTURE" "$host_memtotal" "$host_swaptotal" 2>&1) || true
printf '%s\n' "$output"

if printf '%s\n' "$output" | grep -q '^MEMINFO_SUMMARY_FAILED:'; then
    echo "FAIL: /proc/meminfo machine summary regressed" >&2
    exit 1
fi
printf '%s\n' "$output" | grep -q '^MEMINFO_SUMMARY_OK$' || {
    echo "FAIL: meminfo summary check did not complete" >&2
    exit 1
}

echo "PASS: /proc/meminfo reports real machine totals (host $host_memtotal kB)"
