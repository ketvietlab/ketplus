#pragma once

#include <QString>

namespace ketplus::daemon::journal_space {

// The journal's pages, read with PRAGMAs (#128).
struct Usage final {
    qint64 pageSize{0};
    qint64 pageCount{0};
    qint64 freePages{0};
    // SQLite's auto_vacuum: 0 none, 1 full, 2 incremental.
    int autoVacuum{0};

    [[nodiscard]] qint64 keptBytes() const { return (pageCount - freePages) * pageSize; }
    [[nodiscard]] qint64 freeBytes() const { return freePages * pageSize; }
};

enum class Migration {
    // A new journal, or one already on incremental vacuum.
    NotNeeded,
    Migrated,
    // Too little free disk for the compacted copy; tried again at the next start.
    Deferred,
    Failed,
};

struct MigrationResult final {
    Migration outcome{Migration::NotNeeded};
    Usage before;
    Usage after;
    QString error;
};

// The disk a compacted copy needs beyond its own size before it is written.
[[nodiscard]] qint64 migrationMargin(const Usage& usage);

// Moves an existing journal to auto_vacuum = INCREMENTAL, so deleted rows can be returned to the
// disk from then on. That takes one full VACUUM: the journal is written compacted to a sibling
// file with VACUUM INTO, checked, synced and renamed over the original. Interrupted at any step,
// the original stays whole and the sibling is removed at the next start.
//
// The caller holds the journal's lock and has no connection open to it: the daemon calls this at
// start, before LocalJournal opens the file.
[[nodiscard]] MigrationResult migrateToIncrementalVacuum(const QString& path);

} // namespace ketplus::daemon::journal_space
