# 0012: Product chrome zoom and appearance propagation

Status: accepted; the single interface-size control is superseded by ADR 0104
Date: 2026-09-19

## Context

Interface zoom scaled text but kept product geometry fixed. The private font
overlay also rescaled CM's already-resolved base QWidget font. Per-window
appearance snapshots and a process-lifetime monospace cache left other windows
and chat text stale after Settings saves.

## Decision

- Small, Medium and Large use 12/14, 1 and 16/14 geometry factors. Geometry is
  rounded from Medium dimensions without the text scale's 8 px floor. Zero,
  hairlines and user-resized sidebar widths are not enlarged.
- Private QWidget product composition explicitly registers designed dimensions
  through `chrome_geometry`. Bindings scale from those dimensions on each update
  and inherit the current surface zoom when dynamically created. Unregistered
  widgets, including CM internals and editor/terminal text metrics, are untouched.
- Private QSS geometry is expanded before font placeholders; the CM font overlay
  protects the resolved base QWidget font while scaling fixed font literals.
- QML geometry uses `ChromeMetrics.px` and the shared zoom-factor token. Existing
  text tokens keep their named steps and sidebar/transcript +2 px bias. Transcript
  measurements invalidate on zoom or font changes.
- Each desktop appearance save updates all windows' snapshots before emitting
  synchronous theme changes, then reapplies appearance even when zoom is unchanged.
  Unedited fields in an already-open Settings draft merge with the newer snapshot.
- Chat monospace family resolution occurs on appearance updates, never per row.
  Elided sidebar labels recompute on font/style changes as well as resize.

## Boundary and verification

This is private product composition. No public CM implementation or gitlink is
modified; CM continues to own persistence and editor/terminal typography APIs.
New shared CM behavior must still be released in the public repository first.

Regression tests exercise repeated zoom transitions, dynamic rows, inherited
fonts, tiny gaps, elision, multiple windows, font-only changes and old Settings
drafts. Native captures supplement the offscreen suite without model calls.

## Két kit bridge (2026-11)

The public Lite/Pro repository now owns the shared mapping in
`core/src/ui/InterfaceZoom.h`. `ThemeManager` publishes interface pixels / 14 to
`ket::Appearance`, so kit Widgets and `KetTheme.zoom` follow the same setting.
The geometry adapter delegates rounding to `ket::Appearance::scale`; Pro keeps
its named sizes and Lite keeps its existing numeric input. The legacy TypeScale
roles remain until their consumers migrate to kit recipes. This bridge does not
change editor/terminal text zoom or remove the remaining metric ladder.
