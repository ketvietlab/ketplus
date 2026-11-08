# ADR 0068: Prepare fork media ownership before committing journal history

Status: accepted for local fork materialization; broader fork work bounds remain.

SessionHost previously committed copied history and then invoked an unchecked
media-reference copy. Unsupported source metadata or a failed/conflicting target
could still produce a successful fork. The copy also removed an existing target
before knowing the source could be copied. Temporary-journal/fake-provider tests
reproduce nine failures; the existing database-commit failure, matching target
and empty-source controls pass on the original implementation.

LocalJournal::forkSession accepts an optional synchronous before-commit
preparation. SessionHost always uses it to prepare media ownership after the
history copy checks and before the database commit. A preparation failure rolls
back the copied session and history, returning the existing journal_error result.
The callback does not make filesystem effects part of SQLite's transaction.

Media copy reads only the source and destination reference files, each bounded at
4 MiB, using checked status/read and canonical-digest validation. Reject unknown
root/file shapes, symlinks and unsupported content. Missing source metadata keeps
its legacy empty-set meaning; this does not reconstruct lost ownership. A
preexisting destination must have the exact same hash set, allowing retry after
a prior journal commit failure; conflicting metadata is preserved and rejected.
New target ownership is written atomically with checked owner-only permissions.

If ownership preparation succeeds but the database commit fails, the new target
reference may remain. It conservatively retains shared bytes. Retry accepts the
matching set; a reopen with a complete live-session catalog can remove the orphan
reference while preserving source-owned media. This ordering favors extra retained
metadata over committing a fork without its available ownership. It does not
provide a cross-file or power-loss transaction. A provider thread may already
exist before any local failure; compensating provider cleanup remains H3 work.

Reference parsing, atomic writes, copy and cleanup now live together in
McpMediaReferences.cpp; blob admission and image externalization stay in
McpMediaStore.cpp. The shared parser/writer preserves the existing publication
and catalog bounds. Both files remain below the production-size review threshold.

Initial Release/headless protocol and fork targets pass 2/2 (199 and 44 Qt checks).
The 12-case fork fixture then also verifies reopen cleanup after a deferred commit
failure and passes 14/14 including setup/cleanup. Shared-header changes still
require the full Pro gate and independent exact-head review. Tests use temporary
journals and fake providers; no UI implementation, live data/provider, Windows,
installation or deployment claim is made. Whole-history fork work bounds,
ancestor races and full crash recovery remain outside this package.
