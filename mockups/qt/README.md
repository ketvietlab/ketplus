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
KetPlusMock --screen=workspace --open=model --screenshot=model.png  # model, mode, limits, reviewer, composer (focused) or images (the image viewer)
```

Screens and states (`--screen` / `--state`):

| Screen | States | Covers |
|---|---|---|
| `workspace` | `welcome`, `running`, `question`, `error`, `atlas`, `details`, `compact` | Agent Workspace: 44 px header, transcript activity, gallery, agent messages, changed files, question card, Atlas result, provider error, background tasks, composer (card, then attach, Mode menu, Model & context dialog and the Context & limits ring below it), details pane (changes, Atlas, terminal) |
| `editor` | | Files & Editor: explorer, tabs, breadcrumb, code, status bars |
| `editor` | `terminal` | The terminal below the editor, drawn like `mockups/terminal`, with fixture output |
| `editor` | `markdown`, `markdown-source`, `samples` | `docs/licensing.md` (bundled from the repository) in Preview or Source, with the floating Preview/Source switch 100 px above the bottom; `samples` opens `design-system.md` with QML, C++, shell and JSON code blocks in syntax colours |
| `changes` | | Source Control: changes, staged changes, diff |
| `worktrees` | | Manage worktrees, create dialog, manage menu |
| `connections` | | This Mac, phones, Linux machines (SSH or Relay), pairing dialogs, account |
| `settings` | `typography`, `account`, `browser`, `computer`, `providers`, `archived`, `storage`, `updates` | The app's order: Appearance (live theme, follow system, zoom), typography, account, Agent models (opens the model picker), browser, computer use, providers (CLI status, Claude API access, organization usage), archive, storage (journal size by part, retention, size limit and warning, largest conversations, Compact now), updates |
| `workspace` | `claude`, `codex`, `cursor` | One transcript per provider with every content type its journal produces (see Transcript content types); the composer follows the conversation's provider and model. `--scroll=0.5` starts the transcript part of the way down |
| `workspace` | `storage-warning` | The sidebar's journal warning above Update ready; it opens Settings → Storage and clears after Compact now |
| `atlas` | | Edit an Atlas: flows, map, AI Chat |
| `mobile-picker` | | Repository & worktree picker on iPhone |

App-wide: Quick search (⌘K), the repository and worktree picker, conversation
context menu (rename, fork, change worktree, archive, delete), tag filter
(click a tag in the header), compact window (⇧⌘M), update notice and toasts.

## Transcript content types

The `claude`, `codex` and `cursor` transcripts follow the shapes in a six-week daemon
journal (88 conversations). The counts are completed items and events; the text in the
fixtures is made up. `qml/TranscriptFixtures.qml` maps each journal event to a block type,
`qml/parts/TranscriptBlock.qml` draws the blocks and `qml/parts/TranscriptActivityItem.qml`
the steps of an activity group.

| Content | Journal source | Claude | Codex | Cursor |
|---|---|---|---|---|
| User message, with images, from another conversation | `user.message` (`images`, `agent_messages`) | ✓ | ✓ | ✓ |
| Steer while running, queued message | `turn.steered`, `turn.queued` | ✓ | ✓ | – |
| Commentary between steps | `agentMessage` phase `commentary` | 58k | 6.7k | – |
| Final answer (Markdown, tables, code) | `agentMessage` phase `final_answer` | 2.5k | 1.2k | 16 |
| Command with output, exit code, duration, live output | `commandExecution`, `command.output_delta` | 62k (Bash) | 63k | 270 (Bash) |
| Read, search, list (Codex command actions) | `commandActions` read / search / listFiles, Claude `Read` | Read 1.8k | 35k | – |
| File edits with diff (add, update, delete, move) | Codex `fileChange`, Claude `Write`/`Edit` | 2.4k | 3.5k | – |
| Browser tool with captures, failures | `ketplus_browser.*` (dynamic or MCP tool) | 11k | 38k | 11 |
| Other MCP tools (agents, docs, node/cua REPL) | `mcpToolCall`, `mcp__*` | ✓ | ✓ | – |
| Web search, open page, find in page, fetch | `webSearch`, `WebSearch`, `WebFetch` | 480 | 724 | – |
| Viewed image, generated image with prompt | `imageView`, `imageGeneration` | – | 661, 431 | – |
| Reasoning (summary always empty) | `reasoning` | – | 44k | – |
| Wait, monitor, scheduled wake-up | `sleep`, `Monitor`, `ScheduleWakeup` | 200 | 813 | – |
| Loaded tools, skills, stopped background task | `ToolSearch`, `Skill`, `TaskStop` | ✓ | ✓ | – |
| Subagent | `Task` | – | – | ✓ |
| Question: answered, deferred, expired (pending: `workspace/question`) | `approval.requested` kind `input`, `question.*` | 128 | 211 | – |
| Approval: command, MCP tool, provider tool | `approval.requested` other kinds, `approval.resolved` | 2 | 24 | 5 |
| Agent message: request, reply, notice and their states | `agent_message.state` | ✓ | ✓ | – |
| Compaction, provider/model change, fork, worktree change | `contextCompaction`, `session.*`, `conversation.context_changed` | ✓ | ✓ | ✓ |
| Reconnecting, turn-ending error | `runtime.error` (`will_retry`) | ✓ | ✓ | ✓ |
| Turn end: completed, stopped, failed; Claude cost | `turn.completed`, `usage.session` | ✓ | ✓ | ✓ |
| Detail trimmed by journal compaction | ketvietlab/ketplus#128 | ✓ | – | – |

## Changing the mock

- Use kit components. When the mock needs one the kit lacks, add it to
  ket-qt first (Widgets and Quick twins), then bump the pinned commit.
- Colours come from `KetTheme.t` tokens; lengths go through
  `KetTheme.px(value, KetTheme.zoom)` so zoom applies.
- Keep `--smoke` at zero QML problems.

## Assets

The sidebar logo is the bare KétPlus Pro mark (`assets/ketplus-mark-bare-{dark,light}.svg`) and
the tile `assets/ketplus-mark.svg` from Két Symbol, licensed CC BY 4.0
(`assets/LICENSE-CC-BY-4.0.txt`): "Két Symbol © 2026 Két Việt, CC BY 4.0". The name KétPlus is a
trademark and is not licensed.
