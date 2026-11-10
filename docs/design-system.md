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

Both editions use bundled KÉT Sans, then the platform UI fallback. The Inter
download, Qt resources and product license entry have been removed.
New editor/diff settings use KÉT Sans Mono Medium at 14 px; saved font and size
choices are preserved. Empty terminal family settings use the profile's mono
family. The product and mock share the profile overrides for code, conversation
numbers, sidebar weights and the compact composer usage ring.

Small, Medium and Large retain their persisted 12, 14 and 16 pixel values and
geometry factors 12/14, 1 and 16/14. Both editions retain numeric Settings values
from 8 to 24; a named shortcut only changes the value when selected.
The shared mapping is exported from `core/src/ui/InterfaceZoom.h`.

`ThemeManager` applies application fonts and styles before publishing interface
pixels / 14 to `ket::Appearance`, so Qt repolish cannot overwrite kit recipe fonts. Kit Widgets and QML `KetTheme.zoom` therefore receive
the same zoom as the legacy product adapters. `ChromeGeometry` and `TypeScale`
use the kit's rounding function through the shared mapping: zero and hairlines
stay fixed; the legacy type adapter keeps its 8 pixel readability floor.
Agent Workspace QML reads `KetTheme.t`, `KetTheme.fontFamily` and kit component
recipe text sizes directly, and calls `KetTheme.px(value, KetTheme.zoom)`.
Bundled fonts register before the first QML component reads the kit's constant
font-family properties, including standalone and Atlas hosts.
There is no host palette/type map or per-component token injection. The host
exposes the persisted editor mono family separately; transcript rich-text
formatting reads the same `ket::Appearance` theme and zoom as QML. Scaled values are
always recomputed from designed dimensions, never scaled a second time.

`TypeScale` compatibility names resolve the current kit profile roles; they no
longer own a separate type ladder or add a sidebar/transcript text bias.
Editor and terminal text sizes and user-resized sidebar widths stay independent
of interface zoom. Terminal's Bigger/Smaller Text actions change terminal text independently.
Its View → Interface Zoom menu persists 75–200% under
`appearance/interfaceZoom` in the existing Terminal settings domain. Search
controls use kit fields/buttons and follow that zoom.

## Settings composition

Appearance and Typography are separate sections. Theme choices use `ket::ThemeCard`
directly, shared by the Lite dialog and Pro workspace. Pro reparents the same
controls and retains the shared staged Save, Cancel and Reset behavior.

Typography uses `ket::Select`, with a small `PixelSelect` adapter for bounded
integer preferences. Every supported integer, including saved custom sizes,
remains selectable; line heights clamp to their font size. The font menu loads
fixed-pitch families on interaction and preserves a saved custom family.
The registered code-font family is `KET Sans Mono`. The old display spelling
`KÉT Sans Mono` normalizes to that family; unrelated user choices are preserved.

## Live updates and persistence

Keep the existing `appearance/theme` and `appearance/interfaceFontSizePixels`
keys; `appearance/interfaceZoomPercent` holds the separate interface zoom, and a
profile without it migrates from the interface pixels (ADR 0104). A Pro appearance save publishes its normalized snapshot to every window
before synchronous theme signals fire. It reapplies surfaces for font-only
changes too. Settings applies each change as it is made (ADR 0103); fields the
open dialog did not edit merge with the latest saved snapshot. Monospace families resolve on
appearance updates rather than per transcript row. See
[ADR 0012](decisions/0012-product-chrome-zoom.md).

Update tokens and properties on existing surfaces. Do not recreate transcript
models, conversations, editors or terminal sessions to apply appearance.
Scintilla/Lexilla use the same surface and syntax roles through their adapters.
The transcript remains a virtualized Qt Quick view; macOS uses Metal.

## Migration and verification

The application still contains legacy QSS and TypeScale consumers. Product glyph
renderers now resolve Két Symbols through the kit mapping (optical masters and
light weight). Agent Workspace uses `KetIcon` directly; legacy icon identifiers
from activity/provider data are converted to symbol names at their consuming boundary.
Pro links the static Ket QML module/plugin and its resources for offline loading.
Tooltips use the kit's `KetToolTip` without a local type shadowing it. Working and
history-loading indicators use the kit clock, releasing holds when hidden,
minimized, restoring history or when reduced motion is enabled. The kit clock keeps
20 Hz behind another application too: a visible window there is still watched, and the
earlier 4 Hz made spinners stutter.
Transcript premeasurement stops after all measurable bodies are exhausted;
resource-backed bodies wait for their delegates instead of keeping a polling timer alive.
New rows and layout changes re-arm measurement. Background-task elapsed time and
held-message status polling stop while their window is hidden or minimized.
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

## Conversation composer composition

The Pro composer is split from the workspace/session controller. Its card,
attachments, text line box and send/stop action consume the kit recipes; footer
buttons, header actions, model search and the usage ring use kit controls. The
product retains its input document to preserve IME handling, clipboard image
paste, attachment-only submissions, draft persistence and steering acknowledgement
semantics. A caret-aligned placeholder follows document formatting without
saving a draft on formatting-only changes. Empty running drafts show Stop;
nonempty drafts or attachments show Send guidance in the same action position.
Model/permission selection still uses the existing revisioned configuration path.
Header tags dispatch a filter to the native sidebar; unavailable metadata is not
invented. Automatic reviewer orchestration remains a separate #120 dependency.

## Transcript composition

The virtualized message TextEdit keeps its document, selection, streaming and reading
anchor contracts. Its formatter consumes the kit Markdown and CodeBlock recipes;
it does not replace the document with a second Markdown component. Fenced code
uses the kit syntax scanner through a document highlighter (including multiline
comment state). Reformatting and numeric zoom replace applied padding rather than
accumulating it. File links, raw HTML escaping, original-source copy and Mermaid
source/diagram geometry remain product responsibilities.

User bubbles use UserMessageRecipe. Attachment previews are 120 × 78 logical
pixels; generated images use a 360 × 225 preview, with fixed aspect geometry
before decoding. A user's thumbnails sit above the bubble, right-aligned. Clicking
one opens KetLightbox on the row's images with the mock's tools: zoom, rotate,
copy, download, print and fullscreen. Captions are file names. The host copies
the turned image, saves the original file to Downloads, prints through
`ket::printImage` and turns the top-level window fullscreen. A Mermaid diagram
opens in the same viewer, captioned "Mermaid diagram", as in the mock. The viewer
is dark in both themes, so it shows the dark rendering; in a light theme it is
requested on open, and until it is ready the light one sits on a white ground; an
open viewer takes the dark rendering once it is drawn. The viewer gets a raster at a
whole multiple of the diagram's size, named `@Nx`, so it reads 100% at the
diagram's own size and stays sharp when zoomed. Rasters stop at 8192 px; a larger
diagram is scaled down to that, and its 100% is the cap's size. Lite's Markdown
preview keeps its vector dialog. Day dividers use the journal's `sent_at` carried
as presentation `timestamp` on user messages, including live echoes. Missing or
invalid timestamps do not create invented dates. Labels read "Today",
"Yesterday" or "Thursday, 24 September" (plus the year when it differs), use the
viewer's local date and remain part of the row's measured height.

Activity headers use KetActivityRow with product content for file-link versus
expansion hit targets and multiline command summaries. Group headers consume
ActivityGroupRecipe while the message model retains virtualized child ownership.
ChangedFilesRecipe composes path/rename/removed-file context actions; the generic
card cannot replace those identities. Detail bodies remain loaded on expansion.

System notices use a shared plain-text, selectable presentation. Known journal
provider/model/context/fork/rename events and turn-end receipts share live/replay
projection and stable message keys. A turn end reads as the mock's quiet line: a
status icon, a bold "Completed", "Stopped" or "Failed", then the details, for
example "6m 20s · 141k context · $2.08 session". The duration runs from the
turn's first journal time to its `turn.completed`. A live receipt uses the
turn's start instead. The context is the latest `usage.context`. The cost is
labelled "session" because Claude SDK accounting is cumulative. A part without
evidence on the page is left out. A turn ended by a client's Stop (desktop, phone or
cloud `session.interrupt`) carries `reason: "user"` on its interrupted
`turn.completed`, and reads "… · stopped by you"; other interrupted ends (daemon
restart, fork snapshot) stay neutral. Retryable errors show reconnecting without changing the running lifecycle.
Older retained-output/diff/text markers stay visible. Unsupported queue actions
are not shown; queued status follows the daemon state.

Pro Settings keeps its workspace presentation and uses kit navigation, theme
cards, accent swatch, segmented choices and form controls. The workspace reuses
the core dialog's Restore defaults footer and Follow system control; every change
applies at once (ADR 0103). Appearance has the mock's Interface zoom
(90/100/110/125%) and Typography the Interface font size (13–16 px); the zoom
scales the chrome and the size sets the application text at that zoom
(ADR 0104). Storage, Browser and Computer
preferences retain their existing immediate-save contracts. The sidebar Storage
action selects that section without rebuilding an already open settings page.
Provider status and Claude usage remain authoritative runtime data; unavailable
limits or credentials are never rendered as configured mock state.

Atlas library and conversation-detail actions use kit buttons, fields and badges.
Card colors resolve semantic tokens and the real manifest supplies screen/flow
counts. Builder chat is a docked right pane: hiding it expands the existing Atlas
WebView; showing it reserves space rather than covering the map. The compact chat
keeps its conversation and draft, and Back navigates to that conversation. The
Atlas WebView's flows, map and preview implementation remain unchanged.

Legacy symbol strings in transcript delegates are migration names resolved by
`KetTheme.iconForLucide`; they do not refer to bundled SVG files. Product Lucide
assets and the unused image provider have been removed. `ChromeMetrics.js` is a
compatibility forwarding call to `KetTheme.px`, not a second metric table. The
unused local motion-clock wrapper has been removed; running indicators hold the
kit's gated shared clock directly.

Lite's status bar composes `ket::StatusBar` and `StatusItem` inside a small
QStatusBar host. Existing message timeouts, format menus and visibility APIs stay
intact while the kit paints/scales the bar. Settings list delegates are transparent
beneath their kit NavItems, preventing duplicate labels in unselected rows.

Application font and palette defaults are set on QApplication, not a global
QWidget stylesheet rule: such a rule overrides fonts/colors set by kit labels.
ThemeManager scales the remaining compatibility QSS text once before expanding
tokens; InterfaceUiFont no longer rewrites or rescales that stylesheet. Product
composition titles use `{{px:N}}` inside TypeScale::apply for explicit design
sizes that still follow the shared zoom.

ThemeManager completes application-font/QSS propagation before notifying kit
zoom observers, and resolves theme QSS before component theme observers. It
avoids repolishing an unchanged stylesheet. This preserves EmptyState recipe
font size/weight through preview/cancel, light/dark changes and numeric zoom.
