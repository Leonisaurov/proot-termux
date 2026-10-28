# Contributing

This repository is a fork of PRoot that cross-compiles it for Android aarch64.
The rules below describe how work is actually done here. They are the same
rules that the repository's own agent instructions enforce, written for human
contributors.

## Layers and ownership

The repository has four layers. Each one has a clear owner and a clear
boundary. A change belongs to exactly one of them, and a change that crosses
boundaries is a design change that must be discussed before it is written.

| Layer | Where | Does | Must not |
|---|---|---|---|
| proot core | `proot/src/` | Path translation, explicit bindings, compatibility and isolation features, the opt-in protocol | Assume Android or Termux paths, assume the consumer's bindings, or decide for the harness |
| `termux-isolated` launcher | `proot/bin/termux-isolated` | The concrete way this fork launches proot inside Termux | Control the control fd, or expose storage on its own |
| `control-api` consumer | `proot/control-api/` | The PRCT codec, the launcher and the contract of the control fd | Assume that proot ships internal presets |
| Test harness | `proot/tests/`, `proot/bin/sample-harness` | Read every event and answer while proot is alive | Expect proot to decide on its behalf |

The dependency between them is one-way. `proot/control-api` needs a proot build
that speaks the PRCT protocol and supports `--control-fd`; proot does not need
`proot/control-api`, does not choose its authorization policy, and does not
carry the consumer's presets or application-specific configuration. A consumer
may offer presets such as `load termux`, but it must pass the resulting options
to proot explicitly and stay responsible for their effects.

Before adding a feature to the core, answer three questions: which general
problem of proot does it solve, how does it work without any integration, and
why does it not belong to a consumer. If a feature only makes sense for one
consumer, it does not go into `proot/src/`.

## The revision bump rule

Any change to `proot/src/` or to `proot/ci/termux/packages/proot/` requires
bumping `TERMUX_PKG_REVISION` in
[`proot/ci/termux/packages/proot/build.sh`](proot/ci/termux/packages/proot/build.sh)
**before** the commit.

```bash
# 1. edit the code
# 2. bump the revision in proot/ci/termux/packages/proot/build.sh
# 3. commit, push, and watch the CI
```

Without the bump, the CI does not publish a new release and users keep
receiving the old package. The current revision is always read from that
file; do not copy a number from a document, because the documents age.

The same rule applies to `proot/ci/termux/packages/libtalloc/` and
`proot/ci/termux/packages/libandroid-shmem/`: they are separate packages with
their own revisions.

## Commit message convention

```
<type>(<scope>): <summary>
```

| Type | Use for |
|---|---|
| `fix` | A behaviour correction |
| `enhance` | A new capability or an improvement of an existing one |
| `chore` | Maintenance that does not change behaviour |
| `ci` | Workflows, build scripts, packaging |
| `test` | Regressions and test infrastructure |
| `docs` | Documentation only |

The scope names the component, for example `fix(net-policy): ...` or
`enhance(proc_isolation): ...`. The summary is written in the imperative mood
and stays on one line.

Keep commits atomic: one concern per commit, one file group per commit, and
never mix two hardening phases in the same commit. A hardening phase is a
series of commits, each with its own verification.

## Before you build

Run a preflight before any long build, packaging, installation, or test
battery:

1. `uname -m` and the API level: the target is `aarch64`.
2. Toolchain: the NDK version is pinned to r30 in
   `proot/ci/termux/scripts/properties.sh`.
3. Free space and free RAM. The build is OOM-safe by default with two jobs; use
   `-j 1` on a device with less than 4 GB of RAM.
4. Network and cache availability for the CI path.
5. `TMPDIR` and the writability of the output directory.

Do not start a long compilation by guesswork. If the preflight fails, fix the
cause or report it before spending build time.

## Local build

Local builds use the official script, never `make` directly:

```bash
./proot/scripts/build-native.sh            # compile and package
./proot/scripts/build-native.sh -i         # compile, install and package
./proot/scripts/build-native.sh -c -i      # clean build, then install
./proot/scripts/build-native.sh --skip-build   # only package existing binaries
./proot/scripts/build-native.sh -j 4       # parallelism, default 2
```

The jobs option takes a separate argument (`-j 2` or `--jobs 2`; `-j2` is not
valid). The output is `$PREFIX/bin/proot`, the loaders are in
`$PREFIX/libexec/proot/{loader,loader32}`, the launcher script is
`$PREFIX/bin/termux-chroot`, and the package is written to
`proot/artifacts/packages/proot-<ver>-<rev>-aarch64.pkg.tar.xz`.

The dependency libraries (`libtalloc`, `libandroid-shmem`) are built
automatically by the `-I` flag of the container build.

## CI pipeline

The build runs in Docker on top of the Termux package-builder image, with the
Android NDK r30 as the cross toolchain:

```bash
./proot/ci/termux/scripts/run-docker.sh ./proot/ci/termux/build-package.sh -I -a aarch64 --format pacman proot
```

The workflow [`.github/workflows/build-proot.yml`](.github/workflows/build-proot.yml)
runs on `ubuntu-26.04` with 16 GB of zram swap, restores the `~/.termux-build`
cache, builds the package, uploads it as a workflow artifact, and publishes or
updates the release `proot-<version>-<revision>`. The release tag is derived
from the version and the revision read out of the package definition, so the
two must be in sync.

The `docker_image.yml` workflow is disabled (`if: false`) and is not part of
the active build path.

Monitor a run with the repository's CI notifier:

```bash
gita notify build-proot.yml 2>/dev/null | grep -E '(error|##\[error\]|mbind|Success)'
```

The exit code is 0 on success, 1 on failure and 2 when the run was cancelled.
The notifier blocks until the workflow finishes, or returns immediately if it
already has. Do not wrap it in `timeout` and do not stream it.

## Tests

Every new behaviour needs a reviewable regression script under
`proot/tests/<topic>/test_*.sh`. A feature is not validated by improvised
commands typed in a terminal.

A regression script must have:

- a preflight that states what it needs and skips cleanly when a requirement
  is missing, for example when the Alpine rootfs is not installed;
- temporary fixtures created under `$TMPDIR`;
- an explicit separation between host paths and guest paths;
- a cleanup step that leaves no temporary state behind.

The workflow is:

1. Write the script.
2. Validate it: `bash -n proot/tests/<topic>/test_*.sh` and `git diff --check`.
3. Run it in the Termux environment with the elevated flow, so that it executes
   on the device and not inside the agent sandbox.
4. Run the whole battery: `./proot/tests/run.sh all`.

`proot/tests/run.sh` continues after a failure and reports every failure at
the end. The suites are `build`, `proot`, `termux-isolated`, `harness`,
`control-api`, and `all`.

Tests that exercise the launcher must invoke
[`proot/bin/termux-isolated`](proot/bin/termux-isolated) and use its real
options. A test that compares two views must cover each relevant mode.

The interactive shell path must be exercised with a pseudo-terminal, not with
stdin from a pipe.

## Host and guest paths

Keep the two spaces apart, and choose one mode per test:

| Mode | Guest paths | Notes |
|---|---|---|
| `--termux-paths` | `$PREFIX/bin/sh`, `$PREFIX/etc`, `/data/data/com.termux/...` | The Termux prefix is a valid guest path by design |
| rootfs | `/bin/sh`, `/etc`, `/usr`, `/home` | The guest sees the rootfs layout, not the Android host layout |

Never mix the two spaces in the same test. `/data` and `/storage` are not
generic guest paths, and `termux-isolated` does not expose storage unless
`--with-storage` is passed.

`PROOT_TMP_DIR` and `PROOT_RUNTIME_DIR` are host paths used by proot itself.
They are not guest paths, and they must point to a writable host directory.

## Style rules

- Use `$TMPDIR`, never `/tmp`. On Termux, `/tmp` does not exist and is not
  writable.
- Run proot against a rootfs with `env -i`, and pass `TMPDIR` and
  `PROOT_RUNTIME_DIR` explicitly. The inherited environment causes `execve`
  failures inside the guest.
- Keep the ARM64 limitations in mind. The chained syscall mechanism does not
  work on ARM64, and `accept4` flags are `SYSARG_4`, never `SYSARG_3`.
- Do not change the signature of a shared function such as `insort_binding3`.
  Add a wrapper instead.
- Keep the "no flags, no overhead" rule: new code must return early when its
  feature is not enabled.
- Follow the "emulate, never deny" rule: when a natural result can be
  emulated, return it instead of `EPERM` or `ENOSYS`.
- Do not commit generated artifacts, build trees, or caches.

## Good first issues

These are concrete tasks taken from this repository's own plans and gaps.

1. Add a regression script for the `kompat` extension, which currently has no
   test of its own, under `proot/tests/proot/compat/`.
2. Add a regression script for the `port_switch` extension, which currently has
   no test of its own.
3. Add a regression script for the `sysvipc` extension, which currently has no
   test of its own.
4. Add a regression script for the `hidden_files` and `fix_symlink_size`
   extensions, which currently have no test of their own.
5. Add coverage for `--resource-isolated` and `--single-core`, which are
   implemented but not exercised by a dedicated regression.
6. Write the microbenchmark that measures `translate_path()` without a child
   process in front, as requested in
   [`proot/docs/security/FIXES.md`](proot/docs/security/FIXES.md) section K.2.
   It is the only way to measure the cost of the path guard in isolation.
7. Document the ownership of proot, the consumer and the harness, as listed in
   [`proot/tasks/todo.md`](proot/tasks/todo.md).
8. Remove the Termux paths that are still compiled into the core, as listed in
   [`proot/tasks/todo.md`](proot/tasks/todo.md).
9. Remove the PRCT policy exceptions that are specific to one consumer, as
   listed in [`proot/tasks/todo.md`](proot/tasks/todo.md).
10. Adjust the control-api and document the role of the harness more clearly,
    as listed in [`proot/tasks/todo.md`](proot/tasks/todo.md).
11. Continue the planned features listed in [`README.md`](README.md): the fake
    network namespace, the hostname override, the hidden uid, and the
    additional seccomp filter, each with its own regression script.

## Pull request checklist

- [ ] The change belongs to one layer, and it does not break the ownership
      boundary of that layer.
- [ ] `TERMUX_PKG_REVISION` in
      `proot/ci/termux/packages/proot/build.sh` is bumped if `proot/src/` or
      `proot/ci/termux/packages/proot/` was touched.
- [ ] The commit message follows `<type>(<scope>): <summary>`.
- [ ] A regression script exists under `proot/tests/<topic>/test_*.sh`, with a
      preflight, temporary fixtures, explicit host and guest paths, and
      cleanup.
- [ ] `bash -n` and `git diff --check` are clean.
- [ ] The test was run in the Termux environment, and the affected suite of
      `./proot/tests/run.sh` passes.
- [ ] The documentation that describes the changed behaviour was updated, in
      the language used by that document.
- [ ] No generated artifact, build tree, or cache is part of the diff.
