# ADR 0056: Fail-closed provider ownership reads

Status: accepted; read failure is not evidence that a provider thread is unowned.

Provider cleanup used `sessions()` to check retained references. That compatibility
API returned an empty list when SELECT failed and a partial list when fetching a
later row failed. A synthetic Qt/fake-provider probe reproduced `thread/delete`
for a thread still owned by a retained session in both cases. A new local deletion
also reported success while its ownership query was failing.

`readSessions()` returns an optional complete catalog. Both statement failure and
an error after row iteration return no value; a successful empty catalog remains
distinct. The existing `sessions()` wrapper retains its empty-on-failure API for
other callers, and no longer exposes partial results. This change does not claim
that every other caller's response to failure has been audited.

New local deletion obtains ownership through the fallible API before storing a
tombstone or removing history. Failure returns a journal error and keeps the
conversation. Each queued provider target also checks a complete catalog before
sending a delete or clearing a shared target. Read failure leaves the durable work
pending and returns from the pass; existing round-robin maintenance/restart retries
it. A complete snapshot with a retained reference still protects that thread.

The regression fixture causes an immediate query error and a JSON expression
error on a later row of a temporary catalog view. A fake-provider response barrier
ensures any earlier delete request would be visible before the assertion; no
fixed sleep substitutes for provider ordering. Neither error may issue a delete
or clear its pending target. Repair/restart completes the shared-target receipt
without deleting the retained thread. A separate local-delete case preserves both
sessions during the fault, then permits a fresh request after repair.

This is a database-read safety boundary, not a full provider metadata validation
or storage-work bound. Malformed ownership JSON/unsupported record shapes,
per-record size, catalog enumeration cost and provider lifecycle races remain
separate audits. Tests use temporary journals and fake providers only. No UI,
live journal, provider quota, deployment or installation is involved. The shared
header change requires the full Pro matrix before merge.
