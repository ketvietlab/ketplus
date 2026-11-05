# ADR 0004: Canonical transcript with hybrid retention

Status: accepted; the cloud hot replica is superseded by
[ADR 0024](0024-webrtc-transport-and-forward-only-relay.md) for hosts and clients that
negotiate its transport

## Decision

KetPlus uses a provider-neutral, versioned transcript event stream. Provider identifiers and raw
payload details are references inside normalized events; they are not the primary transcript key.
The stable identity is `(workspace, machine, session, generation, cursor, turn_sequence)`.

The daemon SQLite journal is the full-history authority. It transactionally assigns a monotonically
increasing `turn_sequence` when `user.message` is accepted and maintains a compact replay projection.
The cloud is an opaque encrypted hot replica with a maximum 24-hour TTL, not the archive of record.
Mobile keeps an account-scoped encrypted SQLite projection for fast restore and offline display.

Clients initially request the newest three turns. They request three earlier turns when the reader
reaches the top. A cloud page may report that older content requires the Mac; the local bridge reads
that page directly from the daemon. Live events resume strictly after the page's `snapshot_cursor`.

## Consequences

- App launch cost is bounded by three turns instead of total conversation size.
- Claude, Cursor, Codex and KétRouter adapters must normalize into the same event taxonomy.
- Relay operators cannot read or semantically compact encrypted content; semantic compaction remains
  a daemon concern.
- Data older than 24 hours is unavailable remotely while the authoritative Mac is offline unless a
  future user-approved archival tier is introduced.
- Generation fencing, authenticated encryption metadata, cursor ordering and page/live deduplication
  are required at every boundary.
