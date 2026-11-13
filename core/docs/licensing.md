# Licensing and distribution

This document records the license boundary for KetPlus CM and the direct
dependencies used by this repository. It is an engineering compliance record,
not legal advice.

## Product boundary

| Repository or product | Visibility | License |
| --- | --- | --- |
| `ketplus` / KetPlus CM | Public | MPL 2.0 |
| `ketplus-desktop` / KetPlus | Private | Proprietary |
| the private mobile app | Private | Proprietary |
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
| Qt | 6.8 or later | Dynamic frameworks/libraries | LGPL 3.0, GPL alternatives, or commercial |
| Scintilla | `a1c86144eed9e3d2187e3a8b391d11ca909f00d2` (`rel-5-5-2`) | Compiled statically; `PlatQt.cpp` compiled from a copy with the client-origin fix in `core/cmake/Dependencies.cmake` | Scintilla permissive license |
| Lexilla | `3e6f317eed854bc312b4c4453206189d381c7ae7` (`rel-5-5-2`) | Compiled statically | Scintilla permissive license |
| libvterm | `9d6d2112335080312ef8c36667fa717ded4f7daf` (`v0.3.3`) | Compiled statically | MIT |
| miniz | `77d0dce8627735138c51770d1799a1ef48f2117d` (3.1.2) | Compiled statically into theme input; upstream sources unmodified | MIT; ship `third_party/licenses/miniz-MIT.txt` |
| KÉT Sans | 1.1 official ZIP pinned by SHA-256 | Four faces embedded in Qt resources | SIL OFL 1.1, Reserved Font Names "KÉT" and "KET" |
| KÉT Sans Mono | 0.10.4 Medium, `assets/fonts` | Embedded in Qt resources | SIL OFL 1.1, Reserved Font Names "KÉT" and "KET" |
| Két Design System | 0.1.44 | Mermaid configuration snapshots in Qt resources | MIT |
| KetQt | `d47638214f80695524cd2724996674da7afdbb6a` | Compiled statically (ADR 0035) | MPL 2.0 |
| Két Design System tokens | 0.1.44, inside KetQt | Compiled into KetQt | MIT |
| Két Symbol | 0.12.0 SVGs, inside KetQt; `assets/brand/ketplus-terminal.svg` | Embedded in KetQt Qt resources; rendered into the KétPlus Terminal app icon | CC BY 4.0; credit "Két Symbol © 2026 Két Việt, CC BY 4.0" |

The Mermaid theme snapshots retain their upstream source revision and hashes in
`resources/mermaid/SOURCE.json`. The publisher's MIT license is bundled alongside
the configuration as `KetDesignSystem-LICENSE.txt`; no KétSuite service code is copied.

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

## Bundled color-scheme data

Eight third-party palette conversions are embedded as data alongside the two
existing Két Design System bases. Licenses were verified at the exact official
repository revisions below; complete unmodified notices ship with Lite and Pro.
No upstream extension/plugin implementation is distributed or executed. Generated
files preserve authors, source URLs, commit IDs, SHA-256 hashes, import reports
and input-preparation notes. See `docs/color-schemes.md` and the reproducible lock
at `core/tools/color-schemes/sources.lock.json`.

| Theme data | Pinned revision | Author | Verified license | Distributed text |
| --- | --- | --- | --- | --- |
| VS Code Dark+, VS Code Light+, Solarized Dark | [`1b509615d9f84db862313485737c05e8b81d2977`](https://github.com/microsoft/vscode/tree/1b509615d9f84db862313485737c05e8b81d2977) | Microsoft Corporation; Solarized palette by Ethan Schoonover | [MIT](https://github.com/microsoft/vscode/blob/1b509615d9f84db862313485737c05e8b81d2977/LICENSE.txt) | `VSCode-Themes-MIT.txt` |
| Darcula, IntelliJ Light | [`417e214ea024cd783179244f51329df01277bb93`](https://github.com/JetBrains/intellij-community/tree/417e214ea024cd783179244f51329df01277bb93) | JetBrains s.r.o. | [Apache-2.0](https://github.com/JetBrains/intellij-community/blob/417e214ea024cd783179244f51329df01277bb93/LICENSE.txt) | `JetBrains-Themes-Apache-2.0.txt` |
| Dracula | [`9726e1f6b51a4382334bc36e03128fcf02c3fb45`](https://github.com/dracula/visual-studio-code/tree/9726e1f6b51a4382334bc36e03128fcf02c3fb45) | Dracula Theme | [MIT](https://github.com/dracula/visual-studio-code/blob/9726e1f6b51a4382334bc36e03128fcf02c3fb45/LICENSE) | `Dracula-MIT.txt` |
| One Dark Pro | [`75c80ab18d90655e05ad837bb8d89f5e68d5ce53`](https://github.com/Binaryify/OneDark-Pro/tree/75c80ab18d90655e05ad837bb8d89f5e68d5ce53) | Binaryify | [MIT](https://github.com/Binaryify/OneDark-Pro/blob/75c80ab18d90655e05ad837bb8d89f5e68d5ce53/LICENSE.txt) | `OneDarkPro-MIT.txt` |
| Nord | [`8ead09822c02d0d49d0f764104505e5a34d3689f`](https://github.com/nordtheme/visual-studio-code/tree/8ead09822c02d0d49d0f764104505e5a34d3689f) | Arctic Ice Studio / Sven Greb | [MIT](https://github.com/nordtheme/visual-studio-code/blob/8ead09822c02d0d49d0f764104505e5a34d3689f/license) | `Nord-MIT.txt` |

JetBrains' `JetBrains-Themes-NOTICE.txt` also accompanies its Apache-2.0 license.
The selected 2024 snapshot predates later IDE open-source-build terms; no newer
IDE-build agreement is used as the redistribution grant. Dracula's official
v2.24.3 VSIX is a hash-locked developer input only. Solarized is credited to Ethan
Schoonover through Microsoft's MIT port. Theme names credit their respective
authors and do not imply endorsement. No palettes are recolored after import.

Solarized original palette notice: Ethan Schoonover, MIT; pinned official source
[`62f656a02f93c5190a8753159e34b385588d5ff3`](https://github.com/altercation/solarized/blob/62f656a02f93c5190a8753159e34b385588d5ff3/LICENSE),
complete text in `third_party/licenses/Solarized-MIT.txt`.
