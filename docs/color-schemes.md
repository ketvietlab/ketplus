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
beginning with a letter or digit. User IDs cannot begin with `builtin-` or `builtin.`. Names
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

## JetBrains import

`KetPlusCM::jetbrains_import` exports `ui/theme/import/JetBrainsImporter.h`.
`JetBrainsImporter::importFile(path, rootDirectory)` converts `.icls`, editor
scheme `.xml`, `.theme.json`, or a local plugin `.jar`; `importArchive(path)`
handles the JAR explicitly. Conversion returns `JetBrainsImportResult` containing
one `ColorScheme` and `ImportReport` per variant, plus an aggregate report.
`ok()` requires at least one variant and no error diagnostic. Valid variants may
remain available for inspection when another variant failed; callers must check
`ok()` before treating a package as successfully imported. Conversion does not
install, activate, download, or execute anything. Serialize a variant with
`ColorSchemeWriter::write()` to obtain `.ketscheme.json`; every returned variant
has already passed a canonical write/read validation.

A standalone file defaults to its own directory as its dependency boundary. A
caller may explicitly select a larger source root. Relative `editorScheme`
references resolve against the declaring theme. JetBrains classpath-style names
such as `/editor/Dark.xml` resolve within the selected source/archive root, never
the host filesystem root. Network/UNC names, drive paths, traversal outside that
root and filesystem symlinks remain forbidden. JAR input uses the bounded ZIP
reader described in [theming.md](theming.md). It scans `.theme.json`, `.icls` and
XML with a `<scheme>` root. An editor resource referenced by a UI theme is folded
into that variant rather than exposed again as a duplicate standalone variant.
Other XML and plugin resources are reported as skipped. No Java/plugin code or
extension registration is loaded. Across the import, cumulative reads are capped
at 100 MiB, including repeated editor resource reads.

The mapping tables are explicit in `JetBrainsMapping.h`:

| JetBrains source | KetPlus target |
| --- | --- |
| `TEXT` / `DEFAULT_TEXT` foreground/background | `editor.fg`, `editor.bg` |
| `CARET_COLOR`, `CARET_ROW_COLOR` | `editor.caret`, `editor.lineHighlight` |
| `GUTTER_BACKGROUND`, `LINE_NUMBERS_COLOR` | `editor.gutterBg`, `editor.gutterFg` |
| `SELECTION_BACKGROUND`, `SELECTION_FOREGROUND` | `editor.selection`, `editor.selectionFg` |
| `DEFAULT_KEYWORD`, `DEFAULT_STRING`, `DEFAULT_NUMBER` | syntax `keyword`, `string`, `number` |
| `DEFAULT_LINE_COMMENT`, block/doc comments | shared syntax `comment` |
| `DEFAULT_FUNCTION_DECLARATION`, calls/methods | shared syntax `function` |
| default class/interface, metadata, variable/parameter/field, constant, operation keys | syntax `type`, `attribute`, `variable`, `constant`, `operator` |
| `CONSOLE_*_OUTPUT` ANSI colors | `terminal.ansi0` through `ansi15` |
| normal/error/system console foreground, console background | `terminal.fg`, `ansi1`, `ansi8`, `bg` |
| `DIFF_INSERTED` / `DIFF_DELETED` background | `diff.added`, `diff.removed` |
| selected Panel/Label/ToolWindow/Tree/List/TextField/EditorTabs/Button UI keys | nearest chrome/sidebar/list/input/tab/button token |

XML accepts unpadded RGB numbers (for example `0` is black), six-digit RGB and
eight-digit RGBA; JSON UI colors accept six/eight-digit hex and named entries in
`colors`. Nested UI objects are flattened to dotted keys. Named aliases detect
cycles and have a 16-level limit; nested UI traversal has a 32-level limit.
Invalid/unsupported color values retain the KetPlus fallback with a skipped
report entry. Missing data stays sparse and inherits the selected KetPlus base.

Compatibility limits are deliberate and appear in `ImportReport`:

- IDE `parent_scheme` data (including Default/Darcula) is unavailable. The importer
  chooses an approximate KetPlus base from an opaque editor background's weighted
  brightness (threshold 128), otherwise a Darcula/dark parent name or light. A UI
  theme's explicit `dark` boolean takes precedence, with a report entry on a
  disagreement. Other IDE parent names do not import their original palettes.
- Java/Kotlin keys map only through a short explicit alias table into shared
  roles. Generic DEFAULT roles take precedence. Other language-specific keys and
  `baseAttributes` inheritance without an authored value are skipped. Several IDE
  attributes can share one KetPlus role. Its primary DEFAULT key wins regardless
  of XML order: FUNCTION_DECLARATION for `function`, LOCAL_VARIABLE for `variable`,
  CLASS_NAME for `type`, LINE_COMMENT for `comment`, and the matching KEYWORD,
  STRING, NUMBER, METADATA, CONSTANT or OPERATION_SIGN key for the other roles.
  Secondary shared keys fill only a role with no authored primary value; language
  aliases are the final fallback. Among aliases in the same tier, the first
  authored value wins. Omitted collisions and fallback approximations are reported.
  A primary with only `baseAttributes` and no value block cannot supply a role.
- `FONT_TYPE` bits 1/2 map to bold/italic. Authored value blocks default to plain
  flags, including explicit false resets. Font family and size are ignored so
  user typography stays in control. Syntax backgrounds have no shared role slot.
- Numeric effect 1 maps to a plain underline; wave/bold/dotted effects 2/4/5 are
  approximated by it. Underline effects require a valid authored EFFECT_COLOR,
  regardless of option order; missing/invalid effect colors retain underline=false.
  Border, strikeout and unknown effects are skipped; effect
  colors and geometry are omitted. The importer does not reproduce IDE rendering.
- Only UI keys with corresponding KetPlus components are applied. Wildcards,
  platform rules, icon substitution, background images, fonts, metrics and plugin
  behavior are skipped. Named palettes do not create tokens by themselves.
- Standard external XML DOCTYPE identifiers are accepted without fetching DTDs.
  Internal DTD subsets, entity declarations, unresolved entity references and
  unsafe archives are rejected by the common I/O layer.
  Each report is capped at 4,096 detail items plus an explicit truncation notice.
  No original XML/JSON formatting or executable plugin behavior is retained.

The fixtures in `core/tests/fixtures/jetbrains` are original synthetic MPL-2.0
fixtures, with provenance in their README. Format behavior was checked against
JetBrains' [theme customization guide](https://plugins.jetbrains.com/docs/intellij/themes-customize.html),
[editor-scheme guide](https://plugins.jetbrains.com/docs/intellij/themes-extras.html),
and [attribute serialization at a pinned upstream revision](https://github.com/JetBrains/intellij-community/blob/09f03735d4674eea9f112165fe45744a25fbdd56/platform/core-api/src/com/intellij/openapi/editor/markup/AttributesFlyweightSerDe.java).
No upstream source or theme palette is copied into the fixtures.

## Bundled schemes

The catalog embeds ten read-only schemes. The original Két Light and Két Dark
(displayed as KétPlus Light/Dark) stay first and remain the defaults. Their
persisted IDs `builtin-light` and `builtin-dark` remain compatible with existing
settings and inheritance. The eight additions use stable `builtin.<slug>` IDs.
Both `builtin-` and `builtin.` namespaces are reserved: files in the user directory
cannot shadow them. Older manually authored `builtin.*` files remain untouched
on disk; reimport them under a generated user ID to make them visible. Duplicate creates an independently named user ID; add/remove
cannot overwrite or delete an embedded scheme.

All eight additions are generated by the production VS Code/JetBrains importers.
`metadata.sourceApp` records the input format and `metadata.sourceLabel` records
VS Code, JetBrains, or Community; `author`, `license`, and `upstream` retain credits,
repository commits, source URLs and SHA-256 values. Selection/application depends
on the shared ThemeManager adapter; this data change does not certify every app
surface. Full reports are in [theme-import-reports](theme-import-reports/).
Counts below are mapped / approximated / skipped. Contrast uses the unmodified
opaque editor foreground/background and WCAG relative luminance.

| Theme | ID | Kind | Source label | Pinned source and attribution | License | Import counts | Editor contrast |
| --- | --- | --- | --- | --- | --- | --- | --- |
| VS Code Dark+ | `builtin.vscode-dark-plus` | dark | VS Code | [microsoft/vscode](https://github.com/microsoft/vscode/tree/1b509615d9f84db862313485737c05e8b81d2977) · Microsoft Corporation; Solarized palette by Ethan Schoonover | MIT | 5 / 61 / 115 | 11.25:1 |
| VS Code Light+ | `builtin.vscode-light-plus` | light | VS Code | [microsoft/vscode](https://github.com/microsoft/vscode/tree/1b509615d9f84db862313485737c05e8b81d2977) · Microsoft Corporation; Solarized palette by Ethan Schoonover | MIT | 6 / 61 / 125 | 21.00:1 |
| Darcula | `builtin.darcula` | dark | JetBrains | [JetBrains/intellij-community](https://github.com/JetBrains/intellij-community/tree/417e214ea024cd783179244f51329df01277bb93) · JetBrains s.r.o. | Apache-2.0 | 41 / 12 / 1341 | 6.93:1 |
| IntelliJ Light | `builtin.intellij-light` | light | JetBrains | [JetBrains/intellij-community](https://github.com/JetBrains/intellij-community/tree/417e214ea024cd783179244f51329df01277bb93) · JetBrains s.r.o. | Apache-2.0 | 18 / 5 / 164 | 20.03:1 |
| Dracula | `builtin.dracula` | dark | Community | [dracula/visual-studio-code](https://github.com/dracula/visual-studio-code/tree/9726e1f6b51a4382334bc36e03128fcf02c3fb45) · Dracula Theme | MIT | 47 / 53 / 334 | 13.36:1 |
| One Dark Pro | `builtin.one-dark-pro` | dark | Community | [Binaryify/OneDark-Pro](https://github.com/Binaryify/OneDark-Pro/tree/75c80ab18d90655e05ad837bb8d89f5e68d5ce53) · Binaryify | MIT | 43 / 144 / 461 | 6.57:1 |
| Nord | `builtin.nord` | dark | Community | [nordtheme/visual-studio-code](https://github.com/nordtheme/visual-studio-code/tree/8ead09822c02d0d49d0f764104505e5a34d3689f) · Arctic Ice Studio / Sven Greb | MIT | 52 / 32 / 425 | 9.25:1 |
| Solarized Dark | `builtin.solarized-dark` | dark | VS Code | [microsoft/vscode](https://github.com/microsoft/vscode/tree/1b509615d9f84db862313485737c05e8b81d2977) · Microsoft Corporation; Solarized palette by Ethan Schoonover | MIT | 36 / 35 / 106 | 4.75:1 |

The Két originals use the existing Két Design System MIT snapshots; their
foreground/background contrast is 15.15:1 (light) and 14.53:1 (dark), measured by
the same offscreen test. No upstream palette was recolored. A future snapshot
below 4.5:1 must retain its colors, record `metadata.contrastWarning` and emit a
visible test warning rather than silently claiming accessibility compliance.

Input preparation is limited to extracting Darcula's `<scheme>` from JetBrains'
combined `DefaultColorSchemesManager.xml` and wiring explicit `editorScheme`
resource paths into the two UI JSON files. These steps are recorded in
`metadata.manualAdjustments`. No imported colors or styles are hand-edited.
The importer still approximates IDE parent schemes, UI inheritance, unsupported
attributes/effects, TextMate contexts and omitted workbench keys as documented
above; a bundled name does not promise identical IDE rendering. Darcula has many
omissions because its UI JSON includes component keys without KetPlus equivalents.

JetBrains is pinned to the Apache-2.0 snapshot from 2024, not current IDE-build
terms. Dracula uses the official v2.24.3 VSIX release, matched to its tag commit
and locked by artifact SHA-256; only its Dracula variant is emitted. Neither the
VSIX nor complete upstream extension/plugin trees are committed or executed.
Solarized's palette is credited to Ethan Schoonover through Microsoft's MIT port;
the original palette's MIT notice is also shipped and pinned in `upstreamCredits`.

To reproduce (Python 3, curl and the normal Lite Qt build requirements):

```sh
cmake --preset lite-dev
cmake --build --preset lite-dev --target ketplus_generate_builtin_schemes
python3 core/tools/color-schemes/regenerate.py \
  --generator build/lite-dev/core/ketplus_generate_builtin_schemes --check
```

The script downloads only `core/tools/color-schemes/sources.lock.json` inputs,
verifies every SHA-256 before conversion and compares generated scheme/report
bytes plus both installed license-text copies. Omit `--check` to regenerate;
`--offline --cache <directory>` uses an already verified cache. No timestamps or
machine paths enter the output. Network access is explicit in this developer
command; normal catalog loads and unit tests stay offline. The generation tool
is available only with `KETPLUS_CM_BUILD_TESTS=ON` and is not installed.

`ketplus_builtin_scheme_tests` checks ordering/defaults, kind, source labels,
round trips, all resolved color/style tokens, upstream editor color samples,
contrast, report counts, reserved-ID collisions, and duplicate/remove behavior.

## Verification

Build `ketplus_color_scheme_tests` and run it with `QT_QPA_PLATFORM=offscreen`.
`ketplus_jetbrains_import_tests` covers the JetBrains fixtures, mapping tables,
combined editor/UI variants, JAR discovery and import failures.
The canonical model suite covers alpha parsing, unknown-data round trips, diagnostics, inheritance and
explicit false flags, built-in palette parity, catalog reopen/identity/duplicate/
remove/fallback, lock contention and failed writes. No provider is invoked.

## Theme selection persistence

`ThemeSelectionStore` owns committed selection independently of catalog contents.
ThemeManager consumes it through a snapshot controller. Its default file is `AppDataLocation/color-schemes/selection.json`:

```json
{
  "schemaVersion": 1,
  "revision": "1",
  "followSystem": true,
  "schemeId": "builtin-light",
  "lightSchemeId": "builtin-light",
  "darkSchemeId": "builtin-dark"
}
```

Revision is an unsigned 64-bit decimal string, avoiding JSON number precision
loss. Locked atomic commits advance it monotonically, while identical commits
are idempotent. Unknown scheme IDs are valid selection identities; the consumer
resolves catalog absence with a built-in fallback. Malformed JSON or invalid field
values leave the last known in-memory selection intact on startup/refresh and are
exposed through `lastError()` and `failed`. The next explicit commit repairs them
under the write lock with an atomic replacement and a new revision; an unchanged
selection also repairs the file. No automatic repair happens during observation.
Newer schema versions, oversized files (whose version cannot safely be inspected),
symlinks and read/lock/write errors remain protected from replacement. Invalid
requested identities are rejected.

The constructor accepts a legacy `appearance/theme` value. It migrates
System/Light/Dark only when the file is absent and rechecks under the write lock
so concurrent startup cannot overwrite another process's migration. Existing
selection files always win over legacy settings. The store watches the directory,
surviving atomic replacement of the file; consumers call `refresh()` on focus to
recover missed notifications. `changed` fires once per newly observed revision.
Previews belong in ThemeManager and must never call `commit` until confirmed.

`ketplus_theme_selection_tests` exercises migration, reopen, idempotent and
cross-instance commits, real child-process watcher propagation, lock contention,
malformed/oversized/newer configurations and revision validation. It is a Qt Core
only target and does not launch providers or native windows.

## Consumer adapters

`EditorWidget::applyColorScheme` and `TerminalView::applyColorScheme` accept the
canonical scheme without modifying buffers, selection or terminal process state.
The explicit Lexilla adapter uses the pinned syntax library's lexer-style tables;
keyword/string/number/type/comment/attribute roles carry color and bold/italic/
underline independently of UI accent. Roles the lexer cannot distinguish retain
plain-text fallback; semantic highlighting is not claimed. Embedded gutter and
caret-line tokens retain legacy light/dark colors and caret-line alpha, with a
Scintilla state-parity regression test. Terminal foreground,
background, cursor, selection and all 16 ANSI slots come from the scheme; alpha
for RGB-only Scintilla/vterm paths is composited onto the component background.
The main-window hosts pass canonical snapshots to these consumers. Reserved
built-ins retain legacy per-lexer bold/italic defaults; imported/custom schemes
honor explicit role flags. Python definition names, lexical operators and shell/
CSS variables use dedicated roles where the lexer exposes distinct styles. Other
functions/constants remain deterministic lexical fallbacks, without semantic
inference. Standalone Terminal preserves its historical page background only for
reserved built-ins; custom schemes use their canonical terminal background.

## Live selection API

`ThemeManager::schemeSelection()` exposes the committed `ThemeSelection`.
`setSchemeSelection()` commits a selection through the locked, revisioned store;
`previewSchemeSelection()` changes only this process. `commitSchemePreview()`
commits an installed selection and `cancelPreview()` restores committed state.
`previewColorScheme()` accepts an in-memory canonical import for local inspection;
install that scheme in the catalog before committing its ID. In-memory previews
cannot implicitly install themselves. `hasSchemePreview()` and `schemeError()`
expose state, and `schemeFailed` reports persistence failures.

`colorScheme()` and `palette()` refer to the last complete applied snapshot.
User-file reads, inheritance resolution and kit-override preparation run on the
Qt Concurrent pool. Generation checks discard superseded results. Embedded
built-ins remain synchronous for existing `setMode` callers. A newer committed
revision cancels local previews, and application activation refreshes the store.
System following chooses independent light/dark preferred IDs from Qt style hints;
custom preferences pin the kit to the resolved snapshot to avoid intermediate colors.
The standard built-in pair preserves the existing kit System mode. Direct legacy
Appearance mode changes remain process-local built-in previews.

Publication switches the kit branch under blocked intermediate notifications,
then replaces its overrides once. ThemeManager updates the compatibility palette
and notifies existing consumers during that one kit change. Unchanged built-in
values produce no overrides, preserving all kit defaults and translucent colors.
The explicit kit adapter maps chrome/input/button aliases and ANSI colors; syntax
roles are deliberately not projected onto accent/chart tokens. Consumers read
syntax from the canonical snapshot. Kit recipes without a distinct token for a
schema role still use their shared recipe semantics; they cannot gain independent
sidebar/list/tab colors solely through this override table.

Main-window editor, terminal and diff consumers use canonical snapshots in place.
Terminal panels retain the snapshot for subsequently opened tabs. Diff background
colors preserve imported alpha by compositing over the panel; unchanged built-ins
keep the kit's original soft tints. Pro chat, Markdown/Mermaid and standalone Terminal consume live snapshots too.
The kit is pinned to PR #57's develop merge. Fenced-code formatting shares a
canonical document highlighter; it compares cached styles on ordinary text
updates and rehighlights only when the scheme changes. Existing Quick roots,
transcript models, documents and text selections remain in place.

Mermaid jobs capture the full color configuration when requested; the cache key
includes its fingerprint. Configuration JSON is prepared once per actual theme
change, avoiding work on streaming deltas. Queued diagrams keep their captured
colors even if a later selection is published. Markdown preview uses stable
source-based image URLs and replaces cached image resources without rebuilding
unchanged Markdown, preserving selection and scroll while new colors render.

Native comparison on DELL P2219H covered Light/Dark editor and terminal widgets
and Qt Quick chat. The legacy/scheme chat captures were pixel-identical in both
modes. These are targeted color/state checks, not broader release acceptance.
