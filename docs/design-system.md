# Design system

KetPlus adapts the
[KetJS design system](https://github.com/ketvietlab/ketjs/tree/develop/packages/design-system)
to a native Qt Widgets application. The goal is visual continuity without
copying web-specific implementation details.

## Principles

- Dense, operational layout with restrained decoration
- Neutral canvas and panel surfaces with quiet borders
- Indigo for active and focused states
- Two-pixel active-tab indicator
- Inter for application chrome and the platform monospace font for source text
- Semantic colors shared by application controls and syntax highlighting

## Implementation

`ThemeManager` owns the semantic light and dark palettes, applies the Qt palette
and stylesheet, and follows the operating system by default. The selected mode
is persisted through `QSettings` and can be changed from `View > Appearance`.

Scintilla receives the same surface, selection, focus, and semantic colors. The
Lexilla themes cover C-family languages, JavaScript/TypeScript, Python, HTML,
JSON, Markdown, and Rust.

The source KetJS tokens remain the visual reference. Native controls may differ
slightly where macOS, Windows, or Linux conventions improve usability.
