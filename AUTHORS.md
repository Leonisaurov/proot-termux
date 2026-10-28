# Authors

This file records who wrote the work included in this repository. It has three
parts: the upstream PRoot project this fork is based on, the history inherited
from the Termux packaging of PRoot, and the `proot-termux` fork itself.

## Upstream PRoot

The copyright holder for PRoot and CARE is STMicroelectronics. Both tools are
developed in the Compilation Expertise Center. The table reproduces the credits
recorded in [`proot/docs/legal/proot-COPYING`](proot/docs/legal/proot-COPYING),
which also carries the full text of the GNU General Public License, version 2.

| Author | Role recorded upstream |
|---|---|
| Cédric VINCENT | Original author, maintainer |
| Rémi DURAFFORT | Improved the `-0` feature, many experiments, bug reports, fixes, and articles |
| Christophe GUILLON | Improved CARE, plugin experiments, bug reports, and fixes |
| Yves JANIN | Support for the i386 architecture, many experiments and bug reports |
| Antoine MOYNAULT | Many experiments and bug reports |
| Claire ROBINE | Many bug fixes |
| Clément BAZIN | Plugins interface, dependency tracker plugin, and bug reports |
| Christian BERTIN | Valuable support |
| Denis FERRANTI | Valuable support |
| Paul GHALEB | User manual review |

| Upstream project | Home | Relationship to this fork |
|---|---|---|
| PRoot | <https://proot-me.github.io/> | The sandboxing tool this fork cross-compiles and extends. The source in `proot/src/` is inherited from this project. |
| CARE | Same upstream tree | The companion Caching, Ahead-of-time, and REordering tool developed in the same center. |

The base used by this fork is the Termux-maintained mirror of PRoot at
<https://github.com/termux/proot>. Upstream work is not relicensed here; see
[LICENSE.md](LICENSE.md).

## Inherited Termux packaging and upstream mirror history

The repository carries two bodies of inherited history, both of which are
reachable from `master`:

- The Termux packaging history of the `proot` package, 227 commits, counted
  with `git log -- packages/proot`.
- The upstream mirror history, 273 commits, merged into this branch by
  `ec69bb7f3b` (2026-08-23, "chore(proot): sync upstream history without
  replacing fork tree"). That merge brought the upstream commits into the
  graph without replacing the working tree of the fork.

The people below authored commits in that inherited history. The counts are
taken from the git history of this repository; the same person appears once
per address they committed with.

| Name (as recorded in git) | Contact | Termux packaging history | Upstream mirror history | Contribution area |
|---|---|---|---|---|
| Michal Bednarski / Michał Bednarski / michalbednarski | <bednarski.michal2@gmail.com> | 59 | 144 | Long-term maintainer of the Termux `proot` package and of the upstream mirror; most of the upstream `link2symlink` and packaging updates |
| Sylirre / Leonid Plyushch / sylirre | <sylirre@termux.dev> | 18 | 65 | Upstream PRoot mirror maintenance: `link2symlink` hardening, netlink emulation, syscall entry and exit fixes, NDK build fixes |
| Fredrik Fornwall | <fredrik@fornwall.net> | 15 | 27 | Termux `proot` package build, `link2symlink` packaging, Termux-specific build fixes |
| Corbin Champion | <corbinlc@gmail.com> | 0 | 13 | Upstream PRoot fixes carried by the mirror |
| Tom Yan | <tom.ty89@gmail.com> | 1 | 5 | Upstream PRoot fixes carried by the mirror |
| Henrik Grimler | <henrik@grimler.se> | 2 | 4 | Termux package build fixes, `termux-chroot` adjustments |
| Andrea Riolo | <andry06082002@gmail.com> | 0 | 3 | Upstream PRoot fixes carried by the mirror |
| Alexander Sosedkin | <monk@unboiled.info> | 0 | 2 | Upstream PRoot fixes carried by the mirror |
| Ciriaco Garcia de Celis | <cgarcelis@gmail.com> | 0 | 2 | Upstream PRoot fixes carried by the mirror |
| Chongyun Lee (licy183) | <45286352+licy183@users.noreply.github.com> | 1 | 1 | NDK r29 build fix for the Termux `proot` package |
| Oliver Schmidhauser | <oli@glow.li> | 2 | 0 | `termux-chroot` and Termux package build fixes |
| Twaik Yont | <twaikyont@gmail.com> | 2 | 0 | Termux package maintenance |
| LeapofAzzam-backup | <76893443+LeapofAzzam-backup@users.noreply.github.com> | 0 | 1 | Upstream PRoot fixes carried by the mirror |
| Jeremy Messenger | <1302422+jlmess77@users.noreply.github.com> | 0 | 1 | Upstream PRoot fixes carried by the mirror |
| Hax4us | <lkpandey950@gmail.com> | 0 | 1 | Upstream PRoot fixes carried by the mirror |
| Mark Steward | <marksteward@gmail.com> | 0 | 1 | Upstream PRoot fixes carried by the mirror |
| Josselin Poiret | <dev@jpoiret.xyz> | 0 | 1 | Upstream PRoot fixes carried by the mirror |
| Symphorien Gibol | <symphorien+git@xlumurb.eu> | 0 | 1 | Upstream PRoot fixes carried by the mirror |
| agnostic-apollo | <agnosticapollo@gmail.com> | 0 | 1 | Upstream PRoot fixes carried by the mirror |
| Lucy Phipps | <landfillbaby69@gmail.com> | 1 | 0 | Termux package version bumps |
| Buck Evan | <buck@yelp.com> | 1 | 0 | Termux package metadata fix |
| Vijay Vaidyanathan | <vijay@myemailforever.com> | 1 | 0 | Termux package metadata fix |
| WMCB-Tech | <bautista.wyatt.marcus30@gmail.com> | 1 | 0 | `termux-chroot` option |
| Wetitpig | <winsto003@hotmail.com> | 1 | 0 | Upstream PRoot source formatting |

The remainder of the inherited history is the Termux package-builder
automation, committed by the Termux maintainers and by the `Termux Github
Actions` bot (`<contact@termux.dev>`, 13 commits on the `proot` package path).
Those commits are packaging automation for other packages and are not
attributed here individually.

## proot-termux fork

| Name | Contact | Role |
|---|---|---|
| Leonisaurov | <https://github.com/Leonisaurov> | Fork maintainer / maintainer of the proot-termux fork |

Leonisaurov authored the 243 commits that make up the fork's own work in this
repository; 146 of them touch the proot source tree, the package definition,
the tests or the documentation under `proot/`, and the work spans 2026-07 to
2026-10. The fork maintainer is the declared maintainer of the package in
[`proot/ci/termux/packages/proot/build.sh`](proot/ci/termux/packages/proot/build.sh)
(`TERMUX_PKG_MAINTAINER="@leonisaurov"`).

## How to be listed

- Authorship is recorded from the git metadata of the commit. Set a real name
  and a reachable address in your commit; the name in `git log` is the name
  shown here.
- For joint work, add a `Co-authored-by:` trailer with the name and address of
  the co-author. A `Reviewed-by:` or `Reported-by:` trailer records credit for
  review or reporting without adding an author.
- A patch, a pull request, or a direct commit to `master` are all accepted
  routes. A patch sent by email is applied with the author name you wrote in
  the patch.
- You are added to this file in the same commit that introduces your first
  change, or in the next maintenance commit if the change lands through a
  merge. Do not edit this file in a pull request that has not been merged.
- Contributions to the upstream PRoot project are credited upstream by the
  upstream project, not here. A change that is also sent upstream keeps the
  upstream credit in addition to the entry here.
- Entries are never removed. A contributor who asks to be removed is marked as
  "no longer active" instead.

## Translation and documentation

The operational documentation of this fork is written in Spanish, and the
protocol and reference material is written in English:

| Area | Language | Where |
|---|---|---|
| Operational documentation, hardening history, benchmarks | Spanish | [`proot/docs/`](proot/docs/), [`proot/docs/security/FIXES.md`](proot/docs/security/FIXES.md), [`proot/README.md`](proot/README.md) |
| Protocol and integration reference | English | [`proot/control-api/PROTOCOL.md`](proot/control-api/PROTOCOL.md), [`proot/control-api/README.md`](proot/control-api/README.md), [`proot/docs/architecture/proot-source.md`](proot/docs/architecture/proot-source.md) |
| Repository entry point and build instructions | English | [`README.md`](README.md) |

The translation between the two is part of the fork's documentation work and is
credited to the contributors listed in this file. A documentation change that
touches a Spanish operational page and its English counterpart must land both
so that the two stay consistent.
