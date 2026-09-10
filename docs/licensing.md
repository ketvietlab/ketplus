# Licensing and distribution

This document records the license boundary for KetPlus CM and the direct
dependencies used by this repository. It is an engineering compliance record,
not legal advice.

## Product boundary

| Repository or product | Visibility | License |
| --- | --- | --- |
| `ketplus` / KetPlus CM | Public | MPL 2.0 |
| `ketplus-desktop` / KetPlus | Private | Proprietary |
| `ketplus-app` | Private | Proprietary |
| Ketsuite cloud services and daemon | Private | Proprietary |

KetPlus CM contains the native editor, workspace explorer, Git integration,
terminal, and local document preview. AI Workspace, provider adapters,
KetRouter integration, account and entitlement code, remote sessions, and the
daemon do not belong in this repository.

MPL 2.0 applies at file level. A proprietary larger work may combine MPL files
with separate proprietary files. Modifications distributed to an MPL-covered
file must remain available under MPL 2.0. KetPlus Desktop should therefore
extend CM through separate modules and consume a pinned CM revision instead of
placing proprietary implementation in CM files.

## Direct dependencies

| Component | Pinned version | Integration | License |
| --- | --- | --- | --- |
| Qt | 6.5 or later | Dynamic frameworks/libraries | LGPL 3.0, GPL alternatives, or commercial |
| Scintilla | `a1c86144eed9e3d2187e3a8b391d11ca909f00d2` (`rel-5-5-2`) | Compiled statically | Scintilla permissive license |
| Lexilla | `3e6f317eed854bc312b4c4453206189d381c7ae7` (`rel-5-5-2`) | Compiled statically | Scintilla permissive license |
| libvterm | `9d6d2112335080312ef8c36667fa717ded4f7daf` (`v0.3.3`) | Compiled statically | MIT |
| Inter | 4.1 | Font embedded in Qt resources | SIL OFL 1.1 |

Git and Mermaid CLI are discovered as separately installed executables. They
are not linked or bundled by this source build. If a future installer bundles
either tool, its complete binary dependency set must be audited again.

## Distribution requirements

The planned Qt LGPL distribution uses dynamic linking. Every official package
must permit replacement of Qt libraries and include the exact Qt, platform,
plugin, and transitive-license inventory used by that package. A controlled
copy of corresponding Qt source and build changes must be provided or offered
as required by the selected license.

Release packages must include this repository's `LICENSE`,
`THIRD_PARTY_NOTICES.md`, the dependency license texts under
`third_party/licenses`, Qt notices for the exact deployed build, and an SPDX
SBOM. Linux packages that add Qt WebEngine require a separate Chromium notice
audit; KetPlus CM currently does not depend on Qt WebEngine.

## Release gates

- [ ] Public history and source contain only CM-approved code and assets.
- [ ] Proprietary features remain in private repositories and separate files.
- [ ] Distributed changes to MPL-covered CM files are published.
- [ ] Every bundled library, plugin, font, and executable is in the SBOM.
- [ ] Exact Qt and Qt third-party notices are generated for each platform.
- [ ] License files are reachable from the package and About dialog.
- [ ] The proprietary desktop identifies its pinned CM source revision.
- [ ] A final legal review is completed before the first official release.
