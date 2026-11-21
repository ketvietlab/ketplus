# Color scheme UI review evidence

These original PNGs were captured from the native Qt Widgets Settings window on
macOS using Qt 6.11.2. The harness selected the **secondary DELL P2219H**
(1920×1080) before showing the window and verified that screen on the native
window handle. No window was shown on the primary display. Captures use an
isolated synthetic profile with the repository's original test themes.

| Image | Client viewport | Zoom | State |
| --- | --- | --- | --- |
| appearance-light.png | 1100×900 | 100% | Built-ins and VS Code/JetBrains imports, search, preferred variants, source labels and swatches |
| appearance-dark.png | 1100×900 | 100% | Same controls in dark appearance |
| import-report.png | 1100×900 | 100% | Conversion counts, per-key reasons and explicit preview/accept/cancel actions |

The same capture test measured 760×650 and 1100×900 in light/dark at
90/100/110/125% zoom. All 16 cells had no horizontal overflow; the preferred
variant controls remained visible after theme changes. Vertical scrolling is
intentional for the full Appearance page. The originals were inspected for
legibility, control alignment and focus/selection visibility; they are not pixel
diffs against an unimplemented color-library mock.

Focused offscreen scheme library, existing Settings and Lite window tests passed
3/3. They cover actual ThemeManager selection, in-memory preview cancellation,
keyboard preview/commit/restore, newer external selection, locked writes and the
registered command. This evidence does not claim native acceptance for Pro chat,
terminal or other hosts tracked by the consumer integration task.
