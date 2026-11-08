#include "storage/LocalJournal.h"

#include <QJsonDocument>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <limits>

namespace ketplus::daemon {
namespace {
// Replay rows indexed per background pass; each row costs a few indexed statements.
constexpr int passRows = 2048;

bool isBusy(const QSqlError& error) {
    bool ok = false;
    const int code = error.nativeErrorCode().toInt(&ok) & 0xff;
    return ok && (code == 5 || code == 6);
}
} // namespace

bool LocalJournal::catchUpActivity(const QString& session, const qint64 generation,
                                   const qint64 beforeCursor, const int rowLimit, bool* done,
                                   QSqlError* error) {
    *done = false;
    activityCatchUp_ = true;
    const auto reset = qScopeGuard([this] { activityCatchUp_ = false; });
    QSqlQuery rows(database_);
    rows.prepare(QStringLiteral(
        "SELECT cursor,turn_sequence,event_type,payload FROM replay_events WHERE session_id=? "
        "AND generation=? AND cursor>COALESCE((SELECT indexed_cursor FROM activity_heads WHERE "
        "session_id=? AND generation=?),0) AND cursor<? ORDER BY cursor LIMIT ?"));
    rows.addBindValue(session);
    rows.addBindValue(generation);
    rows.addBindValue(session);
    rows.addBindValue(generation);
    rows.addBindValue(beforeCursor);
    rows.addBindValue(rowLimit);
    if (!rows.exec()) {
        *error = rows.lastError();
        return false;
    }
    // Materialize the page: indexing rewrites the head row the query reads.
    QList<QVariantList> page;
    while (rows.next())
        page.append({rows.value(0), rows.value(1), rows.value(2), rows.value(3)});
    if (rows.lastError().isValid()) {
        *error = rows.lastError();
        return false;
    }
    rows.finish();
    for (const auto& row : std::as_const(page)) {
        QString failure;
        if (!updateActivityGroups(session, generation, row.at(0).toLongLong(),
                                  row.at(1).toLongLong(), row.at(2).toString(),
                                  QJsonDocument::fromJson(row.at(3).toString().toUtf8()).object(),
                                  &failure)) {
            *error = QSqlError(QString(), failure, QSqlError::StatementError);
            return false;
        }
    }
    if (page.size() >= rowLimit)
        return true;
    // Raw-only tail events leave no replay row; the index is complete up to the boundary.
    QSqlQuery settle(database_);
    settle.prepare(QStringLiteral(
        "INSERT INTO activity_heads SELECT "
        "id,generation,current_turn_sequence,0,MIN(last_cursor,?) "
        "FROM sessions WHERE id=? AND generation=? ON CONFLICT(session_id,generation) DO UPDATE "
        "SET indexed_cursor=MAX(indexed_cursor,excluded.indexed_cursor)"));
    settle.addBindValue(beforeCursor - 1);
    settle.addBindValue(session);
    settle.addBindValue(generation);
    if (!settle.exec()) {
        *error = settle.lastError();
        return false;
    }
    settle.finish();
    QSqlQuery marker(database_);
    marker.prepare(
        QStringLiteral("DELETE FROM activity_backfill WHERE session_id=? AND generation=?"));
    marker.addBindValue(session);
    marker.addBindValue(generation);
    if (!marker.exec()) {
        *error = marker.lastError();
        return false;
    }
    *done = true;
    return true;
}

bool LocalJournal::catchUpActivityBeforeEvent(const QString& session, const qint64 generation,
                                              const qint64 cursor, QString* error) {
    QSqlQuery pending(database_);
    pending.prepare(QStringLiteral(
        "SELECT 1 FROM activity_backfill WHERE session_id=? AND generation=? LIMIT 1"));
    pending.addBindValue(session);
    pending.addBindValue(generation);
    if (!pending.exec()) {
        *error = pending.lastError().text();
        return false;
    }
    if (!pending.next())
        return true;
    pending.finish();
    // The event's own transaction indexes the rest of this one session's history first.
    bool done = false;
    QSqlError failure;
    if (!catchUpActivity(session, generation, cursor, std::numeric_limits<int>::max(), &done,
                         &failure)) {
        *error = failure.text();
        return false;
    }
    return true;
}

LocalJournal::ActivityBackfill LocalJournal::backfillActivityGroups() {
    QSqlQuery timeout(database_);
    if (!timeout.exec(QStringLiteral("PRAGMA busy_timeout")) || !timeout.next()) {
        setError(timeout.lastError().text());
        return ActivityBackfill::Failed;
    }
    const int oldTimeout = timeout.value(0).toInt();
    timeout.finish();
    if (!timeout.exec(QStringLiteral("PRAGMA busy_timeout=0"))) {
        setError(timeout.lastError().text());
        return ActivityBackfill::Failed;
    }
    timeout.finish();
    bool transaction = false;
    const auto restore = qScopeGuard([&] {
        if (transaction)
            database_.rollback();
        QSqlQuery restoreTimeout(database_);
        restoreTimeout.exec(QStringLiteral("PRAGMA busy_timeout=%1").arg(oldTimeout));
    });
    const auto fail = [&](const QSqlError& failure) {
        if (isBusy(failure))
            return ActivityBackfill::Deferred;
        setError(failure.text());
        return ActivityBackfill::Failed;
    };
    if (!database_.transaction())
        return fail(database_.lastError());
    transaction = true;
    QSqlQuery next(database_);
    if (!next.exec(QStringLiteral(
            "SELECT b.session_id,b.generation,s.id IS NOT NULL FROM activity_backfill b LEFT JOIN "
            "sessions s ON s.id=b.session_id AND s.generation=b.generation ORDER BY b.rowid "
            "LIMIT 1")))
        return fail(next.lastError());
    if (!next.next()) {
        if (next.lastError().isValid())
            return fail(next.lastError());
        return ActivityBackfill::Idle;
    }
    const QString session = next.value(0).toString();
    const qint64 generation = next.value(1).toLongLong();
    const bool live = next.value(2).toBool();
    next.finish();
    if (live) {
        bool done = false;
        QSqlError failure;
        if (!catchUpActivity(session, generation, std::numeric_limits<qint64>::max(), passRows,
                             &done, &failure))
            return fail(failure);
    } else {
        QSqlQuery stale(database_);
        stale.prepare(
            QStringLiteral("DELETE FROM activity_backfill WHERE session_id=? AND generation=?"));
        stale.addBindValue(session);
        stale.addBindValue(generation);
        if (!stale.exec())
            return fail(stale.lastError());
    }
    if (!database_.commit())
        return fail(database_.lastError());
    transaction = false;
    return ActivityBackfill::Progress;
}

bool LocalJournal::hasPendingActivityBackfill() const {
    QSqlQuery query(database_);
    return query.exec(QStringLiteral("SELECT 1 FROM activity_backfill LIMIT 1")) && query.next();
}

} // namespace ketplus::daemon
