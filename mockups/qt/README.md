# KétPlus Pro · Qt mock

The interactive design reference for KétPlus Pro, built with the
[Két Qt kit](https://github.com/ketvietlab/ket-qt) (`Ket` QML module). It
replaces `ketplus.ketatlas` ([ADR 0033](../../docs/decisions/0033-qt-design-mock.md)).
Every value comes from deterministic fixtures in `qml/Fixtures.qml`; nothing
talks to the daemon, Git, a provider or the network.

## Build and run

Qt 6.8 or later with Quick, QuickControls2 and Svg.

```sh
cmake -S mockups/qt -B build/mock -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build/mock
build/mock/KetPlusMock.app/Contents/MacOS/KetPlusMock      # macOS
```

The kit is pinned by commit in `CMakeLists.txt`. Use a local checkout with
`-DFETCHCONTENT_SOURCE_DIR_KETQT=<path to ket-qt>`, and an offline copy of the
KÉT Sans archive with `-DKETPLUS_MOCK_KET_SANS_ARCHIVE=<path>/KETSans-1.100.zip`.

## Evidence

```sh
KetPlusMock --smoke                                    # every screen, fails on QML warnings
KetPlusMock --screen=workspace --state=running --dark --zoom=1.25 --screenshot=out.png
KetPlusMock --screen=workspace --open=model --screenshot=model.png  # model, mode or limits
```

Screens and states (`--screen` / `--state`):

| Screen | States | Covers |
|---|---|---|
| `workspace` | `welcome`, `running`, `question`, `error`, `atlas`, `details`, `compact` | Agent Workspace: 44 px header, transcript activity, gallery, agent messages, changed files, question card, Atlas result, provider error, background tasks, composer (card, then attach, Mode menu, Model & context dialog and the Context & limits ring below it), details pane (changes, Atlas, terminal) |
| `editor` | | Files & Editor: explorer, tabs, breadcrumb, code, status bars |
| `changes` | | Source Control: changes, staged changes, diff |
| `worktrees` | | Manage worktrees, create dialog, manage menu |
| `connections` | | This Mac, phones, Linux machines (SSH or Relay), pairing dialogs, account |
| `settings` | `typography`, `account`, `providers`, `archived`, `updates` | Appearance (live theme, follow system, zoom), typography, providers and browser, archive, updates |
| `atlas` | | Edit an Atlas: flows, map, AI Chat |
| `mobile-picker` | | Repository & worktree picker on iPhone |

App-wide: Quick search (⌘K), the repository and worktree picker, conversation
context menu (rename, fork, change worktree, archive, delete), tag filter
(click a tag in the header), compact window (⇧⌘M), update notice and toasts.

## Changing the mock

- Use kit components. When the mock needs one the kit lacks, add it to
  ket-qt first (Widgets and Quick twins), then bump the pinned commit.
- Colours come from `KetTheme.t` tokens; lengths go through
  `KetTheme.px(value, KetTheme.zoom)` so zoom applies.
- Keep `--smoke` at zero QML problems.
