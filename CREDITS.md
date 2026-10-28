# Credits

This file lists the third-party components that the `proot-termux` fork builds,
ships, or derives from, together with the license that applies to each one.

The fork itself is licensed as described in [LICENSE.md](LICENSE.md). The table
below covers only the components that come from outside this repository.

## Components

| Component | Where it comes from | License | Where it is used |
|---|---|---|---|
| PRoot | <https://github.com/termux/proot>, the Termux-maintained mirror of the STMicroelectronics project (<https://proot-me.github.io/>) | GPL-2.0-or-later. Full text and upstream credits: [`proot/docs/legal/proot-COPYING`](proot/docs/legal/proot-COPYING) | The whole source tree in `proot/src/`, including the files inherited unmodified from upstream |
| PRoot and CARE upstream credits (Cédric VINCENT, Rémi DURAFFORT, Christophe GUILLON, Yves JANIN, Antoine MOYNAULT, Claire ROBINE, Clément BAZIN, Christian BERTIN, Denis FERRANTI, Paul GHALEB) | STMicroelectronics, Compilation Expertise Center | GPL-2.0-or-later, as recorded upstream | [`proot/docs/legal/proot-COPYING`](proot/docs/legal/proot-COPYING) |
| talloc 2.4.3 | <https://www.samba.org/ftp/talloc/talloc-2.4.3.tar.gz>, fetched by the package definition | Declared as `GPL-3.0` in [`proot/ci/termux/packages/libtalloc/build.sh`](proot/ci/termux/packages/libtalloc/build.sh) | Build dependency of the `proot` package; provides the memory pool contexts used by the tracer |
| libandroid-shmem 0.7 | <https://github.com/termux/libandroid-shmem> | BSD 3-Clause, as declared in [`proot/ci/termux/packages/libandroid-shmem/build.sh`](proot/ci/termux/packages/libandroid-shmem/build.sh) | Build dependency of the `proot` package; System V shared memory emulation on Android |
| Android NDK r30 | <https://developer.android.com/ndk> | Apache-2.0 | Cross toolchain used to build proot for `aarch64`; the version is pinned in [`proot/ci/termux/scripts/properties.sh`](proot/ci/termux/scripts/properties.sh) (`TERMUX_NDK_VERSION_NUM="30"`) and dispatched by `termux_setup_toolchain_30.sh` |
| termux-packages build scripts | <https://github.com/termux/termux-packages> | GPL-3.0, as stated in [`README.md`](README.md) | The package-builder framework under [`proot/ci/termux/scripts/`](proot/ci/termux/scripts/) and the package definitions under [`proot/ci/termux/packages/`](proot/ci/termux/packages/) |
| `config.guess` and `config.sub` (timestamp 2025-07-10) | The GNU project, vendored by the package-builder framework | GPL-3.0-or-later, as stated in the header of each script | [`proot/ci/termux/scripts/config.guess`](proot/ci/termux/scripts/config.guess), [`proot/ci/termux/scripts/config.sub`](proot/ci/termux/scripts/config.sub) |
| Reference license texts | Collected by the Termux project in the `termux-licenses` package | One text per license, as named in each file | [`proot/ci/termux/packages/termux-licenses/LICENSES/`](proot/ci/termux/packages/termux-licenses/LICENSES/) |
| Build container image | Built from [`proot/ci/termux/scripts/Dockerfile`](proot/ci/termux/scripts/Dockerfile) and published as `ghcr.io/leonisaurov/package-builder` | No license is declared for the image in this repository | The CI build environment described in [CONTRIBUTING.md](CONTRIBUTING.md) |
| NDK compatibility patches | Inherited from termux-packages and extended by this fork (4 of the 84 commits) | GPL-3.0, together with the rest of the inherited packaging | [`proot/ci/termux/ndk-patches/`](proot/ci/termux/ndk-patches/) |

## Notes

- The `proot` package definition in
  [`proot/ci/termux/packages/proot/build.sh`](proot/ci/termux/packages/proot/build.sh)
  declares `TERMUX_PKG_LICENSE="GPL-2.0"`, which matches the upstream license of
  the proot source it packages.
- The `libtalloc` package definition declares `TERMUX_PKG_LICENSE="GPL-3.0"`.
  That is the declaration used by this repository's build system, and it is
  the statement carried in the package metadata of the artifact built here.
  This file records the declaration of the repository, not an independent
  assessment of the upstream terms of talloc.
- The control-api consumer in [`proot/control-api/`](proot/control-api/) has no
  third-party runtime dependency: the Rust crate declares no dependency, the
  Bun package declares no dependency, and the Python package requires only
  `setuptools` to build.
- The Docker image is a build environment, not a distributed artifact. Only the
  `.pkg.tar.xz` package produced from it is released.
