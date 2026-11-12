The unreviewed transcript UI layer consumes kit bubble, Markdown and code recipes,
syntax colours and gallery geometry. Journal-dated user messages show day dividers;
legacy undated messages do not. Streaming, original-source copy, local links,
image viewing and Mermaid remain on the existing document/virtualization path.
This is implementation evidence, not native acceptance or release completion.

# Project status

The Lite model layer adds recipe-based Explorer/Git rows, Git decorations, inline file/folder creation, stage/unstage and commit-staged actions, and independent embedded terminal tabs. Markdown headings/code use the kit recipe and bundled mono preference; new preview defaults are 15/26 px while saved overrides remain intact. Git action failures stay visible and preserve the commit message. These changes are part of the unreviewed UI stack, not a released acceptance claim.

The UI stack now composes shared pane headers, find controls, Quick Open, Markdown mode switching, Settings controls and core confirmation dialogs from ket-qt. Quick Open retains fuzzy ranking and keyboard paging; Settings applies every change as it is made, with Restore defaults and extension pages (ADR 0103). Core synchronous document commands use a kit Dialog adapter that returns the existing explicit choices; dismissing it returns Cancel. The broader UI migration and later native acceptance remain open.

This file distinguishes current code from target architecture. Update it when a
milestone changes state.

Tombstone catalog hardening (ADR 0091) adds bounded, opt-in keyset pages and
revision-fenced shared transport consumption by Desktop refresh and
`ketplusctl sessions`. Concurrent mutation/reopen invalidates a cursor; retries
are capped and CLI history is spooled privately. Legacy catalogs exceeding the bounded
response return an explicit upgrade error. This is backend/transport work;
live-catalog bounds and the remaining #68 acceptance gates stay open. Incremental discovery now requests only live sessions
(ADR 0092), avoiding tombstone walks and repeated deletion signals. Full refresh
and CLI retain complete reconciliation; older daemon replies remain compatible.

## Implemented

- Bounded team messages (ADR 0115):
  - A message too large for one frame becomes a failure of its request, or a
    `truncated` event; the client keeps its connection.
  - `team.list` and `team.changed` carry only the members.
  - `team.get` carries a summary without history, plus the newest 50 Updates.
  - History, older Updates and decisions come page by page.
  - The sidebar keeps a team's members together while the daemon reconnects.
- Agent teams, store (ADR 0114, #455): the daemon keeps teams in the journal
  (`teams`, `team_members`, `team_updates`) and enforces the membership rules with
  their error codes; a member cannot be archived, its own Reviewer setting is refused
  with `team_member` and does not run, and a deleted member leaves its team.
  The `teams/1` protocol (#456) lists, reads, creates, changes and deletes teams for
  local clients; `team.create` starts every member as a named conversation (no turn),
  all or nothing, and `team.changed` / `team.removed` reach every local client.
  Tasks (`teams.tasks/1`, #457): `T-n` ids, one owner, the state table of ADR 0114 §6,
  criteria only the leader or the user writes, an append-only history, dependencies,
  and a handover that moves the task's own worktree and branch to the new owner.
  Agent tools (#458): members get the `team_*` tools of the ketplus_agents server, the
  daemon checks the caller's role on every call (`role_forbidden` names the role that
  may), `leader` and `reviewer` are addresses for `agents_send`, `agents_list` shows
  role and lane, and each member's instructions explain its role. The leader's first
  turn carries the goal.
  Automatic turns (#459): only an assignment, a handover, a review and an answered
  decision start a member's turn; every other change waits in Updates for the leader's
  digest, sent once when its turn ends or after 20 s of quiet while it is idle. Pause
  holds every team message in order until Resume, and the team's budget of automatic
  turns pauses it with one decision ("Continue (N more turns)"). The budget (ADR 0114
  open point 2, `teams.settings/1`): a global value the daemon keeps, 40 until changed in
  Settings → Teams, which a team created without its own takes (an older phone included);
  New team and the team's Automatic turns dialog set a team's own, a whole number from 5
  to 200, and a larger budget withdraws the budget decision and resumes the team, its used
  turns still counted. The team page shows the turns used of it.
  Stall watchdog (ADR 0114 §8): 3 minutes after a member's turn ends, a team with work
  left is moved on: an idle developer with a task in progress (or the reviewer with a
  task in review) is nudged once, an idle team wakes the leader with the open tasks at
  most twice, then a `stalled` decision asks the user to wake the leader again or pause.
  A member the user stopped is not woken on its own.
  `team_wait` lets a leader or developer wait 1 to 60 minutes for something outside the
  team and be woken to check, and a member's message to a conversation outside the team
  that failed or was never answered wakes that member (message kinds `wait` and
  `message_failed`). Waits are in memory only; neither has run on a live team yet.
  Per-task review (#460): a task entering In review is queued for the team's reviewer,
  first in first out, as an ADR 0067 `review` message with its criteria and the diff of
  its branch against the integration branch; `no findings` makes it Ready, findings go
  back to the owner with round + 1, and past `max_rounds` the task is Blocked.
  Lanes (#461): after a developer's turn the daemon compares the files its open tasks
  changed (committed or not) with its prefixes and the shared paths; outside paths are
  flagged on the task, actionable in the digest, and cleared when reverted or covered
  by a lane change.
  Decisions (#462): the leader raises a question with two to four choices
  (`team_decision_raise`, `team_decision_withdraw`); only the user answers through
  `team.decision.answer`, which reaches the leader and the blocked owner once and
  unblocks the task. The leader's first plan waits as "Start these N tasks?", and the
  budget's decision continues or stops the team.
  Merging (#463): a Ready task whose dependencies are done is offered once — to the
  leader's digest under `mergePolicy` `leader`, as a "Merge T-n?" decision under
  `user` — and becomes Done when the integration branch contains its branch
  (`git merge-base --is-ancestor`, checked after every member's turn and on
  `team.get`), also for a merge made by hand.
  Done by pull request ([ADR 0118](decisions/0118-team-tasks-done-by-pull-request.md)):
  a task that records a pull request is done once `gh` says it is merged, from any open
  state, checked at load, every 2 minutes and with the merge check (at most 20 pull
  requests a check, once a minute a team, on a worker of its own that stops a batch at the
  first request without an answer); errors move nothing and are said once. The ancestor
  check skips tasks with a pull request, needs the branch's own commits and accepts
  `origin/<integration>` after a fetch, and the leader closes a task by hand with `team_task_close`. Not yet run on a
  live team.
  Sidebar (#464): with a daemon that advertises `teams/1`, each team is a branch of its
  repository's tree before the other conversations, leader, developers, reviewer; a
  member's role is in its hint and accessible description. The branch's name opens the
  team, its chevron folds it, and while a decision waits it shows the needs-input mark and
  a semibold name; its menu adds a member or deletes the team, the
  conversation menu joins or leaves a team with the reason when it cannot, and a drop on
  a folder of the same repository joins as a developer.
  `DaemonClient::teamEventReceived` carries `team.changed` and `team.removed` to the
  desktop's `TeamDirectory`.
  Dialogs (#465): New team sends one `team.create` with the name, goal, integration
  branch, who merges, the permissions of the leader and the developers (`execution_policy`:
  Read only, Workspace or Full access; Workspace unless chosen; the reviewer stays
  read-only), one to five developers and each role's agent (Claude leads and
  reviews, another ready provider develops; the reviewer's chooser offers Claude only).
  A refusal keeps the dialog open with the error at the role it names. Add a developer
  sends `team.member.add` with a required name, lane, paths and agent, hints at the
  reviewer queue from three developers and is disabled at five.
  Member header (#467): a member's Agent Workspace header shows its role as a locked
  label that opens the team and cannot be edited, and has no Reviewer choice (the team's
  reviewer reviews its tasks). Leaving the team brings the Reviewer choice back with its
  setting.
  Team page (#466): Open team shows the team in the workspace area: what waits for the
  user (decisions with the recommended choice first, answered in place through
  `team.decision.answer`), the tasks with a state filter and the outside-lane flag, the
  members, and the updates from `team.get`, newest first. Paused and queue banners explain
  why turns wait. `team.changed` rebuilds it in place and keeps the scroll position. A
  task opens a 640 px dialog: state and lane badges, the facts two to a line, the
  description as Markdown cut at about 150 px behind Show all, the criteria with how many
  are met, and the full history; the body scrolls while the actions stay in view. Hand
  over… opens a dialog of its own and sends `team.task.handover` with another developer
  and a required reason; Open the owner opens that conversation. The page never grows
  wider than its viewport: text wraps or elides with the whole value as tooltip, a
  decision's sentence choices become rows, the task table drops lane, owner, PR and
  criteria in that order, and the filter becomes a select when its segments do not fit.
  Without the team it explains teams and offers New team.
  Transcript blocks (#468): a member's transcript draws the team's own messages as cards
  (assignment, digest, review request, verdict, decision) with Open T-n; a digest of many
  changes is one card. Anything it cannot read stays the message's text.

- The unreviewed workspaces UI layer consumes kit worktree actions/badges,
  searchable picker rows and Connections controls. Repository/worktree switching
  still waits for the existing revisioned acknowledgment. Linux pairing continues
  through authenticated outbound Relay; this layer does not add SSH setup.
  There is no Worktrees page (ADR 0111): worktrees are created for a
  conversation, from New conversation or the repository picker's Create
  worktree… card, which moves that conversation to the new worktree, and
  switched from inside the conversation.
  Connections follows the mock's signed-in page: a page up to 1000 px wide, one
  card per phone and per Linux machine, kit badge metrics, inset activity
  separators and the kit empty state and avatar. Linux machines connect only
  through the Relay; on 2026-11-11 the mock dropped its SSH option, machine
  details and Disconnect, and its phone pairing dialog now follows the app's
  scan, compare and approve flow (no typed pairing code).

- The unreviewed Pro overlays layer uses kit Quick search and conversation menus.
  Compact conversation window preserves the mounted session/draft and restores
  window geometry. The repository/worktree picker follows the mock: it lists
  another repository before switching (ADR 0105). Native acceptance and release
  remain deferred.

- Pro header/composer now compose kit buttons, model-search field, usage ring,
  meters and composer/attachment recipes. The header stays 44 px, with number,
  compact title, status, worktree and Reviewer choice, and no user tags (ADR 0117).
  The input retains IME/draft/steering
  behavior and a caret-aligned placeholder. This is implementation work in the
  unreviewed UI stack; automatic reviewer assignment is not provided by this layer.

- Pro sidebar uses kit navigation, the mock's tree (#537) built from the ket-qt sidebar
  tree parts: a repository is the root, its teams branch directly under it, "Conversations
  (n)" labels the rest where the repository has teams, every conversation is one line
  (the state's mark, title and #number; role, state, origin and worktree in the hint),
  guide lines run under each chevron, and folds of repositories and teams survive a
  restart (a folded repository shows a dot while something inside waits). Also the mock's
  field-styled Quick search, a machine header with its chevron, and update pills. Rows draw no user tags and the sidebar has no tag filter (ADR 0117); the
  daemon still stores tags and their catalog (ADR 0107, 0113) for other clients.
  Storage pressure warnings consume the local daemon's existing size-status contract,
  and "Waiting for you" comes from the daemon (ADR 0112). It matches
  `workspace` and `workspace/storage-warning` at zoom 1 and 1.25 (#158).

- Lite shell implementation now connects the Qt mock activity rail to Explorer,
  Search, Source Control and Settings. First use opens a folder welcome surface;
  file tabs show a kit breadcrumb outside the Scintilla document viewport. The
  Explorer, activity bar, tab strip, breadcrumb View diff, terminal placement and
  the single status bar follow the Lite mock (#149); `KETPLUS_CAPTURE_STEPS` drives
  offscreen review captures (`docs/design-system.md`). Settings opens as a page of
  the window, shared with Pro through `core/src/app/SettingsWorkspace` (#155).

- Két UI stack: Lite and Pro use KÉT Sans as the primary interface face, with
  Inter/platform fallback; fresh editor settings use KÉT Sans Mono Medium at
  14 px without rewriting saved choices. Interface zoom (90–125%) is a setting
  apart from the interface font size (13–16 px) and is published to the
  kit's Appearance (ADR 0104), terminal initial colours resolve tokens, and Mermaid theme
  resources resolve token placeholders at rendering time. Overall UI acceptance
  and rollout remain deferred; see `tasks/ui-implementation-stack.md`.

- The activity-index backfill runs after open in bounded passes (ADR 0097), so
  the first open after upgrade no longer indexes the whole history. A session's
  activity appears ungrouped until it is indexed; sessions with new events go first.
  Groups indexed before `activity_files` existed list their changed files the
  same way after the index drains (ADR 0102).

- Legacy turn-number, title and turn-start backfills run only for journals below
  format 1 (ADR 0096), so opening a stamped journal no longer rescans history.

- The journal records its format in `PRAGMA user_version` (ADR 0095). A daemon
  refuses, without writing, a journal newer than it supports; startup compaction
  leaves it untouched. Earlier releases do not read the marker.

- Session fork copies one read snapshot in bounded event-loop passes into an
  unlisted identity and stores the fork in one final transaction (ADR 0094).
  Failed, abandoned and interrupted copies become history purge work.

- Session deletion commits the tombstone, cleanup work and per-generation state
  with one bounded history batch (ADR 0093). Remaining history is unreachable and
  drained in bounded event-loop passes; the tombstone outlives the purge marker.

- Daemon Atlas readiness requires a complete line for the owned viewer port and
  ignores partial/embedded/truncated-prefix or post-stop output (ADR 0083). Start
  results are delivered once even if the process exits immediately after ready.
  Synthetic CLI cases and the full Pro suite pass; executable/port trust and
  wider tunnel/renderer lifetime acceptance remain separate.

- Atlas process readiness requires a complete banner for the current attempt's
  selected loopback port, including truncation/restart and stop-time guards
  (ADR 0081). Executable/listener identity and broader lifecycle remain open.

- The local Qt client applies transport backpressure before hello (ADR 0079),
  limiting unread socket bytes to one maximum frame plus header while preserving
  valid pipelined events. Broader IPC work/queue/lifetime bounds remain open.

- Local daemon/client credential reads share a 4-KiB raw-file bound (ADR 0078).
  Unix admission checks the opened descriptor's ownership/type/permissions;
  unsupported files remain intact and client reconnect can recover after repair.

- Accepted local IPC sockets bound Qt's unread receive buffer to one maximum
  frame plus header (ADR 0077). Pipelined exact-limit requests resume without
  loss after dispatch pauses, both before and after authentication.

- Local turn acceptance commits state/configuration and its two/three events
  together before publication (ADR 0076). Failed queued/configuration/projection/
  commit writes leave no accepted prompt; repair/retry and independent-reader
  visibility are covered. Whole-command idempotency/crash recovery remains open.

- Reference-based previews use bounded validated ownership and verify blob
  SHA-256 before decoding the same byte snapshot (ADR 0069). Unknown metadata,
  linked roots and mismatched bytes remain intact and return unavailable.

- Fork prepares validated media ownership before committing copied history
  (ADR 0068). Failed preparation rolls back the journal; conflicting destination
  metadata is preserved. Matching prepared ownership permits retry after a
  database commit failure; reopen cleanup preserves source-owned images.

- Image publication requires a checked atomic ownership-file update (ADR 0067).
  Invalid metadata is preserved, repeated hashes are deduplicated and failed
  writes cannot publish successful new references. Fork-copy and orphan bounds
  remain separate work.

- Long-lived provider stderr draining has a Codex/Claude/Cursor process regression:
  two 32-MiB floods retain only the newest 1 MiB while stdout round trips and
  subsequent calls continue on the same process. The fixture uses no real CLI.

- ACP v1 capability negotiation requires explicit boolean support (ADR 0090).
  Missing/mistyped loadSession and image flags no longer retain baseline support;
  Cursor does not issue session/load when it was not advertised.

- Malformed normal-sized provider frames reject their process generation (ADR 0089).
  Syntax/UTF-8/non-object frames and raw control bytes cannot be followed by a
  dispatched suffix. Pending calls receive a static error and a fresh process can
  recover; method schemas and absent-response deadlines remain separate.

- Matched provider responses require one valid result/error envelope (ADR 0075).
  Malformed replies fail their callback once, without successful initialization,
  unrelated-call failure or echoing malformed payloads. Null void results and
  Claude message-only errors stay compatible; method schemas remain separate.

- Provider responses preserve numeric call identity (ADR 0074): strings, booleans
  and fractional ids cannot consume pending calls in Codex, Claude or Cursor,
  including Codex oversized responses. Valid replies still complete exactly once.

- Cursor model-catalog subprocesses drain both channels while running and enforce
  a shared 1-MiB output budget (ADR 0088). Excess output and unsuccessful exits use
  the existing fallback catalog; successful exact-budget output remains usable.

- Provider version probes bound merged output while reading and require successful
  exit before accepting a banner (ADR 0072). Concurrent executable generations
  retain their own result; failed probes remain retryable without a cached version.

- Claude/Cursor stdout enforces the 16-MiB limit per raw line before parsing,
  including complete lines and partial suffixes after valid lines (ADR 0071).
  Bounded reads and failure draining preserve valid frames and fresh generations.

- Oversized provider routing uses bounded JSON string decoding and structural
  path segments (ADR 0073). Invalid captured metadata withholds all routing fields
  for the line; valid escapes/Unicode and subsequent messages remain usable.

- Oversized Codex line scanning retains at most 64 routing frames, tracking deeper
  containers with a scalar depth (ADR 0070). Shallow fields after the nested value
  still route correctly; broader provider framing remains under audit.
- Existing media blobs are validated before dedup returns a reference (ADR 0066):
  exact bounded bytes, regular nonsymlink files and owner-only permissions.
  Invalid entries remain intact for repair; reference durability remains open.

- SessionHost preserves failed stream prefixes and orders later provider events
  behind a bounded retry queue (ADR 0062). Count/byte caps, unsupported oversized
  payloads, rejected command acceptance and session deletion are covered. The
  queue is volatile; crash durability and whole-command atomicity remain open.

- Append verifies complete superseded-delta removal and running-activity
  settlement before committing (ADR 0063). Partial ignored writes roll back;
  partial indexes restrict settlement checks to running rows in the target turn.
  Large running turns and initial index construction remain separate bounds work.

- Failed activity-backfill commits preserve their diagnostic and release the
  write transaction (ADR 0064). Any failed constructor stage also closes the
  journal connection (ADR 0086): subsequent operations cannot revive or mutate
  that object. A separate connection can repair and a fresh object reopen while
  the failed object remains alive; broader startup recovery remains open.

- Media cleanup requires a complete bounded reference catalog before deleting
  metadata or image bytes (ADR 0065). Unknown shapes, symlinks and read failures
  preserve shared data; valid/empty catalogs retain existing cleanup behavior.
  Reference write/copy durability and broader media bounds remain under audit.

- Fork preserves unsupported source cursor/turn values rather than coercing them
  into copied metadata (ADR 0061). Integer-only projection and nonnegative checks
  reject the copy before destination writes; repair/retry/reopen are covered.

- Fork checks complete per-table copy counts and its required timestamp in the
  transaction (ADR 0060). It also refuses a destination claimed during the provider
  round-trip, preserving existing/self/deleted targets and allowing empty history.
  Whole-history fork work bounds and broader source validation remain separate.

- Journal append requires each mandatory delivery/replay/activity/blob row and
  session cursor to be stored before reporting success (ADR 0058). Ignored writes
  roll back partial changes; commit diagnostics survive rollback. Multirow
  projection cleanup remains separate hardening work.

- Append validates integer/nonnegative cursor and turn counters before arithmetic
  and reserves the whole cursor range for a large-item transfer (ADR 0059).
  Unsupported/exhausted values remain intact for recovery; final valid slots work.

- Provider cleanup also validates the ownership JSON shape before deciding a
  thread is unreferenced (ADR 0057). Malformed/newer metadata keeps local deletion
  and queued cleanup pending without rewriting the unsupported value.

- Provider cleanup requires a complete successful ownership-catalog read before
  deleting a provider thread or accepting a new local deletion (ADR 0056).
  Query and mid-fetch failures keep queued work pending and local history intact;
  retries after repair still protect shared threads.

- Current staged-attachment cancellation now bounds candidates and mutation bytes
  (ADR 0054). Logical cancellation retires the identity until all revisions drain
  through existing maintenance. Unknown/oversized manifests remain unchanged for
  recovery; ignored writes, busy writers and commit failures cannot report success.

- Session deletion rolls back and releases its transaction on commit failure;
  ignored tombstone/work writes and incomplete history deletion fail without
  committing partial state (ADR 0055). Retry preserves generation/operation fences.
  Whole-session deletion work bounds remain a separate hardening item.

- Cloud envelope ACK cleanup uses bounded transactional metadata maintenance;
  invalidation instead removes one exact cache identity before retry (ADR 0053).
  Receipts commit with a scoped recovery cursor before credential mirroring/raw
  ACK, preserving retry after storage failure or restart. Regenerated cache cannot
  inherit an old cursor or receipt; stale account upload callbacks cannot restore progress. Oversized cache
  invalidation fails closed; legacy unmarked cache and decoding need separate recovery.

- Provider/cloud deletion completion updates only the identified durable work row;
  completed jobs are reclaimed by bounded metadata maintenance (ADR 0052).
  Indexed pending pages preserve round-robin order without scanning completed
  backlog. Ignored updates and malformed provider-target JSON cannot report
  completion. Per-record JSON limits and explicit deletion remain separate audits.

- Staged attachment expiry now uses bounded transactional manifest/chunk passes
  and durable cursors (ADR 0051). An expiring identity stays reserved until every
  old chunk is gone, preventing reuse from adopting stale bytes. Invalid begin
  requests no longer trigger cleanup; idle maintenance continues unfinished work.

- Command/audit/tombstone/orphan metadata cleanup now runs transactional row/byte
  bounded passes with durable cursors (ADR 0050). Command/audit caps use accepted
  write order to preserve newly stored results after wall-clock rollback; idle
  maintenance resumes unfinished work. Physical caps are eventual, and unsupported
  oversized rows remain retained for the separate recovery workstream.

- ACK acceptance now commits the client cursor and a durable raw-cleanup queue;
  physical delivery-event deletion runs in bounded, transactional passes at ACK,
  reopen and idle maintenance (ADR 0049). Slowest-client/generation fences remain
  intact. Cleanup preserves a contiguous raw tail: an oversized/unsupported row
  defers the rest of that generation, and SQLite builds without the required
  byte-length capability retain queued data for later handling.

- Global large-payload orphan GC now runs bounded transactional passes with durable
  cursors, generation-wide pending-raw protection and exact replay-transfer lookups
  (ADR 0048). Idle maintenance continues chunk cleanup across passes. Unsupported
  oversized legacy blobs/chunks remain retained; other retention sweeps and complete
  media/archive enforcement remain in the backend hardening plan.

- Bounded large-tool manifests now use existing detail transformers when the
  hydrated JSON plus manifest fits the shared 1 MiB pass budget. Digest/identity
  validation and transactional replay/blob cleanup preserve corrupt, shared,
  unacknowledged and unsupported payloads (ADR 0047). Images, legacy monolithic
  blobs and larger payloads remain outside this lane.

- Generated images that older Codex builds journaled inline lose only their unused
  base64 pixels once eligible, one large payload of at most 16 MiB per idle pass,
  keeping prompt and saved path (ADR 0098). Inline images in older MCP and dynamic
  tool results become placeholders that keep their slot (ADR 0099). Previews of
  compacted images remain #128 work.

- The daemon records when a conversation is archived (`session.set_archived`,
  `archived_at`). It deletes conversations archived for more than 30 days, one
  per idle pass, through `session.delete` (ADR 0100). Desktop sends archive,
  restore and worktree archive to the daemon and pushes earlier local archive
  state once; the Storage setting is still to come.

- The owner-only size-policy backend supports revisioned 1/2/5/10 GB SQLite-page
  targets and warning status (ADR 0046). Recent-detail pressure is opt-in via an
  explicit policy save. Desktop now exposes a separate Save database target action,
  allowed targets and warning percentage, compatible with older detail-only daemons. It reclaims
  free pages first and completes bounded expired sweeps before recent detail,
  preserving Forever/off and all existing turn/ACK guards. It is not a hard
  filesystem quota or complete #128 media/archive/storage enforcement.

- Completed MCP/dynamic tool arguments above 5 KiB can retain a 4 KiB display-only
  excerpt with a visible trim label (ADR 0043), including a one-time upgrade of
  text-only compaction markers. Successful plain structured results now support
  sanitized display-only excerpts sharing the result text budget (ADR 0044).
  Resource-bearing shapes, payloads above the bounded lane and media retention remain separate work; existing turn/ACK/idle guards are unchanged.

- Journal replay-detail retention now also stores explicit file-diff excerpts with
  original line statistics (ADR 0040), following command output (ADR 0038) and tool
  text/reasoning (ADR 0039). The existing age/acknowledgment/latest-turn guards and
  per-pass byte budget apply. Configurable budgets, blob/media policies and Storage
  settings remain tracked in #128; these changes do not imply a deployed daemon.
  Adjacent redundant tool starts can also be removed without changing transcript
  order or dropping start-only fields; interleaved and incomplete cases remain
  protected (ADR 0041). Local detail-retention preferences now persist with revision
  checks and explicit IPC capabilities (ADR 0042); archive/media policy and
  the complete Storage page remain pending. The native Storage page now exposes the
  supported detail period and automatic-detail switch, revision-aware immediate saves,
  reload-on-conflict/error and advisory SQLite page usage; image/archive
  controls remain pending. Storage also shows advisory SQLite/WAL/SHM
  file lengths and filesystem availability, with a below-5-GB notice on refresh
  (ADR 0045); these are not total application storage or background monitoring.

- Desktop discovers conversations created by a phone or another daemon client while
  it remains connected, without switching the selected conversation or reloading
  existing transcripts. Catalog/event races recover through journal cursors.

- Conversation numbers and agent-to-agent messaging (0.1.49, ADR 0027): every
  daemon conversation has a host-unique `#4821` number, backfilled for existing
  journals and shown at the start of sidebar rows and the conversation header.
  Claude, Codex and Cursor turns receive their own `ketplus_agents` MCP server
  (the daemon executable's `--mcp-agents` mode, a per-conversation token and an
  agents-only IPC scope). Messages to idle conversations start a turn; busy ones
  receive them when the turn ends, or by steering when `urgent`; a final answer
  returns as the reply. Hop, chain and hourly limits hold runaway exchanges for
  the user's next turn, and requests expire after 24 hours. Delivered messages
  show as labelled agent bubbles, the sender and receiver get "Sent to @agent/N" and
  "Sent as the reply" notes, a woken conversation's sidebar row names its sender
  while the turn runs, and `#` in the composer completes numbers that the daemon
  resolves into references for the agent. Verified with fake providers, a
  metadata-only Codex probe and an authorized live run (Claude Sonnet and Codex
  `gpt-5.6-luna`): both directions of `agents_send`, the automatic reply,
  re-injection after a daemon restart and a fork's own number. Cursor live
  turns and Codex per-thread config on `thread/fork` alone are not verified.

- Automatic turn review (ADR 0067, #120). A conversation can name a
  Claude reviewer conversation, an existing one or a new read-only one, through
  local `session.reviewer.get/set`. Each completed turn with changes sends the
  reviewer one `review` request with the diff between two worktree snapshots
  (tracked and untracked files). The review turn runs in Claude plan mode with
  every permission request denied. Codex is not a reviewer yet, because its
  process-wide browser MCP server approves itself. A reply with findings starts
  the working agent's fix turn until `max_rounds` (default 2); "no findings",
  auto-fix off or the last round only records the reply. Verified with the
  scripted fake providers and sidecar tests. The chat header's Reviewer button
  (#159, as in the mock's ConversationHeader) offers No reviewer, a new Claude
  review agent or another Claude conversation. It is hidden in compact windows
  and until the conversation's first turn. The transcript shows review requests,
  skipped reviews and recorded reviews attributed to the reviewer. Verified
  offscreen and against a real daemon with the fake provider. No live provider
  run or native capture was made.

- Claude usage separates SDK session estimates/tokens and query budget from optional
  Admin-key organization month-to-date cost/tokens. Admin credentials remain in a
  separate host Keychain entry, outside agent environments and journals. Reports are
  available read-only to paired iPhones through a context-fenced configuration read;
  they do not pretend to expose prepaid balance or account spending limits.

- Transcript links open text/source files in Agent detail editors; images, PDFs,
  audio and video use the platform default app (asking first outside the worktree);
  folders, application bundles, executables and other types are revealed in the file
  manager, never launched. Web and mail links open; other schemes ask with the full
  URL, and network file locations (UNC, `file://host`) are refused.
  MIME and bounded content checks keep binary data out of transcript-opened editors.

- Claude BYOK on macOS: Settings → Providers saves a host-scoped key in Keychain,
  checks Anthropic catalog access, selects inherited/API authentication, and sets
  an optional estimated query budget. Local-only IPC never journals keys; paired
  phones use the host configuration. Active queries retain their credentials;
  background-task rotation is fenced. See ADR 0026 for verification and release limits.

- Viewed/generated local images are retained as session-scoped daemon media before
  journaling. Desktop and existing iOS media readers use those references after
  browser capture cleanup; missing/oversized sources retain an explicit unavailable
  result. Existing path-only history still requires its original file.

- Claude conversations advertise worktree switching to Desktop and iPhone between
  turns, after background tasks finish. The same provider thread and conversation
  resume in the selected cwd; returning to a previous cwd copies the newest persisted
  Claude transcript instead of reusing an older project-folder copy. Cursor remains
  gated. Installed-device acceptance requires the updated daemon.

- Private desktop chrome and Agent Workspace UI use embedded KÉT Sans 1.1
  hinted TTF faces (Light/Regular/Medium/Bold), with the existing platform fallback
  stack and unchanged editor/code monospace settings; KÉT Sans Mono 0.10.4 Medium
  is embedded and registered beside it. Both are SIL OFL 1.1. Core's
  `ketplus_cm_add_ket_fonts()` (`core/cmake/KetFonts.cmake`) pins the official
  KÉT Sans archive by SHA-256; font files stay in compiled resources and their
  licenses ship with application notices. CM's defaults are unchanged.

- Desktop compaction status reconciles daemon live events with retained transcript windows.
  Returning after an inactive completion clears the compacting overlay; restoring an
  ongoing compaction preserves it. Canonical item identity prevents duplicate rows, and
  duplicate terminal events cannot reopen a completed compaction. Fake-daemon regressions
  include provider activity arriving during the refresh and overlapping compactions.

- WebRTC host transport phase 3 (0.1.37): libdatachannel with signed SDP,
  active-pairing verification, expiring grants, revocation, connection epochs,
  three bounded ordered channels, cursor subscriptions and encrypted command
  results. WebRTC and mailbox dispatch share the journal's command deduplication
  and the cloud-safe encrypted event projection. Native clients remain phase 4;
  `events.forward_only/1`, relay-frame-only delivery and removal of legacy event
  upload remain gated for the later migration. The current mobile app keeps its
  Control v2 path. See `remote-transport.md` for the precise host wire contract.

- Async assistant questions expire with their originating turn (completion, provider stop,
  or a new turn). Canonical expiry receipts hide live and replayed unanswered question
  cards without leaving empty transcript rows; answered cards remain visible. Late items cannot
  reopen an old question, and an answer queued across completion cannot start another turn.
  Blocking provider questions keep their existing arbitration. Focused fake-provider tests.

- Negotiated daemon activity summaries (0.1.30, ADR 0022): ordinary local Desktop
  transcript reads and live events omit grouped command/tool details. Opening a
  group loads bounded, cursor-fenced detail pages; output/status changes catch up
  while open, collapse fences late replies, and explicit conversation copy still
  reads full canonical history. Capture/compare and screenshot observations stay
  outside groups. Legacy clients and the current relay/mobile projection are unchanged.

- Explicit economic context harness (ADR 0021): persisted `compactWindow` on new
  and resumed daemon turns, idle configuration changes and forks. Codex receives
  native compact config; Claude receives `extraArgs.autocompact`. Model capacity
  is preserved, defaults stay provider-owned, and background tasks prevent policy
  changes that would close their query. Claude usage retains its resolved
  `autoCompactThreshold` when available. Deterministic fake-provider/SDK coverage;
  native threshold resolution and cost savings are not verified. Codex 0.160.0
  accepted the config in a metadata-only start/resume probe with no model turn.
  The native model picker exposes Auto/272k/500k/1M/Custom, preserves drafts
  until Apply, supports Cancel and revision-fenced Retry, and blocks compact
  changes during active/background work or disconnection. Physical model capacity
  remains a separate reported value; unknown capacity is not guessed.

- Desktop consumes CM `cm-v0.3.6`. Reopening an existing file refreshes clean
  buffers from disk while retaining unsaved edits. Mermaid uses the canonical KétSuite light/dark
  theme snapshots shared with iOS: Inter 13px, compact geometry, semantic outcome
  classes, rounded connectors and 12px rectangle corners while retaining pills.
  Qt SVG keeps explicit `classDef`/`style` colours, readable state labels and early
  sequence styles. The v7 renderer cache regenerates diagrams from older themes.
  GitService cancels pending Git processes before teardown so callbacks cannot
  access destroyed snapshot/worktree data when windows close.

- Codex forks request `excludeTurns`: forked history comes from the local journal,
  avoiding oversized provider responses that terminate the shared app-server.
- A Codex message over 16 MiB no longer kills the shared app-server and every
  conversation on it: the daemon skips that line, shows a notice in its thread,
  and still ends the turn when the skipped message was `turn/completed`.

- Claude sidecar completes a settled turn at the SDK result boundary instead of
  waiting for lingering output after closing prompt input. Accepted unanswered
  steering still keeps the turn open. A fake-SDK regression covers output that
  never ends after its result.
- Provider host instructions carry the restored 0.1.10 response guidance
  from `runtime/ResponseGuidance.h`. Claude (system-prompt append) and Codex
  (`developerInstructions`) receive it once per session instead of in each user
  message, where every turn added another ~1.2k-token copy to the history. Cursor's
  ACP has no system prompt, so it still arrives with each turn in the host block.
  The guidance asks for Vietnamese self-address "tui", light playful
  tone, accurate 🟢/🟡/🔴 progress and concrete next actions. The longer flirt-comedy
  prompt is removed. With the ketplus_agents tools (ADR 0106), each user-started
  turn records its work with the `work_report` tool instead of a markdown matrix:
  - the daemon merges items by key into the conversation's ledger, so unfinished work
    carries forward and implementation, verification and delivery stay separate checks;
  - the turn closes with the mock's report card, which counts the items itself; while the
    turn runs, the same card closes the live transcript with a Running badge and the
    turn's activity;
  - Copy conversation exports the report as a table.

  Without the tools, or when the call fails, the turn still ends with the localized
  task/status/evidence/next-action matrix. The header's Work report button and the
  card's footer open one Report tab in the right pane. It shows the selected
  conversation's whole ledger (`session.work_report.read`), with Open and All filters,
  and follows its live `work_report.updated` events. Steered questions or added tasks resume the prior
  unfinished objective afterward unless the user cancels, pauses or replaces it.
  After compaction, agents are instructed to verify and announce repository,
  absolute worktree path, branch and next task before resuming mutations.
  After opening, reviewing or merging a pull request, agents remove that pull
  request's linked worktree when it is clean, pushed and unused, and never the
  repository's main checkout; after a merge they also delete the local branch
  with `git branch -d`. Removal is the turn's last command, run with
  `git -C <main checkout>`, because commands fail once their working directory
  is gone; the conversation then continues from the main checkout (ADR 0018).
  The suffix stays outside user-message and steer transcript
  text. Tests verify provider delivery; model style compliance is not guaranteed.

- Claude background tasks survive the end of their turn: the sidecar keeps the query
  open while they run, the daemon journals and lists them, and desktop shows a bar
  above the composer with output and Stop. iOS shows the same list through the relay
  (`session.task.stop`/`session.task.output`). Not yet verified against a live
  provider run.

- Async assistant-question answers use the conversation's current execution-context
  revision for both steering and follow-up turns; switched conversations no longer
  reject these internal submissions as stale. Delivery errors include the daemon's
  error code and message.

- Standalone browser worker under `apps/browser`, bundled as an external runtime
  on macOS Release builds. Its JSON-RPC/MCP API accepts caller-supplied URLs and
  compares semantic, accessibility, geometry, style, and raster snapshots.
- Browser tools for Codex, Claude and Cursor (ADRs 0008, 0009, 0011): on by
  default, public websites and loopback allowed (never the local network or
  metadata addresses, checked on the resolved IP), isolated temporary profile. Every provider
  uses the first runtime that resolves (bundled, then source checkout), and
  starts without browser tools when none resolves. Settings changes restart
  idle Codex processes and apply to Claude/Cursor on their next turn. The
  built-in skill also resolves in packaged apps outside a source checkout.
  Screenshots stay visible outside collapsed receipts/groups; browser rows omit protocol
  JSON while keeping concise errors and local export. MCP captures expose their
  original local PNG paths for agent asset/evidence reuse; copies must be made
  before browser-session cleanup (ADR 0023).
  Host instructions automatically route browser work through KetPlus on new and
  resumed sessions; disabled/unavailable tools produce explicit no-fallback guidance.
  Codex screenshots render in the transcript gallery; Claude/Cursor gallery
  parity remains separate from MCP tool availability.
- macOS computer use (ADR 0030): `apps/computer` builds the separately signed
  `KetPlus Computer.app`, nested in `Contents/Helpers`. It owns the TCC grants
  and serves the `ketplus_computer` MCP front. It enforces per-conversation app
  approval, tiers, capture of granted apps only, a single control lease, the
  Stop button and Esc kill switch, and an audit log. Input is background-first
  (ADR 0031): accessibility `observe`/`act`, coordinate clicks mapped to
  accessibility actions, keyboard events sent to one process and an overlay
  pointer; the real pointer is opt-in. Granted apps move to a work display,
  secondary by default, which screenshots show (ADR 0032). `modules/computer` gives Claude and Cursor the
  front with an HMAC conversation token when Settings → Computer use is on (off
  by default), and marks remote-started turns so the agent refuses them.
- Pro desktop shell under `apps/desktop`.
- Desktop repository metadata cached in SQLite for sidebar names,
  with background Git validation and QSettings recent-path ordering.
- Conversation-owned Git worktrees persisted in a separate desktop SQLite map
  at conversation create and turn completion, used by conversation-delete
  cleanup.
- AI Workspace and Codex app-server transport under `modules/ai`.
- KetPlus Daemon core under `apps/daemon`, with authenticated framed IPC, Codex,
  Claude, and Cursor process ownership, approval brokering, SQLite cursor replay, outbound Control
  API v2 synchronization, encrypted command/event envelopes, and provider-neutral
  transcript paging by turn. The daemon remains the full-history authority while
  the cloud keeps only an encrypted 24-hour hot replica.
- Desktop daemon client under `modules/remote`; the Agent Workspace starts or
  attaches daemon sessions. A negotiated local transcript window loads only the
  selected conversation's newest turn first, then the preceding two turns with
  event/byte limits and lossless within-turn backward paging. Reopening a warm
  conversation reuses its retained window when nothing changed; otherwise it
  replaces it with the newest three turns in one read.
  Cold inactive conversations are metadata-only;
  older daemons retain compatibility compact replay.
- Independent Linux headless daemon build and versioned runtime packaging
  (0.1.29), systemd user unit, authenticated `ketplusctl`, browser-approved
  device authorization and a durable zero-session workspace catalog. A Qt
  Control v2 client backend is available, but remote Desktop UI/composition
  remains deferred. ADR 0118 records the remote composition (one
  `MachineBackend` for the local socket, the Relay and SSH; machine binding;
  connection states; the SSH trust decision); its local adapter
  `DaemonMachineBackend` is in `ketplus_remote`. See `linux-headless-daemon.md`
  for coverage and acceptance.
- Conversation and linked-worktree visibility can be archived without
  deleting history or stopping an active turn. The daemon deletes a
  conversation 30 days after it was archived (ADR 0100). Archived items are restored only
  from Settings; active archived conversations retain an explicit Stop action.
- Conversation fork (ADR 0016): "Fork this conversation" in the conversation
  name and transcript right-click menus copies an idle Codex or Claude
  conversation through the provider's native fork and a journal copy. Cursor
  has no fork operation; forking from an earlier message is not offered yet.
- Worktrees have no workspace of their own (ADR 0111); conversations create
  and switch them. Settings is a full workspace page rather than a modal popup while
  continuing to reuse the public KetPlus CM settings controls and persistence.
- Streaming presentation is coalesced, offscreen Markdown work is deferred, and
  an anchored live response can retain its displayed source while the user reads
  above the bottom. Inactive session/view data use soft count/text budgets.
- Single-product packaging embeds the daemon beside the macOS application,
  advertises its product version during IPC authentication, and replaces an
  older idle daemon without interrupting active turns.
- Atlas process, WebView, private bundle index, bounded asynchronous library
  thumbnails, stable bundle/flow context, and last-valid recovery under
  `modules/atlas`, with metadata verification for delayed filesystem events and
  the KetAtlas CLI consumer pinned to 0.5.0. The library grid adapts between one,
  two, and three columns at desktop workspace widths.
- The desktop mock's ten screens support KetAtlas 0.5.0 opt-in Light/Dark sync
  without reloads or persisted theme overrides; disabling sync restores each
  mock screen's product/system preference. This does not synchronize native Qt
  appearance with the viewer.
- AI-assisted Atlas creation with a compact builder conversation, automatic
  KetAtlas output instructions, and in-tab preview activation.
- One public MPL 2.0 repository builds KétPlus (Lite, `core/`) and KétPlus Pro
  (ADR 0029).
- Desktop product zoom scales fonts, spacing, control heights and icons together;
  appearance saves synchronize open windows and refresh chat monospace fonts.
- Shared appearance settings from KetPlus CM are applied across the private
  shell, including editor/diff views, Markdown preview, and both standard and
  Agent Workspace terminal surfaces.
- Deterministic Pro integration suite on macOS and Linux CI.
- Layered Qt, FetchContent, compiler-object, and build-tree caching.
- Product, identity, repository, licensing, and remote-system direction in docs.
- Connections workspace for Ketsuite sign-in through the system browser, machine enrollment,
  QR-confirmed mobile pairing, device revocation, and reconnect visibility.

## Planned

- Agent teams ([ADR 0114](decisions/0114-agent-teams.md), epic #450): a leader, one
  to five developers and a Claude reviewer on one goal, with tasks, lanes, per-task
  review, decisions on the team page and a digest instead of status messages. Each
  step of #450 moves its part to Implemented.

- Cache/payer-aware automatic compact-window selection and UI policy/reason
  controls, based on actual runtime retention and occupancy rather than a shared
  timeout. Native boundary-only compaction needs provider capability verification.

- Deployment and production operation of the versioned cloud Control API.
- Entitlements and organization-aware session policy beyond personal workspaces.
- KétRouter client integration and cloud services.
- Push/background delivery and a lower-latency relay transport beyond bounded
  authenticated polling.
- WebRTC DataChannel transport for remote Desktop and mobile clients (direct, then
  TURN, then the HTTPS relay) with a forward-only relay that no longer stores
  transcript events; an offline host leaves clients with their own cache
  ([ADR 0024](decisions/0024-webrtc-transport-and-forward-only-relay.md)). The protocol
  (capability negotiation, signaling through the relay, peer authentication, frames) is specified
  in [remote transport](remote-transport.md) and
  [ADR 0025](decisions/0025-remote-transport-protocol.md).
- Computer use follow-ups ([ADR 0030](decisions/0030-ketplus-computer-agent-app.md)):
  Codex (per-conversation MCP binding), before/after thumbnails in the
  transcript, minimized windows and other Spaces, shortcuts
  that need a key window in background apps, and Windows.
- Windows daemon/desktop CI once required Qt WebView packages are available.
- Cross-block native selection/accessibility mapping and physical-device
  CPU/frame-time/RSS evidence for the bounded transcript renderer. Giant bodies
  already use block rows and the active presentation window is capped at 300
  messages / 16 MiB with cursor-backed rehydration.
- Production signing, notarization, a hosted update feed and its signing key,
  complete Qt notices, and About metadata. The in-app updater itself exists
  (ADR 0019) but is disabled in builds without `KETPLUS_UPDATE_FEED_URL`.

Planned behavior is not an existing API. Do not build code against a diagram or
future directory without first defining and testing its contract.

## Known structural debt

- `modules/ai/src/agent/qml/AgentWorkspace.qml` remains over 2,000 lines after
  header, empty state, status notice, run status, restore overlay, sidebar, and
  question card extraction. New work should continue extracting cohesive
  transcript, composer, and attachment components instead of extending the
  monolith.
- `CodexAppServer` keeps a direct codex app-server transport that only tests use
  (26 constructions in 8 Agent Workspace test files); the application always goes
  through the daemon. Its transports are separate translation units, but about 20
  `usesDaemon()` branches in the `AgentSession*` sources still serve only that test
  path. Retiring it means moving those tests onto a daemon fixture first.
- KétPlus (`core/src/app`) and KétPlus Pro (`apps/desktop/src/app`) each have their own
  `ketplus::MainWindow`, carried over from the two-repository era (ADR 0029). Shared
  behavior moves into `core` libraries one slice at a time; the Language, Line Endings
  and Encoding controls are `DocumentFormatControls` (`core/src/editor`). Search
  (`MainWindowSearch.cpp`, 78 of about 360 lines differ) is the next slice; Documents,
  Navigation and Panels have diverged and need their differences reconciled before they
  can be shared.
- Several test translation units exceed 400 lines. Split reusable fixtures and
  scenario groups when adding coverage, while preserving readable test intent.
- Unsigned developer packages are implemented, but signing, notarization, and
  compliance automation remain release blockers; see `licensing.md`.
- The daemon now owns active Codex processes, but terminal process ownership is
  still in the public CM runtime and has not moved to a persistent SessionHost.

## Supported verification

- Local macOS development with Qt 6.11+.
- GitHub Actions on macOS and Ubuntu.
- Offscreen deterministic Qt tests by default.
- Opt-in native and authenticated smoke tests documented in `testing.md`.

## CM document modes and terminal refresh

Desktop consumes the CM terminal with search, selection, clear scrollback and restart controls. Markdown opens in an embedded Preview by default; the floating Preview/Source control is centered horizontally, 100 logical pixels above the document bottom. Source Control keeps Diff as the default and adds Preview/Source for the right-hand Git revision. Content width is capped at 960 px; switching retains the rendered preview and source undo/cursor state.

### Desktop transcript Atlas alignment

The native transcript now follows the desktop Atlas request → actions → final-answer
layout. Consecutive commands, supported reads, searches and edits within a turn share
one collapsed activity summary. Expanding it shows concise individual actions; file
paths and diffs open only when the reader expands that edit. Evidence, questions,
provider notices and user messages keep their own visible rows. Public commentary and final answers remain visible in live and history projection;
private reasoning stays hidden. Both retain streaming and bounded viewport behavior.
User requests use the Atlas label, bubble, column gutters and spacing; final answers
have copy beneath them without a repeated provider heading. Browser URLs expose Open.
Deferred command details can fall back to retained canonical source with hash validation.

The edits in the existing large AgentMessageModel, session notification/state files,
TranscriptMessage and TranscriptView are limited to their existing projection, payload
resolution and presentation responsibilities. Splitting their lifecycle ownership is
outside this visual alignment and would increase the scroll/streaming regression surface.

Desktop outgoing messages use a darker primary surface (1.35 darkening factor) with white text, content-sized width,
12-point horizontal padding and no visual author row. They cap at 80% of the
reading column and collapse after 20 lines with a reversible Show more control.
Activity rows read as the mock's steps: one line with a short title ("Read", "Ran" or a
command's description) and what the step acted on beside it, every grouped step keeps its
kind's icon, and a failure adds a line with its exit code and duration while the group
summary stays neutral. Provider items map to the mock's step titles: Codex command actions
read as Read, Searched code or Listed files; Codex web actions as Opened a page or Found in
page; Claude Write, Edit and MultiEdit as file changes with line counts; MCP tools as
"Used server: tool". A turn that edited files closes with the mock's changed-files card
above its end: its file-change rows and its daemon groups' `files` (ADR 0022), merged by path;
a row opens the file's diff and Review changes opens Source Control. A steer stays in its turn,
counts the daemon could not tell show no numbers, and a list a group cut short (50 files) reads
"N+ files changed" without totals. Its footer reads "Not committed", as in the mock, while
Source Control shows one of the files uncommitted in the conversation's checkout. Browser-open rows retain
an Open action. A provider failure reads as the kit's danger banner; on the latest failed turn,
while the conversation is idle, Retry sends that turn's prompt again as a new turn (not for
a prompt with images, one from another agent, or while the composer holds attachments). Composer block line-height matches
the transcript at 1.75; focused Qt tests cover Vietnamese single-line sizing and composer undo.

## Qt design mock

The design reference is the Qt Quick mock in `mockups/qt` (ADR 0033), built
from the Két Qt kit. It covers every screen and workflow of the frozen
`ketplus.ketatlas` with deterministic fixtures, and `--smoke` fails on any QML
warning. It is a standalone CMake project, not part of the Lite or Pro build.

KetPlus Terminal has its own Qt Widgets mock in `mockups/terminal` (ADR 0034).
It compiles the shipped terminal from `core/src`, runs a real shell, and draws
the window, colours and KÉT Sans Mono type from the Két Qt kit.

Core links the Két Qt kit (`KetQt::Tokens`, `Icons`, `Widgets`) into `ketplus_ui`
and requires Qt 6.8 (ADR 0035). KetPlus Terminal's window is the first shipped UI
drawn by it (#146): `ket::Style`, the KétPlus profile and `ket::Appearance` replace
`ThemeManager` in that app only; Lite and Pro keep `ThemeManager` until #142. The rest
of the shipped UI moves to it area by area (#141–#167).

## Conversation repository selector design prototype

The Desktop Atlas now includes shared Desktop/iPhone repository-and-worktree
picker prototypes. Current repository stays first; a simulated acknowledged
selection moves the existing conversation to the chosen repository, updates its
header and scope, and promotes the sidebar group while retaining its draft.
The iPhone variant uses a two-step bottom sheet. Search, failed acknowledgment,
offline/reconnect and stale-catalog recovery are covered by focused mock tests.
See `ketplus.ketatlas/README.md` for fixtures and evidence.
The approved flow is implemented in the native Qt picker and SwiftUI companion.
Acknowledged switches preserve the conversation anchor/draft, regroup the sidebar
and update Files/Source Control and file search. Local repositories include both
recent folders and the daemon’s enrolled catalog. Cross-repository remote use
requires the companion relay target-grant authorization update; deployment and
installed-bundle verification remain separate from source/build verification.


The unreviewed UI stack adds local Desktop release of explicitly selected held
agent messages (#130 / #163). The daemon authenticates the UI, checks message IDs
and conversation generation, persists authorization/claims, queues busy receivers
and reconciles a journaled input after restart before any repeat invocation.
The transcript action observes status without granting authorization; only a
click may start a turn. This does not add conversation Plan/harness behavior,
remote mobile authorization or an installed daemon update. ADR 0027 documents
the protocol and limits; tests use fake providers only.

Completed assistant Markdown links to local Atlas manifests now render compact
result cards with current manifest counts and explicit Edit/Open actions. The
receipt identifies a linked artifact, not successful preview validation. Invalid,
missing, oversized or out-of-worktree manifests are unavailable; ambiguous
relative links after a context change are blocked. No provider invocation is
required to inspect a receipt.
