#include "protocol/DaemonProtocol.h"
#include "storage/LocalJournal.h"
#include <QCryptographicHash>
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
    if (!database_.transaction())
        return false;
    for (const QString& table :
         {QStringLiteral("staged_attachment_chunks"), QStringLiteral("staged_attachments")}) {
        QSqlQuery query(database_);
        query.prepare(QStringLiteral("DELETE FROM %1 WHERE session_id=? AND generation=? AND "
                                     "attachment_id=? AND revision=?")
                          .arg(table));
        query.addBindValue(sessionId);
        query.addBindValue(generation);
        query.addBindValue(attachmentId);
        query.addBindValue(expectedRevision);
        if (!query.exec()) {
            database_.rollback();
            if (error)
                *error = query.lastError().text();
            return false;
        }
    }
    return database_.commit();
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
