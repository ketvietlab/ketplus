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
  token; the daemon refuses to start when it cannot restrict the journal and
  token directories to their owner. On macOS it also removes access-control
  entries, such as an inherited `everyone allow read`, which mode bits do not show:
  from those directories before creating anything (so new files inherit none),
  from the journal and its WAL/SHM, and from the media root and every file under it.
  A linked directory is handled at its target. An ACL that cannot be read counts
  as present. A token that carried such an entry is replaced rather than trusted,
  and only after the journal lock is held, so a daemon refused as the second
  instance never replaces the running one's credential. Linux ACLs and Windows
  are not handled;
- the length-prefixed JSON v0 contract accepted in ADR 0002;
- provider-neutral session operations and normalized events backed by SQLite
  WAL cursor replay and durable command deduplication;
- durable conversation-title mutation through `session.rename`, including
  Codex `thread/name/set` synchronization and normalized title-update replay;
- durable sidebar read markers through `session.mark_seen` (`last_seen_cursor` in
  the journal; `session.list` exposes both `last_event_cursor` and
  `last_seen_cursor` so Desktop can restore unread completed conversations);
- idle conversation forks through `session.fork` (ADR 0016): the provider forks
  its native thread (Codex `thread/fork`, Claude SDK `forkSession`), then the
  journal copies the source generation's transcript from one read snapshot in
  bounded event-loop passes at the same cursors, stores the fork in one final
  transaction and the fork appends `session.forked`
  ([ADR 0094](decisions/0094-staged-session-fork-copy.md));
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
sessions, attaches and replays ordered journal events, and starts a detached daemon
if no endpoint exists. The daemon caches a mutating command's result by `message_id`,
but writes that entry after the mutation, outside its transaction, and ignores a failed
write, so a repeated write can run again. After a disconnect Desktop therefore sends
again only in-flight reads. In-flight writes (start turn, steer, interrupt, rename,
mark seen, approval and question answers, fork, delete, pairing, remote account and
storage operations) fail with "check the result before retrying"; requests that were
still waiting are sent after the reconnect. It keeps at most sixteen requests in flight and sends the rest
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
Accepted local IPC sockets cap Qt's unread receive buffer at one maximum frame
plus the four-byte header (ADR 0077), separately from frame assembly and outbound
queues. Pausing request dispatch applies transport backpressure without dropping
valid pipelined requests. This does not bound all per-request or process memory.

Local turn acceptance commits the starting session/configuration, pinned context,
`user.message`, `turn.queued` and optional `session.configuration_changed` in one
transaction. Its bounded receipt is read before commit and published afterward;
failed acceptance cannot retain a rejected prompt (ADR 0076). Existing stream
recovery is drained first; command-result persistence and provider-side effects
remain separate transaction boundaries.

Streamed text reaches the journal coalesced: consecutive `assistant.delta` or
`command.output_delta` notifications for one item are held for at most 150 ms
(or 16 KiB) and journaled as one event. Any other event, a delta of another
item, and SessionHost teardown journal the held text first, so provider order
is kept. One event per provider chunk (a word or less) had every reader, above
all a phone paging the relay, read tens of thousands of events per conversation.
If append fails, SessionHost retains the prefix and later provider events in a
bounded FIFO, retries after one second, and yields between deferred entries.
The queue caps are 128 events / 40 MiB per session and 512 / 80 MiB per host
(encoded payload plus bookkeeping); rejected commands are not deferred. Deletion
releases the queue. This is volatile recovery, not a durable crash spool, and
excess/unsupported events are rejected; see ADR 0062 for the coalescer exception
and exact limits.
The journal transactionally maintains a durable compact replay projection. It
omits command-output deltas that the transcript does not consume and removes
assistant deltas once a completed item supersedes them; deltas for an incomplete
live item remain replayable. Existing journals are backfilled once when a daemon
with projection support first opens them. Subsequent attaches query the
projection directly instead of rescanning and parsing the raw event history on
every page. The raw journal remains available for non-compacted reconnects and
cursor acknowledgement; pruning raw events does not remove the compact
projection.

While idle, a maintenance pass also removes intermediate `usage.context`,
`usage.limits` and `usage.session` replay samples from completed turns older than
the configured full-detail period (default 30 days), retaining the final sample
of each type in each turn. It preserves the
latest 20 actual turns per conversation/generation, any turn with raw events
still awaiting acknowledgment, any turn containing questions or approvals, and
sessions outside an explicitly idle state. The same pass trims eligible inline completed
command output to its first 4 KiB and last 16 KiB on UTF-8 boundaries, with an explicit
notice and `item.compacted` metadata. Messages and images remain unchanged.
Redundant tool starts are removed only when the immediately following retained
event is their same-turn completion and preserves start data (except lifecycle
status and empty known progress placeholders); interleaved/partial/large cases
remain (ADR 0041). Known tool result text is also capped to a shared 4 KiB prefix, preserving
media/resource blocks, arguments, errors and structured result fields. Reasoning raw
content is removed only when a nonempty summary remains. Large file diffs retain a
UTF-8 prefix in an explicit `diff_excerpt` field plus original line statistics, with
a visible `Diff trimmed` label; partial patches never occupy the full `diff` field. See
[ADR 0038](decisions/0038-command-output-retention.md) and
[ADR 0039](decisions/0039-tool-text-and-reasoning-retention.md) and
[ADR 0040](decisions/0040-file-diff-retention.md). Large completed tool arguments use explicit
UTF-8 excerpts rather than partial argument objects; known text markers can be
upgraded once ([ADR 0043](decisions/0043-tool-argument-retention.md)). Successful plain
structured tool results also support sanitized display-only excerpts sharing the
text allowance; error/resource-bearing results stay intact
([ADR 0044](decisions/0044-structured-result-retention.md)). The full-detail period is configurable locally (7/30/90 days or Forever),
with a separate automatic-detail-retention switch and revisioned saves; see
[ADR 0042](decisions/0042-detail-retention-policy.md). Size pressure, archive/media
policies, complete Storage UI and remaining compaction are still tracked in #128.

Each minute's idle pass examines at most 128 replay rows, resumes its scan on the
next pass, and wraps at the end (restart begins a fresh scan). Deletion and the
projection revision increment commit together, so a stale window continuation
gets `snapshot_changed`. A competing SQLite writer aborts maintenance without
waiting; the normal busy timeout is restored. Free-page reclamation follows as
before. See [ADR 0037](decisions/0037-bounded-usage-retention.md).

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
uploads stable AES-256-GCM event envelopes from a durable retry cache (key epoch
1 only, until rotation exists; decryption requires a 12-byte nonce and a 16-byte
tag, because AES-GCM itself accepts others), and polls
serialized encrypted commands. The machine signs a challenge only when it is
exactly 32 canonical base64url bytes: the same Ed25519 key signs event
envelopes, wrapped session keys and WebRTC answers, all longer, so a relay
cannot obtain a host signature over one of them by calling it a challenge. A
domain-separated challenge signature needs a matching relay change and is
tracked in #85. A failed command poll or credential request waits before the
next poll like a failed upload (one second doubling, up to five minutes after
a refusal and one minute otherwise, with jitter); a relay wake-up or a
successful poll ends the wait. Pairing is explicitly confirmed on the Mac, and
the daemon recomputes the claim digest from the claimed keys before approving.
Only devices pinned on this Mac (ADR 0034) receive session keys, have their
commands accepted or open peer links; the devices active when pins were
introduced were adopted once; a
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
signed content. `itemKey` is a public linkage value: it hides the item id
from casual reading, but the relay knows session and generation and can
confirm a guessed provider item id. Each session's cloud cursor is registered with the
journal once per sign-in, not on every upload, and an acknowledgement at or
behind the stored cursor writes nothing. Advancing ACKs commit a monotonic cursor
and durable cleanup queue together. Physical raw deletion is eventual: each pass
visits at most eight queued generations and 128 raw candidates, deletes at most
1 MiB, and rechecks the slowest client's fence. It only removes a contiguous raw
prefix; unsupported rows hold the rest of their generation's tail so raw replay
expiry remains explicit. Reopen/idle maintenance resumes queued work; see [ADR 0049](decisions/0049-bounded-acknowledged-delivery-cleanup.md).
Global orphan GC runs after advancing ACK cleanup, at open, after a session delete,
every 256 command results and on idle maintenance. Each transactional pass scans up
to 32 metadata and 128 chunk candidates, deletes at most 1 MiB, and persists cursors
for restart/wrap. A generation with pending raw delivery is deferred without a JSON
backlog scan; retained replay references protect shared payloads. Unreachable chunks
may wait for later passes. Oversized legacy blobs and malformed rows remain retained;
see [ADR 0048](decisions/0048-bounded-global-payload-gc.md).

Command results, remote audit, deletion tombstones, orphan client cursors and
orphan background tasks each have a transactional metadata-retention lane. Per
table, one pass visits at most 256 age/orphan plus 256 cap candidates and deletes
at most 256 rows/1 MiB across both scans. Durable rowid cursors resume at reopen or
idle maintenance. Command/audit caps retain the latest 10,000 accepted writes by
rowid rather than timestamp, so clock rollback cannot immediately evict a new
result. Fixed 10,001-entry cutoff discovery avoids backlog sorting. Busy work
defers; failed deletions/state writes roll back the lane. These are eventual
physical caps, not a hard storage quota; see [ADR 0050](decisions/0050-bounded-journal-metadata-retention.md).

Cloud envelope cache cleanup has its own 256-candidate/1-MiB maintenance lane.
Only rows with receipts for their exact sent ciphertext are eligible. An additive
receipt flag defaults to pending for legacy/new cache; pre-dispatch verification
resets it for retries. Receipt and a versioned relay/workspace/machine/key-scoped
recovery cursor commit together before credential mirroring and raw ACK; restart
repairs these later steps without re-encrypting already accepted events.
Old account callbacks and old raw cloud cursors cannot authorize a replacement
cache row's deletion. Invalid envelope recovery removes one exact identity before
retry, with a 1-MiB stored-column bound and checked transaction. Unsupported or
legacy unmarked entries remain for separate recovery; see
[ADR 0053](decisions/0053-bounded-cloud-event-cache-cleanup.md).

The journal records its format in `PRAGMA user_version`. A daemon refuses a
journal newer than its `journal_format::current` before writing anything, and
startup compaction leaves such a journal untouched
([ADR 0095](decisions/0095-journal-format-version.md)).

Deleted rows return their space to the disk (#128). A new journal is created
with `auto_vacuum = INCREMENTAL`. An older one moves to it once at daemon start,
before the endpoint listens:
- the daemon holds `<journal>.lock`, so there is one daemon per journal (it waits
  up to 5 s for a daemon that is being replaced);
- its connection uses `locking_mode=EXCLUSIVE`. SQLite refuses that while any other
  connection has the journal open, including a daemon from before the lock, so
  the move waits for the next start and the file is not touched. The lock is
  held until after the rename;
- it creates an owner-only `<journal>.compact-private` directory and runs
  `VACUUM INTO` its `journal.sqlite` child (ADR 0087);
- it checks the copy (`quick_check`, the same schema, incremental vacuum);
- it requires owner-only permissions on the completed copy on POSIX, syncs it
  and renames it over the original, then syncs the directory.

The move runs only when the free disk exceeds the kept data plus the larger of
256 MB and 10%. Otherwise it waits for the next start. If a move is interrupted,
the original stays whole and recognized staging files are removed at the next
start. Unsupported entries or permissions are preserved for repair and reported
as migration failure; cleanup never recursively removes a staging directory. The move
takes as long as one read of the kept data.

After that, once a minute while no session is active, up to 4,096 free pages
(16 MB at 4 KiB) go back to the disk in one transaction. A passive checkpoint
then copies the smaller size into the file. It never waits for a reader: if a
reader holds frames back, the next run finishes it. Emptying the log runs
without the busy timeout. It is skipped while someone still reads the log: the
file has already shrunk, and the next write reuses the log. Compacting old detail,
the size limit and the storage warning are still to come (the rest of #128).

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
are refused). Previews are decoded off the daemon's main thread, on a pool of
two threads with at most 16 reads waiting or running; a read beyond that answers
`busy` at once. They are scaled to at most 1280 px and flattened onto white.
A provider's viewed or generated image is opened without blocking and kept only if
the opened file is regular, so a path replaced by a FIFO cannot stall the daemon.

At startup the daemon also deletes every canonically named media file that no
remaining reference names, so a daemon stopped between removing a deleted
conversation's references and its media, or between writing media and referencing
it, leaves nothing behind once it starts again. Media cleanup first validates a
complete reference catalog: canonical digest
files, regular nonsymlink entries, at most 4 MiB per file, 16 MiB total and 4,096
directory entries. Unknown, unreadable or over-budget ownership defers deletion;
capacity can remain above its limit pending recovery. Valid ownership retains the
existing eviction order. This guard does not establish reference-write durability
or a cross-file transaction; see ADR 0065.

Reference-based previews share the bounded ownership parser and require canonical
references and nonsymlink roots (ADR 0069). The daemon verifies the bounded image
bytes against their SHA-256 and decodes that snapshot through QBuffer; unknown
ownership or mismatched bytes returns media_unavailable without modifying files.

SessionHost prepares fork media ownership inside the journal before-commit
phase (ADR 0068). Unknown source metadata or conflicting destination ownership
fails the final fork transaction; the staged history becomes purge work
(ADR 0094). Matching ownership supports retry after commit failure;
extra prepared references can be reclaimed after reopen using the live catalog.
This is ordering across two stores, not a filesystem/SQLite atomic transaction.

Image attachment and externalization publish new references only after a checked
ownership-file commit (ADR 0067). The store preserves unsupported metadata and
writes unique canonical hashes atomically with owner-only permissions; failure
uses the existing omission path. Blob/ownership/database coordination remains
separate, and a failed ownership update can leave unreferenced blob bytes.

Media deduplication checks the existing file type, size, bounded contents and
owner-only permissions before publishing a blob reference (ADR 0066). Unsupported
entries are preserved for repair. The store rejects symlinks at its requested
directory and blob paths; ancestor races and reference durability remain separate.

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
The policy fails closed. When the root or a parent holds `.git` and Git cannot
evaluate it (corrupt index, bad config, timeout), tree, file, diff, image and
search requests fail as `unavailable`. Only a folder outside any repository is
walked without the ignore check. A diff covers exactly the requested file: the
pathspec is literal, and the request fails as `invalid_path` when the path is a
directory on disk, in the index or in `HEAD`.

Every git command on the read path runs with the hardened arguments and
environment: no optional locks, fsmonitor and the untracked cache off, no pager,
external diff, ssh command or credential helper, and inherited
`GIT_DIR`/`GIT_WORK_TREE`/`GIT_INDEX_FILE` cleared. Each configured
`filter.<driver>` (from `git config --get-regexp`) also gets empty `clean`,
`smudge` and `process` overrides through `GIT_CONFIG_COUNT`/`GIT_CONFIG_KEY_n`,
which keep a driver name containing `=` intact, so a repository's filter never
runs during status or diff. If the drivers cannot be listed, the read fails.
Diffs use `git diff-index`, because porcelain `git diff` rewrites the index
after rehashing stat-dirty files even with optional locks off. As a result, Git LFS files can appear modified to a phone.
Remote worktree creation uses the same environment with fsmonitor off. Its
checkout still runs the repository's hooks and filters, just as
`git worktree add` does on this machine.
A redelivered `session.create` (its acknowledgement was lost) answers with the
result the first attempt journaled instead of running git again, so the phone
never sees `branch_exists` for a conversation that started. Paired devices can
create at most 10 worktrees per repository per hour (counted by the Git common
directory, so every checkout of a repository, bare or not, shares it), and none while the
repository's volume has less than 2 GiB free; both refuse with
`worktree_quota_exceeded` before git runs (`RemoteWorktreeQuota`, in memory,
#86).

Every remote command that can change something is recorded in the journal's
`remote_command_audit` table when it is acknowledged: command id, kind, the
sending device id and key version its content names, session id, result code
and time (#86). Kinds known to read (`remoteCommandMayChange` in
`RemoteAudit.cpp`) and the `attachment.stage.*` steps of an upload (the message
that sends the attachment is recorded) are left out, so a kind this build does
not know is recorded. A `session.create` row names the conversation it created. A redelivered command keeps the row of its first outcome. A command
whose signature did not verify against a pinned device is recorded with that
refusal's result code, so the device id is the one it claimed. Rows are kept
for 90 days and at most 10,000, applied by the journal retention pass and by
every 256th audit row (a command refused before it ran stores no command result
to trigger that pass). The local `remote.audit` request returns them
newest first (`limit`, 1–500, default 100; `device_id` to show one device's).

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
token, journal and SQLite WAL/SHM files are 0600. Both daemon and local client
limit raw token files to 4096 bytes before trimming, with at least 32 trimmed
bytes. On Unix, descriptor-based admission rejects symlinks, nonregular files,
another UID or group/other permissions. Refusal preserves the file for repair;
ancestor-directory and non-Unix ACL guarantees are not implied (ADR 0078).

The local Qt client's unread socket buffer is capped at one maximum IPC frame
plus its four-byte header, including the hello phase. Paused dispatch applies
backpressure; valid pipelined frames resume without loss (ADR 0079). This is not
a process-wide memory or request-work limit.

Advanced model, effort and execution controls are revision-fenced daemon
commands backed by the active adapter's advertised capabilities. Encrypted
attachments use bounded, individually durable command chunks and a locally
validated staging journal; see
[ADR 0007](decisions/0007-capability-configuration-and-attachment-staging.md).

The 24-hour local staging expiry runs bounded passes at reopen, valid upload begin
and idle maintenance: at most 32 manifests and 128 chunks share a 1-MiB mutation
budget. An `expiring` manifest reserves its session/generation/attachment identity
until every old chunk is removed; reads/commit/new begin cannot adopt that retired
upload. Begin/read/commit/chunk operations check the 24-hour eligibility directly,
so an expired ID beyond the sweep window cannot remain usable. Draft quotas count
only usable manifests, while expired identities remain reserved until cleanup.
Current and unknown-format parents protect their chunks. Cancellation retains a
retiring identity and cannot touch a newer manifest revision; remaining mixed
revisions keep the retirement marker until bounded cleanup finishes. Deletions,
retirement markers and durable rowid cursors commit together; busy work defers.
Explicit cancellation reads at most 128 chunk candidates, including other
revisions, and shares a 1-MiB mutation budget with the manifest. It releases an
empty identity immediately; otherwise it commits the retirement marker and the
bounded chunk deletion together. Unsupported/oversized manifests fail unchanged;
oversized chunks remain reserved for recovery. See
[ADR 0051](decisions/0051-bounded-staged-attachment-expiry.md) and
[ADR 0054](decisions/0054-bounded-attachment-cancellation.md) for limits and retries.

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

### Daemon challenge authentication

The daemon requests only `signatureVersion: 2` and `signatureAlgorithm: "Ed25519"`
on both challenge issuance and credential redemption. Before signing, it checks the
same scheme in the reply, the enrolled machine/key binding, ASCII identifiers and
a canonical unpadded base64url nonce of exactly 32 bytes. Missing or legacy fields
fail closed; there is no retry using a raw nonce signature.

The identity key signs UTF-8 compact JSON
`["ketplus",2,"daemon-challenge",id,machineId,keyVersion]`, one NUL byte, then the
decoded nonce. The fixed domain prevents a relay from obtaining signatures for
other identity-key protocols. The relay owns challenge expiry and atomic one-time
redemption. Deploy a v2-capable relay before this client; an old relay cannot issue
new credentials to it. Existing cached credentials remain usable until refresh.
See [ADR 0036](decisions/0036-daemon-challenge-domain-separation.md) for the exact
contract and compatibility rules. This documents client behavior, not relay deployment.

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
   An `error` with the right state (the viewer declined, or the tenant failed)
   ends the sign-in at once with a fixed message, "Sign-in was declined." for
   `access_denied`; `error_description` is never shown (#84).
4. The daemon redeems the code at `/v2/auth/token` with the verifier and uses
   that user session once, to enroll the Mac. The session is not stored.

`remote.status` reports `sign_in_pending` while it waits (ten minutes at most)
and `sign_in_error` when it failed; `remote.sign_in.cancel` and sign-out stop it.

## Lifecycle

ACP v1 `loadSession` and `promptCapabilities.image` are opt-in booleans (ADR 0090).
An omitted, null or wrong-type field removes that baseline capability; only true
preserves support. Each process initializes a fresh narrowing state.

Malformed normal-sized stdout frames quarantine the provider generation before it
is killed (ADR 0089). Buffered suffixes are discarded; the finished path settles
pending calls with a static diagnostic and uses existing restart backoff. JSON
control bytes must be escaped inside strings; outside JSON whitespace remains
allowed. Valid-object RPC schemas and absent-response deadlines remain separate.

Outgoing provider calls use positive integer JSON ids. Response lookup preserves
that type and numeric value across all three adapters, including Codex oversized
responses (ADR 0074). Invalid/unrelated ids leave pending calls untouched; incoming
provider request ids and pending-call timeout policy remain separate.

Matched replies must contain exactly one result (object or explicit null) or error
(object). Invalid envelopes fail that callback with a static protocol diagnostic;
initialize uses its existing failure path, while other pending calls survive.
Provider-specific result schemas remain separate (ADR 0075).

Cursor model discovery reads both channels incrementally with a shared 1-MiB
budget (ADR 0088). It retains stdout only, kills excess output, and parses only
successful normal exits. The existing fallback catalog settles failed/timed-out
requests and the 60-second cache/coalescing behavior remains unchanged.

Provider version probes retain at most 4 KiB of merged output, kill excess output
and accept banners only after successful normal exit (ADR 0072). Failed probes
remain Unknown under the existing launch policy and are retried. Only matching
executable fingerprints share an in-flight result; other checks are deferred and
revalidated, including a binary replaced while the first probe is running.

Claude/Cursor stdout is read in 64-KiB chunks and checks each raw line, including
an unfinished suffix, against the existing 16-MiB limit before parse (ADR 0071).
An over-limit generation is closed/drained before pending callbacks fail and the
process is killed; later output from it cannot publish another request. Normal
lines and a fresh process retain their existing behavior.

Oversized Codex metadata decodes bounded JSON strings and refuses literal dotted
keys as nested paths (ADR 0073). Invalid captured metadata clears routing for that
line; subsequent lines reset normally. This does not validate the entire discarded
JSON payload or resolve missing terminal events and pending-request timeouts.

Codex oversized-line scanning keeps at most 64 routing frames and a scalar count
of deeper containers (ADR 0070). It still extracts shallow fields after the deep
value and resumes on the next line; clear/reset discards all prior scan state.
This bounds scanner nesting storage, not aggregate process I/O or event-loop time.

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

`AtlasLiveTunnel` multiplexes bounded byte streams to registered loopback viewer/renderer ports over a persistent authenticated WebSocket. Each attachment gets a new random key/channel; AES-GCM frames bind channel, direction and exact sequence. The phone uses matching loopback origins and an ephemeral HttpOnly cookie for its native bridge. HTTP, streaming bodies, renderer WebSockets and SSE remain ordinary protocols inside the encrypted byte streams. The relay forwards ciphertext and rechecks grants periodically; it cannot see resource bodies. There is no automatic write replay. Stream EOF is sent when a disconnected origin
has been fully drained, including drains triggered by WebSocket writes; the
client need not return additional credit after receiving the complete response
(ADR 0084).

Daemon readiness requires a complete stdout banner for its current loopback viewer
port. Partial/embedded/wrong-port and post-stop output cannot publish an
attachment; the diagnostic tail retains its line-boundary state. The start
callback is consumed before invocation and cannot run again when the process
exits immediately after readiness (ADR 0083). Synthetic CLI regressions cover
these boundaries without starting a real viewer or renderer.

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

Opening a journal only marks sessions whose activity index is behind in
`activity_backfill`. `SessionHost` drains the markers with passes bounded by rows
and payload bytes that defer on a busy journal. A new event for a marked session
is left to the backfill, which then takes that session first. Groups of a marked
session are not published: its events read ungrouped and its group pages answer
`unknown_group`; each pass increments the projection revision
([ADR 0097](decisions/0097-activity-backfill-after-open.md)).

## Permanent deletion propagation and provider cleanup

An idle `session.delete` transaction removes the session and its per-generation
state, one bounded batch of bulky history and writes a generation tombstone and
durable deletion work. Remaining history gets a `session_history_purge` marker
in the same transaction; `SessionHost` drains it one bounded pass per event-loop
turn, and the tombstone outlives the marker
([ADR 0093](decisions/0093-bounded-session-history-purge.md)). Live background tasks must first
stop. Attached Desktop clients receive `session.deleted`; `session.list` also
returns `deleted_sessions` so reconnecting clients remove stale persisted entries.
Newer generations are not removed by an older tombstone.
Tombstone catalogs support opt-in version-1 keyset pages (128 rows/64 KiB), with
bounded SQL projections and explicit errors for oversized legacy responses. The
shared transport reader covers Desktop refresh and `ketplusctl sessions`,
fences catalog mutations and stale reconnect callbacks, and reads live sessions
after the walk. A changed catalog restarts at most three times; CLI output uses a
private temporary spool instead of accumulating tombstones in memory. See
[ADR 0091](decisions/0091-paged-session-tombstone-catalogs.md); this does not
bound the live catalog itself.
Incremental discovery uses `session_catalog_live_only: true` with page version 1
([ADR 0092](decisions/0092-live-only-session-discovery.md)): one live-catalog
request, no tombstone query or deletion signals, and the same wire-byte guard.
Older paged daemons return page one with live sessions; discovery ignores its
deletion fields. Initial/reconnect reconciliation still walks the full catalog.
The tombstone, cleanup-work and purge-marker inserts must each affect one row.
Each per-generation state delete must leave no matching row, and a history batch
that removes nothing while rows remain fails the deletion. A failed statement or commit
rolls back and releases the transaction before returning failure, so retries
cannot observe uncommitted deletion state. See
[ADR 0055](decisions/0055-session-delete-transaction-recovery.md). Explicit deletion
still performs synchronous work proportional to that session's history; bounded
retirement remains a separate hardening item.

The heartbeat optionally carries `deletions` (session ID, generation, lifecycle
revision and operation ID). Only an exact `acceptedDeletions` receipt clears cloud
work. Missing receipts and transport failures retry after restart; absence from a
session catalog alone never proves deletion. Relay consumers must deploy the
additive control-v2 contract before relying on Mac-to-phone deletion propagation.

Completion changes only the identified durable work row. Fully completed jobs are
logically excluded immediately and reclaimed by a separate metadata lane (up to
256 rows/1 MiB per pass). Pending reads use indexed state/rowid suffix and prefix
ranges sharing one page limit, so retained completed jobs do not force a backlog
scan or round-robin sort. Invalid provider-target JSON remains pending; ignored
completion updates fail. See [ADR 0052](decisions/0052-bounded-completed-deletion-work.md)
for the contract and the remaining per-record/startup limits.

The same transaction captures current and historical provider thread IDs, including
handoffs. A bounded background queue calls Codex `thread/delete` or Claude SDK
`deleteSession`, including Claude transcript copies in previous project directories
and subagent transcripts. Cleanup failures remain durable for retry. A thread still
referenced by another retained conversation is preserved. Ownership checks use a
complete, fallible catalog read: query or row-fetch errors defer queued cleanup
and reject new local deletion without removing history. A partially read catalog
cannot prove that a thread is unreferenced. Work remains durable for a later pass
or restart after repair; see [ADR 0056](decisions/0056-fail-closed-provider-ownership-reads.md).
Cleanup additionally requires supported ownership metadata: an object keyed by
nonempty provider IDs, with object entries containing only string `thread_id`
and/or string-array `retired_thread_ids`. Malformed or unknown fields defer cleanup
without normalizing or deleting their stored values. Both the local source and
retained catalog are checked; see
[ADR 0057](decisions/0057-preserve-unsupported-provider-ownership.md).
Cursor has no supported
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

### Advisory Storage measurements

Local retention responses also include optional SQLite/WAL/SHM logical file lengths
and filesystem available bytes, separately from SQLite page usage. They neither
scan journal content nor checkpoint it, and failures do not invalidate committed
policy saves. Media/backups are excluded; absent filesystem data is unknown. See
[ADR 0045](decisions/0045-journal-file-measurements.md).

### Failed journal initialization

A failed base/activity/agent initialization closes the journal connection before
construction returns. Later operations on that object cannot turn a cleared
operation error into a usable journal. Repair the cause separately and open a
fresh journal; the failed object remains closed. The service's immediate startup
check and existing rollback behavior remain in place. See
[ADR 0086](decisions/0086-quarantine-failed-journal-initialization.md) for scope.

### SQLite-page size pressure

The separate owner-only size policy defaults to a soft 5 decimal GB SQLite-page
target. Recent-detail pressure is opt-in through an explicit owner size-policy save
until size controls/rollout are complete. Bounded idle work reclaims free pages, completes an expired-detail sweep,
then may compact eligible recent detail without bypassing retention guards.
Forever/off still preserve remaining detail. Unsupported/protected data may leave
the target unmet. File/WAL and total-application usage are not this page target;
see [ADR 0046](decisions/0046-journal-size-pressure.md) for scope and remaining work.
Both stored policy JSON values are admitted through a 4 KiB SQL projection before
Qt parses them. Unsupported oversized rows remain intact and disable policy saves
and configured detail trimming until repaired; see
[ADR 0085](decisions/0085-bounded-stored-policy-json.md).

Bounded large-tool retention (ADR 0047) applies the existing detail transformers
to version-1 fragmented payloads within the shared 1 MiB pass budget. It requires
a fully acknowledged session generation, verifies identity and SHA-256, and
atomically replaces the replay manifest, cleans unreferenced chunks and advances
projection revision. Images, legacy blobs and larger payloads remain retained.
