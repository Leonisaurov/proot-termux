# Changelog

All notable changes to the `proot-termux` fork are recorded in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the versioning policy is described in
[Versioning policy](#versioning-policy).

## Unreleased

### Added

- Repository governance documents at the root of the repository:
  `AUTHORS.md`, `CREDITS.md`, `CONTRIBUTING.md`, `CHANGELOG.md`, `SECURITY.md`,
  `CODE_OF_CONDUCT.md`, and a rewritten `LICENSE.md` that states the license
  of each layer of the tree.
- `SECURITY.md` documents the supported version, the private reporting path
  through GitHub Security Advisories, the hardening model, the disclosure
  timeline, and the files that carry the security-relevant behaviour.
- `proot/scripts/build-windows.ps1` builds the fork from a Windows host: it
  drives WSL2 to produce a native Linux binary and to cross-compile the
  Android `aarch64` target with the NDK r30, and it verifies both artifacts.
- `proot/docs/operations/windows-build.md` documents the three build routes
  available from Windows, the measured evidence of why there is no native
  Windows binary of proot, and the two build defects that the exercise
  uncovered.
- `proot/scripts/README.md` describes what each script in `proot/scripts/`
  does and when to use it.
- `proot/tests/build/test_clang_lto_loader_embedding.sh` is the regression
  test for the two build defects below: it checks that `ashmem_memfd` declares
  `<string.h>`, that the makefile adds `-ffat-lto-objects` exactly once with a
  clang-like compiler and never with gcc, and that a `clang` build with LTO
  still embeds the loader in the binary.

### Changed

- `LICENSE.md` now states that the proot source in `proot/src/` is
  GPL-2.0-or-later, inherited from upstream STMicroelectronics, and that the
  fork's own additions keep the same terms so that the combined binary stays
  GPL-2.0-or-later. The build infrastructure under `proot/ci/termux/` remains
  Apache-2.0.
- The source files written by this fork now carry a licence and authorship
  header that names the fork maintainer, and the files inherited from upstream
  keep their original credit. `proot --about` now credits the upstream
  copyright holder and the fork.

### Fixed

- `extension/ashmem_memfd/ashmem_memfd.c` used `strcmp()` and `memset()`
  without including `<string.h>`. The file is compiled only on Android or
  Bionic, and clang 16 or newer turns the implicit declaration into a hard
  error, so the Android build failed at that object. (REV 106)
- The default build, which embeds the loader inside the binary, failed with a
  clang-like compiler: `clang -flto -c` emits a raw LLVM bitcode object that
  neither `objdump` nor `objcopy` can read, so the loader embedding step
  aborted. The makefile now adds `-ffat-lto-objects` when it detects a
  clang-like compiler and LTO is enabled, and the `loader/loader-info.c` rule
  uses the new `READELF` variable so a cross build can point it at
  `llvm-readelf`. (REV 106)

## 5.1.107.89-105 — 2026-10-04

Tag: `proot-5.1.107.89-105`

### Fixed

- `fake_id0` no longer follows a symlink when it opens a meta file, and
  validates the contents it reads. The meta file subsystem is only reached
  from code compiled under `USERLAND`, which no build configuration of this
  repository defines, so the issue was latent rather than reachable in the
  released binary. The fix is kept because the defence is cheap and any build
  with `USERLAND` enabled would inherit the vector. (REV 104)

### Changed

- The host binding guard in `get_binding()` is memoized on the first use
  instead of being recomputed for every candidate binding of the same path.
  (REV 105)
- The build infrastructure is closed on the NDK r30 toolchain: the helper is
  sourced from the toolchain setup script and the push triggers are widened.

## 5.1.107.89-103 — 2026-09-29

Tag: `proot-5.1.107.89-103`

### Fixed

- `/proc/meminfo` is served from the real host file instead of a synthetic
  file of zeroes. `meminfo` and `cpuinfo` are machine summaries, not data
  about another process, so they are a pass-through. `uptime`, `loadavg` and
  the global counters of `stat` stay synthetic, because Android denies them
  to the tracee. (REV 103)

### Changed

- The NDK 30 toolchain helper is sourced from the setup script, and the
  workflow triggers cover the toolchain, patch and script paths.

## 5.1.107.89-95 to 5.1.107.89-102 — 2026-09-13

Tags: `proot-5.1.107.89-95`, `proot-5.1.107.89-96`,
`proot-5.1.107.89-97`, `proot-5.1.107.89-98`, `proot-5.1.107.89-99`,
`proot-5.1.107.89-100`, `proot-5.1.107.89-101`, `proot-5.1.107.89-102`

### Fixed

- The PRCT protocol accepts `AF_UNIX` endpoints. The three control-api
  decoders rejected socket family 1, so any guest that opened a Unix socket
  desynchronized the control channel and blocked the tracee. (REV 96)
- The synthetic file descriptors of the proc view are protected from eviction
  by the read cache. (REV 100)
- The Android prefix check in the proc isolation extension no longer dereferences
  a null pointer. (REV 102)

### Changed

- `translate_path()` reuses the binding it already resolved instead of
  resolving it twice. (REV 97)
- The proc isolation extension classifies a read descriptor once, and reuses
  that classification for the vector reads, instead of issuing a `readlink`
  and an extra ptrace stop per call. (REV 98, REV 99)
- The proc isolation extension distinguishes the `maps` of the calling tracee
  from the `maps` of another tracee in a single classification pass, and the
  dead `maps_fd` state is removed. (REV 101)
- The documentation describes the Android runtime paths that remain visible
  under rootfs isolation. (REV 95)

## 5.1.107.89-93 and 5.1.107.89-94 — 2026-09-06

Tags: `proot-5.1.107.89-93`, `proot-5.1.107.89-94`

### Fixed

- The Android virtual `accept` keeps the real peer address of the abstract
  Unix socket, including in published listeners. (REV 94)

### Changed

- The protocol cleanup and the validation paths are hardened. (REV 93)

## 5.1.107.89-92 — 2026-08-26

Tag: `proot-5.1.107.89-92`

### Added

- Regressions for the nested supervisor and the pthread paths of the proc
  view.

### Changed

- Nested execution is transparent: a proot instance started inside another
  one behaves as the guest expects. (REV 89)
- The nested proc lookup overhead is reduced. (REV 92)

## 5.1.107.89-68 and 5.1.107.89-69 — 2026-08-25

Tags: `proot-5.1.107.89-68`, `proot-5.1.107.89-69`

### Changed

- The repository is consolidated under `proot/`: the source, the control-api,
  the tests, the packaging and the documentation live in one product tree.
  (REV 68)
- The build keeps the sources used for feature detection. (REV 69)
- Releases are published with a versioned tag.

## 5.1.107.89-19 to 5.1.107.89-27 — 2026-08-06 to 2026-08-19

No tag was published for these revisions. The dates are the dates of the
commits that carried them, and the phases are described in
[`proot/docs/security/FIXES.md`](proot/docs/security/FIXES.md).

### Added

- Phase A, isolation P0: the host `stat` and `readlink` oracle over `/proc`
  is closed, the `kill(-1)` broadcast is confined to the guest, and the
  netlink topology of the host is no longer visible. (REV 19, 2026-08-06)
- Phase B, leaks and file descriptors: eight fixes covering the control
  socket of the supervisor, the tracee list, the descriptor map of the virtual
  network, the stack pointer of the guest, the bridge children of the helper,
  the signal descriptor and the loader paths. (REV 21, 2026-08-18)
- Phase C, isolation P1: `MS_RDONLY` is emulated for the mounts, `/etc` is
  read-only by default behind `--recommended-etc-rw`, the `/proc` blocklist is
  extended, the supervisor authenticates its peers with `SO_PEERCRED`, and
  `--fake-permissions` emulates the permissions without touching the host.
  (REV 22, 2026-08-18)
- The virtual network, the resource limits, the network policy and the strict
  proc view are exposed as explicit extensions.

### Changed

- Phase D, performance: the BPF syscall list is copied in sorted order, the
  `ioctl` exit stop is requested dynamically, `faccessat2` and `socket` no
  longer request an exit stop, and the tracee lookup uses a hash table with
  O(1) access. The binding cache and the canonicalization cache were skipped:
  the coherence risk outweighs the gain. (REV 23, REV 24, REV 25)
- Phase E, remaining P2 and P3 findings: `renameat2` and `uname` no longer
  request an exit stop, `mknod` returns a natural error instead of a phantom
  success, `pidfd_open` is confined by the proc isolation, the virtual network
  registry reuses its lock descriptor, and the netlink fast path skips the
  scan when no fake socket is open. (REV 24, REV 26, REV 27)
- The `accept(2)` call is retried as `accept4(2)` when the Android kernel
  answers `ENOSYS`. (REV 13, 2026-08-03)
- The always-on overhead is removed: without a feature flag the tracer takes
  the same path as upstream. (REV 12, 2026-08-03)
- The supervisor frees the root tracee on shutdown, closes the use-after-free
  window, and waits for the daemon children. (REV 17, REV 18)

## 5.1.107.89-1 to 5.1.107.89-18 — 2026-07-23 to 2026-08-05

No tag was published for these revisions. The dates are the dates of the
commits that carried them.

### Added

- The `--supervise` and `--exec` pair: a supervisor instance accepts clients
  over an abstract Unix socket, authenticates them with `SO_PEERCRED`, and
  runs their command inside the same context. (REV 18)
- The resource limits: `--cpu-limit`, `--single-core`, `--mem-limit`,
  `--nice`, `--fd-limit`, `--proc-limit` and `--resource-isolated`. The
  process limit is enforced inside the guest against the processes of that
  instance, not against the user id of the host. (REV 9, REV 10, REV 11)
- The virtual network over abstract Unix sockets, with a shared registry
  between the instances of the same network name, and a helper that bridges a
  virtual port to a real TCP port. (REV 11 to REV 13)
- The individual isolation flags for the dangerous system calls, and the
  process isolation extension. (REV 9 to REV 12)

### Changed

- The package builds the source tree directly. There are no patches and no
  external download: the source lives in the repository. (REV 2)
- The port mapping is hardened and optimized. (REV 2)

## Versioning policy

The version of the package is composed of two parts:

| Part | Meaning | Where it is defined |
|---|---|---|
| `5.1.107.89` | The upstream PRoot base version. The upstream project uses its own version numbering and the fork does not change it. | `TERMUX_PKG_VERSION` in `proot/ci/termux/packages/proot/build.sh` |
| `105` | The package revision of the fork. It is incremented for every change that touches the packaged source or the package definition. | `TERMUX_PKG_REVISION` in `proot/ci/termux/packages/proot/build.sh` |

The release tag is `proot-<version>-<revision>`, for example
`proot-5.1.107.89-105`. The CI workflow reads both values out of the package
definition and publishes the tag, so the two must be in sync.

The revision is the smallest unit of change in this fork. A new feature, a
fix, or a packaging change is one revision. The upstream version only moves
when the base is updated.

## Known inconsistencies

The two build entry points do not agree on the version and the revision:

| Source | Version | Revision |
|---|---|---|
| `proot/ci/termux/packages/proot/build.sh` | `5.1.107.89` | `105` (the last released revision) |
| `proot/scripts/build-native.sh` | `5.1.107.87` | `16` |

The discrepancy is real and is not resolved in this file. The package
definition is the source of truth for the published artifact and for the
release tag; the local native build script carries its own pair of values and
they have not been synchronized. Do not assume that the two agree, and do not
silently synchronize them: pick the file you are editing and use its values.
