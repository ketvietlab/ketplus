# Project status

This file distinguishes current code from target architecture. Update it when a
milestone changes state.

## Implemented

- Image publication requires a checked atomic ownership-file update (ADR 0067).
  Invalid metadata is preserved, repeated hashes are deduplicated and failed
  writes cannot publish successful new references. Fork-copy and orphan bounds
  remain separate work.

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

- Failed activity-backfill commits now preserve their diagnostic and release the
  write transaction (ADR 0064). A separate connection can repair and reopen while
  the failed journal object remains alive; broader startup recovery remains open.

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
  show as labelled agent bubbles, the sender and receiver get "Sent to #N" and
  "Sent as the reply" notes, a woken conversation's sidebar row names its sender
  while the turn runs, and `#` in the composer completes numbers that the daemon
  resolves into references for the agent. Verified with fake providers, a
  metadata-only Codex probe and an authorized live run (Claude Sonnet and Codex
  `gpt-5.6-luna`): both directions of `agents_send`, the automatic reply,
  re-injection after a daemon restart and a fork's own number. Cursor live
  turns and Codex per-thread config on `thread/fork` alone are not verified.

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
  prompt is removed. Each turn ends with a localized task/status/evidence/next-action
  matrix that carries unfinished requests forward and distinguishes implementation,
  verification and delivery. Steered questions or added tasks resume the prior
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
  remains deferred. See `linux-headless-daemon.md` for coverage and acceptance.
- Conversation and linked-worktree visibility can be archived locally without
  deleting history or stopping an active turn. Archived items are restored only
  from Settings; active archived conversations retain an explicit Stop action.
- Conversation fork (ADR 0016): "Fork this conversation" in the conversation
  name and transcript right-click menus copies an idle Codex or Claude
  conversation through the provider's native fork and a journal copy. Cursor
  has no fork operation; forking from an earlier message is not offered yet.
- The product sidebar opens a dedicated Worktrees workspace with live
  conversation/activity state, create/manage actions, and the archived-items
  route. Settings is a full workspace page rather than a modal popup while
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
Collapsed activity rows keep one title line; browser-open rows retain an Open
action. Composer block line-height matches
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
