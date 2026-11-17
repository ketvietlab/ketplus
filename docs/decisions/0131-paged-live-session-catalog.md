# 0131: Paged live session catalog

Status: accepted
Date: 2026-11-17

## Context

ADR 0091 bounds the deletion part of `session.list`, and ADR 0092 lets discovery
skip it, but both still return every live session in one reply. A reply is one
frame, and a frame is at most 1 MiB. A catalog row is roughly 300 bytes to 1 KiB,
depending on the title, tags, configuration and background tasks. An install with
about 2,000 to 3,000 conversations therefore gets `session_catalog_too_large`.
Desktop treats that as a failed refresh, and the sidebar and Agent Workspace list
none of the user's conversations.

A synthetic 3,000-conversation journal across 150 repositories reproduces this in
the real Pro app (`benchmarks/run-large-workspace.sh`). The same run shows that
applying the catalog wrote each workspace's settings once for every conversation
in it, and that every conversation rebuilt its workspace's list. Startup took
about 18 s of CPU, almost all of it on the UI thread.

## Decision

`session.list` accepts `live_sessions_page_version: 1`, and `daemon.hello`
advertises `session.catalog_pages/1`.

The daemon walks live sessions in UTF-8 byte order of the session id. A page
holds at most 512 rows and 512 KiB of compact row JSON, and always at least one
row. That leaves room in the frame for the first deletion page (64 KiB) and the
capability fields. A reply carries `live_sessions_page_version: 1` and
`live_sessions_next`: the last id on the page, or an empty string when the walk is
complete. The existing check of the whole frame against the wire limit stays.

- The first page may be requested on its own (live-only) or together with the
  deletion walk's final request, which returns its usual deletion fields as well.
- A continuation names `live_sessions_after: <id>`. It must also set
  `session_catalog_live_only: true`, with no deletion cursor, snapshot or
  deletion-only flag (ADR 0092), because a continuation never repeats the
  deletion walk.
- The following requests return `invalid_session_catalog`:
  - a page version other than 1;
  - a continuation without live-only;
  - an id that is empty, not a string, or longer than 1,024 bytes.

The shared transport reader (`readSessionCatalog`, used by Desktop refresh and
`ketplusctl sessions`) asks for live pages after the deletion walk. Discovery
uses the new `readLiveSessions`. The conversation list of a Linux machine in
Connections (`MachineCatalog`, over SSH or the daemon route) asks for the same
pages and stops at a page that does not move forward.

- Both readers check that ids strictly increase within a walk and that
  `live_sessions_next` equals the page's last id.
- Both return one catalog: the first page's fields with every row in `sessions`.
- Callers are unchanged.
- A reply without `live_sessions_page_version` comes from an older daemon, and the
  reader accepts it as the complete catalog, as before.
- An older client never sends the field and keeps the old behavior, including
  `session_catalog_too_large` for a catalog over the limit.

The walk is a keyset walk, not a snapshot:

- A session created behind the cursor during a walk is missed by that walk. Its
  first event triggers discovery, which reads it.
- A session deleted during a walk is reconciled by its `session.deleted` event and
  by the deletion walk.

While Desktop applies a catalog, `AgentSessionManager` defers three things to the
next event-loop turn, done once per workspace:

- the workspace settings writes, all through one `QSettings`;
- `conversationsChanged`;
- `activityChanged`.

Outside catalog application, every write and signal is still immediate.
`ensureSession` writes a deferred workspace before reading it back.

## Verification and limits

- `ketplus_provider_deletion_tests` covers the daemon side:
  - an unpaged 1,300-session live catalog over the limit still fails closed;
  - the paged walk returns every id once, in order, in at least three pages, each
    under the frame limit;
  - malformed requests are rejected.
- `ketplus_desktop_daemon_client_tests` covers Desktop refresh and discovery against a
  real `SessionHost` over the socket, with the same catalog.
- `ketplus_machine_composition_tests` covers a machine's list read in three pages.
- With the large-workspace benchmark on 3,000 conversations, Desktop lists all
  non-archived conversations. Startup CPU drops from about 18 s to about 2 s,
  against about 1 s for an empty install.

Hosted Control v2 clients still read an unpaged catalog. Their own paging belongs
to the hosted services and the mobile apps.
