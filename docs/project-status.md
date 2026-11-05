# Project status

This file distinguishes current code from target architecture. Update it when a
milestone changes state.

## Implemented

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
  application bundles and other non-text resources use the platform default app.
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

- Private desktop chrome and Agent Workspace UI use embedded KÉT Sans 1.0.0
  hinted TTF faces (Light/Regular/Medium/Bold), with the existing platform fallback
  stack and unchanged editor/code monospace settings. The official archive is
  SHA-256 pinned; font files stay in compiled resources and its license ships
  with application notices. CM's defaults are unchanged.

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
- Provider-bound start/steer input uses the restored 0.1.10 response guidance
  from `runtime/ResponseGuidance.h`: Vietnamese self-address "tui", light playful
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
  default, all websites allowed, isolated temporary profile. Every provider
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


## Conversation repository selector design prototype

The Desktop Atlas now includes shared Desktop/iPhone repository-and-worktree
picker prototypes. Current repository stays first; a simulated acknowledged
selection moves the existing conversation to the chosen repository, updates its
header and scope, and promotes the sidebar group while retaining its draft.
The iPhone variant uses a two-step bottom sheet. Search, failed acknowledgment,
offline/reconnect and stale-catalog recovery are covered by focused mock tests.
See `ketplus-desktop-redesign.ketatlas/README.md` for fixtures and evidence.
The approved flow is implemented in the native Qt picker and SwiftUI companion.
Acknowledged switches preserve the conversation anchor/draft, regroup the sidebar
and update Files/Source Control and file search. Local repositories include both
recent folders and the daemon’s enrolled catalog. Cross-repository remote use
requires the companion relay target-grant authorization update; deployment and
installed-bundle verification remain separate from source/build verification.
