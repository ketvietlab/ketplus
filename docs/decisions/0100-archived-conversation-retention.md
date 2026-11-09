# ADR 0100: Archived-conversation retention

Status: accepted; part of #128. The 30-day default was chosen by the product owner
(Jay) on 2026-11-09, shorter than the 90 days first proposed in #128.

## Context

Archiving hides a conversation without deleting it. Until now Desktop kept the
`archived` flag only in its own per-workspace settings. The daemon, which owns
the history, could not tell an archived conversation from a visible one, so
#128's "delete archived history after a period" could not be enforced.

## Decision

- The journal stores `sessions.archived_at`: a UTC ISO time, `''` when the
  conversation is not archived. `upsertSession` never writes it; only
  `setSessionArchived` does. A fork therefore starts unarchived.
- `session.set_archived` with `{archived: true|false}` (mutating, deduplicated
  by `message_id`, local or relay) sets or clears it.
  - Archiving an already archived conversation keeps the first time, so the
    period counts from when it was hidden.
  - `session.list` reports `archived_at` for archived conversations. Daemons
    that support this advertise the `session.archive/1` feature.
- Retention: an archived conversation is deleted once it has been archived for
  30 days (`SessionHost::archivedHistoryDays`).
  - The idle maintenance pass (`DaemonService::reclaimJournalSpace`, which
    runs only while no session is active) deletes at most one per pass, oldest
    first.
  - It deletes through `session.delete`, so the same guards, provider cleanup
    and tombstone apply. A conversation with a running turn or background task
    is skipped and retried later without blocking the next one.
  - An unreadable `archived_at` never counts as old.
- Desktop pushes its locally archived conversations to the daemon when it
  connects (separate change). For conversations archived before this release,
  the 30 days start at that push, so no one loses history on upgrade.

## Consequences

- Archived history now has a deadline. Restoring a conversation from Settings
  before it is due clears `archived_at` and cancels the deletion.
- The period is fixed until Settings → Storage offers 30 days / 90 days /
  1 year / Never (#128 UI). That setting will replace the constant with a
  revisioned policy, like the detail-retention period (ADR 0042).
- Phones can read `archived_at` from `session.list`. They do not archive yet.
