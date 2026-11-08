# ADR 0095: Journal format version

Status: accepted; a daemon never opens or rewrites a journal newer than it understands.

The journal had no format marker. Its schema grows with `CREATE ... IF NOT
EXISTS`, `ensureColumn` and one-time backfills, so an older daemon opening a
journal written by a newer one would run its own migrations over it, ignore
newer tables such as history purge markers or fork staging (ADR 0093/0094), and
could compact it with `VACUUM INTO` at startup (#68).

The journal now records `PRAGMA user_version`, defined as
`journal_format::current` (1). Before anything opens the journal for writing or
cleans up beside it, a read-only preflight reads the marker; a read-only close
never checkpoints or truncates the WAL, which may hold a newer marker that the
main file does not. `LocalJournal` and startup compaction share one predicate:
a negative value or one above the supported format refuses the journal with an
error that names both formats. Compaction then returns without removing an
earlier `.compact` artifact, checkpointing or copying, and `LocalJournal` does
not open a writable connection, so the journal file and its WAL stay byte for
byte as found (SQLite may still update the `-shm` index). The daemon itself has
already prepared its state directory, token and lock by then. `LocalJournal`
repeats the check on its open connection, and compaction under its exclusive
connection, in case the file changed after the preflight. A journal the
read-only connection cannot read keeps its previous handling. A journal from
before versioning reads 0; its format is recorded only after every migration
has succeeded, so a failed upgrade retries on the next open.

Raise the version whenever an older daemon would misread or damage what a newer
one writes. Purely additive changes that older code ignores safely may keep it.
A future raise must commit the migration that introduces the new semantics and
the new `user_version` in one transaction, so an interrupted migration cannot
leave new semantics under the old marker. Daemons released before this change
do not read the marker; protection starts with this release. Older journals are
upgraded and migrated forward on open; no reverse migration is provided. No protocol, UI or provider change
is included.
