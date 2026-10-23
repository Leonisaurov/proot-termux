# Termux package build system

This directory contains the package-builder portion of the repository. It is
kept separate from the project source and from local Termux development tools.

## Contents

- `packages/` — package definitions and patches consumed by the builder.
- `ndk-patches/` — NDK compatibility headers and patches selected by the
  toolchain setup scripts.
- `repo.json` — repository metadata used by the Termux build scripts.
- `build-package.sh`, `build-all.sh`, and `clean.sh` — package build and
  cleanup entrypoints.
- `scripts/` — Docker wrapper, build helpers, and package-builder framework.

The GitHub Actions workflows remain in the repository-level `.github/`
directory because GitHub requires that location.

## CI build

From the repository root:

```bash
./proot/ci/termux/scripts/run-docker.sh \
  ./proot/ci/termux/build-package.sh -I -a aarch64 --format pacman proot
```

The container writes intermediate package output to `proot/ci/termux/output/`.
The workflow collects the resulting archives into `proot/artifacts/`; that directory
is intentionally separate from the local build output.

The proot package imports its source from `proot/src/` during
`termux_step_pre_configure()`. The package definition does not add Termux
bindings or harness policy to proot.

To clean the package-builder state, use `./proot/ci/termux/clean.sh`. It is kept
inside this directory because it operates on the Termux package-builder
environment; it is not a generic repository cleanup command.

## Local build

Local native builds use `proot/scripts/build-native.sh`, outside this directory. They
write packages to `proot/artifacts/packages/` by default and support `-o DIR` for an
explicit destination.

## Sync state with `termux/termux-packages`

This directory is a vendored copy of the package builder. It has no submodule and no
commit pin, so drift is measured by content instead of by history: fetch the upstream
tree index (`GET /repos/termux/termux-packages/git/trees/master?recursive=1`), compare
`git hash-object` of each file here against the blob for the same relative path, and
classify each difference as *upstream improvement* or *our intentional edit*.

Measured on 2026-10-23 against `master`: 163 files byte-identical, 53 with differences,
27 local-only. Ported: `ndk-patches/` was **already identical** (0 of 19 files with
drift, including all of `ndk-patches/30/`), and the build steps that proot's pipeline
actually runs — parallel `xz -T`, parallel `strip`/`elf-cleaner`, the restructured
symbol check in `termux_step_massage`, `TERMUX_PKGS__BUILD__REUSE_BUILT_PACKAGES`, the
`cargo --debug` flag, the NDK 23c toolchain prefix — plus `libtalloc` 2.4.3 → 2.5.0.

Deliberately **not** ported, because the difference is ours and taking upstream would
delete it: `build-package.sh` (offline bundle, pacman hooks wiring),
`scripts/properties.sh` (documentation links rewritten to this fork),
`scripts/build/termux_step_install_license.sh` (structural divergence, not a single
upstream fix), `buildorder.py` (patched to skip absent dependencies), `repo.json`
(declares `debian` while the workflow passes `--format pacman`; the pipeline wins), the
gpg keys and the language `termux_setup_*` helpers proot never calls.

`PROOT_WITH_LIBANDROID_SHMEM=true` is also not adopted here; see
`proot/docs/UPSTREAM.md` for why it interacts with `ashmem_memfd`.

The `libtalloc` pin is not cosmetic: dependencies are resolved by downloading the
published `.deb` for the declared version, and only falling back to building from
source when that version is gone from the repo. While this directory pinned 2.4.3 the
pipeline was silently building talloc from `samba.org` on every run; at 2.5.0 it finds
`libtalloc_2.5.0_aarch64.deb` in `termux-main` and uses it, which is also the ABI the
rest of Termux links against. Keep the pins here close to upstream for that reason.
