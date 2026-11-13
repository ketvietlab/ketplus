# Installable themes

Status: proposed integration architecture. The canonical color-scheme data format,
reader/writer, resolution and local catalog are implemented; see
[color-schemes.md](color-schemes.md) and ADR 0121. The package/import and consumer
integration sections below remain proposals.

## Goals and boundaries

Install VS Code color themes into KetPlus and apply them consistently to native
widgets, QML chat, Scintilla editors, terminals, diffs, and previews. A theme
changes colors and supported token font styles, not application geometry, UI
fonts, shortcuts, or behavior.

This is a color-theme importer, not a VS Code extension host. Never execute
extension JavaScript, activation events, dependencies, commands, or build scripts.
File icon and product icon themes are separate future features.

## Current integration points

- `core/src/ui/Theme.h/.cpp` and `core/src/ui/theme/`: ThemeManager selects a
  canonical scheme through a revisioned selection store, migrates `appearance/theme`
  once, prepares user snapshots asynchronously, and publishes kit color overrides
  with one complete palette notification. Interface zoom remains separate.
- `apps/desktop/src/app/MainWindow.cpp`: appearance menu and `themeChanged` propagation.
- `modules/ai/src/agent/AgentWorkspaceView.cpp`: existing Quick roots and transcript
  documents consume canonical colors/font flags without model or view resets.
- `core/src/editor/SyntaxRoleAdapter.cpp`: explicit shared-token/Lexilla role mapping,
  extended lexical function/operator/variable styles and reserved built-in font parity.
- `core/src/terminal/TerminalColorScheme.cpp`: foreground/background/cursor, selection
  and ANSI16; terminal tabs retain their sessions while colors change.
- Diff and Markdown/Mermaid consumers update styles/resources in place; diagram
  cache keys include the captured color configuration.

Preserve these consumers while migrating them; do not replace the editor or
recreate the chat view as part of theme support.

Imported themes reach Két Widgets and QML through the generic runtime color
API merged in ket-qt #57. Geometry, profile metrics and zoom stay outside that
color override. The canonical adapter maps existing kit tokens; recipe roles
without a distinct token retain their shared semantics. See
[color-schemes.md](color-schemes.md) for the implemented API and limits, and
[design-system.md](design-system.md) for semantic roles and migration boundaries.

## Architecture

```text
Local VSIX / theme file / Open VSX (later)
    -> ThemeInstaller: validate, stage, publish
    -> ThemeCatalog: installed packages and theme variants
    -> VSCodeThemeReader: parse and resolve local dependencies
    -> ThemeResolver: defaults, imported values, overrides
    -> ResolvedTheme: immutable, complete snapshot
    -> ThemeManager: select, preview, commit, notify
    -> Widgets / QML / Scintilla / Terminal / Preview adapters
```

Keep the CM implementation under `core/src/ui/theme/`; retain
`ThemeManager` as the public entry point. Do not create a general
extension/plugin framework for this feature.

### Data contracts

- `ThemeDescriptor`: stable theme ID, package ID/version, variant ID, display
  label, source, declared light/dark/high-contrast kind, license metadata.
- `ThemeDefinition`: raw workbench colors, ordered TextMate rules, semantic token
  rules, dependency hashes, and import diagnostics. Preserve unsupported keys.
- `ResolvedTheme`: typed QColor values for chrome, editor, syntax, terminal,
  diff, and preview groups; compatibility report; content fingerprint.
- `ThemeSelection`: fixed theme OR system-following preferred light/dark themes.
  Optional high-contrast preferences remain separate from the dark boolean.

Use `publisher.extension` for package IDs; identify variants with contribution
`id` when available, otherwise normalized package-relative theme path. Do not
use localized labels as identity. Imported standalone files receive persistent
local IDs independent of their original filesystem location.

`ThemePalette` remains a temporary compatibility projection for old consumers.
New consumers use component-specific tokens: editor keywords must no longer be
coupled to UI accent, and strings must not be coupled to success indicators.

## Supported input

First release:

- Local `.vsix`: read `extension/package.json` and `contributes.themes[]`; a
  package can install several selectable variants.
- Local color-theme JSON/JSONC, including bundled relative `include` files.
  Request a source directory when dependencies are outside a standalone file's
  directory; never silently read arbitrary files elsewhere on the computer.
- `tokenColors` arrays and references to XML `.tmTheme` files.
- Light, dark, and high-contrast declarations, including manifest `uiTheme`.
- Colors in VS Code's documented hex forms; parse alpha as RGBA, not Qt's
  eight-digit ARGB string convention.

Theme input infrastructure is exported as `KetPlusCM::theme_io`, with
`ui/theme/io/ThemeInput.h` in namespace `ket::theme::io`. `parseJsonc` uses a
string/escape-aware tokenizer to replace comments and trailing commas with spaces,
then delegates JSON grammar validation to Qt. It preserves offsets and reports
line/column errors. No regex stripping is used.

`ZipArchive` uses pinned miniz 3.1.2 (MIT), reads ZIP metadata from an open file,
and decompresses only explicitly requested entries into bounded memory. It rejects
unsafe paths, special-file/link attributes, duplicate normalized names, Unicode
normalization/case collisions, file/directory conflicts, encryption and unsupported
compression. Declared expanded sizes are checked before allocation; requested
payloads also undergo miniz output-size, stream and CRC validation. No extraction
to disk occurs. ZIP entry names must be UTF-8. Limits apply to every entry,
including unrequested entries.

`parseXml` returns a small `XmlNode` tree using `QXmlStreamReader`. External
DOCTYPE identifiers, including the standard Apple plist header in `.tmTheme`,
are accepted without fetching or validating their DTD. All internal subsets,
entity declarations and unresolved entity references are rejected; no external
resolver or network access is installed. Standard escaped XML characters remain
supported. XML has additional 128-level/100,000-node bounds.

`IncludeResolver` reads from a caller-selected root directory or validated archive.
It rejects remote/absolute references, root escapes and filesystem symlinks, and
returns JSON documents parent first without merging them. The root counts as
level one of the 16-level limit. Failed reads/parses leave outputs empty. Parsed
files are limited to 5 MiB; include chains have a cumulative 100 MiB budget.
On Unix, directory-descriptor reads with `openat`/`O_NOFOLLOW` also prevent a
checked path from being replaced by a symlink before opening. Non-Unix reads
check path containment and symlinks before opening, but do not provide the same
race protection against concurrent replacement of directories/junctions; import
from a directory whose contents are not concurrently modified by another actor.
Format-specific import and merge semantics remain the adapter's responsibility.

Resolve includes relative to the declaring file, detect cycles, and enforce depth
and byte limits. Parent colors are overridden by child keys; token rules retain
their inherited order, followed by child rules. Verify exact merge and selector
semantics against pinned upstream fixtures before claiming compatibility.

## Resolution and mapping

Precedence, low to high: built-in defaults for the declared theme kind, inherited
theme definitions, selected theme definition, user overrides. Preview selection
does not mutate the saved configuration. Missing/invalid values use deterministic
fallbacks with diagnostics rather than leaving controls unstyled.

Example workbench mappings:

| KetPlus token group | VS Code color IDs |
| --- | --- |
| Editor | `editor.background`, `editor.foreground`, `editor.selectionBackground` |
| Sidebar | `sideBar.background`, `sideBar.foreground`, `sideBar.border` |
| Inputs | `input.background`, `input.foreground`, `input.border` |
| Lists | `list.hoverBackground`, `list.activeSelectionBackground`, `list.activeSelectionForeground` |
| Tabs | `tab.activeBackground`, `tab.activeForeground`, `tab.activeBorderTop` |
| Focus/buttons | `focusBorder`, `button.background`, `button.foreground` |
| Terminal | `terminal.background`, `terminal.foreground`, `terminalCursor.foreground`, ANSI colors |
| Diff | `diffEditor.insertedTextBackground`, `diffEditor.removedTextBackground` |

Maintain this mapping as an explicit, testable table with per-token fallback
chains. It is not a promise to implement every VS Code view or color ID. Chat
uses KetPlus semantic tokens derived from editor/panel/list colors where there is
no equivalent component. Keep foreground/background selection pairs together.

Retain alpha for QML/native APIs that support it; otherwise composite against the
actual resolved component background. Preserve explicitly authored colors;
contrast warnings must not silently rewrite them. High-contrast themes need
explicit focus/border treatment, not merely a dark background.

### Syntax compatibility

Lexilla emits lexer-specific integer styles, not TextMate scope stacks. Introduce
an explicit per-language adapter from those styles to representative scope paths
and common syntax roles. Match supported ordered theme rules against these paths
and generate Scintilla styles; preserve bold/italic/underline and explicit resets.

The initial importer supports a documented subset of selectors. Report unsupported
selectors instead of guessing their meaning. Some languages cannot distinguish
all function/type/variable contexts, so syntax colors are an approximation even
when a selector parses successfully. Keep the raw rules for future improvements.

Store `semanticTokenColors` and `semanticHighlighting`, but mark them inactive
until KetPlus has a semantic-token producer and rendering path. Do not pretend
that mapping Lexilla styles implements semantic highlighting. Full TextMate
compatibility is a separate tokenizer/editor project, not an install prerequisite.

## Installation and storage

Canonical user schemes use `QStandardPaths::AppDataLocation/color-schemes/`
(ADR 0121). The following external-package storage is a future proposal.

Use `QStandardPaths::AppDataLocation/themes/` with immutable versioned packages,
a catalog index, selection configuration, and a staging directory. Keep original
theme assets and license files. Compiled caches are disposable and keyed by
dependency hashes plus resolver/adapter schema versions.

1. Inspect package identity, variants, source, license, and compatibility before
   confirmation. Show declared provenance without treating it as verification.
2. Extract only allowed theme data into staging using a maintained ZIP library.
3. Reject absolute paths, traversal, symlinks/hardlinks, duplicate normalized
   entries, case collisions, remote includes, and paths outside the package.
4. Enforce limits: initial targets are 25 MiB download, 100 MiB expanded data,
   2,000 entries, 5 MiB per parsed file, and 16 include levels. Reject packages
   exceeding limits with a reason; tune limits using real theme fixtures.
5. Validate and compile before publishing the package via same-filesystem rename.
   Atomically update the catalog with QSaveFile under QLockFile.
6. Retain the previous valid version during updates. Cancellation, disk errors,
   invalid packages, and interrupted downloads must leave it usable.

Installation does not automatically activate every newly installed variant.
Uninstalling the active package first selects a built-in fallback. Unknown/missing
saved theme IDs also fall back without preventing startup.

Local installation comes first. An optional Open VSX provider later supplies
search, version metadata, cancellable HTTPS downloads, and checksums when
available. Checksum integrity does not establish publisher trust. Do not scrape
the Microsoft Marketplace or depend on undocumented download endpoints.

## Theme selection and multiple processes

Settings > Appearance contains installed-theme search, swatches, variant selection,
Install from VSIX/File, and uninstall/update actions. A keyboard theme picker uses
arrows for temporary preview, Enter to commit, and Escape to restore. Future
Spotlight integration invokes this same command; it does not own theme state.

Separate "Follow system" from theme identity, with independent preferred light
and dark variants. Migrate existing `appearance/theme` preferences once into a
versioned selection file, preserving System/Light/Dark behavior.

Installed themes and committed selection are user-wide, consistent across repo
windows/processes. Use an atomic revisioned configuration file, QLockFile for
writes, and a directory watcher that survives atomic replacement. Re-read the
revision on focus as a missed-notification fallback. Reuse existing IPC only if
it provides a reliable broadcast; repository activation IPC is not assumed to.

Preview is local to the initiating process because QApplication palette/QSS is
process-wide. It never publishes a global revision. If a new committed revision
arrives during preview, cancel the preview and apply the newer committed theme.
Workspace-owned files must not install or activate themes automatically.

## Applying without chat regressions

Parse, resolve, and prepare caches off the GUI thread. Publish one complete
snapshot on the GUI thread, then emit `themeChanged` once. Discard stale async
results when selection changes again. Avoid repeated global stylesheet updates
while navigating the picker by coalescing superseded previews.

Update palette/token properties on existing views. Never reset transcript models,
reload QML, recreate QTextDocuments, restart terminals, or replace editor buffers.
Preserve scroll anchors, selections, undo histories, active streams, and focus.
No theme work should occur for ordinary agent deltas. Audit hardcoded colors and
icon tint paths, including menus, dialogs, scrollbars, diffs, and Markdown previews.

## Delivery and verification

1. Separate resolved chrome/editor/syntax/terminal tokens. Migrate built-ins and
   existing consumers with screenshot parity; keep existing theme preferences.
2. Add local VSIX/JSONC/tmTheme import, secure storage, catalog, compatibility
   reports, preview/commit/cancel, and multi-process synchronization.
3. Add Open VSX discovery and explicit updates using the same installer.
4. Improve scope coverage or add semantic tokens independently, based on demand.

Acceptance tests cover include precedence/cycles, RGBA, partial themes, rule
ordering and style resets, malformed JSON/XML, archive traversal/bombs, transaction
rollback, uninstall fallback, restart persistence, and concurrent installations.
Use small redistributable upstream fixtures pinned by version/hash/license plus
synthetic adversarial fixtures; do not rely on mutable online themes in CI.

UI tests cover light/dark/high-contrast themes across widgets, QML, editor, terminal
ANSI colors, menus, icons, and diff surfaces. Switch themes during chat streaming
and assert model identity, selection, scroll, and undo preservation. Native Metal
image tests remain necessary; passing offscreen layout tests alone is insufficient.

## References

- [VS Code color themes](https://code.visualstudio.com/api/extension-guides/color-theme)
- [Workbench color IDs](https://code.visualstudio.com/api/references/theme-color)
- [TextMate syntax highlighting](https://code.visualstudio.com/api/language-extensions/syntax-highlight-guide)
- [Semantic highlighting](https://code.visualstudio.com/api/language-extensions/semantic-highlight-guide)
- [Open VSX registry](https://github.com/eclipse-openvsx/openvsx)
