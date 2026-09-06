#!/usr/bin/env bash
# Paired benchmark for persistent interactive termux-isolated sessions.
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
TERMUX_ISOLATED="${TERMUX_ISOLATED:-$REPO_ROOT/bin/termux-isolated}"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"

python3 - "$TERMUX_ISOLATED" "$PREFIX" <<'PY'
import os
import math
import pty
import random
import select
import statistics
import subprocess
import sys
import time

wrapper, prefix = sys.argv[1:]
iterations = int(os.environ.get("TERMUX_ISOLATED_BENCH_ITERATIONS", "100"))
pairs = int(os.environ.get("TERMUX_ISOLATED_BENCH_PAIRS", "10"))
seed = int(os.environ.get("TERMUX_ISOLATED_BENCH_SEED", "20260826"))
if iterations < 1 or pairs < 1:
    raise ValueError("iterations and pairs must be positive")


def read_until(master, marker, timeout=120):
    output = bytearray()
    deadline = time.monotonic() + timeout
    while marker not in output:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AssertionError(output.decode(errors="replace"))
        ready, _, _ = select.select([master], [], [], remaining)
        if not ready:
            raise AssertionError(output.decode(errors="replace"))
        try:
            output.extend(os.read(master, 4096))
        except OSError as error:
            raise AssertionError(output.decode(errors="replace")) from error
    return output


def command_for(argv):
    return argv


def measure_once(argv):
    master, slave = pty.openpty()
    child = subprocess.Popen(command_for(argv), stdin=slave, stdout=slave,
                             stderr=slave, env=os.environ.copy(), close_fds=True)
    os.close(slave)
    try:
        time.sleep(0.5)
        os.write(master, b"printf '\\122\\105\\101\\104\\131\\137\\102\\105\\116\\103\\110\\n'\n")
        read_until(master, b"READY_BENCH")
        command = (
            f"{prefix}/bin/sh -c 'i=0; while [ $i -lt {iterations} ]; do "
            f"{prefix}/bin/true || exit; i=$((i+1)); done'"
            "; result=$?; printf '%s:\\105\\116\\104\\137\\102\\105\\116\\103\\110\\n' \"$result\""
        ).encode() + b"\n"
        start = time.monotonic()
        os.write(master, command)
        output = read_until(master, b":END_BENCH")
        if b"\n0:END_BENCH" not in output:
            raise AssertionError(output.decode(errors="replace"))
        elapsed = time.monotonic() - start
        os.write(master, b"exit\n")
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
        if child.returncode != 0:
            raise RuntimeError(f"benchmark child exited with {child.returncode}")
        return elapsed
    finally:
        os.close(master)
        if child.poll() is None:
            child.kill()
            child.wait()


def isolated_args(proc):
    return ([wrapper, "--termux-paths", "--proc-isolated", "--", "sh", "-i"]
            if proc else
            [wrapper, "--termux-paths", "--no-proc-isolated", "--", "sh", "-i"])


def direct_args():
    return [f"{prefix}/bin/sh", "-i"]


def summarize(label, samples):
    median = statistics.median(samples)
    print(f"{label}: median={median:.3f}s min={min(samples):.3f}s "
          f"max={max(samples):.3f}s samples={len(samples)}", flush=True)
    return median


# Direct shell is a context baseline. It is measured with the same repetitions
# but is not part of the paired proc/no-proc significance comparison.
direct_samples = [measure_once(direct_args()) for _ in range(pairs)]
direct = summarize("direct persistent Termux shell", direct_samples)

rng = random.Random(seed)
paired = []
for pair in range(pairs + 1):
    order = [True, False]
    rng.shuffle(order)
    results = {}
    for proc in order:
        results[proc] = measure_once(isolated_args(proc))
    if pair != 0:
        paired.append((results[True], results[False]))

proc_samples = [proc for proc, _ in paired]
no_proc_samples = [no_proc for _, no_proc in paired]
deltas = [no_proc - proc for proc, no_proc in paired]
proc = summarize("persistent termux-isolated --proc-isolated", proc_samples)
no_proc = summarize("persistent termux-isolated --no-proc-isolated", no_proc_samples)
print(f"paired delta (no-proc minus proc): median={statistics.median(deltas):.3f}s "
      f"mean={statistics.mean(deltas):.3f}s wins={sum(d > 0 for d in deltas)}/{len(deltas)}",
      flush=True)
wins = sum(d > 0 for d in deltas)
n = len(deltas)
sign_p = sum(math.comb(n, k) for k in range(wins, n + 1)) / (2 ** n)
print(f"one-sided paired sign-test p={sign_p:.6f} (H1: no-proc is slower)", flush=True)
print(f"iterations={iterations} pairs={pairs} warmup_pairs=1 seed={seed}", flush=True)
print(f"persistent proc/direct ratio: {proc / direct:.3f}x", flush=True)
print(f"persistent no-proc/direct ratio: {no_proc / direct:.3f}x", flush=True)
print("=== SUMMARY: paired termux-isolated benchmark completed ===", flush=True)
PY
