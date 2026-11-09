#include "storage/LocalJournal.h"

#include "protocol/DaemonProtocol.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

namespace ketplus::daemon {
namespace {

// Tables keyed by (session_id, generation) that make up a session's transcript. Staged
// attachments, client cursors and cloud rows belong to the source session and are not copied.
const QStringList& historyTables() {
    static const QStringList tables{QStringLiteral("activity_summaries"),
                                    QStringLiteral("activity_items"),
                                    QStringLiteral("activity_heads"),
                                    QStringLiteral("activity_files"),
                                    QStringLiteral("events"),
                                    QStringLiteral("replay_events"),
                                    QStringLiteral("replay_projection_state"),
                                    QStringLiteral("large_event_payloads"),
                                    QStringLiteral("large_event_payload_chunks")};
    return tables;
}

QStringList columnsOf(QSqlDatabase database, const QString& table) {
    QStringList columns;
    QSqlQuery info(database);
    if (!info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
        return columns;
    while (info.next())
        columns.append(info.value(1).toString());
    return columns;
}

} // namespace

bool LocalJournal::forkSession(const QString& sourceId, const qint64 sourceGeneration,
                               StoredSession& fork, QString* error) {
    const auto fail = [this, error](const QString& message) {
        database_.rollback();
        setError(message);
        if (error != nullptr)
            *error = message;
        return false;
    };
    if (!database_.transaction()) {
        setError(database_.lastError().text());
        if (error != nullptr)
            *error = error_;
        return false;
    }
    // Provider forking is asynchronous: the destination may have appeared since
    // the caller first checked it. Never let the upsert replace another session.
    QSqlQuery destination(database_);
    destination.prepare(QStringLiteral("SELECT 1 FROM sessions WHERE id = ?"));
    destination.addBindValue(fork.id);
    if (!destination.exec())
        return fail(destination.lastError().text());
    if (destination.next())
        return fail(QStringLiteral("The fork destination already exists."));
    if (destination.lastError().isValid())
        return fail(destination.lastError().text());
    QSqlQuery source(database_);
    source.prepare(QStringLiteral(
        "SELECT CASE WHEN typeof(last_cursor)='integer' THEN last_cursor END, "
        "CASE WHEN typeof(current_turn_sequence)='integer' THEN current_turn_sequence END, "
        "typeof(last_cursor), typeof(current_turn_sequence) FROM sessions "
        "WHERE id = ? AND generation = ?"));
    source.addBindValue(sourceId);
    source.addBindValue(sourceGeneration);
    if (!source.exec())
        return fail(source.lastError().text());
    if (!source.next())
        return fail(source.lastError().isValid()
                        ? source.lastError().text()
                        : QStringLiteral("The source session no longer exists."));
    if (source.value(2).toString() != QStringLiteral("integer") ||
        source.value(3).toString() != QStringLiteral("integer") ||
        source.value(0).toLongLong() < 0 || source.value(1).toLongLong() < 0)
        return fail(QStringLiteral("The source session has unsupported journal counters."));
    fork.lastCursor = source.value(0).toLongLong();
    fork.currentTurnSequence = source.value(1).toLongLong();
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

    // Cursors are kept as they are, so the copy replays exactly like its source.
    for (const QString& table : historyTables()) {
        const QStringList columns = columnsOf(database_, table);
        if (columns.isEmpty())
            return fail(QStringLiteral("Unable to read the %1 table.").arg(table));
        QSqlQuery count(database_);
        count.prepare(
            QStringLiteral("SELECT COUNT(*) FROM %1 WHERE session_id = ? AND generation = ?")
                .arg(table));
        count.addBindValue(sourceId);
        count.addBindValue(sourceGeneration);
        if (!count.exec() || !count.next())
            return fail(count.lastError().isValid()
                            ? count.lastError().text()
                            : QStringLiteral("Unable to count the %1 source rows.").arg(table));
        const qint64 expectedRows = count.value(0).toLongLong();
        QStringList selected;
        for (const QString& column : columns)
            selected.append(column == QStringLiteral("session_id")   ? QStringLiteral("?")
                            : column == QStringLiteral("generation") ? QStringLiteral("?")
                                                                     : column);
        QSqlQuery copy(database_);
        copy.prepare(QStringLiteral("INSERT INTO %1(%2) SELECT %3 FROM %1 "
                                    "WHERE session_id = ? AND generation = ?")
                         .arg(table, columns.join(QStringLiteral(", ")),
                              selected.join(QStringLiteral(", "))));
        for (const QString& column : columns) {
            if (column == QStringLiteral("session_id"))
                copy.addBindValue(fork.id);
            else if (column == QStringLiteral("generation"))
                copy.addBindValue(fork.generation);
        }
        copy.addBindValue(sourceId);
        copy.addBindValue(sourceGeneration);
        if (!copy.exec() || copy.numRowsAffected() != expectedRows)
            return fail(copy.lastError().isValid()
                            ? copy.lastError().text()
                            : QStringLiteral("The %1 fork copy is incomplete.").arg(table));
    }
    if (!database_.commit())
        return fail(database_.lastError().text());
    return true;
}

} // namespace ketplus::daemon
