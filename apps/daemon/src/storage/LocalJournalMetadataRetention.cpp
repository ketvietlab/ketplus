#include "storage/LocalJournal.h"

#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <limits>
#include <mutex>

namespace ketplus::daemon {
namespace {
constexpr qint64 byteBudget = 1024 * 1024;
constexpr int candidateLimit = 256;
enum class Outcome { Applied, Deferred, Failed };
struct Table {
    QString name;
    QString bytes;
    QString expired;
    int keep = 0;
};

QString bytesFor(const QStringList& text, const QStringList& integers = {}) {
    QStringList types, lengths;
    for (const auto& field : text) {
        types.append("typeof(t." + field + ")='text'");
        lengths.append("octet_length(t." + field + ")");
    }
    for (const auto& field : integers)
        types.append("typeof(t." + field + ")='integer'");
    lengths.append(QString::number(integers.size() * 8));
    return "CASE WHEN " + types.join(" AND ") + " THEN " + lengths.join('+') + " ELSE -1 END";
}
QString expired(const QString& field, int days) {
    return QString("CASE WHEN octet_length(t.%1)<=64 THEN datetime(t.%1)<datetime('now','-%2 "
                   "days') ELSE 0 END")
        .arg(field)
        .arg(days);
}
Table specification(const QString& name) {
    if (name == "command_results")
        return {name, bytesFor({"message_id", "result", "completed_at"}),
                expired("completed_at", LocalJournal::commandResultRetentionDays),
                LocalJournal::maximumCommandResults};
    if (name == "remote_command_audit")
        return {name,
                bytesFor(
                    {"command_id", "kind", "device_id", "session_id", "result_code", "recorded_at"},
                    {"device_key_version"}),
                expired("recorded_at", LocalJournal::remoteCommandRetentionDays),
                LocalJournal::maximumRemoteCommands};
    if (name == "session_tombstones")
        return {name,
                bytesFor({"session_id", "operation_id", "deleted_at"},
                         {"generation", "lifecycle_revision"}),
                expired("deleted_at", LocalJournal::tombstoneRetentionDays)};
    const QString orphan =
        "CASE WHEN octet_length(t.session_id)<=256 THEN NOT EXISTS (SELECT 1 FROM sessions s WHERE "
        "s.id=t.session_id AND s.generation=t.generation) ELSE 0 END";
    if (name == "client_cursors")
        return {name,
                bytesFor({"client_id", "session_id", "acknowledged_at"}, {"generation", "cursor"}),
                orphan};
    if (name == "background_tasks")
        return {name, bytesFor({"session_id", "tasks"}, {"generation"}), orphan};
    return {};
}
QString scanQuery(const Table& table) {
    // Metadata lengths prevent loading a huge legacy TEXT merely to decide whether
    // it fits. The bounded CASE also keeps orphan-key lookups and date parsing small.
    return QString("SELECT t.rowid,%1,CASE WHEN (%1) BETWEEN 0 AND %2 THEN (%3) ELSE 0 END "
                   "FROM %4 t WHERE t.rowid>? AND t.rowid<=? ORDER BY t.rowid LIMIT ?")
        .arg(table.bytes)
        .arg(byteBudget)
        .arg(table.expired, table.name);
}
QString cutoffQuery(const Table& table) {
    // Rowid describes accepted write order even when the wall clock goes backwards.
    // Offset is the fixed policy bound, never a backlog-sized scan.
    return QString("SELECT rowid FROM %1 ORDER BY rowid DESC LIMIT 1 OFFSET %2")
        .arg(table.name)
        .arg(table.keep);
}

Outcome collect(QSqlDatabase db, const Table& table, QString* error) {
    const auto fail = [&](const QSqlError& failure) {
        bool ok = false;
        const int code = failure.nativeErrorCode().toInt(&ok) & 0xff;
        if (ok && (code == 5 || code == 6))
            return Outcome::Deferred;
        *error = failure.text();
        return Outcome::Failed;
    };
    QSqlQuery capability(db);
    if (!capability.exec("SELECT 1 FROM pragma_function_list WHERE name='octet_length' LIMIT 1"))
        return fail(capability.lastError());
    if (!capability.next()) {
        static std::once_flag warning;
        std::call_once(warning, [] {
            qWarning("Journal metadata retention deferred: SQLite octet_length is unavailable.");
        });
        return Outcome::Deferred;
    }
    capability.finish();
    QSqlQuery timeout(db);
    if (!timeout.exec("PRAGMA busy_timeout") || !timeout.next())
        return fail(timeout.lastError());
    const int oldTimeout = timeout.value(0).toInt();
    timeout.finish();
    if (!timeout.exec("PRAGMA busy_timeout=0"))
        return fail(timeout.lastError());
    timeout.finish();
    bool transaction = false;
    const auto restore = qScopeGuard([&] {
        if (transaction)
            db.rollback();
        QSqlQuery q(db);
        q.exec(QString("PRAGMA busy_timeout=%1").arg(oldTimeout));
    });
    if (!db.transaction())
        return fail(db.lastError());
    transaction = true;
    QSqlQuery state(db);
    state.prepare("SELECT age_row,cap_row FROM journal_metadata_gc WHERE table_name=?");
    state.addBindValue(table.name);
    if (!state.exec())
        return fail(state.lastError());
    qint64 ageRow = 0, capRow = 0;
    if (state.next()) {
        ageRow = state.value(0).toLongLong();
        capRow = state.value(1).toLongLong();
        state.finish();
    } else {
        if (state.lastError().isValid())
            return fail(state.lastError());
        state.finish();
        state.prepare("INSERT INTO journal_metadata_gc(table_name,age_row,cap_row) VALUES(?,0,0)");
        state.addBindValue(table.name);
        if (!state.exec())
            return fail(state.lastError());
        if (state.numRowsAffected() != 1) {
            *error = "Metadata retention cursor was not created.";
            return Outcome::Failed;
        }
        state.finish();
    }
    qint64 deletedBytes = 0;
    int deletedRows = 0;
    const auto scan = [&](qint64& after, qint64 upper, bool cap) -> Outcome {
        QSqlQuery rows(db);
        rows.prepare(scanQuery(table));
        QList<QVariantList> candidates;
        // An empty tail costs no candidates. Wrap once in this pass so a small
        // table is rechecked at reopen without spending an otherwise idle pass.
        for (int attempt = 0; attempt < 2; ++attempt) {
            rows.bindValue(0, after);
            rows.bindValue(1, upper);
            rows.bindValue(2, candidateLimit);
            if (!rows.exec())
                return fail(rows.lastError());
            while (rows.next())
                candidates.append({rows.value(0), rows.value(1), rows.value(2)});
            if (rows.lastError().isValid())
                return fail(rows.lastError());
            rows.finish();
            if (!candidates.isEmpty() || after == 0)
                break;
            after = 0;
        }
        if (candidates.isEmpty()) {
            after = 0;
            return Outcome::Applied;
        }
        QSqlQuery remove(db);
        remove.prepare("DELETE FROM " + table.name + " WHERE rowid=?");
        for (const auto& row : candidates) {
            const qint64 bytes = row[1].toLongLong();
            const bool eligible = (cap || row[2].toBool()) && bytes >= 0 && bytes <= byteBudget;
            if (eligible && (deletedBytes + bytes > byteBudget || deletedRows >= candidateLimit))
                break;
            after = row[0].toLongLong();
            if (!eligible)
                continue;
            remove.bindValue(0, row[0]);
            if (!remove.exec())
                return fail(remove.lastError());
            if (remove.numRowsAffected() != 1) {
                *error = "Metadata retention selected row was not removed.";
                return Outcome::Failed;
            }
            remove.finish();
            deletedBytes += bytes;
            ++deletedRows;
        }
        return Outcome::Applied;
    };
    auto outcome = scan(ageRow, std::numeric_limits<qint64>::max(), false);
    if (outcome != Outcome::Applied)
        return outcome;
    if (table.keep > 0 && deletedBytes < byteBudget && deletedRows < candidateLimit) {
        QSqlQuery cutoff(db);
        if (!cutoff.exec(cutoffQuery(table)))
            return fail(cutoff.lastError());
        if (cutoff.next()) {
            const qint64 upper = cutoff.value(0).toLongLong();
            cutoff.finish();
            if (capRow > upper)
                capRow = 0;
            outcome = scan(capRow, upper, true);
            if (outcome != Outcome::Applied)
                return outcome;
        } else {
            if (cutoff.lastError().isValid())
                return fail(cutoff.lastError());
            capRow = 0;
            cutoff.finish();
        }
    }
    state.prepare("UPDATE journal_metadata_gc SET age_row=?,cap_row=? WHERE table_name=?");
    state.addBindValue(ageRow);
    state.addBindValue(capRow);
    state.addBindValue(table.name);
    if (!state.exec())
        return fail(state.lastError());
    if (state.numRowsAffected() != 1) {
        *error = "Metadata retention cursor was not persisted.";
        return Outcome::Failed;
    }
    state.finish();
    if (!db.commit())
        return fail(db.lastError());
    transaction = false;
    return Outcome::Applied;
}
} // namespace

namespace detail {
QString metadataRetentionScanQuery(const QString& table) { return scanQuery(specification(table)); }
QString metadataRetentionCutoffQuery(const QString& table) {
    return cutoffQuery(specification(table));
}
} // namespace detail

bool LocalJournal::pruneRetention() {
    if (!pruneLargePayloadGarbage())
        return false;
    for (const auto& name :
         {QString("command_results"), QString("remote_command_audit"),
          QString("session_tombstones"), QString("client_cursors"), QString("background_tasks")}) {
        QString error;
        if (collect(database_, specification(name), &error) == Outcome::Failed) {
            setError(error);
            return false;
        }
    }
    commandResultsSincePrune_ = 0;
    remoteCommandsSincePrune_ = 0;
    return true;
}

bool LocalJournal::pruneRemoteAudit() {
    QString error;
    if (collect(database_, specification("remote_command_audit"), &error) == Outcome::Failed) {
        setError(error);
        return false;
    }
    remoteCommandsSincePrune_ = 0;
    return true;
}
} // namespace ketplus::daemon
