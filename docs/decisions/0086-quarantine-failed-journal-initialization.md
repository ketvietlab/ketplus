# ADR 0086: Quarantine failed journal initialization

Status: proposed; H1.2 startup failure boundaries, extending ADR 0064.

A LocalJournal constructor could leave its SQL connection open after failed
initialization. `isOpen()` used the connection state and mutable operation error;
policy reads/writes or session writes could clear the startup diagnostic and
operate on a partially initialized journal. This is separate from ADR 0064's
write-lock release. The daemon service already checks `isOpen()` immediately and
refuses startup on error; the regression demonstrates unsafe reuse of the storage
object, not a demonstrated bypass of that service check.

Close the connection when any constructor stage fails: base schema/backfills,
activity groups, or agent schema. Preserve the initial failure diagnostic for the
caller's startup check. Public operations on that failed object cannot revive the
connection or write through it merely by resetting their operation error. A
later attempted operation can report its own closed-connection error; this is not
a new persistent error-history API.

Repair is external to the failed object. Another connection may repair the
cause, and a new LocalJournal must run every initialization stage before use.
Keep the failed object closed even after repair. Successful initialization and
normal recovery from subsequent operational errors retain their current behavior.
This does not make all startup stages one transaction or undo previously committed
schema work. No UI, schema migration, public interface, or automatic repair is added.

Temporary-journal regressions inject deferred commit failures in turn, replay and
activity backfill, then attempt policy reads, policy writes and session writes on
the failed object. Existing no-operation cases continue checking the initial
diagnostic, released writer, unchanged retained rows and successful reopen. An
incompatible agent schema covers the final constructor stage, preserving history
and conversation-number ownership while repair/reopen restores normal use.

Broader unknown-schema admission, mid-fetch errors, disk exhaustion, interruption,
large index creation and live-journal/platform acceptance remain open. No user
journal, provider quota, installed application or deployment is involved.
