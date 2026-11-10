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
KetPlusMock --edition=lite --smoke                     # the same for KétPlus, and every Search result
KetPlusMock --edition=lite --smoke --keys              # also chooses View > Search in Files by keyboard
KetPlusMock --smoke --display=secondary                # on another display, leaving the main one free
KetPlusMock --edition=lite --chrome=windows --screen=search --screenshot=search.png
KetPlusMock --screen=workspace --state=running --dark --zoom=1.25 --screenshot=out.png
KetPlusMock --screen=workspace --open=model --screenshot=model.png  # model, mode, limits, reviewer, composer (focused) images (the image viewer), mermaid (diagrams instead of their source) toast (the app-wide toast, on any screen), or rename and delete (the conversation dialogs, on any screen)
```

Screens and states (`--screen` / `--state`):

| Screen | States | Covers |
|---|---|---|
| `workspace` | `welcome`, `running`, `question`, `error`, `atlas`, `details`, `report`, `report-running`, `report-details`, `compact` | Agent Workspace: 44 px header, transcript activity, gallery, agent messages, changed files, question card, Atlas result, provider error, background tasks, composer (card, then attach, Mode menu, Model & context dialog and the Context & limits ring below it), work report at the end of a turn (counted by the app from the agent's items; live activity while running), right pane with closable document tabs (changes, Atlas, terminal, file, plan and the work report; `report-details` opens the Report tab, also in the pane's Open content menu) |
| `editor` | | Files & Editor: explorer, tabs, breadcrumb, code, status bars |
| `editor` | `terminal` | The terminal below the editor, drawn like `mockups/terminal`, with fixture output |
| `editor` | `markdown`, `markdown-source`, `samples` | `docs/licensing.md` (bundled from the repository) in Preview or Source, with the floating Preview/Source switch 100 px above the bottom; `samples` opens `design-system.md` with QML, C++, shell and JSON code blocks in syntax colours |
| `changes` | | Source Control: changes, staged changes, diff |
| `connections` | `no-activity` | This Mac, phones, activity from paired devices (the daemon's remote command audit, #86: done and refused requests, a sender that did not verify) or its empty state, Linux machines (SSH or Relay), pairing dialogs, account. `--open=activity` shows all activity, `--open=device` one phone's details, `--open=device-focus` the same with a truncated row focused from the keyboard |
| `connections` | `signed-out`, `expired`, `sign-in` | Signed out: the introduction (this Mac and KetPlus Mobile, the three pairing steps), the expired-login banner, the browser sign-in dialog |
| `settings` | `typography`, `account`, `browser`, `computer`, `providers`, `archived`, `storage`, `updates` | The app's order: Appearance (live theme, follow system, zoom), typography, account, Agent models (opens the model picker), browser, computer use, providers (CLI status, Claude API access, organization usage), archive, storage (journal size by part, retention, size limit and warning, largest conversations, Compact now), updates |
| `workspace` | `claude`, `codex`, `cursor` | One transcript per provider with every content type its journal produces (see Transcript content types); the composer follows the conversation's provider and model. `--scroll=0.5` starts the transcript part of the way down |
| `workspace` | `plan-ready`, `plan-planning`, `plan-running`, `plan-continuing`, `plan-blocked`, `plan-paused`, `plan-limit`, `plan-complete` | Concept: Plan is a document tab in the existing right pane; the transcript and composer remain visible. Automatic planning from repository context, compact tasks and source references, daemon follow-up, Stop, blocker and budget states. See [the harness preview](marketing/plan-harness/README.md). |
| `workspace` | `content-tabs`, `content-terminal`, `content-file` | Editor-style right pane with two files, two independent terminal sessions and the conversation plan. Closable tabs, horizontal overflow, `+` and open-documents menu (`--open=content-tabs`). |
| `workspace` | `turn-report` | English marketing sample: end-of-turn status, evidence, blockers, next actions and owners. Uses the existing final-answer Markdown table renderer; all release data is fictional. |
| `workspace` | `storage-warning` | The sidebar's journal warning above Update ready; it opens Settings → Storage and clears after Compact now |
| `workspace` | `setup-cli-missing`, `setup-cli-outdated`, `setup-signed-out`, `setup-claude-signed-out`, `setup-sidecar-missing` | A provider that cannot start a turn: a new conversation with its setup guide above the composer, which keeps the draft (see Provider setup) |
| `workspace` | `setup-expired`, `setup-key-rejected`, `setup-limit`, `setup-capacity`, `setup-plan-blocked` | A turn the provider ended: the guide closes the turn, with Retry (see Provider setup) |
| `workspace` + `--open=model` or `--open=setup` | any `setup-*` | Model & context names the provider's problem with Show setup steps; `setup` opens that guide in a dialog |
| `settings` | `providers-setup` | Settings → Providers with Cursor not installed and Claude's sign-in expired; each provider shows its badge, details and the same guide |
| `atlas` | | Edit an Atlas: flows, map, AI Chat |
| `mobile-picker` | | Repository & worktree picker on iPhone |

KétPlus (Lite) runs with `--edition=lite` (`qml/LiteMain.qml`): the editor of
`core/src/app` with one side panel at a time, the terminal and the two Settings
pages Lite has. The window opens on the editor with the side panel closed; an
activity-bar button or its shortcut (⌘B, ⇧⌘F, ⇧⌘G) opens that panel, and the same
button or shortcut again closes it. `--chrome=windows` or `--chrome=linux` draws the title bar and
the in-window menu bar (File, Edit, Go, Git, View, with the labels and
shortcuts of `core/src/app/MainWindowActions.cpp`); macOS keeps the system's.
The menu bar works without a pointer: F10 focuses File, Alt with the app's
mnemonic opens File, Edit, Go or View, Left and Right move between headings, and
Enter, Space or Down opens one. `--keys` activates the window to check this, so
it takes the keyboard focus for a moment; without an active window it reports
"skipped". Search reads the editor's own file sources, so each result opens its
file at a line that holds the match (Markdown in Source); the Lite smoke checks
every result. Closing the terminal's last tab hides the terminal, as in the app;
showing it again opens a new tab. The Lite smoke checks this too.

| Screen (`--edition=lite`) | States | Covers |
|---|---|---|
| `editor` | | The default: the editor with the side panel closed |
| `explorer` | `terminal`, `markdown` | Activity bar, explorer, tabs, editor, the terminal below it, `docs/licensing.md` in Preview |
| `explorer` | `welcome` | No folder open: Open Folder and recent folders |
| `explorer` | `windows`, `linux` | The same window with that platform's chrome, whatever `--chrome` says |
| `search` | `no-results` | Search in Files: matches grouped by file, the match in the accent, a line opens its file; the empty state |
| `changes` | | Source Control, as in Pro |
| `settings` | `typography` | Appearance and Typography only |

There is no Worktrees screen: a worktree is made when a conversation needs one
(Repository & worktree → Create worktree…, or the New conversation card) and
switched from the conversation. App-wide: Quick search (⌘K), the repository and worktree picker, conversation
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

## Provider setup

`qml/Fixtures.qml` (`providerSetup`) lists what stops a provider and how the user fixes it;
`qml/parts/ProviderSetupCard.qml` draws the guide: what happened in plain words, the
provider's own message, numbered steps with the exact commands, and one primary action.
Each command can be copied. In the app, Type in Terminal types it into a terminal beside the
conversation for the user to run with Return, and KetPlus never runs it; in the mock it only
shows a toast. Codex and Cursor sign-ins stay with their CLIs; the optional Anthropic API key is
the one credential KetPlus saves (Keychain, Settings → Providers). Check again
and Retry walk the live states (checking, failing once more for `cli-missing`, fixed). The
same guide appears in Settings → Providers, which drops the turn-only steps.

Hiding the guide above the composer is never a dead end: the model button then shows a
warning, Model & context shows the problem with Show setup steps (for any provider tab, not
only the conversation's), which opens the guide in a dialog, and Send brings the guide back
while keeping the draft. A new setup state shows a hidden guide again.

| State | Provider | What the daemon reports | Guide |
|---|---|---|---|
| `cli-missing` | Cursor | Version check `missing`; `CursorAdapter::missingExecutableMessage` | `curl https://cursor.com/install -fsS \| bash`, `agent login`, Check again |
| `cli-outdated` | Codex | Version check `outdated` (below `minimum` in `provider-compatibility.json`) | `npm install -g @openai/codex@latest` or `brew upgrade codex` |
| `signed-out` | Codex | The CLI has no account | `codex login`, or `codex login --with-api-key` from standard input |
| `claude-signed-out` | Claude | Credential source `none` (`ClaudeCredentials::observe`), a cached observation from the last query, not a fresh probe | Claude Code `/login`, or an API key in Settings → Providers |
| `sidecar-missing` | Claude | `provider_unavailable`: "Claude Agent SDK sidecar was not found." | Reinstall KetPlus from Settings → Updates (see #124) |
| `expired` | Claude | `runtime.error`: "Failed to authenticate: OAuth session expired and could not be refreshed" | `/login` again, Retry |
| `key-rejected` | Claude | HTTP 401 on the explicit credential check: "Anthropic rejected this API key." (not yet a turn `runtime.error`) | A new key in Settings → Providers |
| `limit` | Codex | `runtime.error`: "You've hit your weekly limit · resets …" | Wait for the reset (Limits), or another provider |
| `capacity` | Codex | `runtime.error`: "Selected model is at capacity." | Choose another model, Retry; not flagged in Settings |
| `plan-blocked` | Cursor | `CursorAdapter::planLimitMessage` | `agent login` with the right account, or Auto |

The mock's checks and retries are timers, not evidence of these backend behaviours (#170). The
quoted texts are the ones the daemon and the providers write (seen in a daemon journal
or in the source); the email address and the times are made up.

## Changing the mock

- Use kit components. When the mock needs one the kit lacks, add it to
  ket-qt first (Widgets and Quick twins), then bump the pinned commit.
- Colours come from `KetTheme.t` tokens; lengths go through
  `KetTheme.px(value, KetTheme.zoom)` so zoom applies.
- Keep `--smoke` at zero QML problems.

## Connections captures

The signed-out Connections introduction shows this mock and the KetPlus Mobile mock answering a
question (`assets/connections/{desktop,mobile}-question-{light,dark}.png`). Capture them again
when either screen changes:

- Desktop: `KetPlusMock --screen=workspace --state=question --light --screenshot=out.png` (and
  `--dark`), scaled to 1440 × 900.
- Mobile: `ketplus-worktree-mobile.ketatlas/screens/conversation.html?thread=mobile-ui&question=pending&theme=light`
  (and `theme=dark`) from `ketvietlab/ketplus-app` `develop`, at 393 × 852 CSS pixels and 2×,
  with focus moved off the sheet's Close button.

The desktop app's Connections page (`RemoteConnectionsIntro`) shows the same files as resources
under `:/connections`, so a new capture updates both.

## Mermaid and the image viewer

A ```` ```mermaid ```` fence in an answer or a Markdown file (`parts/MarkdownView.qml`) shows its
source by default, as the app does. The Source | Mermaid switch shows the diagram, and a click
opens it in the kit's image viewer (`KetLightbox`). Captures, viewed and generated images and
images attached to a message open there too.

The mock does not run mermaid-cli. Its diagrams (`mermaid/*.mmd`) are rendered ahead of time
with the app's Két theme (`core/resources/mermaid`):

```sh
python3 tools/render_mermaid.py   # needs mmdc on PATH; writes assets/mermaid/*@2x.png
```

A fence that is not one of these samples stays source only.

## Assets

The sidebar logo is the bare KétPlus Pro mark (`assets/ketplus-mark-bare-{dark,light}.svg`) and
the tile `assets/ketplus-mark.svg` from Két Symbol, licensed CC BY 4.0
(`assets/LICENSE-CC-BY-4.0.txt`): "Két Symbol © 2026 Két Việt, CC BY 4.0". The name KétPlus is a
trademark and is not licensed. The Mermaid renderings use the Két Mermaid theme from the Két
Design System (MIT, `core/resources/mermaid/KetDesignSystem-LICENSE.txt`).
