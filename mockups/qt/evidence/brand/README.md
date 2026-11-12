# Brand approval evidence

Rendered with the standalone Qt mock, offscreen, at 1600 × 1300 and zoom 1.
Both screenshots contain light and dark specimens; the app sidebar follows the
named theme. Open `--screen=brand` to inspect interactively.

- [Light shell](light.png)
- [Dark shell](dark.png)

Validation: standalone CMake/Ninja build passed; `--smoke` passed with 72
screen/state entries and zero QML problems. Resolved font: `KET Sans / Bold /
weight 700` (QFontInfo and QRawFont). Sidebar geometry checks passed for light
and dark at zoom 1 and the supported maximum 3. A rendered dark Settings view
at zoom 3 was also inspected: the brand remained fully inside its 720 px sidebar.
Lilac `#AD97FA` against sidebar `#171B20` has WCAG contrast 7.07:1.
`QT_QPA_PLATFORM=offscreen ctest --test-dir build/mock` found no registered tests;
the mock uses its executable smoke checks. Offscreen platform messages about
size hints and the pre-existing Sans Serif alias are not QML warnings.

Source processing detail: alpha values below 8/255 outside the artwork were
ignored for crop bounds, avoiding isolated near-invisible noise in the supplied
PNG. Artwork bounds: (207, 239)–(1049, 998), then square canvas plus 4% margin
per side, resampled to 1024/128/64. The supplied original was not changed.
After Jay’s review, the wordmark is Bold 700 and the mark height is 1.45×
the wordmark pixel size, giving the mark more presence than the initial draft.
The iOS tiles are 60 pt squircle previews corresponding to a 1024 pt design
canvas; these are approval specimens, not release app-icon assets.
