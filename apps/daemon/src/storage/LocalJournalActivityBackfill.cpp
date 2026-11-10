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
// Payload bytes held per pass; the row that crosses it is the last one read.
constexpr qsizetype passBytes = 1024 * 1024;

bool isBusy(const QSqlError& error) {
    bool ok = false;
    const int code = error.nativeErrorCode().toInt(&ok) & 0xff;
    return ok && (code == 5 || code == 6);
}

bool removeMarker(QSqlDatabase& database, const QString& session, const qint64 generation,
                  QSqlError* error, const QString& table = QStringLiteral("activity_backfill")) {
    QSqlQuery marker(database);
    marker.prepare(QStringLiteral("DELETE FROM %1 WHERE session_id=? AND generation=?").arg(table));
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

bool LocalJournal::listActivityFiles(const QString& session, const qint64 generation, bool* listed,
                                     QSqlError* error) {
    *listed = false;
    QSqlQuery position(database_);
    position.prepare(QStringLiteral("SELECT after_group,after_cursor FROM activity_files_backfill "
                                    "WHERE session_id=? AND generation=?"));
    position.addBindValue(session);
    position.addBindValue(generation);
    if (!position.exec() || !position.next()) {
        *error = position.lastError();
        return false;
    }
    const qint64 afterGroup = position.value(0).toLongLong();
    const qint64 afterCursor = position.value(1).toLongLong();
    position.finish();
    // Keys first, in index order: a pass reads at most passRows items whatever their family.
    // The row value seeks activity_items_group to the marker, so a late pass does not walk the
    // items before it. Ungrouped items have group 0; the first pass starts after all of them.
    QSqlQuery keys(database_);
    keys.prepare(QStringLiteral(
        "SELECT group_cursor,first_cursor,item_id,family,last_cursor FROM activity_items "
        "WHERE session_id=? AND generation=? AND (group_cursor,first_cursor)>(?,?) ORDER BY "
        "group_cursor,first_cursor LIMIT ?"));
    const qint64 seekCursor = afterGroup > 0 ? afterCursor : std::numeric_limits<qint64>::max();
    for (const QVariant& value :
         QVariantList{session, generation, afterGroup, seekCursor, passRows})
        keys.addBindValue(value);
    if (!keys.exec()) {
        *error = keys.lastError();
        return false;
    }
    struct Edit {
        qint64 group;
        qint64 first;
        QString item;
        qint64 last;
    };
    QList<Edit> edits;
    int scanned = 0;
    qint64 lastGroup = afterGroup, lastFirst = afterCursor;
    while (keys.next()) {
        ++scanned;
        lastGroup = keys.value(0).toLongLong();
        lastFirst = keys.value(1).toLongLong();
        if (keys.value(3).toString() == QLatin1String("edit"))
            edits.append(
                {lastGroup, lastFirst, keys.value(2).toString(), keys.value(4).toLongLong()});
    }
    if (keys.lastError().isValid()) {
        *error = keys.lastError();
        return false;
    }
    keys.finish();
    // Each edit's current completion, read in this pass's write transaction, so a live
    // completion journaled meanwhile is not overwritten by an older read.
    qsizetype bytes = 0;
    bool exhausted = scanned < passRows;
    for (qsizetype index = 0; index < edits.size(); ++index) {
        const auto& edit = edits.at(index);
        QSqlQuery completion(database_);
        completion.prepare(QStringLiteral(
            "SELECT payload FROM replay_events WHERE session_id=? AND generation=? AND cursor=? "
            "AND event_type='item.completed'"));
        completion.addBindValue(session);
        completion.addBindValue(generation);
        completion.addBindValue(edit.last);
        if (!completion.exec()) {
            *error = completion.lastError();
            return false;
        }
        if (!completion.next())
            continue;
        const QByteArray payload = completion.value(0).toByteArray();
        completion.finish();
        QString failure;
        if (!recordActivityFiles(session, generation, edit.group, edit.item, edit.first,
                                 QJsonDocument::fromJson(payload).object().value("item").toObject(),
                                 &failure)) {
            *error = QSqlError(QString(), failure, QSqlError::StatementError);
            return false;
        }
        *listed = true;
        bytes += payload.size();
        // The edit that crosses the byte budget is the last one this pass lists.
        if (bytes >= passBytes && index + 1 < edits.size()) {
            lastGroup = edit.group;
            lastFirst = edit.first;
            exhausted = false;
            break;
        }
    }
    if (exhausted)
        return removeMarker(database_, session, generation, error,
                            QStringLiteral("activity_files_backfill"));
    QSqlQuery advance(database_);
    advance.prepare(QStringLiteral("UPDATE activity_files_backfill SET after_group=?, "
                                   "after_cursor=? WHERE session_id=? AND generation=?"));
    for (const QVariant& value : QVariantList{lastGroup, lastFirst, session, generation})
        advance.addBindValue(value);
    if (!advance.exec()) {
        *error = advance.lastError();
        return false;
    }
    return true;
}

bool LocalJournal::activityProjectionPending(const QString& session, const qint64 generation,
                                             bool* pending) const {
    QSqlQuery marker(database_);
    marker.prepare(QStringLiteral(
        "SELECT EXISTS(SELECT 1 FROM activity_backfill WHERE session_id=? AND generation=?) OR "
        "EXISTS(SELECT 1 FROM activity_files_backfill WHERE session_id=? AND generation=?)"));
    for (const QVariant& value : QVariantList{session, generation, session, generation})
        marker.addBindValue(value);
    if (!marker.exec() || !marker.next())
        return false;
    *pending = marker.value(0).toBool();
    return true;
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
        next.finish();
        // The index is drained; list the files of groups indexed before activity_files.
        QSqlQuery files(database_);
        if (!files.exec(QStringLiteral(
                "SELECT f.session_id,f.generation,s.id IS NOT NULL FROM activity_files_backfill f "
                "LEFT JOIN sessions s ON s.id=f.session_id AND s.generation=f.generation ORDER BY "
                "f.rowid LIMIT 1")))
            return fail(files.lastError());
        if (!files.next()) {
            if (files.lastError().isValid())
                return fail(files.lastError());
            return ActivityBackfill::Idle;
        }
        const QString session = files.value(0).toString();
        const qint64 generation = files.value(1).toLongLong();
        const bool live = files.value(2).toBool();
        files.finish();
        QSqlError failure;
        if (live) {
            bool listed = false;
            if (!listActivityFiles(session, generation, &listed, &failure))
                return fail(failure);
            bool pending = true;
            if (!activityProjectionPending(session, generation, &pending))
                return fail(QSqlError(QString(), QStringLiteral("Activity files state unreadable."),
                                      QSqlError::StatementError));
            // New files, or the listing finished and groups become visible: snapshots reload.
            if (listed || !pending) {
                QSqlQuery revision(database_);
                revision.prepare(QStringLiteral("UPDATE replay_projection_state SET "
                                                "revision=revision+1 WHERE session_id=? AND "
                                                "generation=?"));
                revision.addBindValue(session);
                revision.addBindValue(generation);
                if (!revision.exec())
                    return fail(revision.lastError());
            }
        } else if (!removeMarker(database_, session, generation, &failure,
                                 QStringLiteral("activity_files_backfill"))) {
            return fail(failure);
        }
        QSqlQuery commit(database_);
        if (!commit.exec(QStringLiteral("COMMIT")))
            return fail(commit.lastError());
        transaction = false;
        return ActivityBackfill::Progress;
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
    return query.exec(QStringLiteral("SELECT 1 FROM activity_backfill UNION ALL SELECT 1 FROM "
                                     "activity_files_backfill LIMIT 1")) &&
           query.next();
}

} // namespace ketplus::daemon
