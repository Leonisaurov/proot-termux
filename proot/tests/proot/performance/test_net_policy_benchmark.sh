#!/usr/bin/env bash
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
WRAPPER="${TERMUX_ISOLATED:-$ROOT/bin/termux-isolated}"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"

if [[ ! -x "$WRAPPER" ]]; then
	printf 'SKIP: termux-isolated unavailable at %s\n' "$WRAPPER"
	exit 0
fi
if [[ ! -x "$PREFIX/bin/python3" ]]; then
	printf 'SKIP: Python unavailable at %s/bin/python3\n' "$PREFIX"
	exit 0
fi

exec python3 - "$WRAPPER" "$PREFIX" <<'PY'
import os
import socket
import statistics
import subprocess
import sys
import time

wrapper, prefix = sys.argv[1:]
iterations = int(os.environ.get("NET_POLICY_BENCH_ITERATIONS", "1000"))
repeats = int(os.environ.get("NET_POLICY_BENCH_REPEATS", "5"))
warmup = int(os.environ.get("NET_POLICY_BENCH_WARMUP", "100"))
guest_python = prefix + "/bin/python3"


def start_receiver():
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind(("127.0.0.1", 0))
    receiver.settimeout(0.1)
    return receiver, receiver.getsockname()[1]


def proot_command(policy_args, port):
    command = [wrapper, "--termux-paths", "--cwd", prefix]
    for arg in policy_args:
        command.extend(("--proot-arg", arg))
    payload = (
        "import socket\n"
        "s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)\n"
        f"a=('127.0.0.1',{port})\n"
        f"[s.sendto(b'x',a) for _ in range({iterations})]\n"
        "s.close()\n"
    )
    return command + ["--", guest_python, "-c", payload]


def run_case(label, policy_mode, port, expect_success):
    samples = []
    receiver = None
    if expect_success:
        receiver, port = start_receiver()
    try:
        for sample in range(repeats + 1):
            current_policy = []
            if policy_mode == "allow":
                current_policy = ["--net-policy", "allow", "--net-allow",
                                  f"127.0.0.1:{port}"]
            elif policy_mode == "deny":
                current_policy = ["--net-policy", "deny"]
            command = proot_command(current_policy, port)
            start = time.monotonic_ns()
            completed = subprocess.run(
                command,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                env={**os.environ, "TMPDIR": os.environ["TMPDIR"]},
                text=True,
                check=False,
            )
            elapsed = (time.monotonic_ns() - start) / 1_000_000
            if expect_success and completed.returncode != 0:
                raise RuntimeError(f"{label} failed: {completed.stderr[-2000:]}")
            if not expect_success and completed.returncode == 0:
                raise RuntimeError(f"{label} unexpectedly succeeded")
            if sample != 0:
                samples.append(elapsed)
            if receiver is not None:
                while True:
                    try:
                        receiver.recvfrom(2048)
                    except socket.timeout:
                        break
    finally:
        if receiver is not None:
            receiver.close()
    median = statistics.median(samples)
    print(f"{label}: median={median:.3f} ms samples=" + ",".join(f"{x:.3f}" for x in samples))
    return median


receiver, allowed_port = start_receiver()
receiver.close()
print(f"iterations={iterations} repeats={repeats} warmup={warmup} allowed_port={allowed_port}")

# Warm up the executable and the loader separately from measured samples.
run_case("warmup off", "off", allowed_port, True)
off = run_case("off", "off", allowed_port, True)
allow = run_case("allow", "allow", allowed_port, True)
deny = run_case("deny", "deny", allowed_port, False)

print(f"allow/off ratio={allow / off:.3f}x")
print(f"deny/off ratio={deny / off:.3f}x")
print("=== SUMMARY: net-policy benchmark completed ===")
PY
