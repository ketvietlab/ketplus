# KetPlus Daemon architecture

Status: local ownership and outbound Control API v2 transport are implemented;
staging deployment and production operations remain pending.

The independent Linux headless build, device authorization, local workspace
administration and remote-client scope are recorded in
[`linux-headless-daemon.md`](linux-headless-daemon.md) and ADR 0022. Desktop
remote UI is deferred; the local socket is not exposed over TCP.

## Role

KetPlus Daemon is the closed-source local control plane for managed remote
sessions. It lives in `apps/daemon` in this repository and runs independently
from the desktop UI. The initial implementation uses C++20 with non-GUI Qt
Core/Network/Sql facilities so it can reuse the private toolchain without
depending on Widgets or QML.

The target daemon owns:

- PTY processes and their lifecycle;
- worktree/session association and resumable session metadata;
- local capability enforcement for filesystem, terminal, and process actions;
- device identity and secure credential storage integration;
- authenticated local IPC with KetPlus Desktop;
- an outbound authenticated connection to the cloud relay;
- bounded buffering, reconnect, heartbeats, and session cleanup.

It does not own editor UI, AI presentation, organization membership, billing,
model routing, or cloud authorization policy. KétRouter remains a cloud service;
the daemon may carry authorized session traffic but must not embed Router billing
or provider secrets intended for the service.

## Implemented local slice

The explicit economic context harness (ADR 0021) accepts
`payload.options.compactWindow` on `session.start_turn`, including existing
sessions that resume a native thread. It is an integer request from 100,000 to
1,000,000 tokens; omission inherits the journaled request, and `null` removes
KetPlus's override. Idle `session.configuration.apply` also accepts it with the
usual revision fences. Invalid values fail before turn creation; Cursor rejects
non-null windows. Minor-1 negotiation advertises `session.compact_window/1`.

The journal persists the request through restart and fork. Session configuration,
list and configuration-change events expose `effective.compactWindow`, which is
the requested policy, not a resolved native threshold. Codex receives compact
config on native start/resume/fork without overriding physical capacity; Claude
receives SDK `extraArgs.autocompact` without replacing its environment. Changes
wait for foreground and background tools to be idle. Claude usage preserves
`autoCompactThreshold` when reported; physical `modelContextWindow` stays separate.
The default inherits provider configuration. Desktop's model picker and the
iOS client expose Auto, 272k, 500k, 1M and a validated custom value.
Drafts are applied explicitly, with a captured configuration revision; conflicts
retain the draft and refresh the fence for Retry. Desktop negotiates the minor-1
capability; mobile recognizes the compact field in the configuration result.
Older daemons keep ordinary model selection and omit the compact field.
Automatic cache/payer selection remains planned, and native auto-compaction
timing remains provider-owned; see [ADR 0021](decisions/0021-economic-compact-window.md).

An opt-in local worktree-context protocol keeps conversation identity stable when
switching between idle Codex turns. Context history and switch events are durable;
legacy remote resource access stays gated after switching until its provenance
migration is complete. See [ADR 0015](decisions/0015-conversation-execution-contexts.md).

`apps/daemon` now builds the headless `ketplus_daemon` executable and a reusable
core target. The local slice includes:

- a user-restricted `QLocalServer` endpoint authenticated by an owner-only local
  token;
- the length-prefixed JSON v0 contract accepted in ADR 0002;
- provider-neutral session operations and normalized events backed by SQLite
  WAL cursor replay and durable command deduplication;
- durable conversation-title mutation through `session.rename`, including
  Codex `thread/name/set` synchronization and normalized title-update replay;
- durable sidebar read markers through `session.mark_seen` (`last_seen_cursor` in
  the journal; `session.list` exposes both `last_event_cursor` and
  `last_seen_cursor` so Desktop can restore unread completed conversations);
- idle conversation forks through `session.fork` (ADR 0016): the provider forks
  its native thread (Codex `thread/fork`, Claude SDK `forkSession`), then one
  journal transaction copies the source generation's transcript at the same
  cursors and the fork appends `session.forked`;
- daemon-owned Codex app-server lifecycle;
- approval and provider-neutral structured user-input requests with stable
  request/question revisions, generation fencing, strict single/multiple/custom
  answer validation, defer, first-responder arbitration, and expiry. Malformed or
  oversized provider question sets fail as one JSON-RPC request; the daemon never
  truncates, answers, or exposes a valid subset. Defer is idempotent per request
  revision, so retries cannot append duplicate lifecycle events;
- bounded inbound frames, event payloads, replay pages, and outbound socket
  queues. A client with more than one MiB of unread answers is not read from
  until it catches up, so a burst of requests waits instead of overrunning the
  two-MiB queue; the queue limit still disconnects a client that stops reading
  while events are broadcast to it.
- daemon-owned model discovery and rate-limit queries used by Desktop settings.

Desktop uses this endpoint through `modules/remote`. It discovers existing
sessions, attaches and replays ordered journal events, retries in-flight commands
with the same idempotency key after disconnect, and starts a detached daemon if
no endpoint exists. It keeps at most sixteen requests in flight and sends the rest
as answers arrive; identical reads (`session.list`, `session.pending_approvals`,
provider and remote status queries) waiting to be sent are merged into one.
Reconnects back off from 100 ms to 5 s, and the backoff resets only after a
connection has stayed up for five seconds. Closing Desktop leaves active turns under daemon ownership.
While connected, an event for an unknown session schedules a coalesced `session.list`
read. Desktop imports only new catalog entries, preserving existing selections,
transcript windows and replay cursors. Events racing the initial catalog or a later
discovery are tracked by generation/cursor and journal replay fills any gap after
the catalog snapshot. Discovery responses from an old connection are discarded.
The reconciliation lives in `CodexDaemonDiscovery.cpp`; the existing large
`CodexAppServer.cpp` retains only transport lifecycle and event-dispatch hooks.
Streamed text reaches the journal coalesced: consecutive `assistant.delta` or
`command.output_delta` notifications for one item are held for at most 150 ms
(or 16 KiB) and journaled as one event. Any other event, a delta of another
item, and SessionHost teardown journal the held text first, so provider order
is kept. One event per provider chunk (a word or less) had every reader, above
all a phone paging the relay, read tens of thousands of events per conversation.
The journal transactionally maintains a durable compact replay projection. It
omits command-output deltas that the transcript does not consume and removes
assistant deltas once a completed item supersedes them; deltas for an incomplete
live item remain replayable. Existing journals are backfilled once when a daemon
with projection support first opens them. Subsequent attaches query the
projection directly instead of rescanning and parsing the raw event history on
every page. The raw journal remains available for non-compacted reconnects and
cursor acknowledgement; pruning raw events does not remove the compact
projection.

The same projection exposes `session.transcript_page`. The journal assigns a durable
provider-neutral `turn_sequence`, returns the newest three turns by default, and pages backward by
turn boundary without parsing the full journal. Pages are filled newest-first under the replay
event/byte budget; when a turn is cut the page reports `truncated: true`, `has_more_before: true`
and `first_event_cursor` (oldest event kept). Continue with `before_cursor = first_event_cursor`
(exclusive event cursor, mutually exclusive with `before_turn_sequence`) to receive the rest of
that turn and then earlier turns; `before_turn_sequence` keeps its exclusive turn semantics. Session summaries also expose a derived title and
opaque repository/worktree hierarchy metadata for native clients. See ADR 0004.

For bounded Desktop restore, the additive local-only `session.transcript_window`
returns a newest-three-turn **upper bound**, further limited to 128 events and
512 KiB. It has intra-turn cursor continuation, explicit partial assistant
prefixes, and a durable compaction revision fence. A stale continuation returns
`snapshot_changed`, never a silently incomplete historical snapshot. Current
lifecycle and pending approvals are delivered separately from historical events.

`session.transcript_window` also pages **forward** inside the same fenced snapshot:
`after_cursor` (exclusive event cursor, mutually exclusive with `before_cursor`, rejected for
restore requests) plus the previous `snapshot_cursor` and `projection_revision` returns the oldest
bounded prefix of the next `turn_limit` turns after that cursor. Every page reports
`after_cursor` (last event cursor, or the request cursor when empty) and `has_more_after`
(a newer projected event exists at or below the snapshot). Events after the snapshot are never
returned; live delivery owns them. `turn_limit` accepts 1-20 (was 1-3); the event and byte caps
are unchanged. `session.list` advertises `transcript_window_max_turns: 20` and
`transcript_window_after_cursor: true`; clients that do not see these keep `turn_limit <= 3`
and fall back to an explicit newest-window reload instead of forward paging.
Forward paging is **local only**: `after_cursor` is not available over the cloud relay. Relayed
(mobile) transcript windows are served as restore requests with `turn_limit` capped at 3, and a
relayed `after_cursor` is rejected with `invalid_page` ("Restore requests cannot page forward with
after_cursor."), so a mobile client that needs newer rows reloads the newest window instead.
Existing cloud/mobile `session.transcript_page` behavior is unchanged. See
[ADR 0005](decisions/0005-bounded-local-transcript-windows.md) for the exact local
contract, access bounds, and remaining renderer responsibilities.

Replay responses are bounded by both count and an encoded-byte allowance below
the one-MiB local frame limit. A supporting daemon advertises this bound in
`session.list`, allowing Desktop to request larger pages; Desktop retains the
legacy 16-event page size when an older protocol-compatible daemon does not
advertise the capability. Desktop serializes replay, prioritizes the selected
conversation and worktree, and yields to the UI event loop between pages.
On macOS the product bundle contains the matching daemon helper. The handshake
reports the daemon product version: a newer Desktop asks an older idle daemon to
exit and then launches its bundled helper, but it continues using an older
protocol-compatible daemon while any turn or approval is active. Launching an
older Desktop never downgrades a newer daemon.
Terminal ownership remains in KetPlus CM. The daemon now enrolls its locally
generated X25519/Ed25519 identity through native Ketsuite credentials, obtains
short-lived challenge credentials, publishes generation-fenced session state,
uploads stable AES-256-GCM event envelopes from a durable retry cache, and polls
serialized encrypted commands. Pairing is explicitly confirmed on the Mac; a
closed pairing dialog revokes its outstanding ticket and paired devices can be
revoked from Connections.

Cloud identity keys, session keys, wrapped device keys and account identifiers
are stored in one owner-only (0600) JSON file,
`Ketsuite/KetPlus/daemon/credentials-<service>.json` under the user's data
directory, not in the macOS keychain. Keychain items are bound to the code
signature, so each rebuilt or re-signed helper prompted once per key. A file
with unsafe permissions or unreadable contents is an error and is never
replaced with newly generated keys. Keychain items from earlier builds are not
migrated, so an existing machine signs in and pairs again once after upgrading.

Deterministic tests use a fake provider and consume
no model quota. Heartbeat snapshots are split into lossless, sequential batches
of at most 100 sessions, matching the Control API request bound; a later batch
is not sent after an earlier failure and event/key synchronization starts only
after the complete snapshot succeeds.

The relay accepts the rest of a snapshot when it refuses some sessions (owned by
another enrollment of the Mac, or behind the stored generation) and names them in
`rejectedSessions`. Until a later heartbeat accepts them, the daemon keeps those
sessions local: it uploads none of their events and wraps none of their keys,
neither for known devices nor in a pairing approval, which the relay would refuse
as a whole for one envelope it cannot hold.

Control requests use HTTP/1.1, never HTTP/2. Over HTTP/2 every request shared
one connection, and through a Cloudflare tunnel one event upload stalled it:
heartbeats, command polls and acks timed out behind it for 30 s, so phones saw
the Mac offline and commands were claimed again and again.

A control reply that fails in transport (connection loss, transfer timeout,
protocol error) is a failure even when its status line had already arrived.
Treated as success, a `200` command poll whose body was lost dropped the
commands the relay had just leased; they came back only after the 30 s lease
lapsed, and each session's queue waited behind them. The daemon logs a poll
that lost its leased commands and every failed command acknowledgement.

Event uploads to `/v2/events:append` hold at most 100 events and at most
768 KiB of encrypted envelopes, below the relay's 1 MiB request limit; a single
event is always sent alone. When the relay, or a proxy in front of it, still
answers 413 (with or without a JSON body), the daemon halves that session's
batch and retries from the same cursor, down to one event, and doubles it again
after each accepted batch. Any other failed upload makes that session generation
wait before its events are sent again, one second doubling on each consecutive
failure: up to five minutes after a refusal (a 4xx other than 401, 408 and 429,
such as `stale_generation` for a session the relay has deleted), and up to one
minute when the relay cannot be reached or fails, a wait that the next successful
heartbeat ends. New journal events do not shorten the wait; without it, every
event of every session retried each refused upload. A 401 waits only for the next
credential, and an accepted batch clears the wait. Streamed deltas and `item.completed` envelopes also
carry a plaintext `itemKey` (a truncated SHA-256 of session, generation and
item id, never the id itself), and a completion whose cloud payload holds the
item's whole text carries `supersedesDeltas`, so the relay can drop the
streamed copy the way the local journal does. Neither field is inside the
signed content. Each session's cloud cursor is registered with the
journal once per sign-in, not on every upload, and an acknowledgement at or
behind the stored cursor writes nothing. An acknowledgement checks only the
large payloads its removed raw events name, finding replay manifests through
the item index; the retention pass (at open, after a session delete and every
256 command results) collects any other unreferenced payload.

Before an event enters the cloud encryption and retry path, the daemon projects
command lifecycle payloads to the full command text plus identity, status and
exit code. It removes aggregated output, output deltas and command actions. The
local SQLite journal remains unchanged and authoritative, while neither relay
ciphertext nor mobile replay carries command output. A phone reads one command's
output tail (`session.output.read`) or a bounded JPEG of an image the journal
references (`session.media.read`) on demand; neither result is stored on the
relay. The same `session.media.read` with a worktree-relative `path` instead of
`media_ref` previews an image from the session's worktree as it is on disk now
(no revision fence; symlinks, paths escaping the worktree and files over 16 MB
are refused). Previews are decoded off the daemon's main thread, scaled to at
most 1280 px and flattened onto white.

Codex `fileChange` items are journaled with a worktree-relative `worktreePath`
on each change, or `outsideWorktree: true` when the file is not beneath the
worktree, so a phone can open the changed file. Codex account rate limits
(`account/rateLimits/updated`) are journaled as a `usage.limits` event, with
`provider: "codex"`, on every Codex conversation that is starting or running a
turn, as the Claude sidecar already does for its plan limits.

Cloud commands, whether from mobile or the web console, run only an explicit set
of session operations: create, start turn, rename, steer, interrupt, delete,
approval and question answers, pending approvals, configuration, execution
policy and mode, attachment staging, projected transcript restore, the output
and image reads above and read-only workspace files. Journal reads (`session.list`, `attach`, `events_after`,
`transcript_page`, `transcript_window`, `transcript_large_event`) return
unprojected payloads and `client.ack_cursor` moves local retention, so those
remain local-only and are rejected remotely.

Read-only workspace files open each path component relative to the worktree
root without following symlinks and require a regular file, so a component
swapped for a link after validation cannot escape the worktree and FIFOs cannot
block the daemon. `.git` is rejected case-insensitively.

Paired devices see only what the repository shares. Git-ignored paths (build
output, `node_modules`, local config) and paths that usually hold credentials
(`.env` and `.env.*` except `.example`/`.sample`/`.template`, `*.pem`, `*.key`,
`*.p12`, `*.pfx`, SSH private keys, `.npmrc`, `.netrc`, `.git-credentials` and
similar) are left out of tree listings and search. File, diff and image reads of
them fail as `permission_denied`. In a repository, search goes through
`git ls-files --cached --others --exclude-standard`, so ignored folders don't use
up its 5,000-file, 350 ms budget. `truncated` is set only when files remained.

Every git command on the read path runs with the hardened arguments and
environment: no optional locks, fsmonitor and the untracked cache off, no pager,
external diff, ssh command or credential helper, and inherited
`GIT_DIR`/`GIT_WORK_TREE`/`GIT_INDEX_FILE` cleared. Each configured
`filter.<driver>` (from `git config --get-regexp`) also gets empty `clean`,
`smudge` and `process` overrides, so a repository's filter never runs during
status or diff. As a result, Git LFS files can appear modified to a phone.
Remote worktree creation uses the same environment with fsmonitor off. Its
checkout still runs the repository's hooks and filters, just as
`git worktree add` does on this machine.

Workspace file reads are fenced by the worktree revision the daemon publishes in
each heartbeat (a hash of HEAD, the status and dirty content). The relay only
accepts a read naming the revision it lists, which trails the worktree by up to a
heartbeat while an agent edits it. A read naming a revision the daemon published
for that session in the last 10 minutes is served from the worktree as it is
now and answers with the requested revision, without running git again. Any
other revision is recomputed with git before and after the read and fails as
`stale_revision` when it differs. Heartbeats compute revisions on their own
thread, so they never queue a phone's read.

The daemon process
runs with umask 077; its state and token directories are owner-only, and the
token, journal and SQLite WAL/SHM files are 0600. A token file that is a symlink
or owned by another account is rejected.

Advanced model, effort and execution controls are revision-fenced daemon
commands backed by the active adapter's advertised capabilities. Encrypted
attachments use bounded, individually durable command chunks and a locally
validated staging journal; see
[ADR 0007](decisions/0007-capability-configuration-and-attachment-staging.md).

Assistant/tool lifecycle payloads larger than 256 KiB use the bounded, versioned
chunk and terminal-manifest representation in
[ADR 0006](decisions/0006-bounded-large-transcript-events.md). Local Desktop
reassembles them losslessly through bounded fragment requests; every opaque cloud
event remains below the existing cap. Mobile must implement the ADR handoff and
an authenticated cold-fragment fetch before claiming cold large-item coverage.

## Target structure

```text
apps/daemon/
  src/main.cpp
  src/service/        process lifetime and configuration
modules/remote/
  src/DaemonClient.*  authenticated Desktop IPC, startup, retry, and replay
modules/accounts/
  src/device/         device registration and credential references
tests/daemon/         lifecycle, authorization, reconnect, and isolation tests
```

Protocol code should be transport-independent. The desktop and daemon may link
`modules/remote`, but neither should include the other's executable internals.

## Trust boundaries

```text
mobile app
  -> Ketsuite Gateway
     -> KetPlus Control API
     -> KetPlus Relay
        -> outbound daemon connection
           -> local capability check
              -> PTY, process, or workspace operation
```

- Expose no unauthenticated public listening port.
- Authenticate both the device and each remote session.
- Use short-lived session credentials; keep long-lived device keys in the OS
  credential store, never plaintext configuration.
- Authorize every operation by explicit capability and workspace scope.
- Treat relay messages, terminal output, filenames, and mobile input as
  untrusted data with bounded sizes.
- Make reconnect idempotent and prevent a stale client from reclaiming a newer
  session generation.
- Redact secrets and user content from routine logs. Security audit events must
  identify subjects/devices without recording command payloads by default.

### Relay address and sign-in

The relay is the standalone KétPlus runtime (`https://ketplus-staging.ketviet.vn`
by default), which serves `/v2/...` at the root of its host. A base URL may still
carry a path, as the tenant gateway's `/native/ketplus` did before the staging
cutover: every request and the `/v2/relay` socket append their path to that base
(`cloud/ControlEndpoint.h`); resolving an absolute path against it would drop it.

KetPlus never asks for a password. Signing a Mac in is a browser handoff
(RFC 8252 with PKCE S256, `cloud/CloudControlSignIn.cpp`):

1. `remote.sign_in` listens on a random `127.0.0.1` port (`BrowserSignIn`),
   posts `/v2/auth/handoffs` with the installation ID, the loopback redirect,
   a random `state` and the code challenge, and answers with the relay's
   `authorization_url`. The daemon refuses a page on another host than its relay.
2. The desktop opens that page in the system browser. The gateway signs the
   viewer in to the tenant and the viewer approves.
3. The browser returns to the loopback port with a one-time code and the same
   `state`; a request with any other state is refused and does not end the wait.
4. The daemon redeems the code at `/v2/auth/token` with the verifier and uses
   that user session once, to enroll the Mac. The session is not stored.

`remote.status` reports `sign_in_pending` while it waits (ten minutes at most)
and `sign_in_error` when it failed; `remote.sign_in.cancel` and sign-out stop it.

## Lifecycle

The intended state flow is `registered -> connecting -> online -> draining ->
stopped`, with a recoverable `offline` state. Session state is separate from
connection state so a temporary network outage does not kill a local process.
Shutdown must stop accepting commands, drain bounded output, terminate or detach
sessions according to explicit policy, and persist only resumable metadata.

## Migration from the desktop

Codex ownership migration is complete; terminal and cloud work remain incremental:

1. Define versioned private protocol types and local IPC tests. (complete)
2. Move Codex session/process ownership without changing UI. (complete)
3. Run and reattach an external daemon in local-only mode. (complete)
4. Add device registration and relay connection. (implemented in code; deployment pending)
5. Enable mobile control only after capability and reconnect tests pass. (implemented in code;
   real staging end-to-end validation pending)

Do not start with cloud connectivity before the local ownership and
authorization boundary is testable.

## Decisions still required

- local IPC mechanism per operating system;
- process survival policy across logout, upgrade, and daemon crash;
- login-time service registration and automatic download/update ownership;
- user-facing device-key recovery and mobile key rotation;
- protocol compatibility window between desktop, daemon, app, and cloud.

Record each resolved item as an ADR under `docs/decisions/` before its contract
is depended on by more than one component.

## Provider handoff history

Switching providers between turns writes the conversation's durable compacted
journal projection (`replay_events`) to the local handoff transcript. Raw delivery
rows (`events`) are removed when clients acknowledge their cursors, so they must
not be used as the history source. Read all projected pages using their returned
cursor: delta compaction leaves cursor gaps and byte limits can shorten a page.
Each provider still resumes its own thread; the handoff note points to the
transcript for turns handled by other providers. Client acknowledgments do not
remove that history.
### Conversation execution worktree (isolated implementation)

`session.worktrees` returns a searched, touched-first page (20 default, 100 max). `session.worktree.switch` validates an idle Codex session, same Git repository and expected context revision. Remote commands/results are encrypted; the switch target opaque IDs are included in AAD. New catalog targets are published before the page response; switching requests an immediate heartbeat. Stable desktop navigation uses `conversation_navigation`, separate from execution cwd. `conversation_worktree_activity` stores new provider evidence, not inferred ownership or reconstructed history; see ADR 0015 and the execution-worktree test lane.

## Atlas live runtime

`AtlasLiveRuntime` owns only the KetAtlas viewer and optional declared renderer it starts. Version 0.5.2 is required, the viewer is read-only, and framework startup uses the local renderer contract without a shell. Discovery returns opaque project IDs. `atlas.start` fences the selected execution context; reconnect by an existing attachment channel instead retains that runtime's source even after a conversation switches. Back releases the connection with a five-minute idle lease; `atlas.detach` explicitly stops the owned runtime. Existing desktop viewers are not adopted or stopped.

`AtlasLiveTunnel` multiplexes bounded byte streams to registered loopback viewer/renderer ports over a persistent authenticated WebSocket. Each attachment gets a new random key/channel; AES-GCM frames bind channel, direction and exact sequence. The phone uses matching loopback origins and an ephemeral HttpOnly cookie for its native bridge. HTTP, streaming bodies, renderer WebSockets and SSE remain ordinary protocols inside the encrypted byte streams. The relay forwards ciphertext and rechecks grants periodically; it cannot see resource bodies. There is no automatic write replay.

`session.resource.resolve` resolves only resource references recorded in an exact event. It returns an opaque source scope and current content revision, never a historical content snapshot or inferred legacy provenance. For agent messages, it also validates inline Markdown file destinations in that exact event. Relative destinations remain bound to the recorded execution context; absolute destinations may identify another existing Git checkout. Switching the current worktree does not rewrite a link’s source. Messages without a recorded context revision remain unverified.

## Activity summary transport

Minor-1 advertises `transcript.activity_groups/1`. Desktop opts in through
`daemon.hello.transcript_activity_groups`; live local events and ordinary local
transcript reads then send summary-only group payloads. `session.activity_group_page`
reads details only for an opened group, with the existing session/generation
checks, a consistent SQLite snapshot, exclusive delta cursor and projection
revision. `raw_activity_details` is an explicit local read option used for
conversation export. Relay/mobile remain on the legacy projection. See
[ADR 0022](decisions/0022-daemon-activity-group-details.md).

## Permanent deletion propagation and provider cleanup

An idle `session.delete` transaction removes local history and writes both a
generation tombstone and durable deletion work. Live background tasks must first
stop. Attached Desktop clients receive `session.deleted`; `session.list` also
returns `deleted_sessions` so reconnecting clients remove stale persisted entries.
Newer generations are not removed by an older tombstone.

The heartbeat optionally carries `deletions` (session ID, generation, lifecycle
revision and operation ID). Only an exact `acceptedDeletions` receipt clears cloud
work. Missing receipts and transport failures retry after restart; absence from a
session catalog alone never proves deletion. Relay consumers must deploy the
additive control-v2 contract before relying on Mac-to-phone deletion propagation.

The same transaction captures current and historical provider thread IDs, including
handoffs. A bounded background queue calls Codex `thread/delete` or Claude SDK
`deleteSession`, including Claude transcript copies in previous project directories
and subagent transcripts. Cleanup failures remain durable for retry. A thread still
referenced by another retained conversation is preserved. Cursor has no supported
thread-deletion API in this integration: an unshared Cursor thread currently rejects
permanent deletion with `provider_cleanup_unsupported` and retains its conversation.
This change does not scan or delete already orphaned historical provider sessions.

## Agent messaging (ADR 0027)

`apps/daemon/src/agents/` owns conversation numbers and agent-to-agent messages.
`LocalJournalAgents.cpp` stores `conversation_numbers` (random four-digit
aliases, five digits once half are held, 30-day quarantine after deletion) and
`agent_messages`. `AgentBroker` mints a capability token per conversation
generation, answers `agents.whoami/list/send/inbox`, delivers queued messages
when a receiver becomes idle (or steers an `urgent` one), turns a final answer
into the reply and applies the hop, chain and hourly limits. `SessionHost`
implements its narrow `AgentHostPort` in `SessionHostAgents.cpp`: an agent turn
journals `user.message` with `origin: "agent"`, and `agent_message.state`
reports a message the conversation sent (`sent`, with its delivery) or answered
with its final message (`replied`), and held, failed and expired messages. While
an automatic turn runs, the broker keeps its sender in memory: `session.list`
adds it as `agent_turn` (`number`, `title`, `kind`) and the heartbeat seals it
into the phone's session metadata as `agentTurn`, beside `number`.

Providers launch `ketplus_daemon --mcp-agents` with the endpoint's absolute path
and the token. `daemon.hello` with `agent_token` authenticates an agents-only
connection that never receives session events; the MCP server opens one per
tool call so idle servers do not hold connection slots. Codex receives the
server as dotted per-thread `config` keys on start, resume and fork; Claude and
Cursor receive it beside `ketplus_browser`.

## Returning to a previous provider

A -> B -> A starts a new native thread on the return to A. The handoff transcript
is rebuilt from durable conversation history, including B's latest turns, and the
fresh provider receives a zero seen-cursor handoff. Prior native A context is never
resumed. Same-provider model changes continue the current complete conversation.
The private Claude `turn/start.freshContext` flag also closes a retained SDK query
before opening the new context, even when its cwd and launch settings match.
Superseded IDs remain in `provider_threads.<provider>.retired_thread_ids` solely
for shared-reference checks and permanent-deletion cleanup, including after restart.
Late notifications naming a retired native thread cannot replace the current one.
