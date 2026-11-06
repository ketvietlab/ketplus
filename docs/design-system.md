# Design system

Proposed extension: [installable themes and VS Code color-theme compatibility](theming.md).

KetPlus adapts the Két Design System (`@ketvietlab/design-system`, now in
`ketsuite/packages/design-system` of ketviet) to a native Qt Widgets and Qt Quick
application. The colours come from its tokens through the Két Qt kit
(`ket::Tokens`, at the ket-qt commit `core/cmake/Dependencies.cmake` pins). The goal is visual continuity
without copying web-specific implementation details.

## Principles

- Dense, operational layout with restrained decoration
- Neutral canvas and panel surfaces with quiet borders
- Két Blue for active and focused states: the `accent` tokens for indicators,
  borders and focus (`color-focus`); `accent-text` for accent-coloured text and
  icons; and `action-primary-bg` with `action-primary-ink` for filled primary
  buttons
- Two-pixel active-tab indicator
- SF Pro for application chrome when present, then bundled Inter, then the
  platform UI font; Agent Workspace and QWidget chrome share that resolver.
  Source/mono text follows the Settings editor font family (Menlo on macOS when
  unset)
- Semantic colors shared by application controls and syntax highlighting

## Implementation

`ThemeManager` builds its light and dark `ThemePalette` from Két DS tokens (a
translucent token is composited over `panel-bg`, so every role is an opaque
`#RRGGBB`) and syntax colours from the kit's `ket::syntaxRecipe`. It applies the
Qt palette and stylesheet, and follows the operating system by default. The selected mode
is persisted through `QSettings` and can be changed from `View > Appearance`.
The shared Settings dialog also owns interface, editor, terminal, and Markdown
preview typography. KetPlus Desktop applies one normalized snapshot to both CM
surfaces and private shell instances, including terminals opened beside Agent
Workspace. Product chrome scales typography and geometry together at Small
(12/14), Medium (1), and Large (16/14). Geometry rounds from Medium dimensions;
zero, one-pixel hairlines and user-resized sidebar widths remain stable. Text
keeps its separate minimum readable size. QWidget product layouts opt into
`chrome_geometry` bindings; QML uses `ChromeMetrics.px` with the same zoom factor.
Both recompute from designed dimensions, including controls created after a
zoom change. Font sizes already resolved by CM are not scaled a second time.

Saving appearance publishes the normalized snapshot to every live desktop window
before theme signals fire, then updates their surfaces even for font-family-only
changes. Open settings forms retain their drafts; unchanged fields merge with the
latest saved snapshot. Chat monospace fonts refresh on appearance updates.
See [ADR 0012](decisions/0012-product-chrome-zoom.md).

The Agent Workspace maps that palette into QML through semantic properties,
rather than defining an independent theme. It follows the KetJS density and
geometry scale: a 216 px navigation rail (the native equivalent of
`--kv-sidebar-width: 13.5rem`), 30–34 px controls, 5–7 px interactive radii,
quiet one-pixel borders, Két Blue active/focus states, SF Pro (then Inter) UI
typography on a collapsed `TypeScale` ladder (micro 10, caption 11, body 12,
reading 14, title 16, heading 22, display 29 at Medium, plus glyphSmall 16 /
glyphLarge 34), and Lucide icons. The chat transcript is constrained to 760 px
and centered when no context panel is open.

Icons are Lucide line icons, 14 px or larger.
Lucide's 2-unit stroke on a 24-unit grid thins with the icon, so strokes never
render thinner than 1.5 logical pixels, like Lucide's `absoluteStrokeWidth`;
icons of 18 px and up keep Lucide's own proportion. Widgets render icons through
`ui/LucideIcon.h` at the size they are shown. QML icons go through
`TintedIcon`, which loads the SVGs in `qrc:/agent/icons` via the `lucide` image
provider to apply the same rule. `TintedIcon` colors by luminance, so those
SVGs are drawn in white (`stroke="#fff"`); `currentColor` resolves to black in
Qt SVG and must not be used.

Agent Workspace has three presentation states:

- Chat only is the default; no editor or terminal context panel is shown.
- Opening a file reveals the existing Scintilla editor in the right half.
- Opening a terminal uses that same contextual right half; it never opens from
  the bottom while Agent Workspace is active.

Each worktree row reports live agent activity, while the transcript, title, and
composer always belong to the selected worktree. Streaming status is expressed
with text and color together, so activity does not depend on color alone. The
selected worktree keeps the Két Blue selection surface whether it is idle or
running. Running rows use a static green secondary status label (`Agent working...`
for worktrees and the current status for conversations), without a path suffix,
shimmer animation, or activity dot. A green dot indicates a successfully completed
turn observed during this app session, not a new idle, stopped, or failed conversation;
it is hidden while running again. Worktree auto-scroll follows the selected
workspace rather than background activity. The composer turns into a stop action
during an active turn.

The QML transcript uses a virtualized `ListView` and the Qt Quick scene graph.
On macOS, KetPlus explicitly selects Metal before the first Quick window is
created. The editor itself remains native Scintilla.

Scintilla receives the same surface, selection, focus, and semantic colors. The
Lexilla themes cover C-family languages, JavaScript/TypeScript, Python, HTML,
JSON, Markdown, and Rust.

The source KetJS tokens remain the visual reference. Native controls may differ
slightly where macOS, Windows, or Linux conventions improve usability.
