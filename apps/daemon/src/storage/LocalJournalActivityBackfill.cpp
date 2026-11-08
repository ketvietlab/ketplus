#include "storage/LocalJournal.h"

#include <QJsonDocument>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {
namespace {
// Replay rows indexed per background pass; each row costs a few indexed statements.
constexpr int passRows = 2048;
// Payload bytes held per pass; the row that crosses it is the last one read.
constexpr qsizetype passBytes = 1024 * 1024;

bool isBusy(const QSqlError& error) {
    bool ok = false;
    const int code = error.nativeErrorCode().toInt(&ok) & 0xff;
    return ok && (code == 5 || code == 6);
}

bool removeMarker(QSqlDatabase& database, const QString& session, const qint64 generation,
                  QSqlError* error) {
    QSqlQuery marker(database);
    marker.prepare(
        QStringLiteral("DELETE FROM activity_backfill WHERE session_id=? AND generation=?"));
    marker.addBindValue(session);
    marker.addBindValue(generation);
    if (!marker.exec()) {
        *error = marker.lastError();
        return false;
    }
    if (marker.numRowsAffected() != 1) {
        *error = QSqlError(QString(), QStringLiteral("Activity backfill marker was not removed."),
                           QSqlError::StatementError);
        return false;
    }
    return true;
}
} // namespace

bool LocalJournal::catchUpActivity(const QString& session, const qint64 generation, bool* indexed,
                                   QSqlError* error) {
    *indexed = false;
    activityCatchUp_ = true;
    const auto reset = qScopeGuard([this] { activityCatchUp_ = false; });
    QSqlQuery rows(database_);
    rows.prepare(QStringLiteral(
        "SELECT cursor,turn_sequence,event_type,payload FROM replay_events WHERE session_id=? "
        "AND generation=? AND cursor>COALESCE((SELECT indexed_cursor FROM activity_heads WHERE "
        "session_id=? AND generation=?),0) ORDER BY cursor LIMIT ?"));
    rows.addBindValue(session);
    rows.addBindValue(generation);
    rows.addBindValue(session);
    rows.addBindValue(generation);
    rows.addBindValue(passRows);
    if (!rows.exec()) {
        *error = rows.lastError();
        return false;
    }
    // Materialize the page: indexing rewrites the head row the query reads.
    struct Row {
        qint64 cursor;
        qint64 turn;
        QString type;
        QByteArray payload;
    };
    QList<Row> page;
    qsizetype bytes = 0;
    bool exhausted = true;
    while (bytes < passBytes && page.size() < passRows) {
        if (!rows.next()) {
            if (rows.lastError().isValid()) {
                *error = rows.lastError();
                return false;
            }
            break;
        }
        page.append({rows.value(0).toLongLong(), rows.value(1).toLongLong(),
                     rows.value(2).toString(), rows.value(3).toByteArray()});
        bytes += page.constLast().payload.size();
    }
    if (page.size() == passRows || bytes >= passBytes)
        exhausted = false;
    rows.finish();
    for (const auto& row : std::as_const(page)) {
        QString failure;
        if (!updateActivityGroups(session, generation, row.cursor, row.turn, row.type,
                                  QJsonDocument::fromJson(row.payload).object(), &failure)) {
            *error = QSqlError(QString(), failure, QSqlError::StatementError);
            return false;
        }
    }
    *indexed = !page.isEmpty();
    if (!exhausted)
        return true;
    // Raw-only tail events leave no replay row; the index is complete through last_cursor.
    QSqlQuery settle(database_);
    settle.prepare(QStringLiteral(
        "INSERT INTO activity_heads SELECT id,generation,current_turn_sequence,0,last_cursor "
        "FROM sessions WHERE id=? AND generation=? ON CONFLICT(session_id,generation) DO UPDATE "
        "SET indexed_cursor=MAX(indexed_cursor,excluded.indexed_cursor)"));
    settle.addBindValue(session);
    settle.addBindValue(generation);
    if (!settle.exec()) {
        *error = settle.lastError();
        return false;
    }
    settle.finish();
    return removeMarker(database_, session, generation, error);
}

bool LocalJournal::activityBackfillPending(const QString& session, const qint64 generation,
                                           bool* pending, QString* error) const {
    QSqlQuery marker(database_);
    marker.prepare(QStringLiteral(
        "SELECT 1 FROM activity_backfill WHERE session_id=? AND generation=? LIMIT 1"));
    marker.addBindValue(session);
    marker.addBindValue(generation);
    if (!marker.exec()) {
        if (error)
            *error = marker.lastError().text();
        return false;
    }
    *pending = marker.next();
    return true;
}

bool LocalJournal::deferActivityToBackfill(const QString& session, const qint64 generation,
                                           bool* deferred, QString* error) {
    if (!activityBackfillPending(session, generation, deferred, error))
        return false;
    if (!*deferred)
        return true;
    // Live events of a session still being indexed wait their turn in cursor order, and the
    // session is indexed before ones nobody is using (ADR 0097).
    QSqlQuery wanted(database_);
    wanted.prepare(QStringLiteral(
        "UPDATE activity_backfill SET wanted=1 WHERE session_id=? AND generation=? AND wanted=0"));
    wanted.addBindValue(session);
    wanted.addBindValue(generation);
    if (!wanted.exec()) {
        if (error)
            *error = wanted.lastError().text();
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
        QSqlQuery cleanup(database_);
        if (transaction)
            cleanup.exec(QStringLiteral("ROLLBACK"));
        cleanup.exec(QStringLiteral("PRAGMA busy_timeout=%1").arg(oldTimeout));
    });
    const auto fail = [&](const QSqlError& failure) {
        if (isBusy(failure))
            return ActivityBackfill::Deferred;
        setError(failure.text());
        return ActivityBackfill::Failed;
    };
    // Take the write lock first: a busy writer then defers here, with SQLite's code intact.
    QSqlQuery begin(database_);
    if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE")))
        return fail(begin.lastError());
    transaction = true;
    QSqlQuery next(database_);
    if (!next.exec(QStringLiteral(
            "SELECT b.session_id,b.generation,s.id IS NOT NULL FROM activity_backfill b LEFT JOIN "
            "sessions s ON s.id=b.session_id AND s.generation=b.generation ORDER BY b.wanted "
            "DESC,b.rowid LIMIT 1")))
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
    QSqlError failure;
    if (live) {
        bool indexed = false;
        if (!catchUpActivity(session, generation, &indexed, &failure))
            return fail(failure);
        if (indexed) {
            // Membership changed: snapshots taken before this pass must reload.
            QSqlQuery revision(database_);
            revision.prepare(QStringLiteral("UPDATE replay_projection_state SET "
                                            "revision=revision+1 WHERE session_id=? AND "
                                            "generation=?"));
            revision.addBindValue(session);
            revision.addBindValue(generation);
            if (!revision.exec())
                return fail(revision.lastError());
        }
    } else if (!removeMarker(database_, session, generation, &failure)) {
        return fail(failure);
    }
    QSqlQuery commit(database_);
    if (!commit.exec(QStringLiteral("COMMIT")))
        return fail(commit.lastError());
    transaction = false;
    return ActivityBackfill::Progress;
}

bool LocalJournal::hasPendingActivityBackfill() const {
    QSqlQuery query(database_);
    return query.exec(QStringLiteral("SELECT 1 FROM activity_backfill LIMIT 1")) && query.next();
}

} // namespace ketplus::daemon
