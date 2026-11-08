# ADR 0066: Validate existing media before publishing a reference

Status: accepted for blob deduplication; reference durability remains separate.

The media store used the existence of a content-addressed filename as evidence
that the requested image was stored. A corrupt, short, oversized or unreadable
file, directory, symlink or dangling symlink could therefore produce a successful
reference. Existing files also kept group/other read permissions. Twenty focused
attachment/inline-item cases reproduce eighteen failures on the original source;
the two valid-file controls already pass.

A dedup hit must be a regular nonsymlink file of the requested size. Read at most
the existing 16-MiB admission limit plus one byte, compare the actual bytes with
the supplied image, check read errors and require owner-only file permissions.
Only then return the reference. Refreshing the LRU timestamp remains best effort.
The media/reference directory helper also rejects a symlink or non-directory at
the requested root before changing its permissions. New blobs retain the existing
QSaveFile atomic-write path and owner-only permissions.

Unsupported existing entries are preserved for repair, not overwritten or
silently trusted. Attachments return an empty object and inline image items use
the existing storage_unavailable omission when blob validation fails. After
repair, the same request succeeds. No public API, retention default or UI changes.

This is not a descriptor-relative filesystem sandbox: ancestor symlinks and
concurrent external path replacement remain separate H1.4 work. Reference writes,
fork copying and journal/blob cross-file atomicity are not made durable by this
check. Capacity eviction retains its prior behavior. Tests use temporary files
and synthetic images, with no live data, provider, Windows or deployment claim.
