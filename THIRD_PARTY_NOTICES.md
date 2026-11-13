# Third-party notices

KetPlus Desktop includes, links, or derives assets from the components below.
Their complete source-distribution license texts are stored in
`third_party/licenses` and must accompany binary distributions.

The ketPlus names, mark and lockups are trademarks, all rights reserved, and are
not covered by any licence listed here; see `TRADEMARKS.md` at the repository root.

| Component | Version | License file |
| --- | --- | --- |
| KetPlus CM | `cm-v0.3.6` — https://github.com/ketvietlab/ketplus/tree/cm-v0.3.6 | `KetPlus-CM-MPL-2.0.txt` |
| Qt | 6.11 or later | Generated from the exact deployed Qt build before release |
| KetPlus Syntax | `cb7435c1e9237ee32d3459a58917b0e84b8b6025` — https://github.com/ketvietlab/ketplus-syntax/tree/cb7435c1e9237ee32d3459a58917b0e84b8b6025 | `KetPlus-Syntax-MPL-2.0.txt` |
| Scintilla | `a1c86144eed9e3d2187e3a8b391d11ca909f00d2` | `Scintilla.txt` |
| Lexilla | `3e6f317eed854bc312b4c4453206189d381c7ae7` | `Lexilla.txt` |
| libvterm | `9d6d2112335080312ef8c36667fa717ded4f7daf` | `libvterm-MIT.txt` |
| miniz | 3.1.2 (`77d0dce8627735138c51770d1799a1ef48f2117d`) — https://github.com/richgel999/miniz | `miniz-MIT.txt` |
| KetQt | `cf3ac4e97b1db4566568887e02bdf681012373d5` — https://github.com/ketvietlab/ket-qt | `KetQt-MPL-2.0.txt` |
| Két Design System tokens (in KetQt) | 0.1.44 | `Ket-Design-System-MIT.txt` |
| Két Symbol (in KetQt and the Terminal app icon) | 0.12.0 — Két Symbol © 2026 Két Việt, CC BY 4.0 | `Ket-Symbol-CC-BY-4.0.txt` |
| KÉT Sans | 1.1.0 — https://github.com/ketvietlab/ket-sans-font/releases/tag/v1.1.0 | `KET-Sans-OFL-1.1.txt` |
| KÉT Sans Mono | 0.10.4 Medium | `KET-Sans-Mono-OFL-1.1.txt` |
| Sparkle (macOS builds with in-app updates) | 2.10.0 — https://github.com/sparkle-project/Sparkle/releases/tag/2.10.0 | `Sparkle-MIT.txt` |

| libdatachannel | 0.24.6 (`6b1e2e620f1e37f0eafeee702eaea0043cb305fd`) | `libdatachannel-MPL-2.0.txt` |
| libjuice | `cb523763255f241442da1fbdd72c498f33a54b35` | `libjuice-MPL-2.0.txt` |
| usrsctp | `fec583d54493f879d2ae44a743423bf8a04371ab` | `usrsctp-BSD-3-Clause.txt` |
| plog | `94899e0b926ac1b0f4750bfbd495167b4a6ae9ef` | `plog-MIT.txt` |
| OpenSSL | Build SDK version (3 or later) | `OpenSSL-Apache-2.0.txt` |

Unmodified WebRTC dependency source corresponding to these binaries is available at
https://github.com/paullouisageneau/libdatachannel/tree/6b1e2e620f1e37f0eafeee702eaea0043cb305fd,
https://github.com/paullouisageneau/libjuice/tree/cb523763255f241442da1fbdd72c498f33a54b35,
https://github.com/sctplab/usrsctp/tree/fec583d54493f879d2ae44a743423bf8a04371ab and
https://github.com/SergiusTheBest/plog/tree/94899e0b926ac1b0f4750bfbd495167b4a6ae9ef.
Private transport glue remains in separate proprietary files. No media/SRTP is linked.

Qt is expected to be dynamically linked under LGPL 3.0
for the initial release plan. This source-tree inventory does not replace the
Qt and Qt third-party notices generated from each exact platform build.

## Bundled color schemes

Palette data is converted to the KetPlus format; source JSON/XML and rendering
semantics are approximated by the importers. No extension/plugin code is shipped
or executed. Original attribution and permission notices are preserved below.

| Theme data and attribution | Pinned official source | License file |
| --- | --- | --- |
| VS Code Dark+, VS Code Light+, Solarized Dark — Microsoft Corporation | [`1b509615d9f84db862313485737c05e8b81d2977`](https://github.com/microsoft/vscode/tree/1b509615d9f84db862313485737c05e8b81d2977) | `third_party/licenses/VSCode-Themes-MIT.txt` (MIT) |
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
