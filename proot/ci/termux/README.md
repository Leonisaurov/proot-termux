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

## NDK pin (r30)

The vendored toolchain is pinned to **Android NDK r30**:

- `scripts/properties.sh` sets `TERMUX_NDK_VERSION_NUM="30"`.
- `scripts/build/termux_step_setup_toolchain.sh` dispatches to
  `termux_setup_toolchain_30`, and `build-package.sh` sources
  `scripts/build/toolchain/termux_setup_toolchain_30.sh`.
- The compatibility headers live in `ndk-patches/30/`.
- `scripts/setup-android-sdk.sh` only knows how to fetch NDK `30` and `23c`.

The reason for the pin is the package-builder image, not the code. The
`ghcr.io/termux/package-builder` image used by the runner no longer ships
`android-ndk-r29`, so a pin on r29 aborted the job before anything was
compiled:

```text
ERROR: NDK (/home/builder/lib/android-ndk-r29) not pointing at a directory!
```

(run `36568563252`). Moving the pin to r30 — together with sourcing the r30
toolchain helper in `build-package.sh`, without which the build then died with
`termux_setup_toolchain_30: command not found` — turned the job green again
(run `36570306638`). Do not go back to r29 unless a builder image that
contains r29 is pinned as well.

## Workflows

The active workflow is `build-proot.yml`, at
`.github/workflows/build-proot.yml` in the repository root. It triggers on
pushes that touch `proot/ci/termux/packages/proot/**`,
`proot/ci/termux/scripts/**`, `proot/ci/termux/ndk-patches/**`,
`proot/ci/termux/build-package.sh`, `proot/src/**` or `proot/scripts/**`, and
it can also be started manually with `workflow_dispatch`.

`docker_image.yml` is **disabled**: its job carries `if: false`, so it never
runs and is not part of the build path.

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
