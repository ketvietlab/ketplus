#include "storage/LocalJournal.h"

#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {
namespace {
constexpr qint64 byteBudget = 1024 * 1024;
constexpr int payloadCandidates = 32;
constexpr int chunkCandidates = 128;

bool isBusy(const QSqlError& error) {
    bool ok = false;
    const int code = error.nativeErrorCode().toInt(&ok) & 0xff;
    return ok && (code == 5 || code == 6); // includes BUSY_SNAPSHOT / LOCKED variants
}
} // namespace

namespace detail {
// Kept as production queries so regression tests inspect the prepared plan and bindings.
QString largePayloadGcRawLookup() {
    return QStringLiteral("SELECT 1 FROM events WHERE session_id=? AND generation=? LIMIT 1");
}
QString largePayloadGcReplayLookup() {
    return QStringLiteral(
        "SELECT 1 FROM replay_events WHERE session_id=? AND generation=? AND "
        "item_id=? AND json_extract(payload,'$.large_event.transfer_id')=? LIMIT 1");
}
} // namespace detail

bool LocalJournal::pruneLargePayloadGarbage() {
    // This maintenance transaction yields immediately to another writer. Its cursors
    // advance only on commit, so a failed or busy pass is safe to retry.
    QSqlQuery timeout(database_);
    if (!timeout.exec("PRAGMA busy_timeout") || !timeout.next()) {
        setError(timeout.lastError().text());
        return false;
    }
    const auto oldTimeout = timeout.value(0).toInt();
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
        if (isBusy(error))
            return true;
        setError(error.text());
        return false;
    };
    if (!database_.transaction())
        return fail(database_.lastError());
    transaction = true;
    QSqlQuery state(database_);
    if (!state.exec("SELECT payload_row,chunk_row FROM journal_gc_state WHERE singleton=1") ||
        !state.next()) {
        if (state.lastError().isValid())
            return fail(state.lastError());
        setError("Missing journal GC cursor state.");
        return false;
    }
    qint64 payloadRow = state.value(0).toLongLong();
    qint64 chunkRow = state.value(1).toLongLong();
    state.finish();
    qint64 deletedBytes = 0;

    // Read only IDs and BLOB lengths, never payload bytes. A malformed TEXT value
    // or legacy monolithic blob over the byte budget is retained for a separate path.
    QSqlQuery scan(database_);
    scan.prepare("SELECT rowid,session_id,generation,transfer_id,item_id,"
                 "CASE WHEN typeof(payload)='blob' THEN length(payload) ELSE -1 END "
                 "FROM large_event_payloads WHERE rowid>? ORDER BY rowid LIMIT ?");
    scan.addBindValue(payloadRow);
    scan.addBindValue(payloadCandidates);
    if (!scan.exec())
        return fail(scan.lastError());
    QList<QVariantList> payloads;
    while (scan.next())
        payloads.append({scan.value(0), scan.value(1), scan.value(2), scan.value(3), scan.value(4),
                         scan.value(5)});
    if (scan.lastError().isValid())
        return fail(scan.lastError());
    scan.finish();
    if (payloads.isEmpty())
        payloadRow = 0;
    QSqlQuery raw(database_);
    raw.prepare(detail::largePayloadGcRawLookup());
    QSqlQuery replay(database_);
    replay.prepare(detail::largePayloadGcReplayLookup());
    QSqlQuery remove(database_);
    remove.prepare("DELETE FROM large_event_payloads WHERE rowid=?");
    for (const auto& row : payloads) {
        const auto bytes = row[5].toLongLong();
        if (bytes >= 0 && bytes <= byteBudget && deletedBytes + bytes > byteBudget)
            break; // retry this individually valid item on the next pass
        payloadRow = row[0].toLongLong();
        if (bytes < 0 || bytes > byteBudget)
            continue;
        raw.bindValue(0, row[1]);
        raw.bindValue(1, row[2]);
        if (!raw.exec())
            return fail(raw.lastError());
        const bool pending = raw.next();
        if (raw.lastError().isValid())
            return fail(raw.lastError());
        raw.finish();
        if (pending)
            continue; // no JSON scan of another turn's pending raw backlog
        replay.bindValue(0, row[1]);
        replay.bindValue(1, row[2]);
        replay.bindValue(2, row[4]);
        replay.bindValue(3, row[3]);
        if (!replay.exec())
            return fail(replay.lastError());
        const bool retained = replay.next();
        if (replay.lastError().isValid())
            return fail(replay.lastError());
        replay.finish();
        if (retained)
            continue;
        remove.bindValue(0, row[0]);
        if (!remove.exec())
            return fail(remove.lastError());
        if (remove.numRowsAffected() != 1) {
            setError("Journal GC did not remove its selected row.");
            return false;
        }
        remove.finish();
        deletedBytes += bytes;
    }

    // Unreachable chunks can outlive removed metadata until subsequent bounded
    // passes. Both old stray chunks and newly unreachable chunks use this cursor.
    scan.prepare("SELECT rowid,session_id,generation,transfer_id,"
                 "CASE WHEN typeof(payload)='blob' THEN length(payload) ELSE -1 END "
                 "FROM large_event_payload_chunks WHERE rowid>? ORDER BY rowid LIMIT ?");
    scan.addBindValue(chunkRow);
    scan.addBindValue(chunkCandidates);
    if (!scan.exec())
        return fail(scan.lastError());
    QList<QVariantList> chunks;
    while (scan.next())
        chunks.append({scan.value(0), scan.value(1), scan.value(2), scan.value(3), scan.value(4)});
    if (scan.lastError().isValid())
        return fail(scan.lastError());
    scan.finish();
    if (chunks.isEmpty())
        chunkRow = 0;
    QSqlQuery parent(database_);
    parent.prepare("SELECT 1 FROM large_event_payloads WHERE session_id=? AND generation=? "
                   "AND transfer_id=?");
    remove.prepare("DELETE FROM large_event_payload_chunks WHERE rowid=?");
    for (const auto& row : chunks) {
        const auto bytes = row[4].toLongLong();
        if (bytes >= 0 && bytes <= 128 * 1024 && deletedBytes + bytes > byteBudget)
            break;
        chunkRow = row[0].toLongLong();
        if (bytes < 0 || bytes > 128 * 1024)
            continue;
        parent.bindValue(0, row[1]);
        parent.bindValue(1, row[2]);
        parent.bindValue(2, row[3]);
        if (!parent.exec())
            return fail(parent.lastError());
        const bool referenced = parent.next();
        if (parent.lastError().isValid())
            return fail(parent.lastError());
        parent.finish();
        if (referenced)
            continue;
        raw.bindValue(0, row[1]);
        raw.bindValue(1, row[2]);
        if (!raw.exec())
            return fail(raw.lastError());
        const bool pending = raw.next();
        if (raw.lastError().isValid())
            return fail(raw.lastError());
        raw.finish();
        if (pending)
            continue;
        remove.bindValue(0, row[0]);
        if (!remove.exec())
            return fail(remove.lastError());
        if (remove.numRowsAffected() != 1) {
            setError("Journal GC did not remove its selected row.");
            return false;
        }
        remove.finish();
        deletedBytes += bytes;
    }
    state.prepare("UPDATE journal_gc_state SET payload_row=?,chunk_row=? WHERE singleton=1");
    state.addBindValue(payloadRow);
    state.addBindValue(chunkRow);
    if (!state.exec())
        return fail(state.lastError());
    if (state.numRowsAffected() != 1) {
        setError("Journal GC did not persist its cursor.");
        return false;
    }
    state.finish();
    if (!database_.commit())
        return fail(database_.lastError());
    transaction = false;
    return true;
}
} // namespace ketplus::daemon
