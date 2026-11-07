#include "protocol/DaemonProtocol.h"
#include "storage/LocalJournal.h"

#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <mutex>

namespace ketplus::daemon {

namespace detail {
QString acknowledgedEventFenceQuery() {
    return QStringLiteral(
        "SELECT min(cursor) FROM client_cursors WHERE session_id=? AND generation=?");
}
QString acknowledgedEventCandidatesQuery() {
    return QStringLiteral(
        "SELECT cursor,CASE WHEN typeof(payload)='text' THEN octet_length(payload) ELSE -1 END "
        "FROM events WHERE session_id=? AND generation=? AND cursor>? AND cursor<=? ORDER BY "
        "cursor LIMIT ?");
}
} // namespace detail

bool LocalJournal::acknowledgeCursor(const QString& clientId, const QString& sessionId,
                                     const qint64 generation, const qint64 cursor) {
    QSqlQuery current(database_);
    current.prepare(
        "SELECT cursor FROM client_cursors WHERE client_id=? AND session_id=? AND generation=?");
    current.addBindValue(clientId);
    current.addBindValue(sessionId);
    current.addBindValue(generation);
    if (!current.exec()) {
        setError(current.lastError().text());
        return false;
    }
    if (current.next() && current.value(0).toLongLong() >= cursor)
        return true; // repeated/older ACKs remain write-free
    current.finish();
    if (!database_.transaction()) {
        setError(database_.lastError().text());
        return false;
    }
    auto rollback = qScopeGuard([&] { database_.rollback(); });
    QSqlQuery query(database_);
    query.prepare(
        "INSERT INTO client_cursors(client_id,session_id,generation,cursor,acknowledged_at) "
        "VALUES(?,?,?,?,?) ON CONFLICT(client_id,session_id,generation) DO UPDATE SET "
        "cursor=MAX(cursor,excluded.cursor),acknowledged_at=excluded.acknowledged_at");
    query.addBindValue(clientId);
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(cursor);
    query.addBindValue(protocol::timestamp());
    if (!query.exec() || query.numRowsAffected() != 1) {
        setError(query.lastError().isValid() ? query.lastError().text()
                                             : QString("ACK cursor was not stored."));
        return false;
    }
    query.prepare("INSERT INTO journal_ack_gc(session_id,generation) VALUES(?,?) "
                  "ON CONFLICT(session_id,generation) DO NOTHING");
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    query.prepare("SELECT 1 FROM journal_ack_gc WHERE session_id=? AND generation=?");
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec() || !query.next()) {
        setError(query.lastError().isValid() ? query.lastError().text()
                                             : QString("ACK cleanup was not queued."));
        return false;
    }
    query.finish();
    if (!database_.commit()) {
        setError(database_.lastError().text());
        return false;
    }
    rollback.dismiss();
    // Acceptance describes the durable cursor, not physical deletion. A maintenance
    // failure leaves the queue for restart/idle retry and must not reject that ACK.
    if (!pruneAcknowledgedEvents())
        qWarning("Journal acknowledged delivery GC failed: %s", qPrintable(error_));
    else if (!pruneLargePayloadGarbage())
        qWarning("Journal payload GC failed after acknowledgement: %s", qPrintable(error_));
    return true;
}

bool LocalJournal::pruneAcknowledgedEvents() {
    // octet_length(column) uses SQLite record metadata, including TEXT with NULs.
    // Older externally supplied SQLite builds keep the durable queue instead of
    // materializing arbitrary legacy text merely to measure it.
    QSqlQuery capability(database_);
    if (!capability.exec("SELECT 1 FROM pragma_function_list WHERE name='octet_length' LIMIT 1")) {
        setError(capability.lastError().text());
        return false;
    }
    if (!capability.next()) {
        static std::once_flag warning;
        std::call_once(warning, [] {
            qWarning("Journal ACK cleanup deferred: SQLite octet_length is unavailable.");
        });
        return true;
    }
    capability.finish();
    constexpr int rowBudget = 128;
    constexpr qint64 byteBudget = 1024 * 1024;
    QSqlQuery timeout(database_);
    if (!timeout.exec("PRAGMA busy_timeout") || !timeout.next()) {
        setError(timeout.lastError().text());
        return false;
    }
    const int oldTimeout = timeout.value(0).toInt();
    timeout.finish();
    if (!timeout.exec("PRAGMA busy_timeout=0")) {
        setError(timeout.lastError().text());
        return false;
    }
    timeout.finish();
    bool transaction = false;
    const auto restore = qScopeGuard([&] {
        if (transaction)
            database_.rollback();
        QSqlQuery restoreTimeout(database_);
        restoreTimeout.exec(QString("PRAGMA busy_timeout=%1").arg(oldTimeout));
    });
    const auto fail = [&](const QSqlError& error) {
        bool ok = false;
        const int code = error.nativeErrorCode().toInt(&ok) & 0xff;
        if (ok && (code == 5 || code == 6))
            return true;
        setError(error.text());
        return false;
    };
    if (!database_.transaction())
        return fail(database_.lastError());
    transaction = true;
    QSqlQuery query(database_);
    if (!query.exec("SELECT queue_id,session_id,generation,after_cursor FROM journal_ack_gc ORDER "
                    "BY queue_id LIMIT 8"))
        return fail(query.lastError());
    QList<QVariantList> queue;
    while (query.next())
        queue.append({query.value(0), query.value(1), query.value(2), query.value(3)});
    if (query.lastError().isValid())
        return fail(query.lastError());
    query.finish();
    int examined = 0;
    qint64 deletedBytes = 0;
    for (const auto& entry : queue) {
        if (examined >= rowBudget || deletedBytes >= byteBudget)
            break;
        // Re-read the slowest client in this transaction: a newly registered client
        // may have lowered the fence since this generation was queued.
        query.prepare(detail::acknowledgedEventFenceQuery());
        query.addBindValue(entry[1]);
        query.addBindValue(entry[2]);
        if (!query.exec() || !query.next())
            return fail(query.lastError());
        const auto minimum = query.value(0);
        query.finish();
        qint64 after = entry[3].toLongLong();
        QList<QVariantList> rows;
        if (!minimum.isNull()) {
            query.prepare(detail::acknowledgedEventCandidatesQuery());
            query.addBindValue(entry[1]);
            query.addBindValue(entry[2]);
            query.addBindValue(after);
            query.addBindValue(minimum);
            query.addBindValue(rowBudget - examined);
            if (!query.exec())
                return fail(query.lastError());
            while (query.next())
                rows.append({query.value(0), query.value(1)});
            if (query.lastError().isValid())
                return fail(query.lastError());
            query.finish();
        }
        const bool finished = minimum.isNull() || (rows.isEmpty() && after == 0);
        if (rows.isEmpty())
            after = 0; // revisit earlier protected/oversized rows after wrap
        examined += rows.size();
        for (const auto& row : rows) {
            const qint64 bytes = row[1].toLongLong();
            if (bytes >= 0 && bytes <= byteBudget && deletedBytes + bytes > byteBudget)
                break;
            after = row[0].toLongLong();
            if (bytes < 0 || bytes > byteBudget)
                continue;
            query.prepare("DELETE FROM events WHERE session_id=? AND generation=? AND cursor=?");
            query.addBindValue(entry[1]);
            query.addBindValue(entry[2]);
            query.addBindValue(row[0]);
            if (!query.exec())
                return fail(query.lastError());
            if (query.numRowsAffected() != 1) {
                setError("Acknowledged event was not removed.");
                return false;
            }
            query.finish();
            deletedBytes += bytes;
        }
        query.prepare("DELETE FROM journal_ack_gc WHERE queue_id=?");
        query.addBindValue(entry[0]);
        if (!query.exec())
            return fail(query.lastError());
        if (query.numRowsAffected() != 1) {
            setError("ACK cleanup queue was not advanced.");
            return false;
        }
        query.finish();
        if (!finished) {
            // AUTOINCREMENT rotation lets other generations progress on the next pass.
            query.prepare(
                "INSERT INTO journal_ack_gc(session_id,generation,after_cursor) VALUES(?,?,?)");
            query.addBindValue(entry[1]);
            query.addBindValue(entry[2]);
            query.addBindValue(after);
            if (!query.exec())
                return fail(query.lastError());
            if (query.numRowsAffected() != 1) {
                setError("ACK cleanup queue was not persisted.");
                return false;
            }
            query.finish();
        }
    }
    if (!database_.commit())
        return fail(database_.lastError());
    transaction = false;
    return true;
}

} // namespace ketplus::daemon
