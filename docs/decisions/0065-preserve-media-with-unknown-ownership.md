# ADR 0065: Preserve media when reference ownership cannot be established

Status: accepted for destructive cleanup; media write/admission remains separate.

Media cleanup treated unreadable or malformed reference files as empty ownership.
Releasing a session, reconciling retained sessions or enforcing capacity could
therefore unlink reference metadata or shared image bytes without a complete
ownership view. Original-source fixtures fail 22 of the initial 24 fault cases
and all nine later catalog-budget/root-symlink cases. Two initial directory cases
already preserve data. A further filename regression caught permissive basename
validation in the first implementation.

Before destructive cleanup, read the reference catalog as a whole or decline the
operation. Recognized reference files must have a canonical lowercase SHA-256
filename and contain only complete lowercase digests or empty lines. Reject
symlink reference roots/entries, nonregular reference files, read/enumeration
errors and unsupported names/contents. An absent or empty catalog remains valid.
Use error-reporting filesystem iteration rather than treating an interrupted
listing as a complete catalog.

Reference reads are limited to 4 MiB per file, 16 MiB total and 4,096 directory
entries. Count unknown filenames too, so filtering cannot hide an unbounded scan.
These are cleanup admission bounds, not new retention defaults. Any failed or
incomplete preflight leaves reference files and image bytes untouched; capacity
may consequently remain above its configured limit until metadata is repaired or
a separate recovery path handles an oversized catalog. This does not stop new
media writes. With a valid catalog, existing shared-reference preservation and
capacity ordering (including its final referenced-media eviction) remain intact.

Reference unlink failures also stop blob deletion. There is no cross-file atomic
transaction: if a later unlink fails, earlier retired reference files may already
be gone, but this pass does not remove any blob. Filesystem races with external
writers, storage-path confinement beyond the reference root/entries, reference
write/copy durability and total media-file enumeration bounds remain H1.4 work.
No automatic repair or destructive fallback is introduced.

The focused temporary-store set passes 41/41: 36 malformed/type/symlink/filename/
budget cases across release, retain and capacity paths; existing media/privacy
controls; and an absent/empty-catalog compatibility case, plus setup/cleanup.
Fixtures restore reference metadata and verify normal shared deletion resumes.
Tests use synthetic PNG bytes and isolated directories. No real user media,
provider, Windows, installation or deployment verification is implied.
