# KetPlus color schemes

The canonical, application-independent format is UTF-8 JSON with the extension
`.ketscheme.json`. VS Code and JetBrains importers convert their supported colors
and syntax styles into this format; they never become runtime theme engines.
Schema version 1 and its C++ contract live in `core/src/ui/theme/ColorScheme.h`.
The machine-readable schema is `core/resources/color-schemes/schema-v1.json`.

## Minimal scheme

```json
{
  "schemaVersion": 1,
  "id": "my-dark-scheme",
  "name": "My dark scheme",
  "kind": "dark",
  "extends": "builtin-dark",
  "editor": { "bg": "#101820", "fg": "#E0E8F0" },
  "syntax": { "keyword": { "fg": "#AB8FFF", "bold": true } },
  "metadata": { "sourceApp": "VS Code", "sourceFile": "theme.json", "license": "MIT" }
}
```

Required headers are `schemaVersion` (integer 1), `id`, `name`, `kind` and
`extends`. IDs are 1–128 lowercase ASCII letters, digits, `.`, `_`, or `-`,
beginning with a letter or digit. User IDs cannot begin with `builtin-`. Names
are nonempty display labels and never identity. `kind` is `light`, `dark`, or
`highContrast`. High contrast remains distinct from dark and explicitly chooses
one of the two bases. `extends` is exactly `builtin-light` or `builtin-dark`;
there are no user-scheme inheritance chains or filesystem dependencies.

All color groups and their entries are optional. Hex colors accept exactly
`#RGB`, `#RRGGBB`, or `#RRGGBBAA`, case-insensitively. Eight digits mean RGBA:
`#12345680` is RGB (18,52,86) with alpha 128. Named colors, `#RGBA`, JSONC,
comments and trailing commas are rejected. The writer normalizes known colors to
uppercase six or eight digits and preserves alpha. Input is limited to 5 MiB.

## Tokens

`ui` contains these objects. The C++ map flattens them to `group.token`, for
example `scheme.ui["sidebar.bg"]`.

| UI group | Color keys |
| --- | --- |
| chrome | appBackground, pageBackground, panelBackground, panelSubtle, surfaceRaised, surfaceHover, textMain, textSecondary, textMuted, textDisabled, border, borderStrong, accent, accentHover, accentActive, accentSubtle, accentMuted, accentText, positive, warning, danger, info |
| sidebar | bg, fg, border |
| input | bg, fg, border |
| list | hover, selection, selectionFg, fg |
| tab | activeFg, activeBg, inactiveFg, inactiveBg |
| focus | border |
| button | bg, fg, hover, active |

Other groups use a flat object and flat C++ map:

| Group | Keys |
| --- | --- |
| editor | bg, fg, selection, selectionFg, caret, lineHighlight, gutterBg, gutterFg |
| terminal | bg, fg, cursor, selection, ansi0 through ansi15 |
| diff | added, removed |

`syntax` maps `keyword`, `string`, `number`, `type`, `comment`, `attribute`,
`function`, `variable`, `constant`, and `operator` to objects with optional `fg`,
`bold`, `italic`, `underline`. Style flags must be booleans: explicit `false`
resets an inherited style, while omission inherits it. `metadata` accepts
`sourceApp`, `sourceFile`, and `license` strings plus arbitrary importer data.
Only record a source file's basename or package-relative path; never publish
machine-specific paths. Declared license metadata is not a verified license grant.

## Reading, validation and resolution

`ColorSchemeReader::read(bytes)` returns a sparse `ColorScheme`, structured
`SchemeDiagnostic` entries (`severity`, JSON key `path`, `message`), and `ok()`.
Invalid required headers, known value types, colors and versions produce errors.
Unknown keys produce warnings and survive read/write/read at every nesting level,
including unknown syntax roles and importer metadata. `preserved` owns these
unknown values; importers should put supported values into the typed maps and
unmapped source data into metadata or preserved extension objects. Unknown JSON
values are inert data. Formatting and original hex spelling are not preserved.

`ColorSchemeWriter::write(scheme, diagnostics)` serializes sparse values. It
returns empty bytes and structured errors for unknown typed-map keys; extensions
belong in `preserved` JSON, where round trips keep them without changing ownership.
Callers construct valid typed maps using documented keys. Validation is performed by reading the
serialized bytes, as the catalog does before writing. Read/write/read of a valid
scheme compares equal using `ColorScheme::operator==`.

`resolveColorScheme(scheme)` starts from the selected embedded base and overlays
only authored values. It returns every documented color and every syntax style
property, without modifying the original sparse definition. Invalid in-memory
colors cannot clear defaults. Missing syntax foregrounds and flags inherit the
same role from the base. New roles added in later versions can receive defaults
without rewriting installed files. `colorSchemePalette` is the temporary RGB
compatibility projection; component consumers should use the resolved QColor
values for alpha support. The legacy projection composites alpha over the resolved panel and emits opaque
hex RGB. A translucent panel is first composited over its embedded base panel.

The embedded `light.ketscheme.json` and `dark.ketscheme.json` capture the current
Két DS built-in palette, including its composited chrome borders. Their MIT
notice and source version/hash are embedded alongside them. A parity test
compares every legacy `ThemePalette` field with the actual `ThemeManager` palette.
This task introduces the data contract; ThemeManager selection and component
adapters integrate it separately.

## VS Code import

`KetPlusCM::vscode_import` exports `ui/theme/vscode/VsCodeImporter.h`.
`importFile(path, rootDirectory)` accepts a local JSON/JSONC theme or `.vsix`;
`importVsix(path)` reads `extension/package.json` and its `contributes.themes`.
The result contains per-variant `ColorScheme`/`ImportReport` values and an aggregate
report. `ok()` requires variants and no error diagnostics; successful variants
may remain inspectable if another variant failed. Callers must check `ok()` before
installing a package. The importer only converts data, and each returned variant
passes canonical write/read validation, including copied manifest license data.
Use `ColorSchemeWriter::write` to produce `.ketscheme.json`.

Includes are loaded through the common root-bound `IncludeResolver` with cycle
checks and a maximum of 16 levels. Keys in a child `colors` object override parent
keys; explicit null removes an inherited color. Invalid values are reported and
leave the prior/default color. Token rules retain parent-then-child order, with
later explicit properties overriding the same shared role. Relative `tokenColors`
strings resolve from the file that declared them and read an XML `.tmTheme`
plist, including standard Apple DOCTYPE headers without external DTD resolution.
The optional source root defaults to the selected file's directory. Directory and
archive root escapes, network references and symlinks remain forbidden by common
I/O. Repeated variant/include/tmTheme reads share a 100 MiB cumulative budget.
`IncludeResolver::loadJsonChain` accepts an optional remaining-byte budget for
this purpose; it is consumed across calls, including unsuccessful parses.

`VsCodeMapping.h` defines ordered workbench fallback chains. For each target the
first authored, valid source color wins; absent chains inherit the KetPlus base.
For example:

| Target | Source IDs, highest priority first |
| --- | --- |
| `editor.fg` | editor.foreground → foreground |
| `sidebar.bg` | sideBar.background → editor.background |
| `input.fg` | input.foreground → foreground → editor.foreground |
| `list.selection` | list.activeSelectionBackground → list.inactiveSelectionBackground → editor.selectionBackground |
| `tab.activeFg` | tab.activeForeground → foreground → editor.foreground |
| `terminal.bg` / `fg` | terminal.background/foreground → editor.background/foreground |
| `terminal.cursor` | terminalCursor.foreground → editorCursor.foreground → terminal.foreground |
| `diff.added` / `removed` | diffEditor.inserted/removedTextBackground → inserted/removedLineBackground |

The table also covers chrome, sidebar borders, inputs, lists, tabs, focus,
buttons, editor selection/caret/gutter and all 16 `terminal.ansi*` colors. ANSI
Black through White map to `ansi0`–`ansi7`; BrightBlack through BrightWhite map to
`ansi8`–`ansi15`. Missing colors retain the selected base. Hex forms `#RGB`,
`#RGBA`, `#RRGGBB` and `#RRGGBBAA` are accepted and normalized to canonical RGBA.
Fallback selections are reported as approximations; unsupported IDs are listed
as skipped. An available but superseded chain candidate is reported separately.

Supported TextMate selectors are single dotted scope atoms matching these prefixes
(and dotted suffixes). More specific prefixes are checked first:

| Prefix | Shared role |
| --- | --- |
| keyword.operator | operator |
| constant.numeric | number |
| entity.name.type, entity.name.class, support.type, support.class, storage.type | type |
| entity.name.function, support.function | function |
| entity.other.attribute-name, meta.annotation | attribute |
| keyword, storage.modifier | keyword |
| string, comment, variable, constant | same-named role |

Comma alternatives and arrays of strings are supported. Whitespace is allowed
around an atom, but selectors containing ancestry/context, exclusions, unions,
parentheses, wildcards or unrecognized scope families are skipped and listed.
No TextMate grammar, extension JavaScript, semantic provider or scope stack runs.
Scope-to-role conversion is always an approximation because Lexilla shares a
small set of roles across languages. Later rules win per explicitly authored
property, rather than reproducing TextMate specificity ranking.

`fontStyle` supports bold, italic and underline. An empty string explicitly resets
all three flags; omitted style properties inherit earlier rules/base values.
Strikethrough, syntax backgrounds and other settings are reported as skipped.
Unscoped rule colors are converted to workbench IDs before fallback resolution.
Foreground/background, caret, selection, lineHighlight, gutterForeground and ANSI
colors supply missing workbench declarations, including sidebar/terminal/gutter
fallbacks. Later global rules override earlier globals; explicit workbench values
(including null resets) take precedence. Unsupported global colors are listed as
unmapped colors, and global typography is skipped.
`semanticTokenColors` is merged parent-first into inert
`metadata.vscodeSemanticTokenColors`, and semanticHighlighting is retained as
metadata only; neither changes applied syntax styles.

Manifest `uiTheme` (`vs-dark`, `vs`, `hc-black`, `hc-light`) takes precedence over
JSON type. Standalone light/dark/hcDark/hcLight declarations are supported;
otherwise opaque background brightness determines kind, with dark as the final
fallback. High contrast remains a distinct kind with an explicit light/dark base.
UI localization, executable extension behavior and unsupported theme properties
are omitted with reasons. Each report has at most 4,096 details plus a truncation
notice. Declared license metadata is copied without verifying a license grant.

Original synthetic MPL-2.0 fixtures and their provenance are under
`core/tests/fixtures/vscode`. Format references are the official
[color-theme guide](https://code.visualstudio.com/api/extension-guides/color-theme),
[color ID reference](https://code.visualstudio.com/api/references/theme-color), and
[syntax-highlighting guide](https://code.visualstudio.com/api/language-extensions/syntax-highlight-guide).

## Catalog

`ColorSchemeCatalog` defaults to `QStandardPaths::AppDataLocation/color-schemes/`.
An explicit directory supports isolated tests. Each user scheme is stored as
`<id>.ketscheme.json`; no separately mutable index is required. Built-ins are
always available and read-only. Listing skips corrupt, symlinked, invalid or
identity-mismatched files. Loading a missing, corrupt or invalid ID returns a
built-in fallback, diagnostics, and `success == false`; startup can continue.

`add` validates before writing, generates a persistent UUID for an empty ID, and
refuses to overwrite existing IDs. `duplicate` copies all data, accepts a new
name and generates a new UUID. `remove` only removes a valid user ID. Mutations
acquire `QLockFile` without blocking, use `QSaveFile` for atomic publication, and
report lock/write errors without replacing an existing scheme. Existing files
are immutable through this API; editing publishes a new scheme ID.

`ImportReport` provides ordered `items` with `Mapped`, `Approximated` or `Skipped`
dispositions, source and target key paths, and a reason, plus structured
diagnostics. Importers must explain approximations and omissions. Full TextMate
scope matching, semantic highlighting and extension execution are outside this
format's guarantees.

## Verification

Build `ketplus_color_scheme_tests` and run it with `QT_QPA_PLATFORM=offscreen`.
It covers alpha parsing, unknown-data round trips, diagnostics, inheritance and
explicit false flags, built-in palette parity, catalog reopen/identity/duplicate/
remove/fallback, lock contention and failed writes. No provider is invoked.
