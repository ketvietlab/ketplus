# ADR 0095: Journal format version

Status: accepted; a daemon never opens or rewrites a journal newer than it understands.

The journal had no format marker. Its schema grows with `CREATE ... IF NOT
EXISTS`, `ensureColumn` and one-time backfills, so an older daemon opening a
journal written by a newer one would run its own migrations over it, ignore
newer tables such as history purge markers or fork staging (ADR 0093/0094), and
could compact it with `VACUUM INTO` at startup (#68).

The journal now records `PRAGMA user_version`, defined as
`journal_format::current` (1). `LocalJournal` reads it right after opening and
before any schema statement: a value above the supported format, or a negative
one, refuses the journal with an error that names both formats and says the
journal was not changed. Startup compaction reads the same marker under its
exclusive connection and defers without checkpointing or copying, so the daemon
fails at `LocalJournal` with that error instead of rewriting the file. A journal
from before versioning reads 0; its format is recorded only after every
migration has succeeded, so a failed upgrade retries on the next open.

Raise the version whenever an older daemon would misread or damage what a newer
one writes. Purely additive changes that older code ignores safely may keep it.
Daemons released before this change do not read the marker; protection starts
with this release. Downgrade (opening an older format) remains supported by
migration; no reverse migration is provided. No protocol, UI or provider change
is included.
