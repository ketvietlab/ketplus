# 0118: Remote machine composition and the machine-neutral backend

Status: proposed
Date: 2026-11-12

Issue #419 scope item 1. Amends ADR 0022 (a Linux host is reached only through Control v2) and
`docs/linux-headless-daemon.md`.

The team task named this ADR 0102; that number was already taken by
`0102-activity-files-backfill.md`, so it is 0118.

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
| `listRepositories` | `workspace.list` |
| `listWorktrees` | `session.worktrees` |
| `listConversations` | `session.list` |
| `transcriptWindow`, `transcriptPage` | `session.transcript_window`, `session.transcript_page` |
| `startTurn`, `steerTurn`, `stopTurn` | `session.start_turn`, `session.steer`, `session.interrupt` |
| `pendingApprovals` | `session.pending_approvals` |
| `resolveApproval`, `answerQuestion` | `approval.resolve`, `question.answer` |
| `connectionStateChanged`, `eventReceived` | readiness of the route; session events |

Requests and results are the daemon's local protocol objects on every route, so
a consumer parses one shape. Each backend has a `machineId` (`local` for This
Mac, the host's machine id otherwise) and a `Route`: `Local`, `Relay` or `Ssh`.

Three implementations:

- **Local** — `DaemonMachineBackend` over `DaemonClient`'s Unix socket. It only
  maps operations to request types and `readyChanged` / `disconnected` to
  connection states; queueing before the hello, replay after a disconnect and
  reconnect backoff stay in `DaemonClient`. This Mac behaves exactly as before.
- **SSH** — the same `DaemonMachineBackend` with a `DaemonClient` whose socket is
  an SSH-forwarded stream to the Linux user's own daemon socket
  (`$XDG_RUNTIME_DIR/ketplus-daemon-v0`). The wire protocol is the local one; only
  the transport differs (see "SSH trust").
- **Relay** — a backend over `ControlV2Client` that turns each operation into an
  encrypted Control v2 command and each authenticated, decrypted event page into
  `eventReceived`. It is scope item 2 of #419.

Existing consumers (`CodexAppServer`, `SessionCatalogReader`, the Connections
sections) still hold a `DaemonClient` today. They move to a `MachineBackend` one
at a time, each with its own tests; the local adapter lands first so that move
changes no behaviour. Operations a consumer needs later (for example fork,
rename, archive) are added to the interface with their local mapping when that
consumer moves, not speculatively.

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

A request made before Connected is the route's to hold or fail. The local route
queues it in `DaemonClient` until the hello, as it always has; a remote route
fails it with the state's reason rather than leaving the UI waiting unseen.
Writes are never replayed after a drop (the existing `DaemonClient` rule).

### SSH trust

SSH carries the daemon's **local** protocol, unchanged, to the Linux user's own
daemon socket. Desktop runs the system `ssh` with the user's configuration and
forwards a stream to the socket (`ssh -W` / stream-local forwarding); it never
opens an inbound port on the host and never talks to another user's socket.

- **Confidentiality and integrity** come from SSH. The local protocol adds no
  encryption of its own on this route, and none is needed: the stream ends at a
  socket only the Linux user can open (the daemon's runtime directory is the
  user's).
- **Host authentication** comes from SSH. The host key is verified against the
  user's `known_hosts`. For a host not yet known, Connections displays the
  host key's SHA256 fingerprint and the user explicitly accepts it on first use;
  Desktop then adds it to `known_hosts`. A changed host key is refused and the
  machine is Offline with the reason; Desktop never offers to bypass it.
- **Client authentication** comes from the user's SSH keys or SSH agent. KetPlus
  never stores, prompts for or forwards an SSH password, and runs `ssh` in batch
  mode so a password or keyboard-interactive prompt fails instead of appearing.
- **Daemon authentication**: the hello still needs the daemon's local token.
  Desktop reads it from the user's own token file over the same SSH connection,
  keeps it in memory for that connection only and never writes it to This Mac.
- The SSH route is the user's own access to their own account, so it needs no
  KetPlus account, pairing or Relay. Approvals on that host are answered by
  whoever holds that SSH access, exactly as `ketplusctl` on the host would.

**Relay** keeps Control v2 unchanged: end-to-end encryption between Desktop and
the host, host keys pinned at pairing (#88, ADR 0034), and a relay that sees only
ciphertext. The two routes never mix: a Relay machine is never reached over SSH
by the Relay's identity, and an SSH machine needs no Relay credential.

## Consequences

- The local adapter (`DaemonMachineBackend`) and its tests
  (`tests/daemon/MachineBackendTest.cpp`) land with this ADR; no consumer moves
  yet.
- Scope item 2 (Relay backend), 3 (SSH transport with the fingerprint flow) and
  4 (Connections: route, Connected/Offline, Details with the verified fingerprint,
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
