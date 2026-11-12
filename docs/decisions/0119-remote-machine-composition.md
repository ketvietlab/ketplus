# 0119: Remote machine composition and the machine-neutral backend

Status: accepted
Date: 2026-11-12

Issue #419 scope item 1. Amends
[ADR 0022](0022-linux-headless-remote-host.md) (a Linux host is reached only
through Control v2) and `docs/linux-headless-daemon.md`.

The team task named this ADR 0102; that number was already taken by
`0102-activity-files-backfill.md`, and 0118 by
`0118-team-tasks-done-by-pull-request.md`, so it is 0119.

## Context

Desktop can sign in, pair a Linux daemon through the KetPlus Relay
(`LinuxMachinesSection`, Control v2, ADR 0025) and list its machine ids in the
machine picker. Choosing a Linux machine only opens Connections: conversations,
worktrees, the transcript and approvals still come from This Mac's daemon only,
because every consumer calls `remote::DaemonClient` (the local Unix socket)
directly.

Issue #419 wants Desktop to work on a connected Linux machine like on This Mac,
over the user's own SSH access (free) or through the Relay (Pro). Without one
seam, each consumer would grow a branch per transport.

## Decision

### One machine-neutral backend

`modules/remote/src/MachineBackend.h` is the interface Desktop uses for one
machine. It carries the operations Desktop needs and nothing transport-specific:

| Operation | Local protocol request |
| --- | --- |
| `listRepositories` | `workspace.list`, mapped to the neutral repository shape |
| `listWorktrees` | `session.worktrees` (query, offset, limit, repository_path) |
| `listConversations` | `session.list` |
| `transcriptWindow`, `transcriptPage` | `session.transcript_window`, `session.transcript_page` |
| `transcriptLargeEvent` | `session.transcript_large_event` |
| `createSession` | `session.start_turn` with `worktree_path` (the local worktree id) and no session |
| `startTurn`, `steerTurn`, `stopTurn` | `session.start_turn`, `session.steer`, `session.interrupt` |
| `pendingApprovals` | `session.pending_approvals` |
| `resolveApproval`, `answerQuestion` | `approval.resolve`, `question.answer` |
| `connectionStateChanged`, `eventReceived` | readiness of the route; session events |

Requests and results are the daemon's local protocol objects on every route, so
a consumer parses one shape, with two exceptions where the routes differ:

- **Repositories** use a neutral shape no route has to fake:
  `{"repositories": [{"id", "label", "path", "available", "worktrees"}]}`,
  one row per repository with its checkouts under `worktrees`
  (`[{"id", "label", "path", "available"}]`). The local and SSH routes group
  `workspace.list` (`workspaces[{path, available, repository_path}]`) by
  `repository_path`, so a repository with linked worktrees is one row, the same
  grain as the Relay's repository id: `id` = `repository_path`, `label` = its
  last path component. The daemon reads `repository_path` from a linked
  worktree's `.git` file and the gitdir's `commondir` without running Git; a
  checkout it cannot read, or a daemon that does not send the field, is its own
  repository. The Relay sends the
  salted repository id and label it already uses and no `path`, so a consumer
  shows a path only where one is present and keys everything else by `id`.
  `workspace.list` joins `DaemonClient`'s read set that is replayed after a
  disconnect.
- **Large transcript events.** A local or SSH page may carry `large_event`
  references; the consumer reads each fragment with `transcriptLargeEvent`.
  The Relay reassembles large events on the host before it encrypts a page,
  so its pages carry no references and its `transcriptLargeEvent` fails.

Each backend has a `machineId` (see "Machine identity") and a `Route`:
`Local`, `Relay` or `Ssh`.

Three implementations:

- **Local** — `DaemonMachineBackend` over `DaemonClient`'s Unix socket. It only
  maps operations to request types and `readyChanged` / `disconnected` to
  connection states; queueing before the hello, replay after a disconnect and
  reconnect backoff stay in `DaemonClient`. This Mac behaves exactly as before.
- **SSH** — `SshMachineBackend` (`ketplus_remote_ssh_backend`): the same
  `DaemonMachineBackend` operations with the host's machine id and
  `Route::Ssh`, over a remote-mode `DaemonClient` that adopts `SshTunnel`'s
  connection, which `ssh -W` bridges to the Linux user's own daemon socket
  (`$XDG_RUNTIME_DIR/ketplus-daemon-v0`).
  The remote-mode client never starts, replaces or reconnects a daemon;
  `SshTunnel` owns reconnects. The wire protocol is the local one;
  only the transport differs (see "SSH trust"). Connection reasons are
  route-neutral ("The ketPlus daemon is not running."), and only the local
  route turns a missing local daemon install into Offline; the SSH route
  reports its own reasons (host unreachable, key changed, not installed on the
  host).
- **Relay** — `RelayMachineBackend` over `ControlV2Client`, which turns each
  operation into an encrypted Control v2 command and each authenticated,
  decrypted event page into `eventReceived` (scope item 2 of #419, see "Relay
  backend"). Each event is re-shaped into the local daemon's `session.event`
  envelope (`session_id`, `session_generation`, `cursor`, `turn_sequence`,
  `sent_at`, `payload {event_type, data}`), so consumers read one shape on
  every route.

Existing consumers (`CodexAppServer`, `SessionCatalogReader`, the Connections
sections) still hold a `DaemonClient` today. They move to a `MachineBackend` one
at a time, each with its own tests; the local adapter lands first so that move
changes no behaviour. Operations a consumer needs later are added to the
interface with their local mapping when that consumer moves, not speculatively.

### Parity of conversation operations

`MachineBackend::supports(Feature)` says whether the machine can run a
conversation operation now, and `unsupportedReason(Feature)` says why not
("Available once this machine is connected." while it is not Connected, "Not
available on Linux machines yet." or "Not available through the Relay yet."
otherwise). Desktop offers every operation on every route: one that is not
supported is disabled with that reason as its tooltip, never hidden, and an
operation that fails shows its error.

| Operation | `MachineBackend` | Local and SSH | Relay |
|---|---|---|---|
| Rename | `renameConversation` | `session.rename {title}` | Control v2 `session.rename` |
| Fork | `forkConversation` | `session.fork {fork_session_id}`, daemon feature `session.fork` | Control v2 `session.fork`, the host's remote operation; the new session arrives in the command's completion |
| Archive | `setConversationArchived` | `session.set_archived {archived}`, daemon feature `session.archive/1` | not offered: `session.set_archived` is not a host remote operation |
| Attachments | none | not offered: `MachineBackend` has no staging operations | not offered |
| Older transcript pages | `transcriptPage` | `session.transcript_page {before_cursor, limit}` | window before a cursor, with a snapshot fence |
| Large events | `transcriptLargeEvent` (`remote::resolveLargeEvents`) | fragments, checked by `LargeEventReassembler` | reassembled on the host |

Over SSH the daemon features come from the remote daemon's `daemon.hello`, as on
This Mac, so an older Linux daemon shows Fork and Archive disabled. The client
names a fork (`fork_session_id`), so a retried request cannot fork twice; the
fork opens once the machine lists it (the Relay verifies it first). A queued
Relay rename or fork finishes when the host confirms the command.

**Rich renderer (deferred, leader decision A).** A remote conversation uses a
simplified widget transcript, with a visible note above it: "Simplified
transcript: rich rendering of conversations on Linux machines is not available
yet." This Mac's renderer is `AgentWorkspaceView`, which is the whole Agent
Workspace: its QML composer is bound to the local provider and model
configuration, file and conversation mentions read from the local disk,
attachments, worktree activation, terminal, details and changes panes, the
reviewer, background tasks and Atlas results (about 80 signals). Hosting it
for a remote machine needs either each of those routed through
`MachineBackend` or a read-only transcript mode in `modules/ai`. Either is
tracked separately and is not part of this decision.

### Machine identity

`local` names This Mac. A Linux machine is named by its daemon's host key
fingerprint (the key pinned at pairing,
[ADR 0034](0034-relay-is-not-trusted-for-device-identity.md)), which is the same whichever
route reaches it:

- The Relay backend maps its relay machine id to the pinned fingerprint.
- The SSH backend reads the fingerprint from the daemon's `daemon.hello`
  reply: `host_key_fingerprint`, advertised by the `daemon.host_identity/1`
  feature at protocol minor 1. It is `CloudCrypto::keyFingerprint` of the
  daemon's key set, the same value a pairing QR code carries as `fp`. The
  daemon creates its keys on first use, so a host that never signed in to the
  cloud has one, and keeps them in its own credential store across restarts.
- A host reachable over both routes is therefore one machine: one picker row,
  one conversation list, and conversations bound to the fingerprint keep
  their binding when the route changes. Desktop prefers SSH while it is
  reachable and falls back to the Relay.

### Host key rotation

An SSH machine has two keys, and they rotate independently:

- **The SSH host key** (OpenSSH's, checked against the KetPlus known_hosts
  file, docs/ssh-transport.md). A changed key is `HostKeyMismatch`: the
  machine is Offline with the reason and Desktop never bypasses it. The user
  checks the new key on the host, removes the old entry and connects again,
  which is a first-use acceptance of the new key. The machine id does not
  change, because it is the daemon's key, not the SSH server's.
- **The daemon host key**, which is the machine id. It changes only when the
  daemon's credential store is lost or replaced (a reinstall or a new user
  account). Desktop stores, for each SSH target, the machine id it last
  trusted. When a hello names a different fingerprint, or none, the
  connection fails closed before any request is sent: `DaemonClient` drops
  the session, fails what it queued with the reason, stops reconnecting, and
  the machine is Offline with "This machine's identity changed". Connections
  then shows the trusted and the presented fingerprint. Only an explicit
  "Trust the new identity" replaces the stored id. When the user does that,
  every conversation binding recorded with the old id moves to the new id in
  one rewrite, so the user keeps their conversations on that host. Rejecting
  leaves the machine Offline, and removing the machine forgets both. A Relay
  pairing is not moved: the Relay pins the old key (#88), so that route needs
  a new pairing.

### Binding to the selected machine

The window has one selected machine (the machine picker in the activity bar,
`mockups/qt/qml/shell/Sidebar.qml`). Everything that shows machine state reads
the selected machine's backend, never a transport:

- **Sidebar** — repositories and conversations come from the selected backend's
  `listRepositories` and `listConversations`. Switching machines replaces the
  list; it never merges machines. This Mac stays the default.
- **Agent Workspace** — a conversation records the `machineId` it was opened on.
  Its transcript paging, turns, steer and stop go to that machine's backend even
  if the picker changes afterwards, so a running turn is never sent to the wrong
  host.
- **Worktree picker** — lists the worktrees of the selected machine. A path is
  always the host's path; Desktop never resolves a remote path on This Mac or
  opens it in a local editor.
- **Approvals and questions** — the card is answered through the backend of the
  conversation's machine; the approval id is the host's. An answer that fails
  (expired, offline) marks the card expired, as for This Mac today.

### Connection states

`MachineBackend::ConnectionState`, reported with a reason for the UI:

| State | Meaning | What the UI shows |
| --- | --- | --- |
| Connecting | No session yet: first attempt, or a failed first attempt | Picker row and Connections show "Connecting…"; the sidebar shows a loading placeholder, no stale list |
| Connected | Hello completed; requests flow | Normal UI; Connections shows "Connected" with the route (SSH or Relay) |
| Reconnecting | A Connected session dropped; the route retries on its own | Last lists stay visible but read-only; composer and approval buttons are disabled with "Reconnecting…" and the reason |
| Offline | The route cannot reach the host or will not retry (host key changed, not installed, signed out, revoked) | Connections shows "Offline" and the reason with Connect; the picker row is dimmed; opening the machine shows the reason instead of a list |

A request made before Connected is the route's to hold or fail. The local and
SSH routes both speak through `DaemonClient`, which queues it until the hello,
as it always has. A request made while a route is Offline (it will not retry
on its own) fails at once with the state's reason rather than leaving the UI
waiting unseen; the Relay route does the same. While Connecting or
Reconnecting the Relay route holds a request until Connected, bounded by the
relay's 15-minute command lifetime. Writes are never replayed after a drop
(the existing `DaemonClient` rule); the Relay route's outbox is the exception
described below.

### SSH trust

SSH carries the daemon's **local** protocol, unchanged, to the Linux user's own
daemon socket. Desktop runs the system `ssh` with the user's configuration and
bridges the socket with `ssh -W <remote socket> <host>` in batch mode
(docs/ssh-transport.md). Current OpenSSH connects `-W` to a Unix socket path as
well as to a TCP host and port. The forward travels on ssh's stdin and stdout,
so ssh binds no listener on either side. On the Mac, a one-connection
`QLocalServer` in a directory only the Mac user can open bridges that stdio to
the one `QLocalSocket` that `SshTunnel::device()` returns, and it is removed
when the connection ends. A `DaemonClient` in its remote mode adopts that
socket and sends `daemon.hello` on it at once, within the daemon's 10-second
hello deadline. Desktop never opens an inbound port on the host and never talks
to another user's socket.

- **Confidentiality and integrity** come from SSH. The local protocol adds no
  encryption of its own on this route, and none is needed: the stream ends at a
  socket only the Linux user can open (the daemon's runtime directory is the
  user's).
- **Host authentication** comes from SSH. The host key is verified against the
  user's `known_hosts`. For a host not yet known, Connections displays the
  host key's SHA256 fingerprint and the user explicitly accepts it on first use;
  Desktop then adds it to its own KetPlus `known_hosts` file, never the user's. A changed host key is refused and the
  machine is Offline with the reason; Desktop never offers to bypass it.
- **Client authentication** comes from the user's SSH keys or SSH agent. KetPlus
  never stores, prompts for or forwards an SSH password, and runs `ssh` in batch
  mode so a password or keyboard-interactive prompt fails instead of appearing.
- **Daemon authentication**: the hello still needs the daemon's local token.
  Desktop reads it from the user's own token file over the verified SSH route
  (the locate step, `SshTunnel::daemonCredential()`), keeps it in memory while
  that connection is up and never writes it to This Mac. A token readable by
  other users is refused (`DaemonTokenInsecure`).
- The SSH route is the user's own access to their own account, so it needs no
  KetPlus account, pairing or Relay. Approvals on that host are answered by
  whoever holds that SSH access, exactly as `ketplusctl` on the host would.

### Relay backend

Reads come from `ControlV2Client`'s host-verified projection, filtered to the
backend's relay machine id; transcript windows and pages from its restore
path. Repositories use the neutral shape with the salted id and no `path`.
`createSession` takes `{"repository_id", "worktree_id", "prompt", "options"}`
and is sent only for a worktree in the verified projection: it reuses the
deployed `POST /v2/mobile/workspaces/{workspace}/session-creates` command and
the host's existing `CloudControlCommands` `session.create` path, sealed to the
machine's pinned agreement key with its envelope bound into the associated
data. No relay change is needed.

**Worktrees without a conversation (decision C).** The deployed relay's
worktree index (`GET /v2/mobile/worktrees`) is a plaintext heartbeat copy with
no host signature, so an entry no verified conversation covers is not shown on
the relay's word. For each such entry of a pinned machine, lazily and up to a
bound of new checks per listing (`verify_worktrees`, default 32, at most 100),
the client sends the existing `worktree.configuration.read` command in the
background: at most four at a time, each abandoned after five seconds. No
listing waits for it; an entry being checked is reported in `pending`, and
`MachineBackend::catalogChanged` announces each verdict. The verdict is cached
per catalog scope (ids, labels and the index's revisions), so a worktree is
checked once until its scope changes; a check that got no answer is tried again
after 30 seconds. The host answers inside its signed, sealed result with the
catalog scope it holds for that worktree (`repositoryId`, `worktreeId`,
`repositoryLabel`, `worktreeLabel`). The host keeps no catalog or lifecycle
revision (it accepts only 0), so the revisions are not part of the comparison.
The entry is shown, marked `verified: "host_read"`, only when every echoed
field equals the index entry.
Otherwise it is reported in `rejected` with a stable reason:
`unsigned_index_entry` (beyond the bound or not active), `worktree_not_granted`
(the relay refused the command as forbidden) or `verification_failed` (no
answer, a bad signature or a different scope). A `worktree_not_granted`
verdict expires after 30 seconds like any other, because grants change without
a scope change: a worktree granted later appears without a new sign-in. This needs no relay change and
no relay deploy. **Option A**, a relay-side index whose entries the host
seals (as it already seals session metadata), would remove the per-entry round
trip and is future work in the private relay service.

**Durable command outbox.** Every write (`startTurn`, `steerTurn`, `stopTurn`,
`resolveApproval`, `answerQuestion`, `createSession`) is
prepared and encrypted once, written to a per-machine owner-only
`CommandOutbox` file, and only then submitted. After a restart or reconnect,
only that stored ciphertext is submitted again, so the relay's idempotency key
answers with the command it already has and the host's command journal runs it
once. A result is verified (host signature, original scope) before a callback
or `commandCompleted` sees it, and each command id is applied at most once,
including after a restart: its completion is written to the outbox before the
result is applied, and a result that cannot be recorded stays pending and is
read back again while the link stays up, after 1 s doubling to 30 s with one
warning (and on a later connection). `pendingApprovals` is a read the relay carries as a command: it is
submitted and polled for its signed result without touching the outbox, so it
writes nothing to disk, is never replayed and does not count toward the
outbox's 256-entry cap; a lost one is asked again by its caller. A refusal
without an error code (`Control refused the request.`) fails that command and
leaves the link `Connected`; a server error, timeout or rate limit without a
code (HTTP 5xx, 408, 429) is a transport failure, so it keeps the command and
retries it. Only transport failures and invalid responses keep a command for
the next connection. Events are polled only for conversations the UI has open:
a transcript read or a write starts following a conversation (a read such as
`pendingApprovals` does not), and `MachineBackend::conversationClosed` removes
one that is closed or deselected, so the poll load does not grow with every
conversation ever opened. No UI consumer uses the Relay backend yet; the one
that moves onto it calls `conversationClosed`. Plaintext never reaches the file. An entry older than the
relay's 15-minute command lifetime is given up. A reported failure means the
command will never be sent: on `disconnectFromRelay()` or a fatal answer, a
command that never left this device is withdrawn from the outbox and fails,
and one the relay may already hold is answered `{"status": "queued",
"command_id"}`, with its outcome arriving later through `commandCompleted`.

**Connection states** follow the relay link: `Connecting` until the first
successful read, `Connected`, `Reconnecting` with bounded backoff (1 s doubling
to 30 s) after a transport failure, and `Offline` after `disconnectFromRelay()`
or a fatal answer (`invalid_access`, a revoked device or machine, sign-out, a
changed account). A refresh refused with 401 emits
`ControlV2Client::credentialRevoked`; the backend fences every request in
flight, so no response issued before the revocation reaches the UI, and it does
not retry until the user signs in again.

**Relay** keeps Control v2 unchanged: end-to-end encryption between Desktop and
the host, host keys pinned at pairing (#88,
[ADR 0034](0034-relay-is-not-trusted-for-device-identity.md)), and a relay that sees only
ciphertext. The two routes never mix: a Relay machine is never reached over SSH
by the Relay's identity, and an SSH machine needs no Relay credential.

### Relay entitlement

SSH is free; the Relay route is ketPlus Pro, and the private account service
decides who has it. Desktop reads it from the user session the account service
returns. That is the JSON body of `POST /v2/auth/token`, sent when the browser
sign-in completes, and of `POST /v2/auth/refresh`, which replaces it on every
credential rotation. Desktop reads one field:

```json
{
  "accessToken": "…",
  "refreshToken": "…",
  "subject": { "id": "…" },
  "entitlements": ["ketplus.relay"]
}
```

- `entitlements` is an array of strings. `"ketplus.relay"` lets the account
  pair Linux machines and work on them through the Relay.
- A session without the field, with the field in another shape, or without
  `"ketplus.relay"` in it is not entitled. Desktop fails closed (decision
  D-4): the Relay route is disabled with an upgrade hint, and SSH stays
  available.
- Desktop never gates the route on its build edition. The relay still
  authorizes each request by account and entitlement, and it trusts nothing a
  client says about itself; the client-side gate only explains the refusal
  before the user meets it.

The code is `ControlV2Client::entitlements()` and
`apps/desktop/src/remote/AccountEntitlements.h`.

## Consequences

- The local adapter (`DaemonMachineBackend`) and its tests
  (`tests/daemon/MachineBackendTest.cpp`) land with this ADR.
- Desktop composition (T-14): `MachineSelection`, `MachineCatalog` and
  `MachineWorkspaceView` (`apps/desktop/src/machine`) bind the sidebar,
  worktree picker, transcript, composer and approvals of a selected remote
  machine to its backend (agent-workspace.md, "Remote machines"). This Mac keeps
  its existing composition, so its behaviour does not change.
- Remote parity (T-18): older pages and large events through `MachineBackend`
  (`MachineConversationSession::loadOlder`, `remote::resolveLargeEvents`),
  rename, fork and archive through `MachineConversationActions` with the
  reasons above, and the visible note for the simplified transcript
  (`tests/MachineTranscriptPagingTest.cpp`).
- Scope item 2 (Relay backend) lands as `RelayMachineBackend`, `CommandOutbox`
  and the host's signed worktree scope in `worktree.configuration.read`, tested
  against the loopback relay (`tests/daemon/RemoteRelayBackendTest.cpp`).
- Scope item 3 (SSH transport with the fingerprint flow) and
  item 4 (Connections: route, Connected/Offline, Details with the verified fingerprint,
  Connect/Disconnect, the two-route "Connect a Linux machine" dialog) build on
  this interface.
- The Qt mock's Connections screen says "there is no SSH"; it is updated to show
  both routes with scope item 4.
- `docs/linux-headless-daemon.md` no longer says the local socket is never
  reached remotely: it is reached only through the user's own SSH access, never
  over a network listener.

## Alternatives considered

- **A `DaemonClient` subclass per transport.** `DaemonClient` owns the local
  socket's framing, hello, queueing and replay; Control v2 shares none of it
  (encrypted commands, signed results, event pages). Subclassing would force
  the Relay to fake a socket. The interface sits above both instead.
- **SSH carrying Control v2 instead of the local protocol.** It would give SSH
  a second end-to-end layer on top of SSH's own, and need pairing and a KetPlus
  account for a user who already has shell access to their account. SSH
  already authenticates the host and the user and ends at the user's own
  socket, so a second layer adds keys to manage and no protection.
- **A TCP listener on the Linux daemon.** Rejected: it adds an inbound port and
  a new authentication surface (ADR 0022 keeps the host outbound-only).
- **Password authentication for SSH.** Rejected: KetPlus would hold or relay a
  secret it cannot protect better than the user's SSH agent.
