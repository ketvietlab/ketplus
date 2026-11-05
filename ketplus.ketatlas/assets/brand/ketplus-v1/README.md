# KétPlus Pro brand assets

The "Khối K+" logo set for KétPlus Pro (indigo `#707BFD`, border `#5D65E4`), supplied by
the KétViệt design team on 2026-11-05. The SVG files are copied unchanged; the PNG files
are rendered from them without redrawing, recoloring or added effects.

- `ketplus-mark.svg`, `ketplus-mark-transparent.png` (supplied 1024 px master): the square
  mark for avatars, welcome, loading, previews and compact navigation.
- `ketplus-logo-light.svg`, `ketplus-logo-light.png` (1440 × 360): horizontal logo for
  the light sidebar. The wordmark already reads "KétPlus Pro", so no edition label
  follows it.
- `ketplus-logo-dark.svg`, `ketplus-logo-dark.png` (1440 × 360): the same on a
  transparent background for the dark sidebar.
- `ketplus-icon.png` (supplied 1024 px master): the screen favicon.

The app icons live outside this bundle: `assets/icons` and `assets/brand` for Pro, and
`core/assets/icons` and `core/assets/brand` for the Lite edition (green `#02A855`, border
`#008F48`). The macOS `.icns` and the runtime window icon (`ketplus-app-icon.png`, which
macOS shows in the Dock) render the mark at 824 of 1024 px, Apple's icon grid, so the
tile matches the size of other Dock icons. The mark's own canvas leaves 4 of 64 units
around the tile, so it is drawn at 92% of the canvas. Windows `.ico` files and in-app
marks use the full canvas.
