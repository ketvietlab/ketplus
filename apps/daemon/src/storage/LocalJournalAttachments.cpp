#include "protocol/DaemonProtocol.h"
#include "storage/LocalJournal.h"
#include <QCryptographicHash>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {
namespace {
QString usableAttachment() {
    // Logical expiry must not depend on when a bounded sweep reaches this ID.
    // The retained manifest still reserves the identity until its chunks are gone.
    return QStringLiteral(" AND status IN ('uploading','ready') AND "
                          "datetime(created_at)>=datetime('now','-1 day')");
}
} // namespace

namespace detail {
QString attachmentCancelManifestQuery() {
    return "SELECT CASE WHEN typeof(status)='text' AND octet_length(status)<=16 THEN status "
           "ELSE NULL END,CASE WHEN typeof(session_id)='text' AND typeof(attachment_id)='text' "
           "AND typeof(name)='text' AND typeof(media_type)='text' AND typeof(digest_sha256)='text' "
           "AND typeof(status)='text' AND typeof(created_at)='text' AND "
           "typeof(generation)='integer' "
           "AND typeof(revision)='integer' AND typeof(total_bytes)='integer' AND "
           "typeof(chunk_count)='integer' "
           "THEN octet_length(session_id)+octet_length(attachment_id)+octet_length(name)+"
           "octet_length(media_type)+octet_length(digest_sha256)+octet_length(status)+"
           "octet_length(created_at)+40 ELSE -1 END FROM staged_attachments "
           "WHERE session_id=? AND generation=? AND attachment_id=? AND revision=?";
}
QString attachmentCancelChunksQuery() {
    return "SELECT rowid,CASE WHEN typeof(payload)='blob' AND typeof(session_id)='text' "
           "AND typeof(attachment_id)='text' AND typeof(digest_sha256)='text' "
           "AND typeof(generation)='integer' AND typeof(revision)='integer' "
           "AND typeof(chunk_index)='integer' THEN length(payload)+octet_length(session_id)+"
           "octet_length(attachment_id)+octet_length(digest_sha256)+24 ELSE -1 END,revision "
           "FROM staged_attachment_chunks WHERE session_id=? AND generation=? AND attachment_id=? "
           "ORDER BY chunk_index LIMIT 128";
}
} // namespace detail

bool LocalJournal::beginAttachment(const QString& sessionId, const qint64 generation,
                                   const QString& attachmentId, const qint64 expectedRevision,
                                   const QString& name, const QString& mediaType,
                                   const qint64 totalBytes, const int chunkCount,
                                   const QString& digestSha256, QString* error) {
    if (attachmentId.isEmpty() || attachmentId.size() > 128 || name.isEmpty() ||
        name.size() > 255 || mediaType.isEmpty() || mediaType.size() > 128 ||
        expectedRevision != 0 || totalBytes < 1 || totalBytes > 20 * 1024 * 1024 ||
        chunkCount < 1 || chunkCount > 640 || digestSha256.size() != 64) {
        if (error)
            *error = QStringLiteral("invalid_attachment_manifest");
        return false;
    }
    if (!purgeExpiredAttachments()) {
        if (error)
            *error = QStringLiteral("attachment_gc_failed");
        return false;
    }
    QSqlQuery quota(database_);
    quota.prepare(QStringLiteral("SELECT COUNT(*),COALESCE(SUM(total_bytes),0) FROM "
                                 "staged_attachments WHERE session_id=? "
                                 "AND generation=? AND attachment_id<>?") +
                  usableAttachment());
    quota.addBindValue(sessionId);
    quota.addBindValue(generation);
    quota.addBindValue(attachmentId);
    if (!quota.exec() || !quota.next() || quota.value(0).toInt() >= 5 ||
        quota.value(1).toLongLong() + totalBytes > 40 * 1024 * 1024) {
        if (error)
            *error = QStringLiteral("attachment_draft_limit");
        return false;
    }
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT INTO staged_attachments(session_id,generation,attachment_id,revision,name,"
        "media_type,total_bytes,chunk_count,digest_sha256,status,created_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,'uploading',?) ON CONFLICT DO NOTHING"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(attachmentId);
    query.addBindValue(1);
    query.addBindValue(name);
    query.addBindValue(mediaType);
    query.addBindValue(totalBytes);
    query.addBindValue(chunkCount);
    query.addBindValue(digestSha256);
    query.addBindValue(protocol::timestamp());
    if (!query.exec()) {
        if (error)
            *error = query.lastError().text();
        return false;
    }
    QSqlQuery verify(database_);
    verify.prepare(
        QStringLiteral(
            "SELECT revision,name,media_type,total_bytes,chunk_count,digest_sha256,status FROM "
            "staged_attachments WHERE session_id=? AND generation=? AND attachment_id=?") +
        usableAttachment());
    verify.addBindValue(sessionId);
    verify.addBindValue(generation);
    verify.addBindValue(attachmentId);
    const bool matches =
        verify.exec() && verify.next() && verify.value(0).toLongLong() == 1 &&
        verify.value(1).toString() == name && verify.value(2).toString() == mediaType &&
        verify.value(3).toLongLong() == totalBytes && verify.value(4).toInt() == chunkCount &&
        verify.value(5).toString() == digestSha256 &&
        (verify.value(6).toString() == "uploading" || verify.value(6).toString() == "ready");
    if (!matches && error)
        *error = QStringLiteral("attachment_conflict");
    return matches;
}

bool LocalJournal::appendAttachmentChunk(const QString& sessionId, const qint64 generation,
                                         const QString& attachmentId, const qint64 expectedRevision,
                                         const int chunkIndex, const QByteArray& bytes,
                                         const QString& digestSha256, QString* error) {
    QSqlQuery manifest(database_);
    manifest.prepare(
        QStringLiteral(
            "SELECT revision,chunk_count,status FROM staged_attachments WHERE session_id=? AND "
            "generation=? AND attachment_id=?") +
        usableAttachment());
    manifest.addBindValue(sessionId);
    manifest.addBindValue(generation);
    manifest.addBindValue(attachmentId);
    const bool known = manifest.exec() && manifest.next();
    if (!known || manifest.value(2).toString() != QStringLiteral("uploading") ||
        expectedRevision != manifest.value(0).toLongLong() || chunkIndex < 0 ||
        chunkIndex >= manifest.value(1).toInt() || bytes.isEmpty() || bytes.size() > 32 * 1024 ||
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) !=
            digestSha256) {
        if (error)
            *error = QStringLiteral("invalid_attachment_chunk");
        return false;
    }
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT INTO staged_attachment_chunks(session_id,generation,attachment_id,revision,"
        "chunk_index,digest_sha256,payload) VALUES(?,?,?,?,?,?,?) ON CONFLICT DO NOTHING"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(attachmentId);
    query.addBindValue(expectedRevision);
    query.addBindValue(chunkIndex);
    query.addBindValue(digestSha256);
    query.addBindValue(bytes);
    if (!query.exec()) {
        if (error)
            *error = query.lastError().text();
        return false;
    }
    QSqlQuery verify(database_);
    verify.prepare(QStringLiteral(
        "SELECT digest_sha256,payload FROM staged_attachment_chunks WHERE session_id=? AND "
        "generation=? AND attachment_id=? AND chunk_index=?"));
    verify.addBindValue(sessionId);
    verify.addBindValue(generation);
    verify.addBindValue(attachmentId);
    verify.addBindValue(chunkIndex);
    const bool matches = verify.exec() && verify.next() &&
                         verify.value(0).toString() == digestSha256 &&
                         verify.value(1).toByteArray() == bytes;
    if (!matches && error)
        *error = QStringLiteral("attachment_chunk_conflict");
    return matches;
}

bool LocalJournal::commitAttachment(const QString& sessionId, const qint64 generation,
                                    const QString& attachmentId, const qint64 expectedRevision,
                                    QString* error) {
    QSqlQuery manifest(database_);
    manifest.prepare(
        QStringLiteral(
            "SELECT total_bytes,chunk_count,digest_sha256,status FROM staged_attachments WHERE "
            "session_id=? AND generation=? AND attachment_id=? AND revision=?") +
        usableAttachment());
    manifest.addBindValue(sessionId);
    manifest.addBindValue(generation);
    manifest.addBindValue(attachmentId);
    manifest.addBindValue(expectedRevision);
    if (!manifest.exec() || !manifest.next()) {
        if (error)
            *error = QStringLiteral("attachment_not_found");
        return false;
    }
    if (manifest.value(3).toString() == QStringLiteral("ready"))
        return true;
    if (manifest.value(3).toString() != QStringLiteral("uploading")) {
        if (error)
            *error = QStringLiteral("attachment_not_found");
        return false;
    }
    QSqlQuery completeness(database_);
    completeness.prepare(QStringLiteral(
        "SELECT COUNT(*),COALESCE(MIN(chunk_index),-1),COALESCE(MAX(chunk_index),-1),"
        "COALESCE(SUM(length(payload)),0) FROM staged_attachment_chunks WHERE session_id=? "
        "AND generation=? AND attachment_id=? AND revision=?"));
    completeness.addBindValue(sessionId);
    completeness.addBindValue(generation);
    completeness.addBindValue(attachmentId);
    completeness.addBindValue(expectedRevision);
    const int expectedChunks = manifest.value(1).toInt();
    if (!completeness.exec() || !completeness.next() ||
        completeness.value(0).toInt() != expectedChunks || completeness.value(1).toInt() != 0 ||
        completeness.value(2).toInt() != expectedChunks - 1 ||
        completeness.value(3).toLongLong() != manifest.value(0).toLongLong()) {
        if (error)
            *error = QStringLiteral("attachment_incomplete");
        return false;
    }
    const StagedAttachment assembled = attachment(sessionId, generation, attachmentId);
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(assembled.bytes, QCryptographicHash::Sha256).toHex());
    if (assembled.bytes.size() != manifest.value(0).toLongLong() ||
        digest != manifest.value(2).toString()) {
        if (error)
            *error = QStringLiteral("attachment_incomplete");
        return false;
    }
    QSqlQuery update(database_);
    update.prepare(QStringLiteral(
        "UPDATE staged_attachments SET status='ready' WHERE session_id=? AND generation=? AND "
        "attachment_id=? AND revision=? AND status='uploading'"));
    update.addBindValue(sessionId);
    update.addBindValue(generation);
    update.addBindValue(attachmentId);
    update.addBindValue(expectedRevision);
    return update.exec() && update.numRowsAffected() == 1;
}

bool LocalJournal::cancelAttachment(const QString& sessionId, const qint64 generation,
                                    const QString& attachmentId, const qint64 expectedRevision,
                                    QString* error) {
    const auto fail = [&](const QString& message) {
        setError(message);
        if (error)
            *error = message;
        return false;
    };
    QSqlQuery timeout(database_);
    if (!timeout.exec("PRAGMA busy_timeout") || !timeout.next())
        return fail(timeout.lastError().text());
    const int oldTimeout = timeout.value(0).toInt();
    timeout.finish();
    if (!timeout.exec("PRAGMA busy_timeout=0"))
        return fail(timeout.lastError().text());
    timeout.finish();
    bool transaction = false;
    const auto restore = qScopeGuard([&] {
        if (transaction)
            database_.rollback();
        QSqlQuery q(database_);
        q.exec(QString("PRAGMA busy_timeout=%1").arg(oldTimeout));
    });
    if (!database_.transaction())
        return fail(database_.lastError().text());
    transaction = true;
    const auto commit = [&] {
        if (!database_.commit())
            return fail(database_.lastError().text());
        transaction = false;
        return true;
    };
    QSqlQuery query(database_);
    query.prepare(detail::attachmentCancelManifestQuery());
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(attachmentId);
    query.addBindValue(expectedRevision);
    if (!query.exec())
        return fail(query.lastError().text());
    const bool current = query.next();
    if (query.lastError().isValid())
        return fail(query.lastError().text());
    const QString status = current ? query.value(0).toString() : QString();
    qint64 spent = current ? query.value(1).toLongLong() : 0;
    query.finish();
    if (!current || status == "expiring")
        return commit();
    constexpr qint64 byteBudget = 1024 * 1024;
    // Reserve the entire manifest, including possible status growth, before
    // deleting any chunks. Unknown formats remain recoverable and unchanged.
    if ((status != "uploading" && status != "ready") || spent < 0 || spent > byteBudget)
        return fail("Attachment cancellation requires bounded manifest recovery.");
    query.prepare(detail::attachmentCancelChunksQuery());
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(attachmentId);
    if (!query.exec())
        return fail(query.lastError().text());
    QList<QVariantList> chunks;
    while (query.next())
        chunks.append({query.value(0), query.value(1), query.value(2)});
    if (query.lastError().isValid())
        return fail(query.lastError().text());
    query.finish();
    QSqlQuery remove(database_);
    remove.prepare("DELETE FROM staged_attachment_chunks WHERE rowid=?");
    for (const auto& row : chunks) {
        // Revision is deliberately checked after the bounded index range read.
        // The primary key has chunk_index, not revision; SQL filtering would scan
        // an arbitrarily large backlog belonging to another revision first.
        if (row[2].toLongLong() != expectedRevision)
            continue;
        const auto bytes = row[1].toLongLong();
        if (bytes < 0 || bytes > byteBudget)
            continue;
        if (bytes > byteBudget - spent)
            break;
        remove.bindValue(0, row[0]);
        if (!remove.exec())
            return fail(remove.lastError().text());
        if (remove.numRowsAffected() != 1)
            return fail("Attachment cancellation chunk was not removed.");
        remove.finish();
        spent += bytes;
    }
    query.prepare("SELECT 1 FROM staged_attachment_chunks WHERE session_id=? AND generation=? "
                  "AND attachment_id=? LIMIT 1");
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(attachmentId);
    if (!query.exec())
        return fail(query.lastError().text());
    const bool remaining = query.next();
    if (query.lastError().isValid())
        return fail(query.lastError().text());
    query.finish();
    // Logical cancellation can finish before physical cleanup. Every revision
    // remains fenced until the existing durable bounded sweeper drains it.
    query.prepare((remaining ? QString("UPDATE staged_attachments SET status='expiring' ")
                             : QString("DELETE FROM staged_attachments ")) +
                  "WHERE session_id=? AND generation=? AND attachment_id=? AND revision=?");
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(attachmentId);
    query.addBindValue(expectedRevision);
    if (!query.exec())
        return fail(query.lastError().text());
    if (query.numRowsAffected() != 1)
        return fail("Attachment cancellation did not preserve or release its identity.");
    query.finish();
    return commit();
}

StagedAttachment LocalJournal::attachment(const QString& sessionId, const qint64 generation,
                                          const QString& attachmentId) const {
    StagedAttachment result;
    QSqlQuery manifest(database_);
    manifest.prepare(
        QStringLiteral(
            "SELECT name,media_type,revision,total_bytes,chunk_count,digest_sha256,status "
            "FROM staged_attachments WHERE session_id=? AND "
            "generation=? AND attachment_id=?") +
        usableAttachment());
    manifest.addBindValue(sessionId);
    manifest.addBindValue(generation);
    manifest.addBindValue(attachmentId);
    if (!manifest.exec() || !manifest.next() ||
        (manifest.value(6).toString() != "uploading" && manifest.value(6).toString() != "ready"))
        return result;
    result.id = attachmentId;
    result.name = manifest.value(0).toString();
    result.mediaType = manifest.value(1).toString();
    result.revision = manifest.value(2).toLongLong();
    result.totalBytes = manifest.value(3).toLongLong();
    result.chunkCount = manifest.value(4).toInt();
    result.digestSha256 = manifest.value(5).toString();
    result.status = manifest.value(6).toString();
    QSqlQuery chunks(database_);
    chunks.prepare(QStringLiteral(
        "SELECT payload FROM staged_attachment_chunks WHERE session_id=? AND generation=? AND "
        "attachment_id=? AND revision=? ORDER BY chunk_index"));
    chunks.addBindValue(sessionId);
    chunks.addBindValue(generation);
    chunks.addBindValue(attachmentId);
    chunks.addBindValue(result.revision);
    if (!chunks.exec())
        return {};
    while (chunks.next())
        result.bytes.append(chunks.value(0).toByteArray());
    return result;
}

} // namespace ketplus::daemon
