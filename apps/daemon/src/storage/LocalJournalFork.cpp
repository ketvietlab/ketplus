#include "storage/LocalJournal.h"

#include "protocol/DaemonProtocol.h"

#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QUuid>

namespace ketplus::daemon {
namespace {

// Tables keyed by (session_id, generation) that make up a session's transcript. Staged
// attachments, client cursors and cloud rows belong to the source session and are not copied.
// Order matters while the copy is staged: rows that reference a large payload are copied before
// the payload, and payloads before their chunks, so orphan GC never sees a staged payload or
// chunk without its parent.
const QStringList& historyTables() {
    static const QStringList tables{QStringLiteral("events"),
                                    QStringLiteral("replay_events"),
                                    QStringLiteral("replay_projection_state"),
                                    QStringLiteral("activity_heads"),
                                    QStringLiteral("activity_items"),
                                    QStringLiteral("activity_summaries"),
                                    QStringLiteral("large_event_payloads"),
                                    QStringLiteral("large_event_payload_chunks")};
    return tables;
}

// One pass copies at most this many rows or bytes, whichever comes first; a single larger row
// (a large payload manifest is capped at 32 MiB) is still copied whole.
constexpr int passRows = 2048;
constexpr qint64 passBytes = 8 * 1024 * 1024;

struct Columns {
    QStringList all;
    QStringList key; // primary-key columns after (session_id, generation), in key order
};

Columns columnsOf(QSqlDatabase database, const QString& table) {
    Columns columns;
    QSqlQuery info(database);
    if (!info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
        return {};
    QMap<int, QString> key;
    while (info.next()) {
        const QString name = info.value(1).toString();
        columns.all.append(name);
        if (const int position = info.value(5).toInt(); position > 0 &&
                                                        name != QStringLiteral("session_id") &&
                                                        name != QStringLiteral("generation"))
            key.insert(position, name);
    }
    columns.key = key.values();
    return columns;
}

// Qt binds a null QByteArray as SQL NULL, so an empty TEXT or BLOB keeps a non-null empty value.
QVariant storedBytes(const QString& type, const QVariant& value) {
    if ((type == QStringLiteral("text") || type == QStringLiteral("blob")) &&
        value.toByteArray().isEmpty())
        return QVariant(QByteArray("", 0));
    return value;
}

// A value read as (typeof, stored bytes) and bound back as type, value, value.
const QString storedValue = QStringLiteral("CASE ? WHEN 'text' THEN CAST(? AS TEXT) ELSE ? END");

bool isBusy(const QSqlError& error) {
    bool ok = false;
    const int code = error.nativeErrorCode().toInt(&ok) & 0xff;
    return ok && (code == 5 || code == 6);
}

qint64 valueBytes(const QVariant& value) {
    return value.typeId() == QMetaType::QByteArray ? value.toByteArray().size() : 8;
}

qint64 countRows(QSqlDatabase database, const QString& table, const QString& sessionId,
                 const qint64 generation, QString* error) {
    QSqlQuery count(database);
    count.prepare(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE session_id = ? AND generation = ?")
                      .arg(table));
    count.addBindValue(sessionId);
    count.addBindValue(generation);
    if (!count.exec() || !count.next()) {
        *error = count.lastError().isValid()
                     ? count.lastError().text()
                     : QStringLiteral("Unable to count the %1 rows.").arg(table);
        return -1;
    }
    return count.value(0).toLongLong();
}

} // namespace

ForkCopy::~ForkCopy() {
    if (readerName.isEmpty())
        return;
    {
        QSqlDatabase reader = QSqlDatabase::database(readerName, false);
        if (reader.isOpen()) {
            reader.rollback();
            reader.close();
        }
    }
    QSqlDatabase::removeDatabase(readerName);
}

std::unique_ptr<ForkCopy> LocalJournal::beginFork(const QString& sourceId,
                                                  const qint64 sourceGeneration,
                                                  StoredSession& fork, QString* error) {
    const auto fail = [this, error](const QString& message) {
        setError(message);
        if (error != nullptr)
            *error = message;
        return nullptr;
    };
    // Provider forking is asynchronous: the destination may have appeared since the caller
    // first checked it. Never stage over another session, staged fork or pending purge.
    if (!database_.transaction())
        return fail(database_.lastError().text());
    auto rollback = qScopeGuard([this] { database_.rollback(); });
    QSqlQuery destination(database_);
    destination.prepare(QStringLiteral(
        "SELECT 1 FROM sessions WHERE id = ? UNION ALL SELECT 1 FROM session_history_purge "
        "WHERE session_id = ? AND generation = ?"));
    destination.addBindValue(fork.id);
    destination.addBindValue(fork.id);
    destination.addBindValue(fork.generation);
    if (!destination.exec())
        return fail(destination.lastError().text());
    if (destination.next())
        return fail(QStringLiteral("The fork destination already exists."));
    if (destination.lastError().isValid())
        return fail(destination.lastError().text());
    QSqlQuery staging(database_);
    staging.prepare(
        QStringLiteral("INSERT INTO session_fork_staging(session_id, generation) VALUES(?, ?)"));
    staging.addBindValue(fork.id);
    staging.addBindValue(fork.generation);
    if (!staging.exec() || staging.numRowsAffected() != 1)
        return fail(staging.lastError().isValid()
                        ? QStringLiteral("The fork destination already exists.")
                        : QStringLiteral("The fork staging was not stored."));
    if (!database_.commit())
        return fail(database_.lastError().text());
    rollback.dismiss();

    auto copy = std::make_unique<ForkCopy>();
    copy->sourceId = sourceId;
    copy->sourceGeneration = sourceGeneration;
    copy->forkId = fork.id;
    copy->forkGeneration = fork.generation;
    const auto abandon = [&](const QString& message) {
        QString ignored;
        discardForkStaging(*copy, true, &ignored);
        return fail(message);
    };
    // A second connection holds one read snapshot for the whole copy, so the fork matches the
    // source at this instant even while the source keeps running or is compacted.
    copy->readerName = QStringLiteral("ketplus-fork-reader-%1")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase reader = QSqlDatabase::cloneDatabase(database_, copy->readerName);
    if (!reader.open())
        return abandon(reader.lastError().text());
    QSqlQuery pragma(reader);
    if (!pragma.exec(QStringLiteral("PRAGMA query_only=ON")) || !reader.transaction())
        return abandon(pragma.lastError().isValid() ? pragma.lastError().text()
                                                    : reader.lastError().text());
    QSqlQuery source(reader);
    source.prepare(QStringLiteral(
        "SELECT CASE WHEN typeof(last_cursor)='integer' THEN last_cursor END, "
        "CASE WHEN typeof(current_turn_sequence)='integer' THEN current_turn_sequence END, "
        "typeof(last_cursor), typeof(current_turn_sequence) FROM sessions "
        "WHERE id = ? AND generation = ?"));
    source.addBindValue(sourceId);
    source.addBindValue(sourceGeneration);
    if (!source.exec())
        return abandon(source.lastError().text());
    if (!source.next())
        return abandon(source.lastError().isValid()
                           ? source.lastError().text()
                           : QStringLiteral("The source session no longer exists."));
    if (source.value(2).toString() != QStringLiteral("integer") ||
        source.value(3).toString() != QStringLiteral("integer") ||
        source.value(0).toLongLong() < 0 || source.value(1).toLongLong() < 0)
        return abandon(QStringLiteral("The source session has unsupported journal counters."));
    copy->lastCursor = source.value(0).toLongLong();
    copy->currentTurnSequence = source.value(1).toLongLong();
    fork.lastCursor = copy->lastCursor;
    fork.currentTurnSequence = copy->currentTurnSequence;
    fork.lastSeenCursor = fork.lastCursor;
    return copy;
}

LocalJournal::ForkStep LocalJournal::continueFork(ForkCopy& copy, QString* error) {
    const auto fail = [this, error](const QString& message) {
        setError(message);
        if (error != nullptr)
            *error = message;
        return ForkStep::Failed;
    };
    if (copy.settled || copy.readerName.isEmpty())
        return fail(QStringLiteral("The fork copy is no longer active."));
    if (copy.table >= historyTables().size())
        return ForkStep::Copied;
    QSqlDatabase reader = QSqlDatabase::database(copy.readerName, false);
    const QString& table = historyTables().at(copy.table);
    const Columns columns = columnsOf(database_, table);
    if (columns.all.isEmpty())
        return fail(QStringLiteral("Unable to read the %1 table.").arg(table));

    // Keyset order over the primary key reads each source row once without an offset scan.
    // TEXT travels as its stored bytes and is cast back, so even malformed UTF-8 is copied
    // exactly as INSERT ... SELECT would have copied it.
    QStringList selected;
    for (const QString& column : columns.all)
        selected.append(QStringLiteral("typeof(%1), CASE WHEN typeof(%1)='text' THEN "
                                       "CAST(%1 AS BLOB) ELSE %1 END")
                            .arg(column));
    QString select = QStringLiteral("SELECT %1 FROM %2 WHERE session_id = ? AND generation = ?")
                         .arg(selected.join(QStringLiteral(", ")), table);
    if (!copy.after.isEmpty())
        select += QStringLiteral(" AND (%1) > (%2)")
                      .arg(columns.key.join(QStringLiteral(", ")),
                           QStringList(columns.key.size(), storedValue).join(QStringLiteral(", ")));
    // Tables keyed by the identity alone hold one row per generation.
    if (!columns.key.isEmpty())
        select += QStringLiteral(" ORDER BY %1").arg(columns.key.join(QStringLiteral(", ")));
    select += QStringLiteral(" LIMIT %1").arg(passRows);
    QSqlQuery rows(reader);
    rows.setForwardOnly(true);
    rows.prepare(select);
    rows.addBindValue(copy.sourceId);
    rows.addBindValue(copy.sourceGeneration);
    for (const QVariant& value : std::as_const(copy.after))
        rows.addBindValue(value);
    if (!rows.exec())
        return fail(rows.lastError().text());

    // Like other maintenance, a pass yields immediately to another writer.
    QSqlQuery timeout(database_);
    if (!timeout.exec(QStringLiteral("PRAGMA busy_timeout")) || !timeout.next())
        return fail(timeout.lastError().text());
    const int oldTimeout = timeout.value(0).toInt();
    timeout.finish();
    if (!timeout.exec(QStringLiteral("PRAGMA busy_timeout=0")))
        return fail(timeout.lastError().text());
    timeout.finish();
    bool transaction = false;
    const auto restore = qScopeGuard([&] {
        if (transaction)
            database_.rollback();
        QSqlQuery restoreTimeout(database_);
        restoreTimeout.exec(QStringLiteral("PRAGMA busy_timeout=%1").arg(oldTimeout));
    });
    const auto writeFailed = [&](const QSqlError& failure) {
        if (isBusy(failure))
            return ForkStep::Deferred;
        return fail(failure.text());
    };
    if (!database_.transaction())
        return writeFailed(database_.lastError());
    transaction = true;
    QStringList values;
    for (const QString& column : columns.all)
        values.append(column == QStringLiteral("session_id") ||
                              column == QStringLiteral("generation")
                          ? QStringLiteral("?")
                          : storedValue);
    QSqlQuery insert(database_);
    insert.prepare(
        QStringLiteral("INSERT INTO %1(%2) VALUES(%3)")
            .arg(table, columns.all.join(QStringLiteral(", ")), values.join(QStringLiteral(", "))));
    int copied = 0;
    qint64 bytes = 0;
    QVariantList after = copy.after;
    while (copied < passRows && bytes < passBytes && rows.next()) {
        QVariantList key;
        for (int column = 0; column < columns.all.size(); ++column) {
            const QString& name = columns.all.at(column);
            const QString type = rows.value(2 * column).toString();
            const QVariant value = storedBytes(type, rows.value(2 * column + 1));
            if (name == QStringLiteral("session_id")) {
                insert.addBindValue(copy.forkId);
            } else if (name == QStringLiteral("generation")) {
                insert.addBindValue(copy.forkGeneration);
            } else {
                insert.addBindValue(type);
                insert.addBindValue(value);
                insert.addBindValue(value);
            }
            bytes += valueBytes(value);
        }
        // The keyset resumes from the stored key bytes, bound the same way as the values.
        for (const QString& name : columns.key) {
            const int column = int(columns.all.indexOf(name));
            const QString type = rows.value(2 * column).toString();
            const QVariant value = storedBytes(type, rows.value(2 * column + 1));
            key << type << value << value;
        }
        if (!insert.exec())
            return writeFailed(insert.lastError());
        if (insert.numRowsAffected() != 1)
            return fail(QStringLiteral("The %1 fork copy is incomplete.").arg(table));
        after = key;
        ++copied;
    }
    if (rows.lastError().isValid())
        return fail(rows.lastError().text());
    const bool tableDone = copied < passRows && bytes < passBytes && !rows.isValid();
    rows.finish();
    if (tableDone) {
        // Every source row of the snapshot must now exist exactly once under the fork.
        QString countError;
        const qint64 expected =
            countRows(reader, table, copy.sourceId, copy.sourceGeneration, &countError);
        const qint64 actual = expected < 0 ? -1
                                           : countRows(database_, table, copy.forkId,
                                                       copy.forkGeneration, &countError);
        if (expected < 0 || actual < 0)
            return fail(countError);
        if (actual != expected)
            return fail(QStringLiteral("The %1 fork copy is incomplete.").arg(table));
    }
    if (!database_.commit())
        return writeFailed(database_.lastError());
    transaction = false;
    copy.copiedRows += copied;
    if (tableDone) {
        ++copy.table;
        copy.after.clear();
    } else {
        copy.after = after;
    }
    return copy.table >= historyTables().size() ? ForkStep::Copied : ForkStep::Progress;
}

bool LocalJournal::finishFork(ForkCopy& copy, StoredSession& fork, QString* error,
                              const std::function<bool(QString*)>& beforeCommit) {
    const auto fail = [this, error](const QString& message) {
        database_.rollback();
        setError(message);
        if (error != nullptr)
            *error = message;
        return false;
    };
    if (copy.settled || copy.table < historyTables().size() || copy.forkId != fork.id)
        return fail(QStringLiteral("The fork copy is incomplete."));
    if (!database_.transaction()) {
        setError(database_.lastError().text());
        if (error != nullptr)
            *error = error_;
        return false;
    }
    QSqlQuery destination(database_);
    destination.prepare(QStringLiteral("SELECT 1 FROM sessions WHERE id = ?"));
    destination.addBindValue(fork.id);
    if (!destination.exec())
        return fail(destination.lastError().text());
    if (destination.next())
        return fail(QStringLiteral("The fork destination already exists."));
    if (destination.lastError().isValid())
        return fail(destination.lastError().text());
    destination.finish();
    QSqlQuery staging(database_);
    staging.prepare(
        QStringLiteral("DELETE FROM session_fork_staging WHERE session_id = ? AND generation = ?"));
    staging.addBindValue(copy.forkId);
    staging.addBindValue(copy.forkGeneration);
    if (!staging.exec() || staging.numRowsAffected() != 1)
        return fail(staging.lastError().isValid()
                        ? staging.lastError().text()
                        : QStringLiteral("The fork staging was not found."));
    fork.lastCursor = copy.lastCursor;
    fork.currentTurnSequence = copy.currentTurnSequence;
    fork.lastSeenCursor = fork.lastCursor;
    if (!upsertSession(fork))
        return fail(error_);
    // A fork is listed first, as a conversation that has just started a turn would be.
    fork.lastTurnStartedAt = protocol::timestamp();
    QSqlQuery started(database_);
    started.prepare(QStringLiteral("UPDATE sessions SET last_turn_started_at = ? "
                                   "WHERE id = ? AND generation = ?"));
    started.addBindValue(fork.lastTurnStartedAt);
    started.addBindValue(fork.id);
    started.addBindValue(fork.generation);
    if (!started.exec() || started.numRowsAffected() != 1)
        return fail(started.lastError().isValid()
                        ? started.lastError().text()
                        : QStringLiteral("The fork timestamp was not stored."));
    QString preparationError;
    if (beforeCommit && !beforeCommit(&preparationError))
        return fail(preparationError.isEmpty()
                        ? QStringLiteral("The fork ownership could not be prepared.")
                        : preparationError);
    if (!database_.commit())
        return fail(database_.lastError().text());
    copy.settled = true;
    return true;
}

bool LocalJournal::abandonFork(ForkCopy& copy, QString* error) {
    return discardForkStaging(copy, false, error);
}

bool LocalJournal::discardForkStaging(ForkCopy& copy, const bool immediately, QString* error) {
    if (copy.settled)
        return true;
    const auto fail = [this, error](const QString& message) {
        database_.rollback();
        setError(message);
        if (error != nullptr)
            *error = message;
        return false;
    };
    // Releasing the staging row and queuing its rows for purge is one transaction, so a crash
    // leaves either a staged copy (recovered at open) or purge work, never unowned rows.
    if (!database_.transaction())
        return fail(database_.lastError().text());
    QSqlQuery staging(database_);
    staging.prepare(
        QStringLiteral("DELETE FROM session_fork_staging WHERE session_id = ? AND generation = ?"));
    staging.addBindValue(copy.forkId);
    staging.addBindValue(copy.forkGeneration);
    if (!staging.exec() || staging.numRowsAffected() != 1)
        return fail(staging.lastError().isValid()
                        ? staging.lastError().text()
                        : QStringLiteral("The fork staging was not released."));
    QSqlQuery purge(database_);
    purge.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO session_history_purge(session_id, generation) VALUES(?, ?)"));
    purge.addBindValue(copy.forkId);
    purge.addBindValue(copy.forkGeneration);
    if (!purge.exec())
        return fail(purge.lastError().text());
    purge.finish();
    // An existing marker is fine, an ignored insert is not: check the result, not the count.
    // Without both, the staging row keeps owning the copy and the transaction rolls back.
    QSqlQuery owned(database_);
    owned.prepare(
        QStringLiteral("SELECT EXISTS(SELECT 1 FROM session_history_purge WHERE session_id = ? AND "
                       "generation = ?) AND NOT EXISTS(SELECT 1 FROM session_fork_staging WHERE "
                       "session_id = ? AND generation = ?)"));
    owned.addBindValue(copy.forkId);
    owned.addBindValue(copy.forkGeneration);
    owned.addBindValue(copy.forkId);
    owned.addBindValue(copy.forkGeneration);
    if (!owned.exec() || !owned.next() || !owned.value(0).toBool())
        return fail(owned.lastError().isValid()
                        ? owned.lastError().text()
                        : QStringLiteral("The fork history cleanup work was not stored."));
    owned.finish();
    if (!database_.commit())
        return fail(database_.lastError().text());
    copy.settled = true;
    if (!immediately)
        return true;
    // The synchronous API promises no partial state: drain this identity now.
    for (;;) {
        if (!database_.transaction())
            return fail(database_.lastError().text());
        bool remaining = false;
        QSqlError failure;
        if (!purgeHistoryBatch(copy.forkId, copy.forkGeneration, &remaining, &failure))
            return fail(failure.text());
        if (!remaining) {
            QSqlQuery done(database_);
            done.prepare(QStringLiteral(
                "DELETE FROM session_history_purge WHERE session_id = ? AND generation = ?"));
            done.addBindValue(copy.forkId);
            done.addBindValue(copy.forkGeneration);
            if (!done.exec() || done.numRowsAffected() != 1)
                return fail(done.lastError().isValid()
                                ? done.lastError().text()
                                : QStringLiteral("The history cleanup marker was not removed."));
        }
        if (!database_.commit())
            return fail(database_.lastError().text());
        if (!remaining)
            return true;
    }
}

bool LocalJournal::recoverForkStaging() {
    // No copy survives a restart: hand every staged identity to the history purge. If the
    // markers cannot all be stored, the staging rows stay and keep owning their rows.
    if (!database_.transaction()) {
        setError(database_.lastError().text());
        return false;
    }
    auto rollback = qScopeGuard([this] { database_.rollback(); });
    QSqlQuery query(database_);
    if (!query.exec(
            QStringLiteral("INSERT OR IGNORE INTO session_history_purge(session_id, generation) "
                           "SELECT session_id, generation FROM session_fork_staging"))) {
        setError(query.lastError().text());
        return false;
    }
    if (!query.exec(QStringLiteral(
            "SELECT EXISTS(SELECT 1 FROM session_fork_staging s WHERE NOT EXISTS (SELECT 1 FROM "
            "session_history_purge p WHERE p.session_id = s.session_id AND "
            "p.generation = s.generation))")) ||
        !query.next()) {
        setError(query.lastError().text());
        return false;
    }
    if (query.value(0).toBool()) {
        qWarning("journal: interrupted fork copies kept staged; their cleanup work was not "
                 "stored");
        return true;
    }
    query.finish();
    if (!query.exec(QStringLiteral("DELETE FROM session_fork_staging")) ||
        !query.exec(QStringLiteral("SELECT EXISTS(SELECT 1 FROM session_fork_staging)")) ||
        !query.next()) {
        setError(query.lastError().text());
        return false;
    }
    if (query.value(0).toBool()) {
        qWarning("journal: interrupted fork copies kept staged; staging was not released");
        return true;
    }
    query.finish();
    if (!database_.commit()) {
        setError(database_.lastError().text());
        return false;
    }
    rollback.dismiss();
    return true;
}

bool LocalJournal::forkSession(const QString& sourceId, const qint64 sourceGeneration,
                               StoredSession& fork, QString* error,
                               const std::function<bool(QString*)>& beforeCommit) {
    auto copy = beginFork(sourceId, sourceGeneration, fork, error);
    if (!copy)
        return false;
    QString failure;
    for (;;) {
        const ForkStep step = continueFork(*copy, &failure);
        if (step == ForkStep::Copied && finishFork(*copy, fork, &failure, beforeCommit))
            return true;
        if (step == ForkStep::Progress)
            continue;
        break; // Deferred is a failure for a synchronous caller.
    }
    if (failure.isEmpty())
        failure = QStringLiteral("The fork copy was deferred by another writer.");
    QString ignored;
    discardForkStaging(*copy, true, &ignored);
    setError(failure);
    if (error != nullptr)
        *error = failure;
    return false;
}

} // namespace ketplus::daemon
