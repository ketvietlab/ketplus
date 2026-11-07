# ADR 0067: Publish media references only after ownership is committed

Status: accepted for image publication; fork-copy coordination remains separate.

Image attachment, inline MCP image/resource and local-image paths returned blob
references even when the session ownership file could not be written. The append
path ignored open, permission, write and close errors, and duplicated hashes on
every reuse. Original-source fixtures reproduce 24 ownership-failure cases and
three repeated-publication growth cases. An isolated POSIX file-size-limit probe
also confirms false success after a real partial reference write.

Reference publication now validates the existing per-session metadata: a regular
nonsymlink file, checked reads bounded at 4 MiB, and canonical digest lines. Keep
unsupported metadata intact and report failure. Merge valid existing and new
hashes into a unique sorted set, keep the serialized file within the same bound,
and use QSaveFile with checked owner-only permissions, complete write and commit.
Capacity enforcement follows a successful reference commit, as before.

A failed ownership update makes attachments return empty and replaces each newly
externalized inline image/resource with its existing storage-unavailable omission.
Other content is preserved. Local-image events follow their existing failed-store
omission path. Callers receive no successful new blob reference from that failed
publication. Repairing metadata permits retry and preserves shared ownership.

The focused temporary-media set passes 91/91, including prior cleanup/dedup
regressions. The added child-process fixture limits regular file size to 90 bytes:
24-byte synthetic images still fit, but the two-hash ownership file cannot. Both
attachment and inline publication fail, the original 65-byte ownership survives,
and retry after restoring the limit succeeds. This fixture is POSIX-only and
explicitly skips elsewhere; limits and signals affect only the test child.

This is checked atomic file replacement, not a cross-file/database transaction or
power-loss durability guarantee. Blob bytes are written before ownership: a failed
update may leave an unreferenced blob, which is not removed because it may be
shared. Write-admission/orphan capacity bounds, fork-copy/journal coordination,
ancestor-path confinement and concurrent external replacement remain H1.4 work.
No retention default, protocol or UI changes. No live journal, paid provider,
Windows, install or deployment claim is made.
