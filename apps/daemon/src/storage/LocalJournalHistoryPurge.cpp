#include "storage/LocalJournal.h"

#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {
namespace {
struct HistoryTable {
    const char* name;
    int rows;
};
// Bulky per-generation history. Row limits keep one pass to a few MiB of freed pages:
// payload manifests hold up to 32 MiB, chunks are fragment-sized and text rows are small.
constexpr HistoryTable historyTables[] = {
    {"large_event_payloads", 1},      {"large_event_payload_chunks", 32},
    {"staged_attachment_chunks", 32}, {"events", 2048},
    {"replay_events", 2048},          {"cloud_events", 2048},
    {"activity_items", 2048},         {"activity_summaries", 2048},
};

bool isBusy(const QSqlError& error) {
    bool ok = false;
    const int code = error.nativeErrorCode().toInt(&ok) & 0xff;
    return ok && (code == 5 || code == 6);
}
} // namespace

bool LocalJournal::purgeHistoryBatch(const QString& sessionId, const qint64 generation,
                                     bool* remaining, QSqlError* error) {
    *remaining = false;
    for (const auto& table : historyTables) {
        QSqlQuery query(database_);
        // The primary key of every history table starts with (session_id, generation).
        query.prepare(QString("DELETE FROM %1 WHERE rowid IN (SELECT rowid FROM %1 WHERE "
                              "session_id=? AND generation=? LIMIT %2)")
                          .arg(QString::fromLatin1(table.name))
                          .arg(table.rows));
        query.addBindValue(sessionId);
        query.addBindValue(generation);
        if (!query.exec()) {
            *error = query.lastError();
            return false;
        }
        const int deleted = query.numRowsAffected();
        query.finish();
        query.prepare(QString("SELECT 1 FROM %1 WHERE session_id=? AND generation=? LIMIT 1")
                          .arg(table.name));
        query.addBindValue(sessionId);
        query.addBindValue(generation);
        if (!query.exec() || query.lastError().isValid()) {
            *error = query.lastError();
            return false;
        }
        if (!query.next())
            continue;
        // A statement that removes nothing while rows remain (for example an ignoring
        // trigger) would otherwise retry forever without progress.
        if (deleted < 1) {
            *error =
                QSqlError(QString(), QStringLiteral("Session history cleanup made no progress."),
                          QSqlError::StatementError);
            return false;
        }
        *remaining = true;
    }
    return true;
}

bool LocalJournal::startHistoryPurge(const QString& sessionId, const qint64 generation,
                                     QString* error) {
    bool remaining = false;
    if (QSqlError failure; !purgeHistoryBatch(sessionId, generation, &remaining, &failure)) {
        if (error != nullptr)
            *error = failure.text();
        return false;
    }
    if (!remaining)
        return true;
    QSqlQuery purge(database_);
    purge.prepare("INSERT INTO session_history_purge(session_id,generation) VALUES(?,?)");
    purge.addBindValue(sessionId);
    purge.addBindValue(generation);
    if (purge.exec() && purge.numRowsAffected() == 1)
        return true;
    if (error != nullptr)
        *error = purge.lastError().isValid()
                     ? purge.lastError().text()
                     : QStringLiteral("The history cleanup work was not stored.");
    return false;
}

LocalJournal::HistoryPurge LocalJournal::purgeDeletedSessionHistory() {
    // Like other maintenance, a pass yields immediately to another writer and
    // commits all of its progress or none of it.
    QSqlQuery timeout(database_);
    if (!timeout.exec("PRAGMA busy_timeout") || !timeout.next()) {
        setError(timeout.lastError().text());
        return HistoryPurge::Failed;
    }
    const int oldTimeout = timeout.value(0).toInt();
    timeout.finish();
    if (!timeout.exec("PRAGMA busy_timeout=0")) {
        setError(timeout.lastError().text());
        return HistoryPurge::Failed;
    }
    timeout.finish();
    bool transaction = false;
    const auto restore = qScopeGuard([&] {
        if (transaction)
            database_.rollback();
        QSqlQuery restoreTimeout(database_);
        restoreTimeout.exec(QString("PRAGMA busy_timeout=%1").arg(oldTimeout));
    });
    const auto fail = [&](const QSqlError& failure) {
        if (isBusy(failure))
            return HistoryPurge::Deferred;
        setError(failure.text());
        return HistoryPurge::Failed;
    };
    if (!database_.transaction())
        return fail(database_.lastError());
    transaction = true;
    QSqlQuery next(database_);
    if (!next.exec("SELECT rowid,session_id,generation FROM session_history_purge "
                   "ORDER BY rowid LIMIT 1"))
        return fail(next.lastError());
    if (!next.next()) {
        if (next.lastError().isValid())
            return fail(next.lastError());
        return HistoryPurge::Idle;
    }
    const qint64 row = next.value(0).toLongLong();
    const QString sessionId = next.value(1).toString();
    const qint64 generation = next.value(2).toLongLong();
    next.finish();
    bool remaining = false;
    QSqlError error;
    if (!purgeHistoryBatch(sessionId, generation, &remaining, &error))
        return fail(error);
    if (!remaining) {
        QSqlQuery done(database_);
        done.prepare("DELETE FROM session_history_purge WHERE rowid=?");
        done.addBindValue(row);
        if (!done.exec())
            return fail(done.lastError());
    }
    if (!database_.commit())
        return fail(database_.lastError());
    transaction = false;
    return HistoryPurge::Progress;
}

bool LocalJournal::hasPendingHistoryPurge() const {
    QSqlQuery query(database_);
    return query.exec("SELECT 1 FROM session_history_purge LIMIT 1") && query.next();
}

} // namespace ketplus::daemon
