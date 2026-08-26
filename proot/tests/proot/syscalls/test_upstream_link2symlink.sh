#!/usr/bin/env bash
# Regression coverage for upstream link2symlink commits:
#   7ff389a181 descriptor names, 894e5789cd statx link count,
#   7266fb3e85 descriptor-backed O_NOFOLLOW l2s directory.
set -euo pipefail

: "${TMPDIR:=/data/data/com.termux/files/usr/tmp}"
export TMPDIR
mkdir -p "$TMPDIR"
test -d "$TMPDIR" && test -w "$TMPDIR"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
TERMUX_ISOLATED="${TERMUX_ISOLATED:-$REPO_ROOT/bin/termux-isolated}"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
GUEST_SH="$PREFIX/bin/sh"

if [[ ! -x "$TERMUX_ISOLATED" ]]; then
	echo "SKIP: termux-isolated wrapper not found at $TERMUX_ISOLATED"
	exit 0
fi

python3 - "$TERMUX_ISOLATED" "$PREFIX" "$TMPDIR" "$GUEST_SH" <<'PY'
import os
import shutil
import subprocess
import sys
import tempfile

wrapper, prefix, tmpdir, guest_sh = sys.argv[1:]


def run_guest(script, *, proc_isolated=True):
    command = [wrapper, "--termux-paths", "--rw-dir", tmpdir,
               "--cwd", prefix]
    if not proc_isolated:
        command.append("--no-proc-isolated")
    command += ["--proot-arg", "--link2symlink", "--",
                guest_sh, "-c", script]
    return subprocess.run(command, check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def new_case():
    return tempfile.mkdtemp(prefix="upstream-l2s-", dir=tmpdir)


def remove_case(path):
    shutil.rmtree(path, ignore_errors=True)


# 7ff389a181: readlink(/proc/self/fd/N) reports the guest name used to open
# a faked hard link.  The proc view is disabled only because this upstream
# case explicitly tests the raw proc-fd name; the launcher remains isolated.
case = new_case()
try:
    script = f'''set -eu
base={case!r}
mkdir -p "$base/links" "$base/l2s"
printf 'content\\n' > "$base/links/original"
PROOT_L2S_DIR="$base/l2s" ln "$base/links/original" "$base/links/link"
exec 3< "$base/links/link"
got=$(readlink /proc/self/fd/3)
test "$got" = "$base/links/link"
'''
    run_guest(script, proc_isolated=False)
    print("PASS: 7ff389a181 descriptor name")
finally:
    remove_case(case)


# 894e5789cd: statx-backed coreutils stat sees both faked hard links.
case = new_case()
try:
    script = f'''set -eu
base={case!r}
mkdir -p "$base/links" "$base/l2s"
printf 'content\\n' > "$base/links/original"
PROOT_L2S_DIR="$base/l2s" ln "$base/links/original" "$base/links/link"
test "$(stat -c %h "$base/links/link")" = 2
'''
    run_guest(script)
    print("PASS: 894e5789cd statx link count")
finally:
    remove_case(case)


# 7266fb3e85: replacing the configured l2s directory with a symlink must not
# redirect backing-file creation outside the guest-controlled directory.
case = new_case()
try:
    script = f'''set -eu
base={case!r}
root="$base/rootfs"
outside="$base/outside"
mkdir -p "$root/.l2s" "$outside"
printf 'escaped\\n' > "$root/original"
rm -rf "$root/.l2s"
ln -s "$outside" "$root/.l2s"
test -L "$root/.l2s"
PROOT_L2S_DIR="$root/.l2s" ln "$root/original" "$root/link" || true
test "$(find "$outside" -mindepth 1 -maxdepth 1 -print | wc -l)" = 0
'''
    run_guest(script)
    print("PASS: 7266fb3e85 O_NOFOLLOW l2s directory")
finally:
    remove_case(case)

print("=== SUMMARY: upstream link2symlink PASS=3 FAIL=0 ===")
PY
