# ADR 0064: Release the activity backfill transaction after failed commit

Status: accepted; failed startup must not retain a write lock.

Activity initialization returned an error on commit failure without rolling back.
A deferred foreign-key fault reproduces the retained write transaction: another
connection cannot BEGIN IMMEDIATE while the failed LocalJournal object remains
alive. Initialization already reports failure; this is a lock-release bug, not
an observed successful-startup or lost-diagnostic bug.

Capture the commit diagnostic, roll back, then preserve the diagnostic in the
journal error. Rollback leaves the activity projection unchanged and releases the
writer so a separate connection can repair the cause and reopen the database.
The failed journal itself stays failed; there is no implicit retry or repair.

The temporary-journal regression covers turn, replay and activity startup commit
failures. Turn and replay are positive controls that already pass on old source;
activity fails the writer-release assertion. After the fix all five checks
(three scenarios plus setup/cleanup) pass. Each scenario keeps the failed object
alive, verifies its diagnostic, acquires a write transaction from another
connection, compares saved rows, removes the injected fault and reopens safely.

No schema, public API, UI or runtime policy change. This does not cover mid-fetch
read errors, unsupported stored formats, disk exhaustion, process interruption or
multi-GB migration bounds. Those remain H1.2/H1.3 acceptance work. No live journal,
provider, Windows, installation or deployment verification is implied.
