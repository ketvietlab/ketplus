# Application icon and brand sources

The ketPlus mark (`assets/brand/ketplus-mark.png`, 1024 px) is the approved artwork
from `mockups/qt/assets/ketplus-mark-v2.png`, in the interface's accent blue. It is
not licensed under MPL 2.0; all rights reserved. The wordmark is `ketPlus` in
KÉT Sans Bold 700 (SIL OFL 1.1), with "Plus" and the framed Pro badge in the
accent-text blue (`#1238DB` light, `#9EB5FF` dark).

Lockup geometry, shared by `mockups/qt/qml/parts/BrandLockup.qml`,
`modules/ai/src/agent/qml/BrandLockup.qml`, the widget `core/src/ui/BrandLockup.*`
and `render-brand.swift`: the mark is 1.45× the wordmark pixel size, the gap is
0.12× the mark; the Pro badge starts 0.3× the wordmark size after it, with 0.6× text,
centred on the x-height, with a 1.5 px stroke at 17 px.

Both editions use the same app icon: the mark on the white tile of the mock's
Brand screen, inside the macOS icon grid (824 px tile on 1024 px). The icon has no
"Pro"; the edition shows in the lockup only.

Run `assets/icons/generate-app-icons.sh` on macOS (swiftc with AppKit/CoreText,
`sips`, `iconutil`, Python 3). It downloads the pinned KÉT Sans release (or uses
`KET_SANS_BOLD=/path/KETSans-Bold.ttf`) and writes:

| Output | Lite (`core/`) | Pro |
| --- | --- | --- |
| macOS icon, 16–512 @1x/@2x | `core/assets/icons/KetPlus.icns` | `assets/icons/KetPlus.icns` |
| Windows icon, 16–256 | `core/assets/icons/KetPlus.ico` | `assets/icons/KetPlus.ico` |
| Window icon, 1024 px | `core/assets/brand/ketplus-app-icon.png` | `assets/brand/ketplus-app-icon.png` |
| Linux hicolor, 16–512 | `core/assets/linux/hicolor` (`ketplus`) | `packaging/linux/hicolor` (`ketplus-pro`) |
| Widget lockup mark, 256 px | `core/assets/brand/ketplus-mark-256.png` | `assets/brand/ketplus-mark-256.png` |
| README lockup, light/dark | `core/assets/brand/ketplus-lockup-*.png` | `assets/brand/ketplus-pro-lockup-*.png` |
| DMG lockup | | `packaging/macos/dmg-lockup.png` |

The Linux desktop entries are `core/assets/linux/ketplus.desktop` and
`packaging/linux/ketplus-pro.desktop`; each edition installs its entry and icons
into `share/applications` and `share/icons/hicolor`. `core/` keeps its own copies
because it builds without the Pro directories.

KetPlus Terminal keeps its Két Symbol icon (`core/assets/brand/ketplus-terminal.svg`,
CC BY 4.0, see `third_party/licenses/Ket-Symbol-CC-BY-4.0.txt`), exported by
`core/assets/icons/generate-terminal-icon.sh`.

Bundle identifiers, executable names and settings locations are unchanged; renaming
them needs a migration.
