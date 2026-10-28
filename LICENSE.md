# License

This repository is a fork of PRoot and combines several layers that carry
different licenses. The license of each layer is stated below. When you
redistribute this repository, or a binary built from it, you must comply with
every license that applies to the parts you redistribute.

## 1. The proot source: GPL-2.0-or-later

The source in `proot/src/` is inherited from the upstream PRoot project. The
copyright holder for PRoot and CARE is STMicroelectronics, and the tools are
developed in the Compilation Expertise Center. Upstream is distributed under
the GNU General Public License, version 2, or, at your option, any later
version.

The full text of the license, together with the upstream credits, is kept at:

- [`proot/docs/legal/proot-COPYING`](proot/docs/legal/proot-COPYING)

The upstream project is <https://proot-me.github.io/>, and the mirror this
fork is based on is <https://github.com/termux/proot>.

The files inherited unmodified from upstream keep the upstream copyright
notices. This fork does not relicense that work.

## 2. The additions of this fork: GPL-2.0-or-later

The changes made by this fork are distributed under the same terms, the GNU
General Public License, version 2, or, at your option, any later version. They
are combined with the upstream source into a single work and into a single
binary, so the combined binary is distributed as a whole under
GPL-2.0-or-later.

This applies to:

| Path | What it contains |
|---|---|
| `proot/src/extension/` | The extensions of this fork: `net_policy`, `virtual_net`, `resource_limit`, `proc_isolation`, `fake_id0`, `kompat`, `port_switch`, `sysvipc`, `link2symlink`, `hidden_files`, `mountinfo`, `fix_symlink_size`, `ashmem_memfd`, and the `supervise` support |
| `proot/src/cli/` | The command line options of this fork and the handlers that implement them |
| `proot/src/supervise/` | The supervisor and the `--exec` clients |
| `proot/bin/` | The launchers of this fork: `termux-isolated`, `proot-exec`, `sample-harness`, `appimage-run` |
| `proot/scripts/` | The local native build script and its helpers |
| `proot/tests/` | The regression scripts, the probes and the rootfs wrappers |
| `proot/control-api/` | The PRCT codec, the launcher and the contract of the control fd |
| `proot/examples/` | The example configurations |

The same terms apply to the documentation in `proot/docs/` and to the
repository-level documents, including this file.

The reason for the choice is technical as well as legal: the extensions link
against the upstream code and are compiled into the same binary. Relicensing
them separately would produce a combined work that is not GPL-2.0-or-later as
a whole.

## 3. The build infrastructure: Apache-2.0

The build infrastructure of this repository is licensed under the
[Apache License, Version 2.0](https://www.apache.org/licenses/LICENSE-2.0).
This covers the package-builder framework, the package definitions, the NDK
compatibility patches, the Docker wrapper and the CI workflows, which live
under `proot/ci/termux/`, and the GitHub Actions workflows in `.github/`.

The scripts and patches that build each package are licensed under the same
license as the actual package they build. The `proot` package definition
therefore declares `TERMUX_PKG_LICENSE="GPL-2.0"`, matching the license of
the proot source it packages.

## 4. Vendored third-party components

The components that come from outside this repository keep their own
licenses. They are listed in [CREDITS.md](CREDITS.md), with the source, the
license and the place where each one is used. Among them are the upstream
PRoot source itself (GPL-2.0-or-later), talloc, libandroid-shmem, the Android
NDK, the vendored `config.guess` and `config.sub` of the GNU project, and the
build scripts inherited from termux-packages.

The full text of the GNU General Public License, version 2, is at
[`proot/docs/legal/proot-COPYING`](proot/docs/legal/proot-COPYING).

## SPDX

The identifiers used by this repository are:

| Identifier | Applies to |
|---|---|
| `GPL-2.0-or-later` | `proot/src/`, `proot/bin/`, `proot/scripts/`, `proot/tests/`, `proot/control-api/`, `proot/examples/`, `proot/docs/` |
| `Apache-2.0` | `proot/ci/termux/` outside the package definitions, and `.github/` |
| `GPL-2.0` (as declared by the package) | `proot/ci/termux/packages/proot/` |
| The license of each component | The vendored third-party components listed in [CREDITS.md](CREDITS.md) |

The SPDX identifier is a short form for the license names. It does not replace
the full text, and it does not remove any condition of the licenses it names.

## How to comply

If you redistribute the source of this repository, or a binary built from it:

1. Keep the copyright notices of the upstream files, and state that the
   modified files carry notices of the change and its date, as section 2 of
   the GNU General Public License requires.
2. Ship the full text of the GNU General Public License, version 2, together
   with the binary. In this repository that text is
   [`proot/docs/legal/proot-COPYING`](proot/docs/legal/proot-COPYING). The
   upstream file also carries the upstream credits, which must be preserved.
3. Offer the corresponding source code of the whole work under the same
   terms. The source of this repository, at the revision that produced the
   binary, is what that means here.
4. Keep the Apache-2.0 notice in the files of the build infrastructure, and
   reproduce the Apache License, version 2, with the parts of the build
   infrastructure that you redistribute.
5. Reproduce the notices of the vendored components that you redistribute,
   with the license that applies to each of them, as listed in
   [CREDITS.md](CREDITS.md).
6. Do not add restrictions of your own to the recipients' exercise of the
   rights granted by these licenses. A downstream distributor may not narrow
   what the GPL or the Apache License permits.

This is a statement of intent, not legal advice. The obligations above come
from the license texts themselves, which are the only authority.
