#include "storage/LocalJournal.h"

#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <mutex>

namespace ketplus::daemon {
namespace {
constexpr qint64 byteBudget = 1024 * 1024;
constexpr int manifestLimit = 32;
constexpr int chunkLimit = 128;

QString rowBytes(const QString& alias, const QStringList& texts, const QStringList& integers,
                 const QString& blob = {}) {
    QStringList types, sizes;
    for (const auto& field : texts) {
        types.append("typeof(" + alias + "." + field + ")='text'");
        sizes.append("octet_length(" + alias + "." + field + ")");
    }
    for (const auto& field : integers)
        types.append("typeof(" + alias + "." + field + ")='integer'");
    sizes.append(QString::number(integers.size() * 8));
    if (!blob.isEmpty()) {
        types.append("typeof(" + alias + "." + blob + ")='blob'");
        sizes.append("length(" + alias + "." + blob + ")");
    }
    return "CASE WHEN " + types.join(" AND ") + " THEN " + sizes.join('+') + " ELSE -1 END";
}
QString identityGuard(const QString& alias) {
    return "octet_length(" + alias + ".session_id)<=256 AND octet_length(" + alias +
           ".attachment_id)<=128";
}
} // namespace

namespace detail {
QString stagedManifestGcQuery() {
    const auto bytes = rowBytes("a",
                                {"session_id", "attachment_id", "name", "media_type",
                                 "digest_sha256", "status", "created_at"},
                                {"generation", "revision", "total_bytes", "chunk_count"});
    return QString("SELECT a.rowid,%1,CASE WHEN (%1) BETWEEN 0 AND %2 AND %3 AND "
                   "octet_length(a.status)<=32 AND octet_length(a.created_at)<=64 THEN "
                   "CASE WHEN a.status='expiring' THEN 2 WHEN a.status IN ('uploading','ready') "
                   "AND datetime(a.created_at)<datetime('now','-1 day') THEN 1 ELSE 0 END "
                   "ELSE 0 END FROM staged_attachments a WHERE a.rowid>? ORDER BY a.rowid LIMIT ?")
        .arg(bytes)
        .arg(byteBudget - 8)
        .arg(identityGuard("a"));
}
QString stagedChunkGcQuery() {
    const auto bytes = rowBytes("c", {"session_id", "attachment_id", "digest_sha256"},
                                {"generation", "revision", "chunk_index"}, "payload");
    // Any parent except our explicit retirement marker protects its chunks,
    // including unknown/malformed status and different manifest revisions.
    return QString("SELECT c.rowid,%1,CASE WHEN (%1) BETWEEN 0 AND %2 AND %3 THEN "
                   "NOT EXISTS (SELECT 1 FROM staged_attachments a WHERE "
                   "a.session_id=c.session_id AND a.generation=c.generation AND "
                   "a.attachment_id=c.attachment_id AND CASE WHEN typeof(a.status)='text' "
                   "AND octet_length(a.status)=8 THEN a.status<>'expiring' ELSE 1 END) "
                   "ELSE 0 END FROM staged_attachment_chunks c WHERE c.rowid>? "
                   "ORDER BY c.rowid LIMIT ?")
        .arg(bytes)
        .arg(byteBudget)
        .arg(identityGuard("c"));
}
QString stagedRemainingChunksQuery() {
    return "SELECT EXISTS(SELECT 1 FROM staged_attachment_chunks c WHERE "
           "c.session_id=a.session_id AND c.generation=a.generation AND "
           "c.attachment_id=a.attachment_id) FROM staged_attachments a WHERE a.rowid=?";
}
} // namespace detail

bool LocalJournal::purgeExpiredAttachments() {
    const auto fail = [&](const QSqlError& error) {
        bool ok = false;
        const auto code = error.nativeErrorCode().toInt(&ok) & 0xff;
        if (ok && (code == 5 || code == 6))
            return true;
        setError(error.text());
        return false;
    };
    QSqlQuery capability(database_);
    if (!capability.exec("SELECT 1 FROM pragma_function_list WHERE name='octet_length' LIMIT 1"))
        return fail(capability.lastError());
    if (!capability.next()) {
        if (capability.lastError().isValid())
            return fail(capability.lastError());
        static std::once_flag warning;
        std::call_once(warning, [] {
            qWarning("Staged attachment cleanup deferred: SQLite octet_length is unavailable.");
        });
        return true;
    }
    capability.finish();
    QSqlQuery timeout(database_);
    if (!timeout.exec("PRAGMA busy_timeout") || !timeout.next())
        return fail(timeout.lastError());
    const auto oldTimeout = timeout.value(0).toInt();
    timeout.finish();
    if (!timeout.exec("PRAGMA busy_timeout=0"))
        return fail(timeout.lastError());
    timeout.finish();
    bool transaction = false;
    const auto restore = qScopeGuard([&] {
        if (transaction)
            database_.rollback();
        QSqlQuery q(database_);
        q.exec(QString("PRAGMA busy_timeout=%1").arg(oldTimeout));
    });
    if (!database_.transaction())
        return fail(database_.lastError());
    transaction = true;
    QSqlQuery state(database_);
    if (!state.exec("SELECT manifest_row,chunk_row FROM journal_attachment_gc WHERE singleton=1"))
        return fail(state.lastError());
    if (!state.next()) {
        setError("Staged attachment cleanup cursor is missing.");
        return false;
    }
    auto manifestRow = state.value(0).toLongLong();
    auto chunkRow = state.value(1).toLongLong();
    state.finish();
    qint64 spent = 0;
    const auto read = [&](const QString& sql, qint64& after, int limit,
                          QList<QVariantList>& candidates) {
        QSqlQuery q(database_);
        q.prepare(sql);
        // A single empty-tail wrap never increases the actual candidate budget.
        for (int attempt = 0; attempt < 2; ++attempt) {
            q.bindValue(0, after);
            q.bindValue(1, limit);
            if (!q.exec())
                return q.lastError();
            while (q.next())
                candidates.append({q.value(0), q.value(1), q.value(2)});
            if (q.lastError().isValid())
                return q.lastError();
            q.finish();
            if (!candidates.isEmpty() || after == 0)
                break;
            after = 0;
        }
        return QSqlError{};
    };
    QList<QVariantList> manifests;
    auto error = read(detail::stagedManifestGcQuery(), manifestRow, manifestLimit, manifests);
    if (error.isValid())
        return fail(error);
    QList<QVariantList> retiring;
    QSqlQuery mark(database_);
    mark.prepare("UPDATE staged_attachments SET status='expiring' WHERE rowid=?");
    for (const auto& row : manifests) {
        const auto bytes = row[1].toLongLong() + 8; // includes status growth on transition
        if (row[2].toInt() == 1) {
            if (spent + bytes > byteBudget)
                break;
            mark.bindValue(0, row[0]);
            if (!mark.exec())
                return fail(mark.lastError());
            if (mark.numRowsAffected() != 1) {
                setError("Staged attachment was not marked expiring.");
                return false;
            }
            mark.finish();
            spent += bytes;
        }
        manifestRow = row[0].toLongLong();
        if (row[2].toInt() > 0)
            retiring.append({row[0], bytes});
    }
    QList<QVariantList> chunks;
    error = read(detail::stagedChunkGcQuery(), chunkRow, chunkLimit, chunks);
    if (error.isValid())
        return fail(error);
    QSqlQuery removeChunk(database_);
    removeChunk.prepare("DELETE FROM staged_attachment_chunks WHERE rowid=?");
    for (const auto& row : chunks) {
        const auto bytes = row[1].toLongLong();
        if (row[2].toBool()) {
            if (spent + bytes > byteBudget)
                break;
            removeChunk.bindValue(0, row[0]);
            if (!removeChunk.exec())
                return fail(removeChunk.lastError());
            if (removeChunk.numRowsAffected() != 1) {
                setError("Selected staged chunk was not removed.");
                return false;
            }
            removeChunk.finish();
            spent += bytes;
        }
        chunkRow = row[0].toLongLong();
    }
    QSqlQuery remaining(database_), removeManifest(database_);
    remaining.prepare(detail::stagedRemainingChunksQuery());
    removeManifest.prepare("DELETE FROM staged_attachments WHERE rowid=?");
    for (const auto& row : retiring) {
        const auto bytes = row[1].toLongLong();
        if (spent + bytes > byteBudget)
            continue;
        remaining.bindValue(0, row[0]);
        if (!remaining.exec())
            return fail(remaining.lastError());
        if (!remaining.next()) {
            setError("Expiring staged attachment disappeared.");
            return false;
        }
        const bool hasChunks = remaining.value(0).toBool();
        remaining.finish();
        if (hasChunks)
            continue;
        removeManifest.bindValue(0, row[0]);
        if (!removeManifest.exec())
            return fail(removeManifest.lastError());
        if (removeManifest.numRowsAffected() != 1) {
            setError("Expiring staged attachment was not removed.");
            return false;
        }
        removeManifest.finish();
        spent += bytes;
    }
    state.prepare("UPDATE journal_attachment_gc SET manifest_row=?,chunk_row=? WHERE singleton=1");
    state.addBindValue(manifestRow);
    state.addBindValue(chunkRow);
    if (!state.exec())
        return fail(state.lastError());
    if (state.numRowsAffected() != 1) {
        setError("Staged attachment cleanup cursor was not persisted.");
        return false;
    }
    state.finish();
    if (!database_.commit())
        return fail(database_.lastError());
    transaction = false;
    return true;
}
} // namespace ketplus::daemon
