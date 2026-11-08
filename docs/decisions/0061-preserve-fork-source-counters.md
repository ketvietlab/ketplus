# ADR 0061: Preserve unsupported fork-source counters

Status: accepted; fork cannot normalize unsupported source metadata by coercion.

Fork previously converted source cursor/turn columns with toLongLong without
checking their SQLite storage class or sign. Eight temporary-journal probes
reproduced accepted forks for negative, fractional, TEXT and BLOB values in either
counter. The copy could therefore advertise metadata different from its source.

The source query now returns integer counter values only, with separate type
indicators. Noninteger or negative counters reject the fork before assigning its
metadata or writing the destination. Unsupported TEXT/BLOB contents are not
hydrated. Existing transaction rollback preserves the source and leaves no target;
SQLite read failures retain their diagnostic rather than masquerading as absence.
No arithmetic is performed by this copy guard, unlike append's range reservation.

Tests preserve exact source counter representations and history, check destination
absence and transaction release, repair the temporary fixture, then retry and
reopen. The original eight scenarios fail; source copy resumes after repair. The
existing copy-count, destination-race, tombstone and empty-history fixtures remain
part of the combined fork suite.

No schema, public interface, CMake or UI change. This does not validate every source
column, repair unsupported values, or prove startup/backfill compatibility. Whole
fork work bounds, higher-level handling of post-copy append failures, multirow
projection mutations and process interruption remain H1/H2 work. No live journal,
provider, Windows, multi-GB, installation or deployment verification is implied.
