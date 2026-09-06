#!/usr/bin/env bash
# Opt-in paired comparison. Ordinary suite runs do not require an old binary.
set -euo pipefail
if [[ -z "${AUDIT_BASELINE:-}" ]]; then
    echo 'SKIP: set AUDIT_BASELINE to compare two PRoot builds'
    exit 0
fi
: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"
test -x "$AUDIT_BASELINE"
command -v python3 >/dev/null
python3 - <<'PY'
import hashlib
import json
import os
import resource
import signal
import socket
import statistics
import subprocess
import tempfile
import threading
import time

prefix = os.environ['PREFIX']
binaries = [os.path.abspath(os.environ['AUDIT_BASELINE']), prefix + '/bin/proot']
pairs = int(os.environ.get('AUDIT_PAIRS', '10'))
assert pairs > 0

def expired(signum, frame):
    raise TimeoutError('benchmark exceeded 90 seconds')

signal.signal(signal.SIGALRM, expired)

with tempfile.TemporaryDirectory(prefix='proot-comparison.', dir=os.environ['TMPDIR']) as host:
    env = dict(PREFIX=prefix, PATH=prefix + '/bin', TMPDIR=host,
               PROOT_TMP_DIR=host, PROOT_RUNTIME_DIR=host)
    def run(binary, case):
        flags = ['--proc-isolated'] if case == 'proc' else []
        guest = [prefix + '/bin/true']
        if case.startswith('net_'):
            mode = case[4:]
            flags = ['--net-policy', mode]
            # Local UDP only. The test asserts that denial or success matches policy.
            code = ('import socket\ns=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)\n'
                    'denied=0\nfor _ in range(50):\n try:s.sendto(b"x",("127.0.0.1",9))\n'
                    ' except PermissionError:denied+=1\n'
                    f'assert denied=={50 if mode == "deny" else 0}\n')
            guest = [prefix + '/bin/python3', '-c', code]
        command = [binary, '-r', '/', '-w', prefix, '--kill-on-exit', *flags, *guest]
        if case.startswith('nested_'):
            for _ in range(int(case[-1]) - 1):
                command = [binary, '-r', '/', '-w', prefix, '--kill-on-exit', *command]
        peer = child_fd = worker = None
        if case == 'prct':
            peer, child_fd = socket.socketpair()
            command[1:1] = ['--control-fd', str(child_fd.fileno())]
            def drain():
                # Root / is explicit RW; this workload emits HELLO only.
                try:
                    while peer.recv(4096):
                        pass
                except OSError:
                    pass
            worker = threading.Thread(target=drain)
            worker.start()
        with tempfile.TemporaryFile(dir=host) as errors:
            start = time.monotonic()
            process = subprocess.Popen(command, env=env, stdout=subprocess.DEVNULL,
                                       stderr=errors, start_new_session=True,
                                       pass_fds=(() if child_fd is None else (child_fd.fileno(),)))
            if child_fd is not None:
                child_fd.close()
            try:
                signal.alarm(90)
                _, status, usage = os.wait4(process.pid, 0)
                elapsed = time.monotonic() - start
                process.returncode = os.waitstatus_to_exitcode(status)
                errors.seek(0)
                assert process.returncode == 0, errors.read().decode(errors='replace')
                return dict(seconds=elapsed, cpu_seconds=usage.ru_utime + usage.ru_stime,
                            maxrss_kib=usage.ru_maxrss)
            finally:
                signal.alarm(0)
                if process.returncode is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                if peer is not None:
                    peer.close()
                    worker.join(2)
    report = dict(binaries=[dict(path=b, sha256=hashlib.sha256(open(b, 'rb').read()).hexdigest())
                           for b in binaries], pairs=pairs, cases={})
    for case in ('plain', 'proc', 'prct', 'net_off', 'net_allow', 'net_deny', 'nested_2', 'nested_3'):
        samples = [[], []]
        for pair in range(pairs + 1):
            for index in ([0, 1] if pair % 2 == 0 else [1, 0]):
                result = run(binaries[index], case)
                if pair:
                    samples[index].append(result)
        before, after = [statistics.median(row['seconds'] for row in sample) for sample in samples]
        report['cases'][case] = dict(before=samples[0], after=samples[1], ratio=after/before)
        print(f'{case}: before={before:.6f}s after={after:.6f}s ratio={after/before:.3f}', flush=True)
    destination = os.environ.get('AUDIT_REPORT')
    if destination:
        with open(destination, 'w') as output:
            json.dump(report, output, indent=2)
PY
