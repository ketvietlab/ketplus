# ADR 0051: Bounded staged attachment expiry

Status: accepted; the 24-hour staging policy is unchanged, physical cleanup is eventual.

Starting an upload previously scanned all staged chunks and removed the entire
expired backlog in one transaction. A synthetic Qt fixture with 160 files and
102,400 chunks (50 MiB) took 105 ms even with no expired rows, or 554 ms deleting
all expired rows. Invalid manifests also ran this global maintenance before their
input was rejected. The regression reproduces both paths without user data.

Begin, read, commit and chunk append also check the manifest's 24-hour eligibility
directly by identity. An expired upload cannot remain usable merely because it is
beyond the current sweep window. Its stored identity remains reserved while the
bounded physical cleanup catches up. These checks use the existing SQLite date
function and do not depend on the optional byte-length cleanup capability.
The five-file/40-MiB draft quota likewise counts only usable manifests. Expired
rows awaiting physical cleanup cannot block a different new upload; the retiring
identity itself remains reserved and current drafts still enforce both limits.

`purgeExpiredAttachments` now scans at most 32 manifest and 128 chunk candidates
per call, using durable rowid cursors. One empty-tail wrap may occur without
increasing the candidate limit. Marking and deleting rows share a 1-MiB budget
of conservatively measured column bytes; it is not a bound on SQLite page/WAL
overhead. Stored byte lengths are inspected without reading chunk content.
Full session/generation/attachment identity lookups use the existing primary
indexes; there is no backlog-sized correlated DELETE. Reopen, valid upload begin
and the existing idle timer continue maintenance. Invalid input is rejected first.

Expiry has two phases inside the same durable maintenance protocol:

1. A supported expired manifest becomes `expiring`. Reads/commit/new begin cannot
   use this identity; chunk append already requires `uploading`. This retirement
   marker remains while any chunk with that session/generation/attachment identity
   exists, regardless of revision.
2. Bounded passes remove chunks whose parent is expiring, and orphan chunks with
   no parent. A current or unknown-format parent protects its chunks. Only when
   the indexed existence check finds no chunks can the manifest be removed and
   its identity reused by a new upload.

Removing the manifest first would allow revision-1 ID reuse to adopt leftover
chunks from a retired upload. The marker is therefore an integrity boundary,
not merely progress metadata. Existing public command envelopes do not change.
Begin retries during retirement receive the existing conflict result. Older
daemons' whole-expiry transaction can still remove these date-expired records;
they cannot successfully append or commit an `expiring` upload.

Markers, chunk/manifest deletions and both cursors commit together. Aborted or
ignored mutations and commit failure roll back the entire pass. Busy writers
defer immediately and the prior busy timeout is restored. Missing `octet_length`
defers with a one-time warning. Unsupported/oversized rows remain retained and
do not starve other candidate identities; a retained chunk keeps its retired
identity reserved. Recovery of such data remains a separate hardening item.

Cancellation also preserves the identity boundary. Cancelling an already retiring
upload succeeds logically without removing its marker or doing another bulk chunk
delete. A stale expected revision leaves the current parent and all its chunks
untouched. For a current upload, cancellation removes the requested revision's
chunks; if any other revision remains, it retires the manifest instead of removing
it. Only an identity with no chunks can be released. Ignored manifest writes or a
failed commit roll back, including releasing the failed transaction for retry.
ADR 0054 subsequently bounds explicit cancellation of a current upload with a
shared 1-MiB manifest/chunk budget and 128 candidates across all revisions.
Remaining explicit session-delete bounds stay in the hardening plan.
Older cancellation code does not enforce this mixed-revision fence, so partial
retirement must be drained or recovered before a downgrade; rollback acceptance
remains a migration/release gate, not a claim of this focused change.

Attachment operations move from the oversized LocalJournal translation unit to
their owning file; maintenance is a separate file. Media decoding/materialization,
quota limits and provider cleanup remain separate audits. No UI implementation, provider calls, live
journal mutation, installation or deployment is part of this change.

The focused Qt suite covers backlog work, invalid-input side effects, current and
unknown parents, revision/generation isolation, byte-limited retirement/reopen/ID
reuse, cancellation during retirement, stale cancellation, unsupported chunks,
actual prepared query plans, failure rollback, busy
deferral and capability absence. Mutations remove the identity fence or increase
the byte budget; the retirement regression must fail for both.
