# Application icon sources

Két Symbol © 2026 Két Việt, CC BY 4.0. See the shipped
`third_party/licenses/Ket-Symbol-CC-BY-4.0.txt`; product names remain trademarks.

The KétPlus tile and Pro light/dark bare marks are exact copies of the approved
`mockups/qt/assets/ketplus-mark*.svg` artwork. Lite and Pro use this same tile;
no distinct Lite tile is present in the approved source. Terminal's source is
`mockups/terminal/assets/ketplus-terminal.svg`. These are the approved repository
artifacts, not assets extracted from a newer Két Symbol release.

| Source | SHA-256 |
| --- | --- |
| `core/assets/brand/ketplus.svg` | `78bfac071736dc59173a49355fe4b7868d24557e78f946da5e99be3a956ccd24` |
| `core/assets/brand/ketplus-terminal.svg` | `88b95ca00b8c631f0bd8f2c2dc22fd18fab515133c3e0e51b604b78090ca3087` |
| `assets/brand/ketplus-mark-light.svg` | `b9a362901f7ffd234f8d89f11652d3a95e56a346ff923fed8bc41b7c354653d6` |
| `assets/brand/ketplus-mark-dark.svg` | `103b0304bac0dd70aa099bff8b789d463fb894857f204ccd8c4fa2cd4bfa7050` |

Run `assets/icons/generate-app-icons.sh` from a macOS checkout with Swift/AppKit,
`sips`, `iconutil` and Python 3. It renders the checked-in core SVGs at 1024 px,
creates all macOS ICNS sizes and a six-size PNG-backed Windows ICO, then copies
the KétPlus outputs to the Pro assets. The core exporter is standalone and can
export `lite` or `terminal` separately. The historical Terminal command delegates
to it. The Pro bare marks stay vector sources selected by theme.

The existing Lite/Pro CMake resource files consume the new PNG/ICNS/ICO paths.
The SVG and PNG files are available for Linux desktop packaging. Terminal's
Windows resource/ICO are prepared; the Terminal target remains macOS-only in
this baseline. Linux desktop entries, Windows Terminal target support, signing,
bundle installation and a machine without Homebrew Qt belong to the later
packaging/acceptance phase and have not been verified here.
