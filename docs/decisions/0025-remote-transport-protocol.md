# 0025: Remote transport protocol

Status: proposed
Date: 2026-10-12

## Context

[ADR 0024](0024-webrtc-transport-and-forward-only-relay.md) moves remote clients to a WebRTC
DataChannel (direct, then TURN, then the HTTPS relay) and stops the relay from storing transcript
events. It left four questions to a spike: the library, when to gather TURN candidates, the channel
layout, and signaling.

The [transport spike](../webrtc-transport-spike.md) answered the first three:

- libdatachannel with libjuice on the host interoperates with Chromium 154 (direct and TURN over
  UDP) and with iOS WebRTC 154 (direct and TURN over UDP, TCP and TLS).
- TCP needs libjuice `cb52376`: libjuice 1.7.4 refuses the zero ICE tiebreaker that libwebrtc
  sends from TURN-over-TCP ports.
- Channels share one SCTP association, so they do not isolate latency.
- iOS completes gathering long after its first usable candidate.

What remains is how the host, the relay and each client agree on the transport, and how a
connection is set up, authenticated and used. The full specification is
[remote transport](../remote-transport.md).

## Decision

- **Library.** The host uses libdatachannel 0.24.6 without media and with libjuice at `cb52376` or
  later, statically linked. iOS uses Google's WebRTC.
- **Negotiation by capability name.**
  - `transport.webrtc/1` and `events.forward_only/1` are advertised by the host in its heartbeat,
    by each client on its device record, and by the relay in the heartbeat answer and the machine
    list.
  - The relay enables `events.forward_only/1` for a machine only when every active device paired
    with it advertises the name. Until then the host keeps uploading events.
- **Signaling on the existing `/v2/relay` sockets.**
  - The relay issues a `connectionId` and TURN REST credentials after its device and grant checks.
  - It forwards offer, answer and trickled candidates in memory only.
  - It bounds and rate-limits them, and never retries one.
  - The client offers; the host answers.
- **Peers authenticate each other, not the relay.**
  - The offer is signed with the device's Ed25519 key and the answer with the machine's key, each
    over the exact SDP. This binds the DTLS fingerprints to the paired identities.
  - The relay adds a short-lived grant to the offer, carrying what it stamps on mailbox commands
    today. The host applies the same per-command checks and closes the connection when the grant
    expires or the device is revoked.
- **No ICE restart.** libjuice does not support it. A network change, an ICE failure or an expiring
  credential builds a new connection. The host keeps one connection per device and numbers them by
  epoch; clients drop frames from an older epoch.
- **Channels.** There are three ordered, reliable channels: `control`, `events` and `bulk`.
  - Messages are at most 64 KiB.
  - The sender gives strict priority to `control`, then `events`.
  - `bulk` keeps at most 1 MiB buffered.
  - Larger frames become transfers on `bulk`.
- **The same frames on every path.**
  - Frame bodies reuse the Control v2 schemas: sealed commands, `SessionEvent` envelopes and
    encrypted results.
  - A frame is the same on WebRTC and on the forward-only relay.
  - Clients resume after their cursor and retry commands with the same ID; the journal's recorded
    result makes a retry safe on any path.
- **Mailbox-only commands.** A command whose associated data binds relay-issued state, today
  `session.delete` with its deletion challenge, stays on the mailbox.

## Consequences

- The `/v2/relay` socket becomes a message socket on both ends, not only a wake-up hint.
- The host must keep its device list current. The relay must send revocation messages and renew
  grants for live connections.
- The host opens UDP ports only for connections being set up or open, and every check needs ICE
  credentials from signed signals. The rule of no unauthenticated public listening port still holds.
- Clients verify machine signatures on answers with the key they already use for events.
- The relay must enforce signaling bounds, keep SDP and candidates out of storage and logs, and
  forward frames through bounded per-device queues.
- A device that pairs without `events.forward_only/1` turns event upload back on for its machine;
  it reads earlier history through restore requests.
- Running more than one relay replica needs host-affine routing or a pub/sub layer for signals and
  frames. PostgreSQL notifications are queued in files under the data directory, so they cannot
  carry SDP.
- The pinned libjuice must move to a released version once a libdatachannel release includes the
  fix.

## Alternatives considered

- **ICE restart on a network change.** Rejected: libjuice refuses a remote description with new ICE
  credentials, and accepts local ones only before gathering.
- **A separate signaling service with rooms, like KétViewer's.** Rejected: both ends already hold an
  authenticated relay socket, and KétViewer's rooms admit anyone with a code instead of a paired
  device.
- **Trusting the relay or TURN credentials to authenticate peers.** Rejected by ADR 0024: the relay
  could then join a connection.
- **A new binary frame format.** Rejected: reusing the Control v2 bodies keeps one protocol on every
  path and reuses the existing verification code. Only transfers above 64 KiB are binary.
- **One channel with sender-side priority.** Rejected: separate SCTP streams keep a lost bulk packet
  from blocking control delivery, except over TURN on TCP.
- **The host deciding `events.forward_only/1` from its own device list.** Rejected: the host does
  not see a pairing in progress, and would stop uploading events a new device still needs.
- **Waiting for complete gathering before offering.** Rejected by the spike: one iOS run had not
  completed after 15 seconds.
