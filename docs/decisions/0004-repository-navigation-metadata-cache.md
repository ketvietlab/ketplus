# ADR 0004: Desktop repository navigation metadata cache

Status: accepted
Date: 2026-09-11

## Context

Repository pickers previously rediscovered the identity of every recent worktree
through Git before showing names. Persist the verified mapping in a disposable
desktop-owned SQLite cache, separate from daemon sessions and transcripts.

## Decision

`repository-metadata.sqlite` lives in `QStandardPaths::AppLocalDataLocation`.
Its `repository_metadata` table records canonical lookup path, Git common
directory (identity), main checkout path, and verification timestamp. The schema
contains no credentials or transcript content. Recent-path order remains in
QSettings; existing recent entries are populated into the cache as background
validation visits them, without modifying that history.

Sidebar names and Spotlight first read cached metadata, then validate with Git
off the GUI thread. Missing checkout/common-directory paths produce a cache
miss. So does an entry whose path is now owned by a checkout that no longer
belongs to the cached identity (a folder deleted and re-cloned, or a worktree
recreated for another repository). The owning checkout is the nearest folder at
or above the path with a `.git` entry. Its `.git` directory must be the
identity, or its `gitdir:` file must point inside it. Successful validation replaces old mappings; definitive Git failures
invalidate them. Timeout/start failures retain existing metadata. A moved
repository is rediscovered when its new location is opened; this does not scan
the filesystem to guess its new path.

Every cache operation owns a uniquely named Qt SQL connection on its calling
thread. SQLite WAL permits concurrent windows/processes. Read operations never
wait for database locks; writes have a bounded 100 ms busy timeout. Missing,
locked, or unreadable databases are cache misses and do not prevent navigation.
The cache can be removed and rebuilt, without touching the daemon journal.

The cache is a presentation optimization, not routing authority. RepositoryRouter
still verifies Git identity and file existence for activation and incoming IPC.
Incoming IPC runs those checks on the router's lookup pool and answers the
socket when they finish, so a slow repository doesn't block the receiving
window. A request is one line of at most 16 KiB. The server's read buffer is
bounded to that size, and a client that sends anything more is disconnected.

The one exception: when the identity lookup times out, the router uses the
cached identity for that path, which has passed the check above, and routes to
the checkout that owns it, not to a subfolder. It doesn't treat the path as a
repository of its own. Otherwise, linked
worktrees of a slow repository would route to different windows and
endpoints. A definitive Git failure still means "not a repository".

## Consequences

Warm navigation names do not require a subprocess. Cold lookups and stale data
are still resolved by Git in the background. Multiple application instances may
temporarily show an older name until validation completes. Activation trusts the
cache only when Git times out, as described above.

## Alternatives considered

An in-memory-only cache would lose all metadata at restart. Putting navigation
metadata in the daemon journal would couple desktop-only repository browsing to
the agent runtime and its connection lifecycle. QSettings remains suitable for
recent-path ordering but is not the metadata lookup store.
