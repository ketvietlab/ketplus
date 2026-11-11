# Agent Workspace runtime

History loading and scroll preservation: [chat pagination](chat-pagination.md).

Desktop transcript body, outgoing bubbles and composer share the mobile design-system
body leading (`contracts/mobile-design-system-v1.json`): 1.45 times the configured
body font size. Native iOS renders assistant markdown at 1.46 (Inter 16 pt, natural
1.21 em plus 4 pt spacing) and outgoing bubbles at 1.50 (Inter 15 pt plus 4.28 pt).
Heading and code-block leading retain their own typography.

Outgoing bubbles stop at 80% of the reading column. Messages longer than 20
rendered lines start collapsed with Show more/Show less; the full source remains
available to copy. The composer keeps Stop visible during an active turn and
Send visible beside it, disabled until text or an attachment is present. An
image alone can steer the current turn: the daemon validates and journals the
image before passing it to the provider. Waiting-for-input conversations use a
pause-circle status icon, matching the mobile clients.

Composer line-height formatting opens an edit block only when a paragraph needs
updating. QML schedules formatting and draft persistence only when the plain text
changes; formatting-only notifications must not restart the zero-delay timer.
Font changes still reformat the document, and formatting joins the text edit's
undo step. The idle composer regression verifies that document notifications settle.

## Context compaction

`contextCompaction` item start/completion events update one lifecycle row and the
conversation status. The active status remains `Compacting context...` until all
active compaction items finish; reasoning updates do not overwrite it. Completion
keeps the row as `Context compacted` and does not mark the turn idle prematurely.
Interrupted or disconnected operations are not presented as successful compaction.
Daemon live events and retained windows use the same compaction item id. Restoring or
refreshing a window reconciles current-turn compactions, including completions received
while another conversation was selected. Later reasoning cannot revive a completed
compaction status, and refreshing a still-running compaction preserves its status.

`/compact` calls `thread/compact/start` for an existing, idle Codex conversation.
It is a client command, not a model prompt. Arguments/attachments and use during
an active turn are rejected. The RPC acknowledgement alone does not indicate
completion. Older servers return a visible error; there is no prompt fallback.
Saved compaction items are also shown when conversation history is restored.

## Current behavior

Desktop read acknowledgements use the latest event cursor received by the daemon
transport for that session generation, rather than the older catalog snapshot.
Opening a completed conversation and switching away keeps it read; a subsequent
background completion still makes it unread. This is covered through a real local
daemon with a fake provider, without consuming provider quota.

New conversations receive a short title from the first request. When a pasted
handoff contains an explicit `User request:` line, that request supplies the
title instead of the handoff instructions. Provider-suggested titles use the
same length limit; manually renamed titles remain under user control.
On daemon startup, a legacy title containing both `User request:` and a pasted
response-preferences block is shortened and persisted once.

The product sidebar lists recent repositories. Clicking a repository name
collapses or expands its conversations; expanded groups show the five newest
threads and keep running or selected ones visible. Its add action opens a folder
chooser; recent linked worktrees
are grouped by their main repository path so each repository appears once.
The sidebar uses the main repository name even while a linked worktree is active.
Spotlight searches only files and Atlases; it never switches repositories or
worktrees and has no `@` scope filters. Repository-name lookups run off the GUI thread and only when the workspace path
changes; replay/status updates must not launch synchronous Git work on the UI.
Pickers first read persisted SQLite metadata, then refresh names from background
Git validation. See [ADR 0004](decisions/0004-repository-navigation-metadata-cache.md)
for cache location, invalidation, and the separate action-time routing checks.

A circular usage ring sits at the bottom-right of the composer, next to Send,
and opens the context-and-limits panel. The sidebar footer still shows the weekly
account quota as an inline summary when that value is available. Context data is
shown only when the active provider reports an exact current-window value; it is
never inferred from cumulative token usage.
The panel's 5-hour, weekly, and model-scoped rows are account-level values, not
per-conversation counters. The Compact session action is available only for an
idle Codex conversation with reported context data and submits the existing
`/compact` command. A usage snapshot carries its provider id; a snapshot for a
different provider is never rendered. Conversation rows store the selected
provider, so the usage ring and plan-limit rows follow the active conversation
instead of always showing Codex weekly quota. Codex weekly quota is shown only
for Codex conversations; Claude and Cursor show unavailable plan limits until
those providers report their own account usage.

The footer follows the active conversation's provider, falling back to the first
visible conversation when none is active; an empty list hides the footer. Legacy
conversations without provider metadata are Codex conversations. Unsupported
providers show unavailable quota, never another provider's values.

Codex quota uses the existing app-server connection: `account/rateLimits/read`
and `account/rateLimits/updated`. Only the `codex` bucket and a 10,080-minute
window qualify as weekly quota; the percentage is used, not remaining. Refresh
runs on initial workspace creation, reconnection, every five minutes while ready,
and at reset. Account changes clear old values. Missing data, errors and expired
windows show `--`; requests time out after 15 seconds without disrupting chat.
Hover the quota bar for the local reset time.

Claude's usage panel separates SDK session accounting from organization reporting.
`usage.session` carries only the captured authentication mode, configured query budget,
SDK-estimated session cost and cumulative per-model input/output/cache tokens (including
subagents). It replaces the previous reading; result totals are never summed. Restored
session history may already exceed the current query budget, whose counter excludes
that restored spend. Accordingly the panel shows the two amounts separately, with no
misleading spent/budget percentage. A new query clears its old accounting reading until
SDK data arrives. API-key mode hides subscription windows. Context occupancy remains
independent. These safe session snapshots are journaled and restored with the conversation.

A separate Admin API key in Settings → Providers enables desktop organization usage.
The existing usage popover requests the UTC calendar-month-to-date cost and token report
on opening or Refresh. It covers all products and keys in that Admin key's organization,
not just KétPlus or the inference key (which may belong to another organization).
Cost is after discounts and before credits, not a credit balance. Credit balance and
account spending limits remain in Anthropic Console; no inferred balance is displayed.
Organization reports are available locally and through the paired iPhone read-only
configuration extension. They are cached in host memory for five minutes, and show the
report's data freshness timestamp. Errors never appear as zero usage. See ADR 0026.

Context occupancy is the current window only, never lifetime or per-turn totals.
Codex uses `thread/tokenUsage/updated` (`last.totalTokens` against
`modelContextWindow`). Claude uses Agent SDK `query.getContextUsage()`
(`totalTokens` / `maxTokens`), with a result `contextUsage` fallback. Cursor
uses ACP `session/update` `usage_update` (`used` / `size`), and a prompt result
only when that result includes a window size. The daemon journals each snapshot
as `usage.context` so the composer panel can restore it with the conversation.
A conversation stays at Not reported until its provider has sent that
current-window value.

- The Codex CLI is discovered from `PATH`, `~/.local/bin/codex`, Homebrew, or
  `/usr/local/bin/codex`.
- The Cursor CLI is discovered from `PATH` and `KETPLUS_CURSOR_AGENT`. On
  macOS/Linux the fallbacks are `~/.local/bin/agent`, Homebrew, and
  `/usr/local/bin/agent`. On native Windows the installer puts `agent.cmd` in
  `%LOCALAPPDATA%\cursor-agent`, which GUI apps often miss even after a
  successful install, so the daemon probes that folder and launches `.cmd`
  through `cmd.exe /c` (`.ps1` through PowerShell) without a console window.
  KetPlus never bundles the CLI. Missing CLI shows the platform install command
  (`curl … | bash` or `irm … | iex`). A signed-out CLI tells the user to run
  `agent login` (and the `agent.cmd` path on Windows if `agent` is not on PATH).
  The Cursor picker lists models from a separate `agent models` process. CLI
  ids that only differ by effort (`-low`, `-high`, `-xhigh`, …) are grouped into
  one family; Fast variants are omitted when a non-fast sibling exists. Selecting
  a family pins ACP with `--model` and `session/set_config_option` using the CLI
  id for the chosen effort. The effort slider appears when a family has more
  than one variant. Cursor ACP Auto is config value `default` (CLI still lists
  `auto`). Setting `auto` or `auto-smart` over `session/set_config_option` is
  invalid. Auto has no Cost/Balance/`optimize_for` parameter on ACP; those exist
  only in Cursor desktop. Composer 2.5 and Grok expose a `fast` select that
  defaults to on, so KetPlus sets `fast=false` unless the user picked a Fast
  CLI variant, and sets `effort` when ACP advertises it. Newer ACP catalogs
  encode those flags in the model value itself (`composer-2.5[fast=false]`);
  KetPlus prefers that advertised value over the bare id.   ACP Auto at a usage
  cap, or a Free-plan CLI refusing named models (`Upgrade your plan`, `Named
  models unavailable`), is journaled as a `runtime.error` that names the Agent
  CLI account from `~/.cursor/cli-config.json` and shows `agent login` — Cursor
  desktop and Agent CLI use separate logins. KetPlus does not silently switch to
  Composer. A following `turn.completed` failed event without an
  error object must not add a second transcript row of `{}`. Cursor ACP
  `cursor/ask_question` requests are blocking: KetPlus presents a structured
  Question card and returns the selected option ids, never acknowledging the
  request as skipped. Tool/file approvals use the mock's Approval card:
  "Approval needed · <tool>" (Command, File change, `server.tool`, Claude or
  Cursor tool), the reason, and the whole command in a code block, with Allow
  once and Deny. A settled approval is one note: Approved, Denied or Approval
  expired, then the tool and command. `approval.resolved` records
  `answers.decision`, so a restored transcript and other clients show how it
  ended; older journals read "Approval closed". Question and
  Approval rows are never folded into an activity group: one that arrives
  between commands splits the group so the card stays on screen. Both cards
  show the full question text, and an answered card is read-only. Expired unanswered
  questions remain in canonical history but occupy no visible row or transcript
  geometry, including on history/cache reload. Permission-approval receipts stay visible. A
  declined approval reads as declined, not with the granted checkmark. Claude exposes
  tool permissions but no structured user-question surface through its Agent
  SDK sidecar. Claude lists models from the Agent SDK sidecar (`supportedModels`), falling back to
  the `opus` / `sonnet` / `haiku` aliases when that listing is empty.
- KetPlus starts one lazy `codex app-server --stdio` process for the application.
- Every canonical worktree path owns an independent conversation registry. Each
  conversation retains its transcript, status, and Codex thread id.
- Each Agent Workspace view owns one active `AgentMessageModel` and one
  `TranscriptView` ListView. Session storage owns conversation history; switching
  does not retain hidden models/delegates. `TranscriptMessage` owns the message
  layout and `TranscriptActivityRow` uses the renderer-family registry. Scroll
  state is independent of content growth; viewport anchors remain keyed by scope.
- Activity expansion and gallery batch size are remembered by
  conversation/message key, bounded to 256 recently used entries. Detail/payload
  roles above 32 KiB contain references, not full JSON. The resolved-payload cache
  is limited to 64 entries/4 MiB. Evicted/oversized entries are fetched lazily from
  the owning active session and checked against the reference hash; stale scopes,
  unknown fields and changed content are rejected. Oversized payloads are not
  retained in this cache. Shell commands that are clearly file reads or code searches show a
  compact semantic label (file name, line range, or query) in the activity timeline. Build,
  test, and common Git commands also receive short action labels. Opening a command receipt
  shows its sanitized original command, working directory, exit status, duration, and a
  scrollable local-only stdout/stderr stream; output follows the live command, keeps only its
  newest 128 KiB, and remains excluded from cloud/mobile projection.
  Thumbnails decode asynchronously in batches of 12.
  This bounds retained view data, not total canonical history or transient parsing
  cost; load benchmarks remain a separate cutover gate.
- Changing worktrees swaps the visible session. Turns in other worktrees keep
  running and remain marked in the worktree list.
- Final answers and public progress commentary stream into existing transcript rows;
  private reasoning remains hidden. The one exception is the running line: while a
  turn works it reads "Working · <heading>", where the heading is the bold title at
  the start of the provider's latest reasoning summary, first letter lowercased
  (#385, as Codex CLI shows it). The summary's body and the reasoning content stay
  out of the line, the transcript and its copy; the heading clears when the turn
  ends. Each conversation keeps its own heading and the line shows the selected
  one, so another conversation in the same worktree never changes it. The heading
  appears when a reasoning item completes; streaming it from summary deltas is
  #397. Command activity updates
  its compact receipt without rendering output. The transcript viewport leaves a
  fixed 20 px gap above the composer in idle and streaming states.
  Streaming preserves reading intent: a viewport following the bottom continues
  following new content. Ending a scroll within 32 logical pixels of the bottom
  resumes following regardless of scroll direction; farther away preserves the
  historical reading position.
  The run-status footer shares the rows' visual anchor compensation.
  The Working indicator uses the kit's shared stepped animation clock. Restore
  motion and conversation shimmer remain disabled. Indicators stop while idle,
  restoring, hidden or minimized; the shared clock slows when the app is inactive.
  Set `KETPLUS_REDUCED_MOTION=1` before launch for the static control. CPU savings
  remain unverified until measured on the same visible transcript and hardware.
- Assistant bodies above 16 Ki characters become independently virtualized
  Markdown block rows. Pathological snapshots are segmented away from the UI
  thread and published from the retained edge in bounded batches. Daemon-backed history keeps a bounded selected window;
  evicted edges remain reloadable from the durable local journal.
- Opening a daemon-backed conversation for the first time reads and publishes its
  newest turn first. The preceding two turns load afterward; later pages stay lazy.
- Returning to a conversation whose retained window is still current shows that
  window as it was, without a daemon read. The window is current when nothing
  newer was trimmed off, no event of the conversation arrived while it was
  inactive, and the daemon client still holds a ready window for it. Re-reading
  it would replace the reader's rows with the newest turn and then prepend the
  rest: two more transcript rebuilds and a lost reading position.
- Any other return (new events while inactive, a trimmed newer edge, a reconnect
  or an expired cursor) reads the newest three turns in one request while the
  cached rows stay visible, then replaces them once. A running conversation no
  longer replays a long inactive gap before showing its latest page. Cached
  conversations remain sendable while the newest page refreshes. Live rows and
  assistant deltas newer than that page's snapshot survive its asynchronous
  installation.
- `ketplus_conversation_return_benchmark` (`KETPLUS_BUILD_BENCHMARKS=ON`, Release)
  measures A -> B -> A through the real manager, view and an in-process daemon on
  its own thread: transcript replacements, daemon reads, GUI busy/CPU time, the
  longest GUI stall and frame gaps. Run `clean` (nothing changed) and `dirty` (the
  window was invalidated while away). The window stays on top because macOS stops
  rendering occluded windows; rounds without frames are not valid.
  `KETPLUS_BENCHMARK_SCREEN=<name>` places it on another display.
- KetPlus enables Codex's `default_mode_request_user_input` feature when starting
  the app-server, so both regular and planning turns can ask structured questions.
  `request_user_input` pauses the turn on an inline Question card. Questions are
  presented one at a time with selectable options, optional free-form/secret
  input, Back navigation, and a compact answer receipt after the final submit.
  Switching worktrees or conversations keeps the pending card with its owning
  session. Stopping the turn dismisses unanswered requests so the app-server is
  not left waiting.
- Codex async assistant questions are scoped to the turn that emitted them. Completion,
  interruption/provider stop, or a new turn expires unanswered async cards through the canonical
  `question.expired` event. They remain read-only in history and cannot reopen from a late item
  replay. A queued answer can steer only its original active turn; it cannot start a later turn.
  Blocking provider approval/input requests keep their existing arbitration semantics.
- Pending question drafts are retained only in a bounded in-memory cache keyed by
  transcript scope and request identity. Delegate recycling and conversation
  switches therefore preserve non-secret and secret input without writing either
  to settings; answered, expired, and evicted requests discard their draft.
- Streaming presentation is coalesced independently of canonical session state;
  see [chat pagination](chat-pagination.md) for latency and flush semantics.
- Repeated Git/worktree status refreshes on the same active path do not rescan
  skill files. Switching paths and explicit skill-picker refresh still reload
  discovery; ordinary Git status updates are not skill-discovery triggers.
- Consecutive tool steps within the same turn collapse into a muted summary line.
  Browser-only groups read as `Used the browser`; text-only observations join the group.
  `browser_capture` and `visual_compare` always stay outside groups, including while
  running or when evidence is unavailable. Any other tool returning images also stays
  outside, so its evidence remains visible. Command output remains collapsed,
  including failed commands.
  Opened groups hang under a rail. User requests have a You label and accent bubble;
  final answers omit the provider heading. As in Claude, an agent turn has one
  copy action, shown on hover or keyboard focus below its last answer once the turn
  is done; it copies the text the agent wrote in that turn, without commands, files
  or tool steps.
  Browser targets offer a compact Open action. Conversation export remains at the
  right of the footer, separate from answer copy.
- Deferred activity details resolve from the external payload owner first, then the
  retained canonical row with the same payload hash. This keeps large command output
  expandable without eagerly rendering it.
- Transcript copy buttons (request, response and code block)
  share `TranscriptCopyButton.qml`. A click swaps the copy icon for Lucide's
  check and the label for "Copied" for 1.5 s, then the button returns to idle.
- Transcript text is selectable per message. A press into the transcript gives the
  embedded Quick window keyboard focus, so Cmd+C copies the selection and Cmd+A
  selects the focused message even while an agent detail editor is open. While
  any agent workspace (including the Atlas chat) holds focus, the Edit menu's
  commands stay enabled and act on the focused transcript text or composer: AppKit
  claims the key equivalent of a disabled menu item without delivering the key,
  so a disabled Copy swallowed Cmd+C. Read-only transcript text claims Copy and
  Select All in `ShortcutOverride`, which Qt consults before firing the menu item;
  a command chosen from the menu is sent directly to the focused Quick item as
  its shortcut. It must not pass through the Quick window's shortcut map: that
  reactivates the same menu action and can overflow the main-thread stack.
  Selection cannot span messages, because each message is its own text item.
- Daemon approval resolution/expiry closes a restored pending question and blocks
  resubmission. When the journal supplies only the approval ID, the receipt says
  closed rather than inventing an answer. Copy/export uses sanitized presentation
  data and redacts secret answers; replay never opens resource links automatically.
- Typing `$` in the composer offers matching project and user skills. Selecting
  `$skill-name` attaches that skill's `SKILL.md` instructions to the submitted
  turn, including steering input, without enabling it persistently. The
  transcript shows only what the user typed: `session.start_turn` and
  `session.steer` carry it as `display_text`, which the daemon journals while
  the provider receives the wrapped prompt. `/skills`
  remains the workspace-level manager for skills that should accompany every
  turn. A project skill shadows a user skill with the same invocation name, and
  both shadow a built-in skill. KetPlus ships `ket-atlas`, `enterprise-grade` and
  `ket-design-system` as built-in skills, and the host policy tells every agent
  when to read them without a mention (ADR 0021).
  Switching worktrees rescans the skill folders on the thread pool. Until that
  scan lands the previous list stays, and a prompt or steering input sent in that
  window rescans synchronously first, so it never carries another worktree's skills.
- Every daemon conversation shows its number (`#4821`, ADR 0027) at the start of
  its sidebar row and before the header title. Typing `#` in the composer offers
  numbered conversations by number or title; the daemon lists the ones a
  message names for the agent, which contacts them through `ketplus_agents`
  only when asked. A turn another agent started shows its message as a
  user bubble under a muted "From @agent/1093 · title · provider" label
  (reviews keep "Review request from …"); a prompt's images lead its bubble and a steer adds "Sent
  while <provider> was working" below it, naming the provider its `turn.steered`
  event records (older steers read "the agent"). The sender's transcript adds the mock's note for
  each message it sent: "Message to @agent/1093" (or "Reply to", "Notice to"),
  then "title · provider · Sent · reply requested" and the first 500 characters
  of the text, which the daemon journals on the `sent` event. Later states of
  that message (held, expired, not delivered) and its reply ("Replied") update
  the same note, live and within a restored page; a reply in another page
  leaves the note at its last state. Incoming held messages and reviews keep
  their notices. While a
  turn another conversation started is running, its sidebar row ends with a
  muted "from #4821" and the tooltip names that conversation.
- Typing `@` offers files and folders of the conversation's worktree (tracked and
  untracked, not ignored). Choosing one inserts `@path` (`@"path"` when it has
  spaces; folders stay open so their children can be picked). On submit and
  steering, mentions that resolve inside the worktree are listed for the agent
  as referenced paths. A mention that is absolute, home-relative, `..`-relative
  or a symlink leading out of the worktree blocks the submission and keeps the
  draft; text such as `@handle` that names no path stays plain text.
- Text files opened from chat links use closeable Agent detail tabs beside the
  conversation. Links come from model output, so only viewer types (images
  including SVG, PDFs, audio, video) open through the operating system's default
  application, after a confirmation when they are outside the execution worktree.
  Directories, application bundles, executables and every other type are revealed
  in Finder/Explorer instead (`MainWindowTranscriptLinks.cpp`). `http`, `https` and
  `mailto` links open; other schemes ask first, showing the URL, and UNC or
  `file://host` links are refused before any file access. Inline-code file
  references resolve suffixes against the background `@` mention listing
  (`git ls-files -z`), so formatting never runs Git on the GUI thread; when that
  list arrives, shown messages are formatted again (`fileLinksChanged`). Code spans
  starting with `//` or `\\` are never looked up, so rendering cannot reach a UNC
  share.
  Transcript activation checks MIME type and a bounded content sample before
  entering the editor route; text links retain source-line navigation. Explicit
  editor and Git diff actions are unchanged. The detail-tab context menu can close
  one tab, the other tabs, or all tabs. A text-file link outside the current workspace is routed to its owning
  repository and exact linked worktree; an existing repository window is reused,
  or a new window is opened when needed. Agent detail tabs never reveal the main
  editor split as a third column. With focus in a detail editor, Find/Replace (⌘F, ⌘⌥F)
  opens the shared find bar under the detail tabs and Go to Line (⌘L) moves
  that editor; with focus in the transcript, Find searches the conversation. Inline Markdown links remain directly clickable
  while response text stays selectable. Bare HTTP(S) URLs in assistant prose are
  converted to explicit autolinks before Markdown import, including loopback URLs,
  ports, query strings and fragments. This uses the existing link hover/open route;
  code spans/blocks and explicit Markdown links keep their original meaning, and
  copy/export keeps the original source. The row measurer uses the same preparation.
  Responses retain distinct paragraph, list, and heading
  spacing for long-form answers. Headings stay close to the body size (1.25x for
  H1, 1.125x for H2, body size below) at semibold, bold text is semibold, and
  inline code uses the editor font at 0.875x the body size minus 1 px (code
  blocks at the body size minus 2 px, 10 px minimum for both), instead of Qt's
  Markdown defaults of 2x headings and bold weight. Raw HTML in an answer is
  shown as text: `chat_markdown::escapeRawHtml` escapes any `<` that could open a
  tag outside code before Qt imports the Markdown, because an unclosed tag such
  as a `<title>` placeholder hid the rest of the answer except its code spans.
  The row measurer applies the same escaping. The bare-URL autolinker stops at a
  backslash, so a URL just before an escaped tag still closes its autolink. Code keeps
  the body colour. Inline code is plain monospace text, as in the kit and the mock,
  with no chip or extra tracking. Code blocks keep their frames.
  Nothing a model writes reaches the network. `installLocalOnlyNetwork` gives the
  transcript's QML engine a network manager that serves only `file:`, `qrc:` and
  `data:` URLs and fails every other request before any lookup. An inline
  `![alt](https://…)` is shown as a link to the image (`chat_markdown::prepareForDisplay`).
  A file path that names another machine (`file://host/share/a.png`, `//host/share/a.png`,
  `\\host\share\a.png`) counts as remote: opening it would connect to that host, on Windows
  over SMB with the account's credentials. `transcriptMayOpen` allows such a path only inside
  the conversation's worktree, when that worktree is on the share itself.
  A reference-style remote image stays a broken image that is never fetched. An inline
  local photo is drawn as an 88 px thumbnail, and the document keeps it at that size
  for the screen's density instead of the full decode. Links take the kit's link
  colour on every format pass, so they follow a theme change. Qt's Markdown importer
  had kept the colour from the theme in use when the answer was parsed.
  The layout trace (`KETPLUS_CHAT_LAYOUT_TRACE`) is off unless that variable names a
  file. It records geometry and keys, never message text.
  Their fill and border come directly from `KetTheme.codeBlockRecipe`, using the
  kit's panel-subtle surface in each theme. Appearance-driven rich-text formatting
  waits until QML font and width bindings settle before measuring overlay frames.
  `TranscriptTableStyle` removes Qt 6.11's extra empty paragraph in a populated
  table cell after a code fence, preserving intentionally empty cells and header
  emphasis. Table padding owns cell spacing; code frames retain separation from
  the following table. The transcript and
  composer share one reading column, 760 px by default: as in the mock, answer
  text, code frames and the composer span the whole column, and rows keep a
  20 px inset from the pane's edges only when the pane is narrower. Settings > Typography >
  Conversation width offers Wide (864 px, close to Claude's wide reading column),
  Extra wide (1200 px) and Full width; a Wide saved as 960 px before it was narrowed
  still reads as Wide. Like the other Typography fields, the choice applies as it is
  made (ADR 0103), and Restore defaults covers it. It is stored in the desktop app's
  `agent/conversationWidth` setting and applies to the AI workspace only; Atlas's
  embedded chat keeps the default column.
- Changed-file cards show paths from the worktree directory instead of the full
  machine path. Left-click opens the diff in the owning repository/worktree, while
  the context menu can copy the absolute path, reveal it in the platform file
  manager, or open the complete file in the owning KetPlus repository window.
- Stop sends `turn/interrupt` to the selected conversation. New conversation
  keeps existing conversations available, including ones still running.
- Conversation ids, thread ids, titles, and the active selection are stored in
  `QSettings` and resumed after restart.
- The product sidebar is organized repository > conversation > worktree. Clicking
  a repository name collapses or expands its conversations; a repository with no
  conversations still opens that folder. Conversations across all of a
  repository's worktrees are ordered by when their latest turn started, so
  sending a message, from the desktop or a phone, moves that conversation to
  the top; ties and conversations that never started a turn follow by title.
  Repositories are ordered by their newest conversation, then by name, so
  opening or switching one never reorders the list. The start time
  (`lastTurnStartedAt`) is the daemon's: the journal stamps it on the session
  with each `user.message`, `session.list` reports it, and the heartbeat seals
  it into the session metadata for phones, which apply the same order. Each expanded
  repository shows the five
  newest conversations and keeps any running or selected conversation visible,
  with Show more revealing the rest. A completed background conversation is
  marked with a blue check until it is opened; Desktop persists that read marker
  in the daemon journal (`last_seen_cursor` via `session.mark_seen`) so rebuilds
  and relaunches keep the same unread state. Opening the conversation, or
  completing a turn while it is selected, advances the marker. A later background
  completion makes it unread again. The sidebar rotates the working indicator,
  drawn in the main text colour, while an agent turn remains active.
  Collapse state is stored in `QSettings`.
  Opening a conversation switches the window to the worktree it runs in. A
  conversation owned by another repository routes to that repository's window.
  Activating that worktree publishes the chosen conversation directly, so the
  transcript resets once instead of first showing the worktree's previous
  conversation.
  Right-clicking
  a conversation of the open worktree exposes Fork this conversation, Archive and
  Delete actions. Right-clicking the transcript offers Copy (the selected text),
  Copy conversation (Copy loaded conversation while a turn runs, Cancel copying
  conversation while history is fetched, like the footer button) and Fork this
  conversation for the active conversation; rows with their own menu (changed
  files) keep theirs. Forking is enabled only for a started, daemon-backed
  conversation whose provider reports `threadFork` (Codex and Claude, not
  Cursor). The fork is titled "<title> (fork)", keeps the worktree without
  owning it, and opens selected (ADR 0016). Running conversations may be forked;
  the source keeps running while the copied transcript is settled in the ready fork.
  A fork contains the provider history available when its native fork call executes,
  not future output from the source.
  Deletion confirms the local transcript, optional removal of every worktree
  stored for that conversation, and optional force-delete for those worktrees.
  Deletion failures open a visible error dialog even when the status bar is
  hidden. Confirmation keeps the original worktree/conversation identity;
  asynchronous failure retains the conversation rather than hiding its history.
- Daemon discovery restores that persisted conversation registry before merging
  durable sessions, so a reconnect cannot replace UI conversation ids or custom
  titles with default placeholders. Non-custom titles follow the daemon journal.
  Renaming an attached conversation updates both the daemon journal and Codex's
  user-facing thread name; provider title changes flow back through normalized
  daemon events.
- Daemon transcript restore prioritizes the selected conversation, then the
  other conversations in its worktree, before continuing through cold sessions
  in the background. A loading overlay covers only the selected chat pane; the
  worktree and conversation sidebar remains interactive. The render-first state
  names the operation, keeps an accessible Settings action available, and does
  not expose machine paths. Selecting a cold
  conversation reprioritizes it and shows the same pane-local overlay until its
  replay finishes. The overlay uses the KetPlus mark instead of a generic spinner.
  Set `KETPLUS_REDUCED_MOTION=1` before launch to keep status semantics while
  disabling the repeated mark and working-indicator animation.
- The daemon maintains a durable compact replay projection alongside the raw
  journal. Completed assistant items replace their streaming deltas in that
  projection, so startup does not repeatedly scan and parse the full event log.
  Replay remains serialized across sessions and each response is bounded by both
  event count and encoded byte budget to protect the local IPC queue.
- Atlas Builder creates a normal worktree conversation with an `Atlas:` title,
  so its transcript is available from Agent Workspace. Its compact presentation
  hides worktree and conversation navigation while retaining transcript,
  composer, model, permission, skills, attachments, stop, and question cards.
  Selecting a builder tab restores that builder's conversation as the active
  conversation for the worktree.
- The worktree add button asks for a new branch name and a searchable base
  branch selected from asynchronously loaded local/remote-tracking refs. New
  conversation opens a wider dialog with a Base worktree selector and an
  optional Create new worktree field (placeholder: branch name). Leaving the
  field empty starts the conversation on the selected worktree. Entering a
  name creates a linked worktree from that checkout's HEAD. Duplicate branch
  names or existing worktree paths stay in the dialog with an error asking for
  a different name. Creating a worktree this way switches to it and keeps that
  conversation linked to the worktree.
  Opening New conversation also runs `git worktree list` again and updates the
  Base worktree list in place, so a worktree made outside KetPlus (another agent
  or a terminal) appears without reopening the folder. The worktree already
  picked stays selected.
  There is no Worktrees page (ADR 0111): a worktree is created for a
  conversation and switched from inside it. Conversation deletion confirms
  whether to remove linked worktrees and whether to force that removal; Git
  still refuses the primary worktree, and a failed non-forced remove leaves an
  error after the conversation is gone. KetPlus stores the worktrees each
  conversation owns (the primary checkout created with the conversation, plus
  any Git worktrees captured when that conversation's turn completed) for that
  cleanup; the composer no longer shows a worktree bar. See [ADR 0010](decisions/0010-conversation-owned-worktrees.md).
- The product sidebar has no worktree level. Activating a worktree creates an
  untitled placeholder conversation; placeholders stay hidden so temporary
  review and detached worktrees add no sidebar rows. A conversation the user
  creates, including one that owns a linked worktree, is listed even before
  the first turn.

## Safety and process policy

### Externally removed worktrees

Removing a worktree directory outside KetPlus does not delete or archive its
conversations. Existing and persisted conversations, including daemon-discovered
sessions, remain addressable without an existing directory. Stop still interrupts
the selected running session. When the execution checkout is missing, sending a
new turn requires explicitly choosing a registered worktree of the same repository.
The daemon lists replacements through its recorded repository mapping; it never
silently falls back to the main checkout. History and drafts stay in the conversation.
Desktop exposes this recovery through Choose worktree; ordinary Files/Source Control
selection remains independent. The phone keeps the draft editable but disables Send
until the replacement is confirmed. A running Codex turn may select the next turn's
checkout without interrupting the current turn or changing its event provenance.
Startup, shutdown, and pending approval states still block context changes.

The Agent Workspace marks missing paths when its worktree model refreshes,
including when the application becomes active or Git refresh fails. Selecting a
missing entry opens its conversation without switching the editor workspace to
an invalid directory. File diffs and new terminals are unavailable for that entry.
This is not a filesystem-wide watcher or an archive browser: worktrees already
pruned from Git before startup do not yet have a dedicated history discovery UI.
Directory removal does not automatically stop agents or terminal processes.

Codex threads use the selected worktree as `cwd`. The composer defaults to
`workspace-write`; users can select read-only or full access. The Codex adapter
maps these profiles to thread sandbox strings and per-turn `sandboxPolicy` objects.
The daemon uses `approvalPolicy=on-request` unless the user explicitly chooses
full execution access. Requested permissions are shown as persistent confirmation
cards, including requests that arrive while another conversation is selected.

The daemon owns provider processes independently of Desktop. Crashes and transport
errors fail affected sessions visibly; the next prompt can start a fresh provider
process and resume the saved thread. The old direct transport remains only as a
compatibility/test path.

## Intentional first-version limits

- Codex is the only provider.
- Approval prompts are not interactive yet.
- Transcript persistence is provided by Codex thread resume; KetPlus does not
  maintain a second on-disk transcript database.
- Agent-created worktrees are attributed when a turn completes, by comparing
  `git worktree list` to the snapshot taken at `turn/started`, then stored in
  desktop SQLite. They are not reconstructed from the daemon journal or from
  scrolling the transcript.

## Browser tools

Browser tools are on by default for Codex, Claude and Cursor. The shared launch
resolver chooses the bundled runtime or `apps/browser/src/mcp.mjs` from a source
checkout with Node >= 22 and production dependencies (`playwright-core`). It
prefers system Chrome, then Edge, then cached/downloaded Chrome for Testing.
Settings probes never launch or download a browser. If automatic download is
allowed, first use may still require network access.

Settings → Browser controls "Let agents use a browser" and "Public websites
and this computer" (default, `*`; never the local network, link-local or cloud
metadata addresses) / "Only these origins" (named origins may reach the local
network). Every session uses an isolated temporary
profile. Missing runtime or invalid settings omit the injected MCP server.
Browser calls from the injected server need no per-call approval; other tools
retain the selected permission mode.

Settings → Providers lists each provider's installed or bundled version, its
support status, the supported range and the update command (ADR 0015). A turn on
an outdated Codex or Cursor CLI fails with that command. A newer-than-tested or
unreadable version shows a muted "Provider version" notice once per session.
`/compact` and the usage popover's compact button appear only when the daemon
reports the selected provider's `threadCompact` capability; no provider offers
it through the daemon yet.

Unknown Codex input never disappears silently (ADR 0015 §5). A server request
KetPlus does not handle shows a muted "Request declined" notice. A known message
missing a required field shows a muted "Protocol error" notice naming the
method and the fields. An item of an unknown type renders as a generic activity
row titled with its type name.

Codex restarts when idle after a setting changes. Claude gets fresh MCP settings
on each SDK query; Cursor receives them when creating or loading the session
before its next turn. Running turns retain their current policy. The packaged
skill appears in the picker even outside a KetPlus source checkout, and MCP
initialization always supplies compact browser instructions. Agents also receive
host routing guidance automatically, without enabling or mentioning the skill:
Codex gets developer instructions on thread creation/resume (direct and daemon
transports); Claude appends the policy to the Claude Code system prompt on each
query; Cursor receives a separate ACP context text block on each turn. The policy
uses that provider's captured MCP availability, preserves user text/attachments,
and directs real-browser work to `ketplus_browser`. Missing/disabled tools and
unsupported operations must be reported instead of falling back to native browser
tools, other browser MCPs, ad-hoc automation or computer use on another browser.
Computer use (desktop screen and input control) is allowed in two cases: driving
Safari when a task specifically needs Safari or WebKit, which the Chromium-only
`ketplus_browser` cannot provide, and native desktop applications that are not web
browsers. Origin denials still require a host settings change and must not be
bypassed through Safari. This is model guidance, not a new tool-execution firewall;
provider permission modes and the browser origin proxy are unchanged. See
[ADR 0013](decisions/0013-browser-agent-routing.md) and
[ADR 0014](decisions/0014-computer-use-for-safari-and-native-apps.md).

Expanded command activity shows its input once: a one-line clipped preview with
Show more on the same row. Show more reveals the full selectable input; output
and exit metadata remain separate. Closing the activity resets the input preview.

The desktop transcript shows KetPlus browser `open`, `capture`, and `compare` rows.
Browser observation, inspection, network, logs, session cleanup, and other internal
steps remain in the canonical transcript for copy/export but do not add visible rows.
Capture and comparison images remain visible, including replayed history.
KetPlus browser rows show an operation label and any concise failure message, without
an expandable JSON command receipt. Sanitized payloads remain available to local export.
Missing/pruned screenshots retain a visible placeholder; clicking an image opens the
existing preview.
Claude/Cursor can consume screenshot tool results, while their transcript
presentation remains the existing provider-specific behavior. See
[ADR 0011](decisions/0011-browser-tools-for-all-providers.md).

## Computer use

Settings → Computer use is off by default. It is stored as
`computer-tools.json` (`enabled`, `deniedApps`, `display`) beside `browser-tools.json`;
`KETPLUS_COMPUTER_TOOLS_CONFIG` overrides the path. When the setting is on and
`KetPlus Computer.app` is present, each Claude query and each Cursor ACP
session/new or session/load receives a `ketplus_computer` stdio server. The
server's environment carries `KETPLUS_COMPUTER_CONVERSATION` and
`KETPLUS_COMPUTER_TOKEN`, the HMAC of the conversation ID under
`<KetPlus data>/computer/host.key` (or `KETPLUS_COMPUTER_DIR`). Turn input
cannot add or replace the server. The Claude sidecar and Cursor permission
handling allow `ketplus_computer` tools only when KetPlus injected the server.
The agent app is the enforcement point:

- A conversation sees and controls only the apps the user approved for it in
  the agent's native approval panel (`request_access`). Screenshots include
  only those apps. KetPlus windows, the agent's own panels and the menu bar are
  never captured.
- Tiers: KetPlus, Keychain Access, password managers and `deniedApps` are
  blocked. Browsers other than Safari are view-only. Terminals and IDEs accept
  clicks only, and `act` `set_value` counts as typing.
- Input runs in the background by default (ADR 0031). `observe` reads a granted
  app's accessibility tree, even behind other windows, and `act` drives its
  elements by reference; references go stale after any input. `left_click`,
  `right_click` and `scroll` at screenshot coordinates become accessibility
  actions on the frontmost granted app's window at that point. `type` and `key`
  go to one process: the `app` parameter, the app last acted on, or the
  frontmost app. Shortcuts first run the matching enabled menu item. Typing is
  refused when the target's focused element is a secure text field. Without an
  accessible element, calls fail with `NEEDS_FOREGROUND`.
- `"foreground": true` uses the real pointer and HID keyboard, as in the first
  release: pointer input is checked against the topmost window at the point and
  keyboard input against the frontmost app, refused while secure input is
  active. Drags, pointer moves and double, triple, middle or modified clicks
  always need it.
- The agent works on one display (ADR 0032). `display` is `secondary` (the
  default: the first display that is not the main one, or the main display when
  it is alone) or `main`. After a grant, and when `open_application` opens or
  reveals an app, granted apps' windows move to the work display, and the
  results report `movedToWorkDisplay`. `screenshot` captures the work display
  unless its `display` parameter is `main` or an ID from `list_apps`, which
  lists the displays, and answers `NOTHING_ON_DISPLAY` when no granted app has
  a window there. Granted windows that other apps hide are raised first on a
  separate work display, or reported as `covered` (their pixels may be old)
  on the main display. The banner shows on the work display.
- One conversation holds the control lease at a time. The lease ends after 90 s
  without an action. A banner with a Stop button shows while it is held, and an
  overlay pointer marks background actions. Stop, or Esc while a granted app is
  frontmost or within 15 s of foreground input, ends the lease, revokes that
  conversation's grants and answers its next calls with `CANCELLED` until the
  user approves apps again. Esc posted by the agent itself never counts.
- `startTurn` writes a remote-turn marker for turns from remote clients and
  removes it for local turns. The agent refuses a marked conversation with
  `REMOTE_TURN`.
- Each action appends a line, with its mode but without images or typed text,
  to `computer/audit/<conversation stem>.jsonl`.

Outside KetPlus, `ketplus-computer-mcp --local-client <name>` mints a token for
the conversation `local:<name>` from the user-only host key; approval, tiers,
the lease and the audit apply unchanged (see `apps/computer/README.md`).

Codex app-server binds MCP servers per process rather than per conversation, so
Codex does not receive `ketplus_computer` yet. Tools follow
`computer_toolset_20260801` names, plus `list_apps`, `request_access`,
`open_application`, `observe` and `act`. `open_application` leaves the app
behind the user's work unless `foreground` is set. `wait` is capped at 60 s.

## Live smoke test

Authenticated model calls are opt-in. The regular test suite uses a deterministic
fake app-server and never consumes quota. To run the live Codex transport test:

```sh
KETPLUS_RUN_LIVE_AGENT_TEST=1 build/dev/ketplus_agent_tests liveCodexSmoke
```

The live test explicitly selects `gpt-5.6-luna` with medium reasoning effort.

## Composer and provider capabilities

- The two-row composer includes attachment chips, permission selection, model and
  reasoning selection, and Send/Stop. Enter sends; Shift+Enter inserts a new line.
- `model/list` is requested as soon as the daemon or Codex transport is ready,
  so Codex, Claude, and Cursor catalogs are already merged before the picker
  opens. Opening the picker does not start a new catalog load when models are
  present. Pagination is handled by the manager. Codex uses the app-server catalog.
  Claude uses the sidecar's live SDK list (with alias fallback). Cursor uses
  `agent models`, groups effort-suffixed ids into families, and advertises those
  efforts for the picker slider.
- Model entries carry a provider id, permission profiles, and default permission.
  QML renders these profiles; each adapter owns the protocol mapping. Claude
  supplies its own profiles (`read-only` / `workspace-write` / `danger-full-access`)
  and translates them to Claude Agent SDK permission modes rather than Codex
  sandbox parameters. Claude Code still sends some commands to the permission
  callback in `bypassPermissions` (its own safety checks); the sidecar allows
  them, so full access never prompts. `AskUserQuestion` still reaches the user.
  Direct (non-daemon) mode still rejects non-Codex
  submissions. Daemon mode accepts Codex, Claude, and Cursor.
- Settings are captured before asynchronous thread startup. Every turn sends the
  selected model, effort and permission, including changes on an existing thread.
- Attach images up to 20 MiB and UTF-8 text files up to 512 KiB via the picker or
  drag/drop. Images use `localImage`; text file contents become reference context.
  The backend revalidates files on submission. Unsupported files are not silently
  sent as empty attachments. Attachment-only messages are supported.
- Draft text, attachments and selections are retained separately per conversation
  during the current app session. New conversations copy the current conversation's
  model, effort, permission, and provider; if that conversation has no model yet,
  they use the last model the user applied. Opening an existing conversation restores
  that conversation's stored model and never substitutes the catalog default
  (Cursor Auto). The composer label always includes the provider, so Auto reads as
  `Cursor · Auto` (Cursor chooses the model for the turn).
- Presentation keeps connection availability, folder availability, history load,
  and turn activity as separate axes. The native header displays repository and
  branch labels rather than the absolute worktree path; scope changes cannot turn
  a late response into the selected conversation's status.
- Model selection uses one popover with provider tabs, searchable models,
  a discrete effort slider, and reset-to-default. The slider follows the models
  on the open tab, and hides when a model has no separate effort list. Claude is enabled when the
  daemon advertises Claude catalog entries (sidecar present). Cursor is enabled
  when the daemon advertises Cursor catalog entries (CLI present). KetRouter
  remains a disabled placeholder. The picker
  stays open after selecting a model, supports keyboard navigation and retrying
  catalog loads, and adapts to the window bounds using the active theme tokens.
  Reopening restores the conversation's provider tab when Codex and Cursor share
  a model id (Cursor effort families such as `gpt-5.6-luna`). The composer and `/model` open the same picker. Voice input is not included.

Authenticated Claude turns need the Agent SDK sidecar. Packaged KetPlus stages
it into `Contents/Resources/claude-sidecar` (`index.mjs`, `transcript.mjs`, `input.mjs`, `node_modules`,
`runtime.json`) and runs it with the bundled `ketplus-browser` Node. Override
with `--claude` / `KETPLUS_CLAUDE_SIDECAR`, or `KETPLUS_CLAUDE_NODE` to pick a
different Node. A source checkout can `npm install` in
`apps/daemon/providers/claude-sidecar` and point the env vars at that tree.

Claude turns accept steering. The sidecar passes each turn's prompt to the SDK
as an open input stream (`input.mjs`) and pushes a steer onto it with
`priority: "next"`, so it never interrupts the running step: Claude Code lets
the current tool call finish and folds the steer in before its next model
request. If the turn is already producing its final answer, the steer is
answered by one more request in the same daemon turn. The stream closes after a
result only once no user prompt or steer is waiting. Each outbound prompt has a
UUID; result correlation and `queued_turn_count` prevent a stale/background
resume result from consuming a new user turn. Synthetic no-response assistant
frames are not answers. Older SDKs without correlation retain nonempty-result
compatibility. An empty startup result before any model request keeps draining.
If no request follows within 60 seconds, or output closes without an accepted
result, the turn fails visibly instead of reporting successful completion. A steer that arrives after the turn ended is journaled as
`turn.steer_failed`, and the desktop keeps the draft. Cursor does not support
steering yet.

Cursor turns need the installed CLI. Packaged KetPlus does not copy `agent`.
Override with `--cursor` / `KETPLUS_CURSOR_AGENT`. Do not run ACP `authenticate`
from the daemon; users sign in with `agent login` or `CURSOR_API_KEY`.

Protocol shapes were checked against `codex app-server generate-json-schema` from
locally installed Codex. The tests check model catalog metadata, per-turn model and
permission changes, image/text payloads, unsupported-provider rejection in direct
mode, daemon Claude and Cursor turns, and draft
isolation. The live smoke test loads the actual model catalog and sends a read-only
Luna Medium turn.

## Chat layout diagnostics

Set `KETPLUS_CHAT_LAYOUT_TRACE` to a writable JSONL file before launching a fresh
app process. Existing processes do not pick up this setting:

```sh
KETPLUS_CHAT_LAYOUT_TRACE=/tmp/ketplus-chat-live.jsonl \
  build/dev/KetPlus.app/Contents/MacOS/KetPlus /path/to/repository
```

The opt-in trace appends numeric layout state, message indices/lengths, message
keys, process and view identifiers, sequence numbers, and monotonic timestamps. It
does not record message text or repository paths. Recording stops at 8 MiB. Raise
the limit with `KETPLUS_CHAT_LAYOUT_TRACE_MAX_MB`, and use a new filename for
another capture. The view, message model, row geometry and session history write
to the same file in one sequence (`TranscriptTrace`). Transcript data events are:

| Event | Meaning |
| --- | --- |
| `reader-jump` | While idle and not following the tail, the row being read moved more than 2 px on screen between two idle frames. The first idle frame after a gesture is skipped: it still carries the gesture's last motion. |
| `anchor-resolved` / `anchor-lost` | A reading anchor's row stopped being laid out. The anchor either moved to the row showing its message, or was dropped and the current position adopted. |
| `restore-moved` | A workspace restore moved the viewport, so its anchor disagreed with the view's. |
| `model-reset` | The message model rebuilt every row; `reason` names the fallback path. |
| `model-diff` | Before a `session` reset of a non-empty model: the first differing row's key and field names (never values). |
| `history-page-canceled` | A conversation was left while an older or newer page was loading; the loaded window was kept. |
| `height-correction` | A row's first real height differed from its estimate by 2 px or more. |
| `history-prepend` / `history-window` | A history page landed. |
| `history-trim` / `history-trim-skipped` | The idle trim removed rows, or skipped because the reader's row was unknown. `trimMs` is the GUI-thread time spent choosing the retained rows. |
| `underfilled-prefetch` | Idle prefetch requested an older page because the rows did not fill the viewport. |
| `live-append-behind-window` | A live row arrived while the window lacked newer turns; the latest window was loaded. |
| `settle-begin` / `settle-end`, `jump-latest-shown` | How long the transcript was hidden while settling. |
| `reader-pin`, `follow-tail`, `submit-reveal-latest` | Reader pin on a streaming row, tail-follow changes, and the return to Latest after sending. |
 Events cover incoming updates, paragraph formatting, text/list
height changes, scrolling, restore requests, and frame presentation notifications.
Restore samples also include the saved row offset, wheel-active state, and the
anchored delegate's position after compensation. Text-anchor samples remain in
memory and are never written to this trace.
`frame-presented` samples GUI state in a queued callback, not the render-thread
state at the exact instant of presentation. A transient height change alone does
not prove that an incorrect frame was displayed.

The native pixel comparison can be run separately from the offscreen test suite:

```sh
QT_QPA_PLATFORM=cocoa QSG_RHI_BACKEND=metal \
KETPLUS_CHAT_CAPTURE=/tmp/ketplus-chat \
KETPLUS_CHAT_LAYOUT_TRACE=/tmp/ketplus-chat-test.jsonl \
  build/dev/ketplus_agent_tests streamingPreservesReadingPosition
```

It samples an unchanged visible reading region while streaming text and a Mermaid
fence below it, with short/long messages and several distances from the bottom.
It saves reference images and any failing frame. RGB differences up to 2/255 are
allowed for glyph antialiasing rounding. This is a synthetic reproduction attempt,
not proof that a reported live-session flash has been resolved; sampled captures
can also miss an intermittent frame.

## Background tasks

Claude can leave shells (`Bash` with `run_in_background`) and subagents running after a
turn ends. The sidecar keeps the SDK query open while any such task is live, so the
next user turn reuses it instead of killing the tasks; a model run the agent starts on
its own after a task notification becomes a turn of its own. Stopping a turn interrupts
the model, not its background tasks. A different launch (worktree, MCP servers,
readable directories) replaces the query and reports its tasks as ended.

The sidecar reports the visible set as `tasks.changed` (full snapshot, progress
throttled to 2 s). The daemon bounds it to 16 rows, journals the latest snapshot per
session generation and lists it in `session.list` as `background_tasks`. A provider
exit or daemon restart marks live rows `stopped` with reason `worker_restart`.
`session.task.stop` and `session.task.output` (last 8 KiB) accept only listed task ids;
the relay maps them to the existing interrupt and output-read grants.

The desktop shows a collapsible bar above the composer with one row per task, a
confirmation before Stop, and an output pane that refreshes every 2 s while a shell
runs. Offline, rows show the last known state and Stop is disabled. Codex and Cursor
report no background tasks.

## Provider transcript and permission lifecycle

The daemon owns outstanding permissions independently of the selected conversation.
Desktop conversation rows show a static warning Pause icon while a question or
confirmation is pending, including nonblocking async questions on a running turn.
The pending-request projection refreshes background rows as well as the selected
conversation. Answers, remote resolution and expiry restore the current turn icon.
There is no default timeout: opening another conversation or detaching Desktop never
answers, declines, or expires a request. An explicit provider deadline can still
expire a request. Stop interrupts the provider and cancels only that session's
pending requests; provider exit invalidates its old requests without replying to a
replacement process. Cursor cancellation uses the ACP notification form and
permission replies use the option IDs actually offered by that agent.

Claude preserves each assistant message's identity, repairs streamed text from
complete snapshots, records tool inputs/results and failures, and emits context
compaction activity. AskUserQuestion is routed through the same question broker.
Cursor preserves partial tool updates, input/output, file diffs and plan activity;
its create-plan extension waits for a user decision. Public assistant text has an
explicit phase for live rendering. Thinking content remains hidden.

Codex assistant messages carrying asynchronous questions create nonblocking cards.
Answers become input to the owning conversation (steering a running turn or starting
its next turn). Completed turns do not dismiss these nonblocking cards. History
restores runtime errors, available command output deltas, and local answer receipts.
Secret answers are redacted before journal storage; cloud events omit all answer
receipt text. Previously stored ID-only receipts continue to display as closed.
Command output stays local and bounded in the renderer; completed snapshots replace
output deltas in the replay projection. Older projections that omitted command
output cannot reconstruct that output until a completed snapshot is available.

Provider normalization lives in dedicated Claude and Cursor helpers and a separate
async-question implementation. Existing large transport files are touched only at
their dispatch/lifecycle boundaries; they remain large for the existing provider
startup, discovery and session routing code.

For a synthetic native browser-transcript capture (no browser or provider calls):

```sh
QT_QPA_PLATFORM=cocoa QSG_RHI_BACKEND=metal \
KETPLUS_DISABLE_DAEMON_AUTOSTART=1 \
KETPLUS_BROWSER_TRANSCRIPT_CAPTURE=/tmp/ketplus-browser-transcript \
  build/dev/ketplus_agent_tests browserEvidenceStaysVisible
```

Verify that capture and visual-compare evidence is visible below its label
without expanding any command, and that the preceding/following rows do not overlap it.
The same test covers completion from a collapsed group, history replay, preview opening,
missing media, and readable browser errors without raw JSON.

`browserStepsCollapseAroundEvidence` verifies internal browser steps are absent from
the UI while the canonical transcript retains them, alongside visible open,
capture and comparison at 1100 px and 520 px and on history replay. Set
`KETPLUS_BROWSER_GROUP_CAPTURE=/tmp/ketplus-browser-groups-`
to save native screenshots while running that test. Capture/comparison rows remain
separate before results arrive.

History pagination of an already loaded conversation does not enter conversation restoration. It keeps scoped image previews and the reading window active; only initial loading/replacing a conversation advertises restoration. The desktop transcript window suite verifies this distinction.

History page publication fences both source rows and the current reading key. If the reader moves while a page is prepared, it is bounded again before publication. Both item-count and source-byte trimming retain that reading row and nearby rows; eviction can occur at both edges, with both paging cursors rebuilt from the retained range. This prevents a late older page from discarding a reader who moved toward the newer end. The regression first failed because the current reading row was absent after publication; viewport coverage also replaces both edges while holding the same screen position.

Warm conversation row-height reports carry both their source model and delegate identity. The shared viewport discards reports from retired delegates (including the height reduction when an image gallery is destroyed), so returning to a cached thread cannot overwrite its measured image rows with teardown geometry. All transcript attachment and tool images use fixed 88×88 cropped thumbnails, matching mobile. Each gallery starts with at most three images plus a +N tile for the remaining count. Clicking +N reveals all images in a wrapping grid; expansion is retained per conversation/message. Inline Markdown photos fit within 88×88 while retaining aspect ratio; Mermaid diagrams keep their dedicated layout. Clicking opens the original image. Thumbnail grids reserve their height independently of decode and Grid recreation. `warmSwitchKeepsMixedImageRowHeights` switches four times through 120 mixed portrait/landscape image rows, with single-image and four-thumbnail variants, checking total height and the reading row position on offscreen Qt and native Cocoa/Metal.

Transcript file and diff links select the containing Git checkout for Files and Source Control in the current window. The conversation and running agent keep their execution context. Diff requests wait for Git discovery after switching; ordinary files outside Git open directly in the current window.

Mobile resource reads can carry `resource_event_cursor` and `resource_source_path` alongside `resource_context_revision`. The daemon resolves the exact recorded read/edit event to its containing checkout, checks the request's repository/worktree identity, and verifies provenance again before releasing the result. This permits Files previews in another checkout or repository without changing the conversation execution context. Resource resolution returns optional `source_path`, `repository_label`, and `branch` for the viewer. Paths that were not explicitly recorded remain rejected.

## Mermaid in chat

The shared host harness gives Codex, Claude and Cursor the same Mermaid authoring
guidance, even when browser tools are disabled: consistent flow direction, shallow
responsibility groups, short labels, fewer crossing edges, theme-compatible styles,
and overview/detail splits for crowded diagrams. It preserves meaningful dependencies
and asks for rendered inspection only when tooling is available; it does not call a
second model or guarantee a particular generated layout.

Clicking a rendered transcript diagram opens the existing CM viewer full-screen.
Its initial Fit runs after layout; pan, zoom, Fit, 100% and Close remain available.
This launch policy belongs to Agent Workspace; the shared viewer implementation and
submodule are unchanged. Tests check full-screen and initial containment for wide,
tall and compact diagrams.

A ```` ```mermaid ```` fence is an ordinary framed code block that shows its source by
default. A Source / Mermaid switch sits at the top right of the frame (left of the copy
button while showing source). Nothing is rendered, and `mmdc` is not run, until the reader
switches a block to Mermaid; the choice is kept per message and block ordinal, and only a
closed fence can switch (a streaming fence stays source).

Once rendered, the frame has no fixed height: it grows to the diagram's natural aspect at
`min(naturalWidth, frameWidth)` and the diagram is centred horizontally through the block's
left margin (`AlignHCenter` does not centre an inline image in `QTextDocument`). The fixed
box only exists for the pending and unavailable placeholders. Failures fall back to the
source with a note. `AgentWorkspaceView::mermaidLayout` reports the frame geometry that
`TranscriptMessage.qml` uses to place the frame and the switch.
Renderer completion/failure notifications reach the transcript on the next event-loop turn.
An immediate cache/start failure cannot re-enter the document render and then be overwritten
by its pending placeholder when a selected diagram's delegate is recreated.

Switching runs inside `runAnchoredMutation`, and text anchors relocate across a mode change
(`renderedMermaidModes` in `TranscriptPosition.js`), so a reader below the diagram does not
drift. `ChatMermaidTest::switchingToMermaidWhileStreamingIsSmooth` switches while an agent
streams (diagram above, below and at the tail) and asserts per-presented-frame drift under
1.5 px and a bounded GUI gap. `realMmdcSmoke` and `realLargeMmdcSmoke` run the real `mmdc`
when `KETPLUS_REAL_CHAT_MERMAID=1` (`KETPLUS_CHAT_MERMAID_CAPTURE=<png>` saves a capture).

## Pending conversation settings and file actions

Model/provider/effort and compact-window changes remain available while a turn or
background task runs. Apply saves defaults for the next submitted turn. The active
turn retains its submitted options, including during asynchronous provider attach;
steering is sent to the current provider. Execution-policy changes still require
an idle session. Offline clients cannot apply daemon configuration.

On successful Codex command/file-change completion, absolute path evidence can
identify another registered worktree of the same Git repository. At turn end the
conversation follows that checkout through the context-revision transaction. Late
turn events and evidence from before a manual switch cannot override the chosen
context. File/Source Control navigation remains independent and cannot overwrite
the conversation header or its missing-worktree state.

Claude conversations can select another worktree once the current turn and its
background tasks finish. Pending questions and approvals still block switching.
The next turn resumes the same Claude thread in the chosen directory, including
when returning to an earlier worktree. The daemon advertises this to the iPhone
picker too; installing only a mobile build cannot enable it on an older daemon.

Right-clicking a local file link in the transcript offers Reveal in Finder on macOS
(or the platform's file-manager equivalent). Resolution uses the clicked link,
requires an existing local path, and does not reuse an earlier menu target. Network
links and unverified relative paths after a context switch have no Reveal action.

Transcript selection copy (keyboard and context menu) exports the exact selected plain
text. It does not serialize Qt rich-text fragments, which add a leading paragraph when
copying inline code from a list. Intentional spaces and line breaks are preserved.

Reopening a transcript file link refreshes its already open clean editor using
CM's `EditorWidget::refreshFromDisk()`. The same tab is reused, with caret/selection,
scroll and Markdown mode preserved. Unsaved edits remain untouched. A failed read
keeps the current buffer and opens a visible error dialog. Refresh happens on an
explicit reopen; this is not a background filesystem watcher.


### Conversation repository picker

The desktop picker follows the mock (ADR 0105). The current repository is listed
first. Selecting a repository lists its worktrees, with the current or last-used
one chosen and the main worktree marked "Main". Worktree search stays scoped to
that repository. Selecting a worktree only chooses it.

**Use this repository** switches the conversation's execution context after the
daemon acknowledges. The picker then closes and announces "Next turn:
<repository> · <branch>". Transcript, conversation identity, draft and an active
provider turn survive, and the new scope applies from the next turn. A failed switch
keeps the old scope and shows the error, and Use retries. **Create worktree…** opens
the Create worktree card in place (ADR 0111). iPhone uses a Repository → Worktree bottom sheet over the
granted catalog.


### Claude API key

On macOS, open Settings → Providers → Claude API access. Enter a key from Anthropic
Console, select **My Anthropic API key**, and choose **Save Claude settings**.
Leave the key field blank to keep a saved key while changing mode or query budget.
**Check connection** validates catalog access without model inference. The key is
stored on the daemon's Mac in Keychain; paired phones use the same configuration.
No key needs to be pasted into a conversation or sent through the relay.

Changes apply to new queries. If Claude still owns background tasks, finish or stop
those tasks before starting a turn with the changed configuration. Removing a key
returns new queries to existing runtime authentication; it does not stop active work.
An old daemon shows an update-required message instead of forwarding credentials to
another provider. Non-macOS hosts retain inherited runtime authentication.

The optional query budget and latest-query cost are estimates, not a hard account
spend cap or bill. Consult Anthropic Console for actual usage. See
[ADR 0026](decisions/0026-claude-api-key.md) for credential boundaries and release checks.

## Teams

[ADR 0112](decisions/0112-agent-teams.md) (epic #450). A team is several conversations of
one repository on one goal: exactly one **leader**, one to five **developers** and exactly
one **reviewer** (a Claude conversation). A conversation is on at most one team.

- **Roles.** The leader plans, writes acceptance criteria, assigns tasks, changes lanes
  and orders merges; it does not write code or review, and it is the only member that
  speaks to the user. A developer works on its assigned task inside its lane (path
  prefixes). The reviewer reviews each task read-only against its criteria.
- **A task's life.** Created by the leader with criteria, lane and dependencies →
  assigned (its branch and worktree are cut from the integration branch, and the
  owner's conversation is pointed at that worktree) → `in_progress` → handed to review
  with its work committed → `ready` when the reviewer reports no findings, or back to
  the owner with the findings → merged by the leader or by the user, as the team is set
  → `done` once its head is in the integration branch.
- **What wakes whom.** An assignment or handover starts a turn of the new owner; a task
  entering review, the reviewer; findings, the owner; an answered decision, the leader
  and the blocked owner; actionable updates, one leader digest. Notes, met marks and
  other state changes start nothing.
- **Decisions.** Only the leader raises one; the user answers on the team page. The
  folder in the sidebar shows the count.
- **Limits.** 40 automatic turns per team since the user last acted on it, then the team
  pauses and asks. ADR 0027's loop guard still applies. **Pause** holds every team
  message until Resume.
