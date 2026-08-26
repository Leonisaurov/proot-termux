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
