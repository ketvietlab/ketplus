# ADR 0087: Keep the journal migration copy private while it is written

Status: proposed; implementation and local verification complete before review.

The migration previously created `<journal>.compact` through SQLite and applied
owner-only file permissions after the copy was complete. A synthetic 64-MiB
fixture observed a nonempty copy with mode 0644 under umask 0022. The umask 0077
control passed. This demonstrates a permission gap in the copy itself, not an
observed disclosure of user data: a private parent directory also restricts
access, and the tests use a private temporary root.

Create `<journal>.compact-private` with owner-only directory permissions before
copying. SQLite creates `journal.sqlite` and any sidecars inside that container.
On POSIX, check the directory owner and absence of group/other permissions, then
require chmod 0600 on the completed copy before it can be promoted. Retain the
existing integrity/schema checks, sync and atomic rename. Do not change the
process-wide umask. Use an absolute destination filename.

The target must be created by SQLite. Precreating an empty file works with some
SQLite builds, but the macOS SQLite used by the tested Qt plugin rejected it as
an existing output file. A private container protects the copy without relying
on that platform behavior.

Interrupted cleanup admits only the four known regular, nonsymlink files in the
private staging directory. Stop after four entries and one rejection; never
recurse. Unknown entries, symlinks, non-directory roots and unsupported POSIX
ownership/permissions are preserved for repair. Legacy `.compact` siblings are
removed only when they are regular nonsymlink files. If cleanup or protection
fails before promotion, preserve the original journal and report failure. A
cleanup error after successful promotion reports that migration succeeded but
staging artifacts remain; it cannot undo the completed rename.

Tests observe the copy during writing under two umasks, compare retained hashes,
verify integrity and final permissions, exercise recognized/unsupported stale
artifacts, and retry after removing a synthetic blocker. Removing final chmod
causes the final-permission assertion to fail even though the staging directory
still protects the in-progress copy.

This does not establish Windows ACL behavior, descriptor-relative protection
against ancestor/same-user replacement races, crash/power-loss safety at every
instruction, or mixed multi-GB acceptance. Those existing plan gates stay open.
No live journal, provider, installation or deployment is involved.
