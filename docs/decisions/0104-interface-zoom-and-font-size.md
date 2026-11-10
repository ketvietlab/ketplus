# ADR 0104: Interface zoom and interface font size are separate settings

Status: accepted. Supersedes the single interface-size control of ADR 0012.

## Context

Until now one value, `appearance/interfaceFontSizePixels`, was both the interface
text size and the zoom: the chrome scaled by that size / 14. Pro showed it as
Small/Medium/Large (12/14/16) and Lite as a 57–171% step slider. The mock has two
settings instead: an Interface zoom of 90/100/110/125% on Appearance and an
Interface font size of 13–16 px on Typography. Jay chose the mock on 2026-11-10.

## Decision

- `AppearanceSettings` gains `interfaceZoomPercent`, stored under
  `appearance/interfaceZoomPercent` and normalized to the nearest of
  `InterfaceZoomMetrics::steps()` (90, 100, 110, 125). `interfaceFontSizePixels`
  is now 13–16 px.
- A profile without the zoom key was saved before this change: its interface
  pixels were the zoom. It loads with the zoom step nearest pixels / 14 (12 → 90%,
  14 → 100%, 16 → 110%, 18 → 125%) and a 14 px interface size. The first save
  writes both keys, after which the size is read as a size.
- The zoom alone scales geometry, kit widgets (`ket::Appearance::setZoom`) and
  legacy product text (`TypeScale`, QSS `font-size` literals, tooltips). The
  integer that `chrome_geometry`, `TypeScale` and the surfaces' `applyPalette`
  pass is now the zoom percent, not interface pixels.
- The interface size sets the application font, at the zoom:
  `ThemeManager::interfaceTextPixels()` is the size × zoom. Markdown follows it,
  one pixel larger (ADR 0103), also at the zoom. Kit recipes and QML text keep their
  designed steps and follow the zoom only; a text scale inside the kit would be a
  ket-qt change.
- `ThemeManager::setInterfaceScale(size, zoom)` applies both at once. It records the
  zoom in `InterfaceZoomMetrics::current()` before changing the application font,
  because surfaces restyle on `ApplicationFontChange`, before the kit's
  `zoomChanged` fires.
- Lite's Appearance shows the zoom as a `ket::SegmentedControl` and Typography shows
  the size; Pro reuses both controls in its workspace pages.

## Consequences

Restore defaults returns both to 100% and 14 px. Older releases read only the
interface pixels, so a profile opened there again shows that release's size
without the zoom. Small/Medium/Large and the Custom choice are gone.
