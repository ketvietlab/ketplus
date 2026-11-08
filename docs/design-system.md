# Design system

Két Design System is the visual source for KétPlus. Native applications consume
its tokens, component recipes and product profile through the public Két Qt kit
(`ketvietlab/ket-qt`), pinned in `core/cmake/Dependencies.cmake`. The interactive
design reference is `mockups/qt`; the older HTML Atlas is frozen
([ADR 0033](decisions/0033-qt-design-mock.md)). A mock demonstrates a design, not
production implementation or backend support.

## Ownership and source of truth

| Concern | Owner |
| --- | --- |
| Semantic colours, control metrics and shared component drawing | Két Qt tokens, recipes and `ket::Profile` |
| Light, dark, following the system and kit zoom | `ket::Appearance` |
| Shared settings, legacy palette/QSS adapter and zoom mapping | `core/src/ui` and `core/src/app` |
| Pro shell layout and typography adapters | `apps/desktop/src/ui` |
| Agent transcript and conversation composition | `modules/ai` |
| Proposed installable themes | [theming.md](theming.md); not implemented |

Choose a kit component before composing a custom control. Product composition
owns placement and behavior; the kit owns its shared visual mechanics. Changes
needed by Lite and Pro belong in `core/`, with public include paths and exported
`KetPlusCM::*` targets. Never introduce a Pro dependency into core.

## Tokens, profile and style

`ThemeManager` installs `ket::Profile::ketPlus()` before `ket::Style`. The style
renders menus, tooltips, tab bars, select popups, progress bars and sliders; the
legacy application stylesheet leaves those selectors to the kit. A size-only
`QToolTip` rule prevents the base QWidget font rule from overriding the kit's
scaled tooltip recipe. The application font overlay preserves that rule.

Light, dark and system-following mode go through `ket::Appearance`. Existing
`ThemePalette` consumers receive a compatibility projection of Két tokens.
Translucent colours are composited over the panel before entering legacy opaque
QSS/HTML colour fields. Syntax roles come from `ket::syntaxRecipe`, independently
of chrome colours.

Use semantic roles rather than local palette literals:

| Intent | Két tokens |
| --- | --- |
| Window, panel and subdued panel | `page-bg`, `panel-bg`, `panel-bg-subtle` |
| Main, secondary and muted text | `text-main`, `text-secondary`, `text-muted` |
| Dividers and stronger boundaries | `border-default`, `border-strong` |
| Active indicators and focus | `accent`, `color-focus` |
| Accent text and icons | `accent-text` |
| Filled primary action | `action-primary-bg`, `action-primary-ink` |
| Outcomes | `success`, `warning`, `danger` |

A progress ring uses completion semantics; a capacity meter uses pressure
semantics. Do not reuse a capacity warning colour to mean successful completion.
Status text or an icon must accompany colour.

## Fonts and zoom

Both editions use bundled KÉT Sans, then Inter and the platform UI fallback.
New editor/diff settings use KÉT Sans Mono Medium at 14 px; saved font and size
choices are preserved. Empty terminal family settings use the profile's mono
family. The product and mock share the profile overrides for code, conversation
numbers, sidebar weights and the compact composer usage ring.

Small, Medium and Large retain their persisted 12, 14 and 16 pixel values and
geometry factors 12/14, 1 and 16/14. Pro maps legacy values up to 13 to Small and
values from 15 to Large. Lite retains its existing numeric interface-size setting.
The shared mapping is exported from `core/src/ui/InterfaceZoom.h`.

`ThemeManager` publishes interface pixels / 14 to `ket::Appearance` before
updating fonts and styles. Kit Widgets and QML `KetTheme.zoom` therefore receive
the same zoom as the legacy product adapters. `ChromeGeometry` and `TypeScale`
use the kit's rounding function through the shared mapping: zero and hairlines
stay fixed; text keeps its 8 pixel readability floor. The legacy QML
`ChromeMetrics.px` uses the same factor from its token map. Scaled values are
always recomputed from designed dimensions, never scaled a second time.

The old `TypeScale` role ladder and sidebar/transcript text bias remain a
compatibility adapter while those surfaces migrate to kit recipes. Removing
that ladder is still part of #142; it is not a second approved design system.
Editor and terminal text sizes and user-resized sidebar widths stay independent
of interface zoom. Terminal's Bigger/Smaller Text actions change terminal text independently.
Its View → Interface Zoom menu persists 75–200% under
`appearance/interfaceZoom` in the existing Terminal settings domain. Search
controls use kit fields/buttons and follow that zoom.

## Live updates and persistence

Keep the existing `appearance/theme` and `appearance/interfaceFontSizePixels`
keys. A Pro appearance save publishes its normalized snapshot to every window
before synchronous theme signals fire. It reapplies surfaces for font-only
changes too. Unchanged fields in an open Settings draft merge with the latest
saved snapshot; edited fields stay in that draft. Monospace families resolve on
appearance updates rather than per transcript row. See
[ADR 0012](decisions/0012-product-chrome-zoom.md).

Update tokens and properties on existing surfaces. Do not recreate transcript
models, conversations, editors or terminal sessions to apply appearance.
Scintilla/Lexilla use the same surface and syntax roles through their adapters.
The transcript remains a virtualized Qt Quick view; macOS uses Metal.

## Migration and verification

The application still contains legacy QSS and TypeScale consumers. Product glyph
renderers now resolve Két Symbols through the kit mapping (optical masters and
light weight). Legacy QML resource URLs are compatibility identifiers; the
SymbolImageProvider renders kit symbols, not the old SVG paths.
The mock uses Két kit components and symbols; a design capture is not evidence
that every production control has migrated. Terminal defaults and Mermaid configuration now resolve semantic tokens.
Mermaid resources retain diagram/security settings and token placeholders; the
renderer resolves colours (including alpha compositing) before invoking Mermaid.
Legacy per-surface typography remains to be migrated with its UI issue.

For shared theme/zoom changes, run the Lite and Pro suites from
[testing.md](testing.md), including desktop zoom, settings and font tests.
Check repeated zoom transitions, theme switching, newly created controls,
multiple windows, inherited fonts and shown tooltips. Verify native light/dark
captures in addition to model assertions. The standalone mock has its own build
and smoke suite in `mockups/qt/README.md`; passing it does not test the product.

For an offscreen Lite review capture, set `KETPLUS_CAPTURE=<file>` and optionally
`KETPLUS_CAPTURE_THEME=light|dark`. Such captures verify rendering, not installed
bundle signing, deployment or device behavior.
