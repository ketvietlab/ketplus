# 0092: Live-only incremental session discovery

Status: accepted
Date: 2026-11-08

## Context

The revision-fenced tombstone reader (ADR 0091) fixes oversized catalogs and
concurrent deletion reconciliation. Incremental discovery also used that reader,
although it only consumes live sessions. Independent review of #280 measured a
100,000-tombstone discovery walk at 857 ms, with a deletion signal for every row.
Repeated new-session events therefore repeated work proportional to retained
history on the Desktop event loop.

## Decision

`session.list` accepts `session_catalog_live_only: true` together with
`deleted_sessions_page_version: 1`. The live-only flag must be boolean. True
cannot be combined with any deletion cursor, snapshot or `deleted_sessions_only`
field. Invalid combinations return `invalid_session_catalog`; unsupported page
versions remain `invalid_tombstone_page`. False preserves normal catalog semantics.
`daemon.hello` advertises `session.catalog_live/1`.

The live-only reply contains `ok`, `session_catalog_live_only: true`, `sessions`
and the normal capability fields. It contains no deletion array, page version,
next cursor or snapshot. It cannot be mistaken for a complete deletion walk.
The server reads neither tombstone rows nor their revision. It still checks the
size of the complete serialized response envelope; an oversized live catalog
returns `session_catalog_too_large`, never a truncated success.

Incremental Desktop discovery makes one such request, keeps its reconnect epoch
fence and event-gap replay, and ignores all deletion fields. Initial/reconnect
refresh and `ketplusctl sessions` keep the complete revision-fenced paged reader.
Live `session.deleted` events also keep their existing generation-scoped handling.
No UI layout, rendering or ownership changes are involved.

The two-field request is intentionally compatible with older daemons: a paged
daemon predating live-only ignores the new flag and returns its first bounded
page with live sessions; discovery does not follow the next cursor. A pre-paging
daemon ignores both fields and returns its original legacy response. Its historic
large-catalog transport limit is not fixed retroactively.

## Verification and limits

Temporary real-daemon/fake-provider fixtures cover initial reconciliation and
subsequent phone-created sessions with 0, 20,000 and 100,000 tombstones, requiring
zero additional deletion signals during discovery. Scripted peers cover legacy
and older paged replies, event coalescing, cross-workspace discovery and replay of
events arriving before the catalog reply. Server tests remove the tombstone
revision/table to prove they are not read, while normal reconciliation still
fails closed; ambiguous requests and the actual wire limit remain tested.

This removes discovery's dependence on deletion history. It does not bound the
live session count, total reconnect reconciliation, per-row catalog-revision
triggers, whole-session deletion, whole-history fork, or all issue #68 gates.
