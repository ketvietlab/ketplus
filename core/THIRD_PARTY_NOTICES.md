# Third-party notices

KetPlus CM includes or links the components below. Their complete license texts
are stored in `third_party/licenses` and must accompany binary distributions.

The ketPlus names, mark and lockups are trademarks, all rights reserved, and are
not covered by any licence listed here; see `TRADEMARKS.md` at the repository root.

| Component | Version | License file |
| --- | --- | --- |
| Qt | 6.8 or later | Generated from the exact deployed Qt build before release |
| Scintilla | `rel-5-5-2` | `third_party/licenses/Scintilla.txt` |
| Lexilla | `rel-5-5-2` | `third_party/licenses/Lexilla.txt` |
| KetPlus Syntax | `cb7435c1e9237ee32d3459a58917b0e84b8b6025` — https://github.com/ketvietlab/ketplus-syntax | `third_party/licenses/KetPlus-Syntax-MPL-2.0.txt` |
| libvterm | `v0.3.3` | `third_party/licenses/libvterm-MIT.txt` |
| miniz | 3.1.2 (`77d0dce8627735138c51770d1799a1ef48f2117d`) — https://github.com/richgel999/miniz | `third_party/licenses/miniz-MIT.txt` |
| KÉT Sans | 1.1.0 | `third_party/licenses/KET-Sans-OFL-1.1.txt` |
| KÉT Sans Mono | 0.10.4 Medium | `third_party/licenses/KET-Sans-Mono-OFL-1.1.txt` |
| KetQt | `d47638214f80695524cd2724996674da7afdbb6a` — https://github.com/ketvietlab/ket-qt | `third_party/licenses/KetQt-MPL-2.0.txt` |
| Két Design System tokens (in KetQt) | 0.1.44 | `third_party/licenses/Ket-Design-System-MIT.txt` |
| Két Symbol (in KetQt and the Terminal app icon) | 0.12.0 — Két Symbol © 2026 Két Việt, CC BY 4.0 | `third_party/licenses/Ket-Symbol-CC-BY-4.0.txt` |

Qt is expected to be dynamically linked under LGPL 3.0 for open-source release
builds. This source-tree notice is not a substitute for the Qt and Qt
third-party notices generated from each exact deployed build.

## Bundled color schemes

Palette data is converted to the KetPlus format; source JSON/XML and rendering
semantics are approximated by the importers. No extension/plugin code is shipped
or executed. Original attribution and permission notices are preserved below.

| Theme data and attribution | Pinned official source | License file |
| --- | --- | --- |
| VS Code Dark+, VS Code Light+, Solarized Dark — Microsoft Corporation; Solarized palette by Ethan Schoonover | [`1b509615d9f84db862313485737c05e8b81d2977`](https://github.com/microsoft/vscode/tree/1b509615d9f84db862313485737c05e8b81d2977) | `third_party/licenses/VSCode-Themes-MIT.txt` (MIT) |
| Darcula, IntelliJ Light — JetBrains s.r.o. | [`417e214ea024cd783179244f51329df01277bb93`](https://github.com/JetBrains/intellij-community/tree/417e214ea024cd783179244f51329df01277bb93) | `third_party/licenses/JetBrains-Themes-Apache-2.0.txt` (Apache-2.0) |
| Dracula — Dracula Theme | [`9726e1f6b51a4382334bc36e03128fcf02c3fb45`](https://github.com/dracula/visual-studio-code/tree/9726e1f6b51a4382334bc36e03128fcf02c3fb45) | `third_party/licenses/Dracula-MIT.txt` (MIT) |
| One Dark Pro — Binaryify | [`75c80ab18d90655e05ad837bb8d89f5e68d5ce53`](https://github.com/Binaryify/OneDark-Pro/tree/75c80ab18d90655e05ad837bb8d89f5e68d5ce53) | `third_party/licenses/OneDarkPro-MIT.txt` (MIT) |
| Nord — Arctic Ice Studio / Sven Greb | [`8ead09822c02d0d49d0f764104505e5a34d3689f`](https://github.com/nordtheme/visual-studio-code/tree/8ead09822c02d0d49d0f764104505e5a34d3689f) | `third_party/licenses/Nord-MIT.txt` (MIT) |

The JetBrains Apache-2.0 data also retains
`third_party/licenses/JetBrains-Themes-NOTICE.txt`. Solarized palette design is by
Ethan Schoonover, included through Microsoft's MIT port. Generated metadata and
`core/tools/color-schemes/sources.lock.json` identify the exact source files and
hashes. The Két originals retain the existing Két Design System MIT credit.

Solarized original palette notice: Ethan Schoonover, MIT; pinned official source
[`62f656a02f93c5190a8753159e34b385588d5ff3`](https://github.com/altercation/solarized/blob/62f656a02f93c5190a8753159e34b385588d5ff3/LICENSE),
complete text in `third_party/licenses/Solarized-MIT.txt`.
