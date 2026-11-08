# ADR 0058: Journal append requires every durable row

Status: accepted; append success requires the delivery and replay representations to commit together.

SQLite can execute a statement successfully while a trigger ignores its write.
Previously the append path checked execution alone. Synthetic faults reproduced
successful cursors despite missing delivery events, replay rows/state, large-item
manifests/fragments, activity projections, or the session cursor itself. The small
append path also returned an empty diagnostic for a deferred-constraint commit failure.

Both ordinary and chunked append now require exactly one affected row for each
mandatory insert/upsert or identity update. This includes replay revision changes
after delta replacement, replay cursor state, activity item/summary/head updates,
large payload storage, delivery events and the final session cursor. Existing
transaction rollback remains responsible for discarding all prior writes. Commit
errors are captured before rollback, separately from the final statement error.
No successful cursor is returned until commit succeeds.

The deterministic Qt fixture injects ignored writes at early and late positions,
UPSERT insert/update branches and a deferred foreign-key failure at commit. It
compares delivery/replay data and projection state before/after failure, verifies
that the connection accepts a fresh transaction, removes the fault, retries, and
reopens the journal. All payloads and databases are temporary. Thirty scenarios
fail against original production; the existing large-append commit recovery already
passes. The fixed focused fixture passes 33/33 including setup/cleanup. Affected Release/headless targets pass 13/13 (84.57 s);
protocol 74/74 and retention 124/124. Live-provider and opt-in 9 GiB cases remain
skipped by design.

This is not a new schema, protocol or UI. The existing large LocalJournal.cpp
keeps the small row checks beside its append/replay transaction; moving that code
would broaden this safety fix and is deferred. Large-payload and activity behavior
remain in their existing owning translation units.

Remaining H1.3 work includes ignored multirow delta removal/activity settlement,
integer counter limits and unsupported stored types, complete startup/migration
failure recovery, process interruption and other transaction callers. No live
journal, provider, Windows, multi-GB, packaging, install or deployment acceptance
is implied by this fixture.
