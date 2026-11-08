# ADR 0059: Preserve unsupported or exhausted append counters

Status: accepted; append cannot coerce or wrap the journal's ordering counters.

The existing append path converted SQLite cursor/turn values without checking
storage class and incremented signed 64-bit counters without proving headroom.
Synthetic fixtures reproduced accepted writes with negative, fractional, text/blob
or exhausted counters. Large-item append also needs enough cursors for every
transport chunk and its final manifest, not just the first row.

The session lookup projects integer values only and checks both SQLite storage
classes before conversion. Unsupported TEXT/BLOB counter contents are not hydrated.
Append refuses negative counters, an exhausted cursor, or an exhausted turn when
the event starts a new user turn. A maximum turn remains usable by events that do
not increment it. Chunked append checks the complete cursor range before writing
its payload manifest, fragments or delivery rows. Invalid state is preserved for
recovery; the caller receives an error and the transaction is released.

The focused fixture covers unsupported storage classes, negative values, ordinary
and chunked overflow, the last available cursor/turn, an unchanged maximum turn,
and a chunk transfer fitting exactly at the integer boundary. Refused writes leave
counter values and history unchanged; fixture repair, retry and reopen succeed.
Original production fails 14 scenarios. Fixed focused fixture passes 25/25,
including setup/cleanup and four positive boundary controls.

No schema, public interface, protocol or UI changes. Small checks stay at the
existing append arithmetic in LocalJournal.cpp; broader extraction of that large
file is outside this fix. This is not a complete audit of migration/backfill,
fork, revision counters, arbitrary catalog field bounds or crash recovery. Those
remain in H1.2/H1.3; live journals/providers, Windows and deployment are untested.
