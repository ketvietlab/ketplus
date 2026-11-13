# Remote transport

Status: host phase 3 implemented; native client phase 4 and forward-only rollout
remain pending. ADRs [0024](decisions/0024-webrtc-transport-and-forward-only-relay.md)
and [0025](decisions/0025-remote-transport-protocol.md) define the target protocol.
The daemon advertises `transport.webrtc/1` and `teams.remote/1` in this phase, so it continues
legacy encrypted event uploads and mailbox processing for existing clients.
Same-machine Desktop keeps IPC. No current iPhone release is claimed to use P2P.
A Desktop that can already reach a Linux host over OpenSSH can instead use the
SSH route in [`ssh-transport.md`](ssh-transport.md), which bypasses the relay.
Desktop reaches a paired Linux host over the Relay today through Control v2
(`RelayMachineBackend`, ADR 0119, `linux-headless-daemon.md`); this document's
WebRTC transport is a later, direct path for that same route and changes
nothing in how Desktop composes the machine.

This document specifies how a paired remote client and the daemon agree on the transport, set up a
WebRTC connection through the relay, authenticate it, and exchange frames, and how both ends
structure the code. Field names follow Control v2; exact relay endpoints are settled in the private relay service
in phase 2.

## Terms

When the relay definitively refuses a machine challenge with HTTP 404 and
`machine_not_found`, the host signs out, drops peer and Atlas access, and stops
retrying that enrollment. Local sessions and project files remain. Transient
failures and unrelated 404 responses keep the enrollment for retry.

- **Host**: the daemon of one machine.
- **Client**: a paired device (iOS, Android, or Desktop on another machine). Desktop on the same
  machine keeps local IPC and is not a client here.
- **Relay**: the Control v2 service, the `ketplus` tenant in the private relay service.
- **Path**: a direct WebRTC connection, a WebRTC connection through TURN, or the relay.
- **Connection**: one WebRTC peer connection between a host and a client, named by a
  relay-issued `connectionId`.
- **Epoch**: the host's number for a client's current connection; it grows with every connection
  the host accepts from that client.

## Capabilities

Features are gated by capability name, never by version number, as on local IPC:

| Name | Meaning |
| --- | --- |
| `transport.webrtc/1` | Signals through `/v2/relay`, obtains TURN credentials and speaks the frames below. |
| `events.forward_only/1` | Reads transcript events only from frames and host pages, never from stored events. |
| `teams.remote/1` | Answers sealed `team.*` commands through the mailbox and adds `team` to session metadata (ADR 0116). |
| `session.prepare/1` | Allocates a private inactive attachment scope before a conversation's first turn (ADR 0122). |

Each party advertises its own set:

- The host lists its names in every heartbeat (`capabilities`, next to `controlProtocolVersion`).
- A client stores its set on its device record when it pairs and whenever the set changes, such as
  after an app update.
- The relay answers each heartbeat with the names it enables for that machine
  (`compatibility.capabilities`) and shows clients the same set in each `/v2/machines` entry.

The desktop offers first-turn attachments only when the target daemon and relay both
advertise `session.prepare/1`. Preparation and its cancellation use the encrypted
mailbox; existing-session attachment commands keep their current route and format.
Before reporting preparation success, the host publishes private routing and a key
wrap only for the creating device. The scope stays out of conversation lists until
the first real turn. Cancel discards inactive files; abandoned scopes expire after
a fixed 24 hours. See [ADR 0122](decisions/0122-first-turn-remote-attachments.md).

The relay enables `transport.webrtc/1` for a machine when it and the host both support it. A client
tries WebRTC only when it supports the name too.

The relay enables `events.forward_only/1` only when, in addition, every active device paired with
the machine advertises it. Only the relay sees every device, including one whose pairing is still
in progress, so the relay decides and the host follows the heartbeat answer:

- Without the name, the host uploads events to `/v2/events:append` as today, also while it serves
  WebRTC clients.
- With it, the host stops uploading events. Heartbeats, session metadata, session keys, the
  command mailbox and push signals continue.
- When a device without the name pairs later, the next heartbeat answer withdraws it and the host
  uploads again from its current cursor. Earlier events are not uploaded again; that device reads
  them through restore requests, as it does today for history beyond the stored window.

## Paths

The client chooses the path. The host never initiates a connection.

1. When the app becomes active or a conversation opens, and the machine lists `transport.webrtc/1`,
   the client starts a connection. It gathers every candidate type from the start: ICE tries direct
   pairs before relayed ones by itself, and a delayed TURN phase only slows clients behind
   carrier-grade NAT.
2. Until the data channels open, and whenever no connection is open, the client uses the relay
   path: commands through the mailbox, wake-ups through `/v2/relay`, and events from the stored-event
   API or, with `events.forward_only/1`, from frames the relay forwards.
3. An attempt that has not opened within 10 seconds is abandoned. The client tries again after
   30 seconds, 2 minutes and then every 10 minutes, and at once when its network changes.

The path is visible to the user only as latency; diagnostics show its type.

## Signaling

Signals ride the `/v2/relay` socket that the daemon and the iOS app already hold for wake-ups.
The relay routes them by `connectionId` in memory. It never writes SDP or candidates to the
database or logs, and it forgets a connection 60 seconds after its last signal or at `signal.end`.

| Message | From → to | Fields |
| --- | --- | --- |
| `signal.start` | client → relay | `machineId` |
| `signal.started` | relay → client | `connectionId`, `iceServers`, `expiresAt` |
| `signal.offer` | client → relay → host | `connectionId`, `sdp`, `deviceKeyVersion`, `issuedAt`, `signature`; the relay adds `deviceId`, `grant` and the host's `iceServers` |
| `signal.answer` | host → relay → client | `connectionId`, `sdp`, `machineKeyVersion`, `issuedAt`, `signature` |
| `signal.candidate` | either → the other | `connectionId`, `candidate`, `sdpMid`; a null `candidate` ends gathering |
| `signal.grant` | relay → host | `connectionId`, a renewed `grant` |
| `signal.end` | any | `connectionId`, `reason` |
| `signal.error` | relay → client or host | `connectionId`, `code`: `host_offline`, `not_authorized`, `rate_limited` or `unsupported` |

- Before `signal.started` and again before it forwards an offer, the relay checks that the device is
  active, is paired with the machine and holds a grant, and that the host's socket is open.
- Bounds: SDP up to 16 KiB, at most 64 candidates per side of a connection, one `signal.start` per
  device every 2 seconds and at most 4 open connection IDs per device. The relay refuses the excess
  with `rate_limited` and does not queue it.
- The client creates the data channels and the offer, and trickles candidates as they are gathered.
  On iOS the first relayed candidate arrived after 54–75 ms but gathering completed only after
  1.1 seconds, once not within 15 seconds (spike finding 5). The host answers and trickles its own.
  libjuice on the host takes the controlled ICE role.
- A lost signal fails only that attempt; the client starts another with a new `connectionId`.
  Signals are never retried.

## TURN credentials

`iceServers` carries coturn REST credentials, the scheme the private relay service already issues:
username `<expiry>:<random id>` and credential `base64(HMAC-SHA1(secret, username))`. The ID is
random per issue, so TURN logs name neither a device nor a machine.

- The relay issues credentials only in `signal.started` and `signal.offer`, after the device and
  grant checks. A TURN credential alone reaches no host (see peer authentication).
- The host receives `stun:` and `turn:` over UDP; libjuice reaches TURN only over UDP. Clients also
  receive TCP and `turns:` URLs. Clients validate the `turns:` certificate (the spike did not,
  against a self-signed one).
- A credential outlives a typical connection. A client builds a new connection before its
  credential expires.

## Peer authentication

The relay can read, delay, drop or replace signals. It must not be able to join or read a
connection.

- The client signs its offer with its Ed25519 device key over
  `["ketplus", 2, "webrtc-offer", connectionId, workspaceId, machineId, deviceId, deviceKeyVersion, issuedAt, sha256(sdp)]`.
- The host signs its answer with its machine key over
  `["ketplus", 2, "webrtc-answer", connectionId, workspaceId, machineId, machineKeyVersion, deviceId, sha256(offerSdp), issuedAt, sha256(sdp)]`,
  which binds the answer to the offer it answers.
- Both arrays use the canonical encoding of command associated data (`CloudCrypto::canonical`).
- Each end verifies with a key it already trusts from pairing:
  - The host checks the device key from its device list (`/v2/devices`).
  - The client checks the machine's signing key, the one it already uses to verify event
    signatures.
  - A key version an end does not hold is fetched once.
  - A revoked device, an unknown key or a failed signature ends the attempt.
- The signatures cover the exact SDP bytes, so they cover the DTLS fingerprint and the ICE
  credentials. Both stacks refuse a DTLS peer whose certificate does not match the fingerprint in
  the remote description (libdatachannel's `checkFingerprint`). A relay that rewrites signals
  therefore cannot complete DTLS.
- An `issuedAt` more than 5 minutes from the receiver's clock, or a `connectionId` already used,
  ends the attempt.
- Content inside the connection stays end-to-end encrypted, exactly as on the relay. DTLS protects
  the transport; it does not authorize anything.

Authorization stays with the relay, as for mailbox commands today. The `grant` the relay adds to an
offer carries what the mailbox stamps on each command now: `workspaceId`, `ownerSubjectId`,
`credentialEpoch`, the device's capabilities and `expiresAt` (minutes, not hours).

- The host binds the connection to that grant and applies the same per-command checks as for
  mailbox commands.
- The relay sends `signal.grant` renewals while the device stays authorized. The host closes a
  connection whose grant expires.
- Commands whose associated data binds relay-issued state stay on the mailbox: `session.delete` and
  its deletion challenge.

This keeps "no unauthenticated public listening port". The host's ICE agent binds UDP ports only
for a connection being set up or open. A connectivity check must carry that connection's ICE
credentials, which travel only inside signed signals.

## Connections and epochs

- The host keeps at most one connection per client device and at most 8 per machine. A verified
  offer from a device that already has a connection supersedes it: the host answers, and closes
  the old connection when the new one's `hello` arrives.
- libjuice cannot restart ICE. It accepts local ICE credentials only before gathering, and it
  refuses a remote description with new ones ("ICE restart is not supported"). A network change
  therefore builds a new connection with a new `connectionId`. While the old connection still
  works, the client opens the new one before closing it.
- The host numbers the connections it accepts from each device and sends the number as
  `welcome.epoch`. Every host frame carries its epoch, and the client drops frames from an older
  epoch, so late frames from a superseded connection cannot reorder state.
- The client builds a new connection when:
  - its network path changes, as `RelayNetworkPath` already detects on iOS;
  - ICE stays `disconnected` for 5 seconds or reaches `failed`;
  - three pings go unanswered (one every 15 seconds while idle);
  - its TURN credential or grant is about to expire.
- iOS closes the connection when the app is suspended and builds a new one when it becomes active
  again. Its cache covers the gap.
- Revoking a device makes the relay close the device's `/v2/relay` socket (code 4003), send
  `device.revoked` on the host's socket, and stop renewing that device's grants. The host closes
  the device's connections at whichever comes first: that message, its next device-list refresh,
  or grant expiry.
- Signing the host out, or revoking the machine, closes every connection.

## Channels and frames

The client creates three ordered, reliable data channels:

| Label | Carries | Rule |
| --- | --- | --- |
| `control` | `hello`, `welcome`, commands, results up to 64 KiB, pings | Sent first. |
| `events` | Event pages, live events, gaps | Sent while `control` has nothing queued. |
| `bulk` | Transfers: larger results (output tails, media, restore windows) and large events | Sent only while the other two are idle; buffered amount 256 KiB to start, 1 MiB at most. |

All channels share one SCTP association, so they do not isolate latency by themselves (spike
finding 1). With a 1 MiB high-water mark, the control round trip during bulk was 41–56 ms at p50
directly and through TURN over UDP. Through TURN over TCP or TLS it reached 0.9 seconds at p95.
Separate streams still keep a lost bulk packet from blocking control delivery, except on a TCP leg.

The sender keeps one queue per channel and passes SCTP a lower channel's message only when the
higher ones have nothing waiting. Phase 3 tunes the limits on real networks, including a smaller
usrsctp send buffer and bulk on a second peer connection.

Messages are at most 64 KiB. Both stacks advertise 256 KiB, but without SCTP I-DATA one message
holds the association until it is sent.

A frame is one UTF-8 JSON text message. A larger frame becomes a transfer: binary messages on
`bulk`, each holding:

- a 12-byte header: an 8-byte transfer ID and a 4-byte index, big-endian, with the high bit set on
  the last part;
- then up to 65,524 bytes of the frame.

The receiver caps a transfer at 8 MiB, and a connection at 4 transfers in flight. A large event
sends `event.transfer` on `events` in its place, so events still apply in cursor order.

Host frames carry `epoch`. Bodies reuse the Control v2 schemas unchanged, so a frame is the same on
WebRTC and on the relay path:

| Frame | Direction | Body |
| --- | --- | --- |
| `hello` | client → host | `connectionId`, `capabilities`, `appVersion` |
| `welcome` | host → client | `connectionId`, `epoch`, `capabilities`, `daemonVersion` |
| `subscribe` | client → host | `sessionId`, `sessionGeneration`, `afterCursor` |
| `unsubscribe` | client → host | `sessionId` |
| `events` | host → client | `sessionId` and an `EventPage` of `SessionEvent`s; `live: true` once caught up |
| `event` | host → client | one `SessionEvent` |
| `event.transfer` | host → client | `sessionId`, `cursor`, `transferId` |
| `gap` | host → client | `sessionId`, `afterCursor`: live events were dropped, subscribe again |
| `session.reset` | host → client | `sessionId`, `sessionGeneration`: reload the newest window |
| `command` | client → host | the sealed command a client posts to `/v2/mobile/sessions/{id}/commands` today |
| `command.result` | host → client | `id`, `status`, `attempt`, `encryptedResult`, as the relay returns them today |
| `hint` | host → client | what `/v2/relay` wake-ups announce today beyond events and results, such as a changed session list |
| `ping`, `pong` | either | `nonce` |
| `close` | either | `reason` |

Events are the `SessionEvent` envelopes the host uploads today, from the same cloud projection
(command output removed), encrypted under the session key and signed with the machine key. Clients
verify and decrypt them the way they already do. `hello` and `welcome` carry capability names, so
later frames are gated by name like everything else.

The projection marks an event the transcript never shows with `transcript: "hidden"` in its
payload (`modules/remote/src/TranscriptVisibility.h`): KetPlus's own agents tools
(`ketplus_agents`, `mcp__ketplus_agents__*`, or Cursor's `providerIdentifier`), and the
`tasks.changed` background task snapshot. Desktop applies the same rule to its live view, its
history windows and the daemon's activity groups, and clients hide those rows. The events stay in
the journal and in the stream: the task bar still reads `tasks.changed`, and an older client that
ignores the marker draws them as before. A journal grouped before this rule existed is grouped
again once on open, for each session where such a call had joined a group (`activity_rules` row
`internal_steps_hidden`, run by the bounded passes after open of ADR 0097). A large item's stub
carries no marker; the phone hides that call by its tool name once it fetches the item.

## Resume and retry

- **Events.** A client keeps a durable cursor per conversation in its cache and subscribes after it
  on every new connection.
  - The host pages from there, bounded like uploads (100 events, 768 KiB), then sends live events.
  - A client more than 2,000 events behind receives `session.reset` and loads the newest window
    instead.
  - Each subscriber's live events pass a bounded queue of 4 MiB. When the queue fills, the host
    drops that subscriber's live events and sends `gap`, and the client subscribes again after its
    cursor. The forward-only relay applies the same rule.
- **Commands.** A client persists a sealed command before sending it, as clients do today. Without
  a result, it resends the same ciphertext with the same ID on the next connection or through the
  mailbox.
  - The host runs a relayed command under its command ID as the local `message_id`. A mutating
    command returns the journal's recorded result (`SessionHost::handleCommand`), so a retry on any
    path never runs it twice.
  - Each result is sealed for its delivery: `attempt` is 0 on WebRTC, and the lease attempt through
    the mailbox, as today.

## Relay path

Commands, wake-ups and restore requests keep the existing Control v2 API. Clients without
`events.forward_only/1` read stored events as they do today.

With `events.forward_only/1`:

- The host sends event frames for a device on its `/v2/relay` socket, and the relay forwards them to
  that device's socket. `subscribe` frames travel the other way.
- The relay holds a bounded queue per device. When no reader is attached or the queue is full, it
  drops frames, and the client recovers with `gap` and `subscribe`.
- This path refuses frames over 64 KiB. Large results keep the mailbox's continuation
  (`requestedByteCap`).

The forwarding is built in the private relay service in phase 2. `AtlasLiveTunnel` keeps its own `/v2/atlas/tunnel`
socket.

## Code structure

The names are proposals for phases 3 and 4. The host gets `apps/daemon/src/transport/`, which does
not depend on Control v2 HTTP internals:

- **`PeerLink`**: sends frames on a channel, reports buffered amounts, and signals received frames
  and closure. `WebRtcPeerLink` wraps libdatachannel's `rtc::PeerConnection`. `RelayPeerLink`
  carries the same frames over the relay socket.
- **`RemotePeers`**: one entry per device, holding:
  - the current link, epoch, grant and subscriptions;
  - three send queues with the priority rule.

  It decodes frames, hands commands to the same handler the mailbox path uses, publishes events from
  the cloud projection, and closes links on revocation or grant expiry.
- **`Signaling`**: the `signal.*` messages on the socket that `RelayWakeups` opens today, which grows
  from wake-ups to messages.
- **Frame codec and transfer reassembly**: with caps, unit-tested without a network.
- **Threading**: libdatachannel calls back on its own threads. Every callback is queued onto the
  daemon's main thread, and none blocks.

On iOS, `KetPlusRemoteCore` gets a matching `PeerLink` over Google's WebRTC, and
`HTTPControlPlaneClient` stays the relay path. A path selector above them picks one. Desktop's
`modules/remote` follows the same split. Android chooses its WebRTC build in phase 4.

The host builds libdatachannel 0.24.6 without media, with libjuice at `cb52376` or later (the
zero-tiebreaker fix) and statically linked. `docs/licensing.md` gains its dependencies, with
notices, in the change that ships them.

## Logs and privacy

- Never log SDP, candidates, ICE or TURN credentials, or IP addresses.
- Log the selected path (`host`, `srflx` or `relay` with its TURN transport), open time, epoch,
  close reason and counts.
- The relay sees candidate addresses in memory only, as it already sees client addresses.

The daemon reports `ice-path local=<type> remote=<type> turn=<transport>
epoch=<number> event=<open|ice-connected|path-changed> at=<UTC time>` when its data channels
open and when ICE reconnects after checking/disconnection. If completion changes
the selected candidate types without a reconnect, it logs `path-changed`, not a second `open`. Types are allowlisted
(`host`, `srflx`, `prflx`, `relay`, `unknown`). `turn` describes the **local**
client-to-TURN leg only: this libjuice backend supports UDP, so it is `udp` for
a local relayed candidate, `unknown` when the local type is unavailable, otherwise `none`. A remote relayed candidate does not
reveal that remote client's TURN transport; correlate its own diagnostics instead
of guessing from its candidate protocol. Native errors and raw candidates never
enter this log line.

## Tests

Tests consume no provider quota:

- **Host:**
  - the frame codec, transfers, send priority and caps;
  - an in-process libdatachannel pair, as in the spike, with fake signaling, covering resume,
    supersede, epochs, revocation and grant expiry.
- **iOS:**
  - the simulator against the host, as in the spike;
  - TURN over TCP, as the regression test for the zero tiebreaker.
- **Relay:**
  - routing, bounds and refusals;
  - credential issuance only after device and grant checks;
  - no SDP or candidate reaching the database or logs.

## Open

- Signaling and forwarding across several relay replicas: host-affine routing or a pub/sub layer.
  PostgreSQL's notification queue is kept in files under the data directory, so it cannot carry
  signals.
- Whether coturn refuses to refresh an allocation once its REST credential expires. This sets the
  credential lifetime in phase 2.
- Bulk isolation on real networks, and a physical iPhone on Wi-Fi and LTE.
- The content-free push signal of ADR 0024.

## Teams on paired devices

ADR [0116](decisions/0116-teams-on-paired-devices.md) defines the contract; the fixture is
`tests/daemon/fixtures/team-remote-v1.json`. The daemon side works as follows:

- `team.*` commands travel through the command mailbox only. A WebRTC peer that sends one is
  refused, and the session host keeps refusing `team.*` from the plain cloud path.
- A command is sealed to the Mac (`keyId` `team-command:<id>`, `sessionId` empty, generation
  0). Its additional data appends `ownerSubjectId`, `repositoryId` or empty,
  `expectedCatalogRevision` or -1, `recipientDeviceId`, `recipientKeyVersion` and
  `ephemeralPublicKey`. The result is sealed to the sending device (`team-result:<id>`), with
  `ownerSubjectId`, that device's id and key version appended.
- The sender must be a device pinned on this Mac. The command must name this Mac and its
  current key version, and carry `teams.read` for reads and `teams.write` for changes.
  Otherwise it is refused with `team_scope_forbidden`.
- `team.create` names its repository by the catalog's `repositoryId`. A path in any payload is
  refused. Results name repositories and worktrees by their salted ids, never by path.
- Each command runs through `TeamCoordinator`, the same rules a local client meets. Changes carry
  `expected_revision` and are fenced per part (settings, members, each task, each decision).
  A newer part is refused with `stale_team` and the current summary. A changing result is kept
  under the command id, so a retried command acts once.
- A result is at most 384 KiB. Past that, the daemon drops answered decisions first, then
  updates, then done tasks, and flags each `<part>_truncated`; otherwise `response_too_large`.
- Session metadata carries `team {id, revision, role}` for each member. After a team change the
  daemon sends a heartbeat, at most one every 2 seconds, so the phone sees the new revision.
  A deleted team leaves the metadata.
- Every changing command is written to the remote audit with its `team_id`; reads are not.

## Phase 3 host implementation and wire details

`apps/daemon/src/transport` owns the native peer, signaling, bounded frame/transfer
codec, scheduler and peer lifetimes. `CloudControlTransport.cpp` bridges the
existing active-device API, credentials, command dispatcher and encrypted event
projection. The relay socket keeps wake-up handling and now exposes bounded
`signal.*` messages and immediate authorization-revoked notification.

- Every frame has `type`. A client command is `{type: "command", command: sealedCommand}`.
  Results are `{type: "command.result", epoch, result: {id, attempt: 0, status,
  resultCode, encryptedResult, completedAt}}`; the exact existing result AAD is
  used. A restore's signed response envelopes are in `responses` inside the
  encrypted result, avoiding an unauthenticated parallel response channel.
- A page is `{type: "events", epoch, sessionId, page: {events, nextCursor, hasMore},
  live}`. `nextCursor` is the journal's scanned cursor, including compacted gaps.
  Up to 100 events / 768 KiB are read per page. The compacted projection keeps a large
  item's manifest but not its `transcript.large_chunk` events; the page reads them back
  from the journal and sends them before the manifest, as the relay upload does. One
  item may span pages: a page can end after any chunk, never on a manifest whose chunks
  it has not sent. A generation change, missing/refused
  session or lag above 2,000 source cursors resets the subscription. Caught-up
  subscriptions wake on journal events; they do not poll the database while idle.
- Offer/answer SDP digests are lowercase SHA-256 hex; signatures are base64url.
  Hash input is the exact UTF-8 SDP. Offers expire at ±5 minutes and used IDs are
  retained for 10 minutes. A changed grant identity, key epoch or capability set
  closes the peer. Deletion remains mailbox-only.
- Three reliable ordered channels are required. Native callbacks enqueue at most
  64 messages onto Qt; overflow closes the connection. Transfers are at most
  8 MiB, four inbound transfers, 30-second lifetime; packets have the documented
  12-byte header. Send queues cap control at 256 KiB, events at 4 MiB and bulk at
  16 MiB; each native channel buffers at most 256 KiB. The host pulls pages only
  when its application queue is empty and bulk is drained. A full result queue
  closes the link, letting the client recover through its durable cursor/mailbox.
- Eight connections including replacements, sixteen subscriptions and thirty-two
  in-flight commands per connection. A replacement supersedes the old link on
  `hello`. A peer must say hello within 10 seconds; idle ping is every 15 seconds,
  with closure after three unanswered pings. Candidates are capped and expire
  with their attempt. No SDP, candidates, addresses or TURN credentials are logged.

Verification uses an actual in-process native WebRTC pair with signed offer and
answer, plus deterministic grant/queue/codec tests and encrypted command retry
across WebRTC and mailbox. Real TURN, physical iPhone Wi-Fi/LTE and native client
failover remain unverified in phase 3. Forward-only `RelayPeerLink` and push
without cloud-stored events must land before advertising `events.forward_only/1`.
