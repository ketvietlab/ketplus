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
KetPlusMock --smoke                                    # every screen and the teams checks, fails on QML warnings
KetPlusMock --edition=lite --smoke                     # the same for KétPlus, and every Search result
KetPlusMock --edition=lite --smoke --keys              # also chooses View > Search in Files by keyboard
KetPlusMock --smoke --display=secondary                # on another display, leaving the main one free
KetPlusMock --edition=lite --chrome=windows --screen=search --screenshot=search.png
KetPlusMock --screen=workspace --state=running --dark --zoom=1.25 --screenshot=out.png
KetPlusMock --screen=teams --open=new-team --size=1024x640 --zoom=1.25 --screenshot=out.png
KetPlusMock --screen=workspace --open=model --screenshot=model.png  # model, mode, limits (limits-cost opens Cost details, limits-api-key has none), reviewer, composer (focused) images (the image viewer), mermaid (diagrams instead of their source) toast (the app-wide toast, on any screen), or rename and delete (the conversation dialogs, on any screen)
```

Screens and states (`--screen` / `--state`):

| Screen | States | Covers |
|---|---|---|
| `workspace` | `welcome`, `running`, `question`, `error`, `atlas`, `details`, `report`, `report-running`, `report-details`, `compact` | Agent Workspace: 44 px header, transcript activity, gallery, agent messages, changed files, question card, Atlas result, provider error, background tasks, composer (card, then attach, Mode menu, Model & context dialog and the Context & limits ring below it), work report at the end of a turn (counted by the app from the agent's items; live activity while running), right pane with closable document tabs (changes, Atlas, terminal, file, plan and the work report; `report-details` opens the Report tab, also in the pane's Open content menu) |
| `editor` | | Files & Editor: explorer, tabs, breadcrumb, code, status bars |
| `editor` | `terminal` | The terminal below the editor, drawn like `mockups/terminal`, with fixture output |
| `editor` | `markdown`, `markdown-source`, `samples` | `docs/licensing.md` (bundled from the repository) in Preview or Source, with the floating Preview/Source switch 100 px above the bottom; `samples` opens `design-system.md` with QML, C++, shell and JSON code blocks in syntax colours |
| `changes` | | Source Control: changes, staged changes, diff |
| `teams` | `task`, `large`, `empty`, `ready`, `paused`, `live`, `live-task` | Concept: a team's page (see Teams). `live` is a team as its members write it (choices that are sentences, a long Markdown task description, long lanes and paths, an update that is a paragraph) and `live-task` opens its first task; the page and the dialog must stay inside their width with it. `task` opens a task, `large` adds a fourth developer, `empty` has no team, `ready` has the user merge and T-1 Ready to merge with "Merge T-1 into release/1.4?" waiting, `paused` has used its automatic turns (banner with Resume, and the daemon's Continue? decision with the team's own budget; the header shows the turns used of it). `--open=new-team` (with Permissions: what the leader and the developers may do without asking, Workspace unless chosen; the reviewer stays read-only; and Automatic turns, filled from Settings › Teams), `turn-budget` (the team's own automatic turns, changed while it runs), `team-agent`, `reviewer-agent`, `team-created`, `add-member`, `member-agent`, `remove-member`, `delete-team`, `task` or `handover` opens that dialog, and `repo-menu`, `team-menu` or `row-menu` a sidebar menu, `drag-member` a conversation held over a team's folder; `--scroll=1` starts at Members and Updates |
| `connections` | `no-activity` | This Mac, phones, activity from paired devices (the daemon's remote command audit, #86: done and refused requests, a sender that did not verify) or its empty state, Linux machines (SSH or Relay), pairing dialogs, account. `--open=activity` shows all activity, `--open=device` one phone's details, `--open=device-focus` the same with a truncated row focused from the keyboard |
| `connections` | `signed-out`, `expired`, `sign-in` | Signed out: the introduction (this Mac and KetPlus Mobile, the three pairing steps), the expired-login banner, the browser sign-in dialog |
| `settings` | `typography`, `account`, `teams`, `browser`, `computer`, `providers`, `archived`, `storage`, `updates` | The app's order: Appearance (live theme, follow system, zoom), typography, account, Agent models (opens the model picker), teams (automatic turns per team: what a new team's budget starts from), browser, computer use, providers (CLI status, Claude API access, organization usage), archive, storage (journal size by part, retention, size limit and warning, largest conversations, Compact now), updates |
| `workspace` | `claude`, `codex`, `cursor` | One transcript per provider with every content type its journal produces (see Transcript content types); the composer follows the conversation's provider and model. `--scroll=0.5` starts the transcript part of the way down |
| `workspace` | `plan-ready`, `plan-planning`, `plan-running`, `plan-continuing`, `plan-blocked`, `plan-paused`, `plan-limit`, `plan-complete` | Concept: Plan is a document tab in the existing right pane; the transcript and composer remain visible. Automatic planning from repository context, compact tasks and source references, daemon follow-up, Stop, blocker and budget states. See [the harness preview](marketing/plan-harness/README.md). |
| `workspace` | `content-tabs`, `content-terminal`, `content-file` | Editor-style right pane with two files, two independent terminal sessions and the conversation plan. Closable tabs, horizontal overflow, `+` and open-documents menu (`--open=content-tabs`). |
| `workspace` | `team-assignment`, `team-digest`, `team-review`, `team-verdict`, `team-decision` | What a team sends into its members' conversations, each as one block (`TeamTranscriptBlock`): the developer's assignment (criteria, lane, branch, who assigned it), the leader's digest (ten changes grouped by task, what needs the leader marked), the reviewer's request (criteria and the change in short), the owner's verdict (findings and the round) and the leader's answered decision. Open T-n, and a digest group's title, open the task on the team page |
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
context menu (rename, fork, change worktree, archive, delete), compact window (⇧⌘M),
update notice and toasts. Conversations carry no user tags (ADR 0117).

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

## Teams

A concept, not built in the app: several conversations that share one goal. Agent messaging
(ADR 0027) has no owner of a task, no roles and no shared view of the work; a team adds them.

- **In the sidebar** a team is a folder inside its repository (`qml/shell/TeamFolder.qml`), beside
  the conversations that belong to no team. The folder lists its members' conversations (leader,
  developers, reviewer), each with its role as its only tag. The icon folds it, the name opens
  the team's page, and the count is the decisions that wait for the user. A conversation belongs to one team at a time.
- **Creating and changing a team** starts in the sidebar. A repository's `+` (or a right click)
  offers New conversation and New team…. A team has one leader and one reviewer, always; New team
  asks for a name, a goal, an integration branch, who merges a reviewed task into it (the leader or
  the user; the team menu changes it later), the number of developers (one to five) and the
  model and effort each role uses, from the app's Model & context dialog. The reviewer's dialog
  offers Claude only, the provider that keeps a review read-only (ADR 0067). Create starts every
  member as a named conversation (Lead *team*, Developer 1, Reviewer 1); developers have no lane
  until the leader gives one. Add member starts one more developer: its lane, its agent and a
  required name. A conversation that already exists joins without a dialog, as a developer:
  dragged onto the team's folder, or from its menu's Add to *team*. The role is the
  conversation's only tag, locked, in the sidebar and in its header; the team sets it and the
  conversation cannot change it. A member's header has no Reviewer choice: its
  team's reviewer reviews it. A team folder shows Add member and its menu (Open team,
  Add member…, Delete team…) while hovered or focused, and on a right click; a conversation's menu
  has Add to *team* or Remove from *team*…. The page repeats them: Add member, a member's Remove
  from team… and the team menu's Delete team…. Deleting a team or removing a member keeps the
  conversations; a removed member's open tasks become unassigned. A team keeps its leader, its
  reviewer and at least one developer, and has five developers at most; an action that would break
  that is disabled and says why. A conversation joins only a team of its own repository. The
  conversations New team and Add member start are kept beside the fixtures' (`Main.started`), so
  they outlive their team.
- **Checks**: `--smoke` drags a conversation onto a team's folder with the pointer, then runs
  `TeamsScreen.selfCheck`: a removed member and a deleted team leave their conversations, team ids
  stay unique after a deletion, one to five developers, no join across repositories.
- **The page** (`qml/screens/TeamsScreen.qml`) answers three questions in order: what waits for
  the user, where every task stands, and who holds what.
- **Leader**: one conversation that assigns tasks, orders the merges into the team's integration
  branch and is the only one that reports to the user. It also writes the requirements (the
  acceptance criteria of each task) unless New team turns that off. It neither codes nor reviews.
- **Developer**: owns a lane, a named set of path prefixes. An edit outside the lane is flagged on
  the task ("Outside its lane"); the leader decides who makes that change.
- **Reviewer**: one conversation that checks every lane against the acceptance criteria and never
  merges. It keeps up with two or three developers; at four the page says reviews will queue and
  suggests a second team (`teams/large`).
- **Task**: one owner, who also holds its worktree; To do, In progress, In review, Blocked or Done;
  its branch, worktree, pull request and what it depends on. Only the leader changes the
  acceptance criteria, and a change is recorded in the task's history. Hand over changes the owner,
  who receives all of that.
- **Waiting for you**: a question that changes the product waits here with its choices instead of
  in one conversation. The answer goes to the leader and to the owner of the task it blocks.
- **Updates**: task changes wake nobody; the leader reads them as one digest when its turn ends.

- **In a transcript**: the daemon's messages to a member (assignment, digest, review, verdict,
  decision) are blocks, not paragraphs (`workspace/team-*`). Each is one accessible item named by
  its kind and title. A client that does not know a kind shows the message body as text.
- **Ready to merge**: a task that passed review with every criterion met. Under the `user` merge
  setting the team asks "Merge T-n?" in Waiting for you (`teams/ready`).
- **Paused**: a team that used its automatic turns says so above its decisions and asks whether to
  go on (`teams/paused`).

Not drawn yet: a team on the phone. ket-qt has the group header whose name activates instead of
folding (ket-qt #47); Két Symbol still needs a team icon (the mock uses `bot`).

- **Content as a team writes it** (`--state=live`). The page never scrolls sideways. A decision
  whose choices are sentences (any choice over 28 characters) lists them as rows, one choice a
  row, the recommended one marked and first; short choices stay buttons. A task's dialog is 640
  wide and scrolls; its description is Markdown, cut at about seven lines with Show all. An
  update shows three lines and Show more. Below 860 the role moves beside the member's name and
  the status column goes; lanes, paths and titles elide.

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

## Sidebar tree (concept)

The repository list is a tree, drawn by mock-local parts until the design is approved; they
then move into ket-qt (both layers) as a tree row beside `KetGroupHeader` and
`KetConversationRow`. Two earlier concepts were dropped: second lines with tag chips (#535),
and a flat list of labels without icons or guide lines. The tree follows Jay's design idea,
without its "Teams (n)" level: a team is a branch of its repository, directly.

- **A repository is the root** (`qml/shell/SidebarRepoHeader.qml`): a chevron that folds it,
  a folder, the name in small capitals, and the count of its conversations, which stays.
  Folded, one dot says that something inside waits for the user. `+` shows on hover or focus,
  left of the count, and offers a new conversation or a new team.
- **A team is a branch** (`qml/shell/TeamFolder.qml`): the chevron folds it, the name opens
  the team's page, the count is its members. A warning mark before the count and a semibold
  name say that a decision waits for the user. The highlight of a selected team starts right
  of the chevron.
- **"Conversations (n)"** (`qml/shell/SidebarSection.qml`) labels the conversations that
  belong to no team, with `+` for a new one. It shows only where the repository also has
  teams; a repository without a team lists its conversations directly.
- **Every conversation is one line** (`qml/shell/SidebarRow.qml`): the state's own mark and
  colour, the title, `#number`. Finished and unread is a dot and a semibold title. Role,
  state in words, what started the turn and worktree are in the hint of a resting
  pointer and in the accessible description.
- **Guide lines** run under a chevron's centre for as long as what it holds: one for the
  repository, one for each team. A row's highlight starts right of its guide line.
- **Two text edges.** The repository's name, the "Conversations" label and the titles of its
  own conversations start together; a team's name and its members' titles start together,
  one level in, and a team's icon stands over its members' marks. `--smoke` checks these
  edges and that every conversation is one line high (`Sidebar.layoutProblems`).
- "Show N more" says how many it adds.

`--open=repo-folded` folds every repository for a capture of the labels alone.

The supplied KetPlusMark PNG is the approved mark in the sidebar,
Settings and transcript (`assets/ketplus-mark-v2.png`). Its 64/128 px derivatives
use explicit `-64` / `-128` names so Qt cannot select a small `@2x` image for the
1024 px source on Retina displays. No separate asset licence was supplied with
the new PNG; this mock does not assert a reuse licence or grant trademark rights.
The product owner must settle the asset licence before release distribution.

The name KétPlus is a trademark and is not licensed. The Mermaid renderings use
the Két Mermaid theme from Két Design System (MIT,
`core/resources/mermaid/KetDesignSystem-LICENSE.txt`).

## Approved brand reference

Open **Brand** in the sidebar (`--screen=brand`). It compares Lite/iOS and Pro at
17, 32 and 64 px on light and dark backgrounds, the mark at 16–128 px, 60 pt iOS
icon tiles. Jay approved the new mark and **`ketPlus`** casing on 2026-11-12
(team decision D-5). The wordmark uses Bold 700 and the mark is 1.45× the
wordmark pixel size so the icon has more presence.

The supplied KetPlusMark PNG is cropped to artwork bounds (alpha ≥ 8/255 ignores
near-transparent isolated noise), centred in a square with a 4% margin on each
side of the final square (rounded to whole pixels), and resampled to 1024, 128
and 64 px. The artwork's hue is shifted from
its original hue to the interface accent blue (hue 228.7°, the kit's `accent-text`); only
hue changes, saturation, value and alpha are kept, and all three sizes come from
the one 1024 px master.

`BrandLockup` owns mark geometry and wordmark spacing. KÉT Sans Bold (700,
registered by Qt as `KET Sans`) is embedded; smoke checks the resolved family,
Bold face and weight. `ket` follows the main text colour, `Plus` the kit's
`accent-text` token (`#1238DB` light, 7.4:1 on the sidebar `#F7F5F5`; `#9EB5FF`
dark, 8.65:1 on `#171B20`), with −0.015 em letter spacing. Pro uses KÉT Sans Bold (700) at 0.6×,
a transparent rounded frame with a 1.5 px stroke scaled with interface zoom,
0.35 em horizontal / 0.12 em vertical padding, and a 0.3 em wordmark gap.
The frame and Pro text use the same token; there is no separate brand colour.

The Brand navigation entry retains the approved lockup specimens.
The badge is centred on the wordmark’s x-height; its fractional
stroke disables pixel alignment to preserve 1.5 px at 100% zoom. Jay’s direct
review changed the original Black 900 requirement to Bold 700 and increased the
mark relative to the wordmark. A further review tightened the icon-to-wordmark
gap from 0.35× to 0.12× mark height. The unused Black face is no longer embedded.
