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
