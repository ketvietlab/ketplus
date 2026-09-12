# Current capability matrix

This matrix records the implemented public CM surface observed at base
`7ac9d758f1b095b4f85362d40fe4b6e55c261680`.

| Area | Target/header | Implemented capability | Public notes | Proposed gap |
| --- | --- | --- | --- | --- |
| Theme | `KetPlusCM::ui`, `src/ui/Theme.h` | `ThemeManager` owns system/light/dark modes, `ThemePalette`, Qt palette, QSS, Inter loading, interface font size, and preview/cancel. | Public include path is `src`; no install/export package boundary yet. Palette fields are concrete string colors, not token identifiers. | Export a stable native token view and metrics contract without replacing canonical KDS tokens. |
| Typography | `KetPlusCM::settings_ui`, `src/app/AppearanceSettings.h`; `KetPlusCM::editor`, `src/editor/EditorSettings.h` | Interface, editor, terminal, and preview sizes/line heights are persisted and normalized. Editor typography is applied to Scintilla and reused by diff views. | Interface line height is automatic; document-like panes have explicit line height. | Document token mapping and keep font/line-height normalization in public structs. |
| Native controls | `KetPlusCM::ui`, `src/ui/FindReplaceBar.h`; QSS in `Theme.cpp` | Buttons, tab bar, sidebar, tree, tables, settings pages, status bar, scrollbars, tooltips, terminal/preview/diff roles use semantic palette fields and `kvRole` selectors. | Roles are QSS conventions rather than typed component APIs. | Promote reusable native component metrics gradually after QA evidence. |
| Settings | `KetPlusCM::settings_ui`, `src/app/SettingsDialog.h` | `SettingsPage` supports stable id/title, `hasChanges`, `resetToDefaults`, `apply`, and `changed`. `SettingsDialog::addPage` composes extension pages with shared Save/Cancel/Reset. | Existing editor-only constructor and `settingsSaved` signal remain for compatibility. | Formalize page registration metadata, stable page selection by id, and collision/ownership rules. |
| Editor | `KetPlusCM::editor`, `src/editor/EditorWidget.h` | Scintilla adapter with document loading, dirty state, theme application, syntax selection, find/replace, large-file mode, hibernation, and normalized settings. | Scintilla API is kept out of app shell but `EditorWidget` remains widget-level. | Keep current implementation; expose no private product editor concepts in Wave 1. |
| Diff | `KetPlusCM::git_ui`, `src/git/GitDiffView.h`; `KetPlusCM::git`, parser/types | Staged/unstaged/combined diff rendering, header metadata, theme/settings application, parser coverage, and synthetic Git temp-repo tests. | Read-only; no staging/checkout/commit. | Add public linked-worktree operation preflight/outcome API before any mutating workflow is considered. |
| Terminal | `KetPlusCM::terminal`, `src/terminal/TerminalPanel.h`, `PtyProcess.h`, `TerminalView.h` | Lazy PTY-backed terminal on Unix-like platforms, theme and typography application, close/status signals. | Terminal workflows remain user-owned source of truth for repository mutation. | No Wave 1 API expansion beyond documenting typography and theme mapping. |
| Workspace | `KetPlusCM::workspace`, `src/workspace/ExplorerPanel.h` | Lazy filesystem tree, open folder, create/rename/reveal/copy path, move-to-trash UI. | Destructive action uses native trash, not delete. | Keep destructive tests inside disposable repositories/directories only. |
| Git primitives | `KetPlusCM::git`, `src/git/GitService.h`, `GitTypes.h` | Git CLI availability, repository discovery, status, scoped diffs, worktree list parsing, async bounded commands. | Git support is read-only; existing workspace selection is not a repository mutation. | Introduce linked-worktree observation and operation-specific policy with Git common-dir identity, cancellation, and revalidation. |
| Preview | `KetPlusCM::preview`, Markdown/Mermaid headers | Native Markdown preview and Mermaid SVG rendering/viewer. | Not a primary Wave 1 API focus, but typography/theme settings include preview. | No new public contract in Wave 1. |
| Build/package | `CMakeLists.txt` aliases | `KetPlusCM::*` alias targets exist for build-tree consumers. | No `include/` tree or installed CMake package files were observed. | Release slices should separate build-tree compatibility from installable SDK work. |

Implemented capability vs proposed API:

- Implemented: everything in the "Implemented capability" column, plus focused
  baselines for current settings composition/persistence behavior, current
  native palette values, and worktree parser flags.
- Proposed: native token model, richer settings host registration, and
  linked-worktree operation interface. These are not implemented in production
  in Wave 1.
