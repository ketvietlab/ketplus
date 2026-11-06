# 0024: WebRTC transport and a forward-only relay

Status: accepted
Date: 2026-10-12

## Context

The Control v2 relay stores every encrypted transcript event in PostgreSQL
(`ketplus_control.SessionEvent`) for 24 hours, the hot replica of
[ADR 0004](0004-canonical-transcript-hybrid-retention.md). Each appended event costs an insert,
several cursor and digest reads, a push job and a realtime notification, so relay load grows with
the number of events rather than the number of users or sessions. Phones read the newest turns and
catch up after backgrounding from that table.

The daemon journal is already the full-history authority, and the relay cannot read the content it
stores (against an active relay only since devices are pinned per Mac, [ADR 0034](0034-relay-is-not-trusted-for-device-identity.md)). Staging `productivity-web` has crashed on heap exhaustion; memory was raised as a stopgap
and the dominant heap owner has not been identified.

KétViewer already uses WebRTC between two devices, with in-memory signaling and short-lived TURN
credentials issued by `ketviewer_control` in `ketviet`. Its coturn staging deployment exists as a
runbook and is not deployed.

## Decision

- **Path order.** A remote client (Desktop on another machine, iOS, Android) reaches the daemon by
  a direct WebRTC DataChannel first, then by the same WebRTC session through TURN (UDP, then
  TCP/TLS), and only then by the HTTPS relay. A Desktop on the same machine keeps local IPC.
- **One application protocol on every path.** Session, generation, cursor, turn sequence and
  command ID do not change with the path. After a path change the client resumes after its durable
  cursor and retries commands with the same ID; the daemon records the deduplicated result before
  acknowledging. A connection epoch rejects late frames from a superseded path.
- **The relay does not store transcript events.** It forwards event frames in memory through
  bounded per-peer queues and drops them when no reader is attached or a queue is full. A reader
  that missed frames asks the daemon for a bounded page after its cursor. Relayed forward paging
  (`after_cursor`, currently rejected with `invalid_page`) is allowed under the existing event and
  byte caps.
- **No cloud copy of transcript content.** While the host is offline, a client shows only its own
  encrypted cache with an explicit offline state. There is no server-side last-turn snapshot.
  Content the client never received stays unavailable until the host is reachable again.
- **The cloud keeps the control plane only:** accounts, pairing, device keys, grants, revocation,
  presence, session metadata, the command mailbox, push registrations, signaling and TURN
  credentials. SDP and ICE candidates live in memory and are never written to the database or logs.
  TURN credentials are short-lived and issued after the same device and grant checks as the relay.
- **Push without stored events.** The daemon sends a content-free notification signal (event type,
  session and routing IDs); the relay no longer reads a stored event row to deliver a push.
- **Peer authentication.** The paired device keys of both ends bind the DTLS fingerprints and the
  session grant. A TURN credential alone grants no access. KétViewer's fixed-password model is not
  reused.
- **Compatibility.** The transport is negotiated as a capability. A daemon keeps the current event
  upload while the relay or any client paired with that host does not advertise it; the
  stored-event API remains until those versions are retired.

## Consequences

- Transcript ciphertext leaves the database; relay rows grow with sessions and devices, not events.
- A sleeping or offline Mac shows only cached transcripts on the phone. Results finished before the
  Mac slept but not yet received by the phone appear only after the Mac wakes. Linux headless hosts
  are normally online.
- The iOS cache is a bounded LRU, so an uncached conversation shows metadata only while its host is
  offline.
- Catching up after backgrounding costs a round trip to the daemon instead of a database read and
  requires a reachable host.
- Forwarding across several web replicas needs host-affine routing or a pub/sub layer, because a
  PostgreSQL notification payload is limited to about 8 KB. Decide this before running more than one
  replica.
- Revoking a device must also close its live WebRTC connections at the daemon.
- TURN adds a coturn deployment and its operations.
- ADR 0004's cloud hot replica is superseded for hosts and clients that negotiate this transport.
  Its canonical transcript identity, daemon authority, turn paging and generation fencing remain.
- This decision is not implemented yet. Documents that describe current behavior change with the
  implementation.

## Alternatives considered

- **Keep the 24-hour replica.** Rejected: relay cost grows with events and the server keeps
  ciphertext it never needs.
- **Per-session encrypted last-turn snapshot.** Rejected by product decision on 2026-10-12 in favor
  of the device cache only.
- **TURN over TLS on port 443 as the last resort, without the HTTPS relay.** Rejected: networks that
  allow only HTTP through a proxy block TURN, and existing clients need the HTTPS API.
- **Forward-only relay without TURN.** Rejected as the main fallback: it costs more per byte, and a
  path change would need application-level resume instead of an ICE path switch.

## Open for the transport spike

- libwebrtc or libdatachannel. libdatachannel needs its libnice backend for TURN over TCP/TLS.
  Review the license against [licensing](../licensing.md) before adoption.
- Gather TURN candidates from the start or only after a direct-connection timeout. ICE prefers
  direct pairs either way; a delayed TURN phase slows clients behind carrier-grade NAT.
- DataChannel layout (control, events, bulk), chunk size and backpressure limits.
- Signaling for several clients per host and several signaling replicas.

The [transport spike](../webrtc-transport-spike.md) and
[ADR 0025](0025-remote-transport-protocol.md) answer these except signaling across several relay
replicas, which stays open.
