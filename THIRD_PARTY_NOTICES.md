# Third-party notices

KetPlus Desktop includes, links, or derives assets from the components below.
Their complete source-distribution license texts are stored in
`third_party/licenses` and must accompany binary distributions.

| Component | Version | License file |
| --- | --- | --- |
| KetPlus CM | `cm-v0.3.6` — https://github.com/ketvietlab/ketplus/tree/cm-v0.3.6 | `KetPlus-CM-MPL-2.0.txt` |
| Qt | 6.11 or later | Generated from the exact deployed Qt build before release |
| KetPlus Syntax | `cb7435c1e9237ee32d3459a58917b0e84b8b6025` — https://github.com/ketvietlab/ketplus-syntax/tree/cb7435c1e9237ee32d3459a58917b0e84b8b6025 | `KetPlus-Syntax-MPL-2.0.txt` |
| Scintilla | `a1c86144eed9e3d2187e3a8b391d11ca909f00d2` | `Scintilla.txt` |
| Lexilla | `3e6f317eed854bc312b4c4453206189d381c7ae7` | `Lexilla.txt` |
| libvterm | `9d6d2112335080312ef8c36667fa717ded4f7daf` | `libvterm-MIT.txt` |
| Inter | 4.1 | `Inter-OFL-1.1.txt` |
| KetQt | `b286dce5344f05e55bcfd86c7c1d6e10a5ab2587` — https://github.com/ketvietlab/ket-qt | `KetQt-MPL-2.0.txt` |
| Két Design System tokens (in KetQt) | 0.1.44 | `Ket-Design-System-MIT.txt` |
| Két Symbol (in KetQt and the KétPlus Terminal icon) | 0.12.0 — Két Symbol © 2026 Két Việt, CC BY 4.0 | `Ket-Symbol-CC-BY-4.0.txt` |
| KÉT Sans | 1.1.0 — https://github.com/ketvietlab/ket-sans-font/releases/tag/v1.1.0 | `KET-Sans-OFL-1.1.txt` |
| KÉT Sans Mono | 0.10.4 Medium | `KET-Sans-Mono-OFL-1.1.txt` |
| Lucide Static | 1.40.0 | `Lucide-ISC-and-Feather-MIT.txt` |
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

The Lucide file contains both the ISC notice and the MIT notice for icons
derived from Feather. Qt is expected to be dynamically linked under LGPL 3.0
for the initial release plan. This source-tree inventory does not replace the
Qt and Qt third-party notices generated from each exact platform build.
