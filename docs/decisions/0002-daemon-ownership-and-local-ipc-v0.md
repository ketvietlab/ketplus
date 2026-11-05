# 0002: Daemon ownership and local IPC v0

Status: accepted
Date: 2026-09-10

## Context

Remote operation must survive the Desktop UI closing. The current Desktop owns
the Codex app-server child process and UI-oriented session projections, so its
lifetime and data model cannot provide that boundary. Desktop and KetDaemon also
need a local contract that does not expose provider-specific JSON-RPC.

## Decision

KetDaemon is a separate C++20 executable under `apps/daemon` using only Qt Core,
Network, and Sql. It owns the Codex app-server process, normalized session state,
approval/input arbitration, and a SQLite WAL journal. Desktop will attach as a
client; disconnecting a local client never stops the provider process.

Local IPC v0 uses a four-byte unsigned big-endian length followed by one UTF-8
JSON object over `QLocalSocket`. `QLocalServer::UserAccessOption` restricts the
endpoint, and the first message must be `daemon.hello` with the user-only local
credential. The credential and journal are stored with owner-only permissions on
Unix. No public TCP listener is created.

Every envelope contains `protocol_version`, `message_id`, `message_type`,
`sent_at`, and an object `payload`. Session operations additionally carry
`session_id`, `session_generation`, and cursors where applicable. Version 0 is an
exact compatibility match; unsupported versions fail closed. ADR 0015 adds an additive
`protocol_minor` and a `features` list, negotiated in `daemon.hello`, without
changing this major. Mutating
`message_id` values are durable idempotency keys, and stale generations fail.

`session.rename` is a generation-fenced mutation with a non-empty title of at
most 4 KiB. It updates the journal before acknowledging, emits a normalized
`session.renamed` event, and lets the provider adapter synchronize its own
user-facing thread name without exposing a provider thread id over IPC.

`session.fork` (minor-1 feature, ADR 0016) copies an idle session into a new
session. Its optional `fork_session_id` makes a retry return the same fork.

The v0 limits are:

- 1 MiB maximum IPC JSON frame, checked from the prefix before allocation;
- 2 MiB maximum queued outbound bytes per local connection;
- 256 KiB maximum prompt and individual journal event payload;
- 500 events maximum in one replay response;
- 60 seconds for an approval or user-input request.

Replay callers may request a smaller page and completed-item compaction. A
compacted page omits command output deltas that have no transcript projection
and assistant deltas superseded by a final `item.completed` payload. Its
`next_event_cursor` advances across omitted source events; deltas for incomplete
assistant items remain intact. These optional fields preserve v0 behavior for
older clients and servers.

The journal assigns a monotonic cursor within `(session_id, generation)` and
deletes only the prefix acknowledged by every recorded local client. Requests
older than the retained prefix return `cursor_expired`. Cloud routes and relay
transport remain disabled and outside this decision.

## Consequences

- Desktop integration can migrate incrementally without making Codex JSON-RPC a
  product protocol.
- A Desktop restart can replay the same ordered suffix from its last cursor.
- Clients must preserve their client ID, idempotency keys, generation, and last
  acknowledged cursor.
- SQLite content is local but may include prompts and output, so the database is
  sensitive user data and must not be logged or copied into diagnostics.
- Version negotiation, OS credential-store-backed device keys, cloud transport,
  daemon crash survival, and installer service registration require later ADRs.

## Alternatives considered

- Newline-delimited JSON was rejected because bounded framing is harder to
  enforce before buffering a complete message.
- Raw Codex JSON-RPC was rejected because provider changes would force Desktop,
  mobile, and backend contract changes.
- WebSocket or cloud relay work was deferred until local ownership, replay, and
  idempotency semantics are proven.
