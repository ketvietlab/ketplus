#include "storage/LocalJournal.h"

#include "protocol/DaemonProtocol.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace ketplus::daemon {
namespace {

// Normalized lifecycle payloads retain both the provider item and selected
// top-level compatibility fields. An 8 MiB assistant body can therefore occupy
// slightly more than 16 MiB as compact JSON; keep the hard bound explicit while
// allowing that production shape to round-trip.
constexpr qsizetype maximumLargeEventBytes = 32 * 1024 * 1024;
constexpr qsizetype cloudChunkBytes = 96 * 1024;
constexpr qsizetype storedFragmentBytes = 128 * 1024;

QString itemIdOf(const QJsonObject& payload) {
    const QString direct = payload.value(QStringLiteral("item_id")).toString();
    return direct.isEmpty() ? payload.value(QStringLiteral("item"))
                                  .toObject()
                                  .value(QStringLiteral("id"))
                                  .toString()
                            : direct;
}

bool insertEvent(QSqlDatabase database, const QString& sessionId, const qint64 generation,
                 const qint64 cursor, const qint64 turnSequence, const QString& eventType,
                 const QJsonObject& payload, const QString& sentAt, QString* error) {
    const QByteArray encoded = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (encoded.size() > protocol::maximumEventPayloadBytes) {
        if (error != nullptr) {
            *error = QStringLiteral("Generated large-item chunk exceeds the journal event cap.");
        }
        return false;
    }
    QSqlQuery insert(database);
    insert.prepare(QStringLiteral(
        "INSERT INTO events(session_id, generation, cursor, event_type, payload, sent_at, "
        "turn_sequence) VALUES(?, ?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(sessionId);
    insert.addBindValue(generation);
    insert.addBindValue(cursor);
    insert.addBindValue(eventType);
    insert.addBindValue(QString::fromUtf8(encoded));
    insert.addBindValue(sentAt);
    insert.addBindValue(turnSequence);
    if (!insert.exec()) {
        if (error != nullptr) {
            *error = insert.lastError().text();
        }
        return false;
    }
    return true;
}

} // namespace

qint64 LocalJournal::appendLargeEvent(const QString& sessionId, const qint64 generation,
                                      const qint64 firstCursor, const qint64 turnSequence,
                                      const QString& type, const QJsonObject& payload,
                                      const QByteArray& encodedPayload, QString* error) {
    if (encodedPayload.isEmpty() || encodedPayload.size() > maximumLargeEventBytes) {
        database_.rollback();
        if (error != nullptr) {
            *error = QStringLiteral("Large transcript item exceeds the 32 MiB local bound.");
        }
        return -1;
    }
    const QString transferId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString itemId = itemIdOf(payload);
    const QString itemType = payload.value(QStringLiteral("item_type"))
                                 .toString(payload.value(QStringLiteral("item"))
                                               .toObject()
                                               .value(QStringLiteral("type"))
                                               .toString());
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(encodedPayload, QCryptographicHash::Sha256).toHex());
    const qint64 totalBytes = encodedPayload.size();
    const qint64 chunkCount = (totalBytes + cloudChunkBytes - 1) / cloudChunkBytes;
    const qint64 revision = firstCursor + chunkCount;
    const QString sentAt = protocol::timestamp();

    QSqlQuery large(database_);
    large.prepare(QStringLiteral(
        "INSERT INTO large_event_payloads(session_id, generation, transfer_id, event_type, "
        "item_id, revision, digest_sha256, payload, created_at, total_bytes) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    large.addBindValue(sessionId);
    large.addBindValue(generation);
    large.addBindValue(transferId);
    large.addBindValue(type);
    large.addBindValue(itemId);
    large.addBindValue(revision);
    large.addBindValue(digest);
    // The bytes live in fragment-sized rows; an empty (non-NULL) blob keeps the legacy column.
    large.addBindValue(QByteArray(""));
    large.addBindValue(sentAt);
    large.addBindValue(totalBytes);
    if (!large.exec()) {
        database_.rollback();
        if (error != nullptr) {
            *error = large.lastError().text();
        }
        return -1;
    }
    for (qint64 offset = 0; offset < totalBytes; offset += storedFragmentBytes) {
        QSqlQuery slice(database_);
        slice.prepare(QStringLiteral(
            "INSERT INTO large_event_payload_chunks(session_id, generation, transfer_id, "
            "chunk_offset, payload) VALUES(?, ?, ?, ?, ?)"));
        slice.addBindValue(sessionId);
        slice.addBindValue(generation);
        slice.addBindValue(transferId);
        slice.addBindValue(offset);
        slice.addBindValue(encodedPayload.mid(offset, storedFragmentBytes));
        if (!slice.exec()) {
            database_.rollback();
            if (error != nullptr) {
                *error = slice.lastError().text();
            }
            return -1;
        }
    }

    qint64 cursor = firstCursor;
    for (qint64 offset = 0, index = 0; offset < totalBytes;
         offset += cloudChunkBytes, ++index, ++cursor) {
        const QByteArray bytes = encodedPayload.mid(offset, cloudChunkBytes);
        const QJsonObject chunk{{QStringLiteral("large_event_chunk_version"), 1},
                                {QStringLiteral("transfer_id"), transferId},
                                {QStringLiteral("item_id"), itemId},
                                {QStringLiteral("revision"), revision},
                                {QStringLiteral("chunk_index"), index},
                                {QStringLiteral("chunk_count"), chunkCount},
                                {QStringLiteral("offset"), offset},
                                {QStringLiteral("total_bytes"), totalBytes},
                                {QStringLiteral("digest_sha256"), digest},
                                {QStringLiteral("data"), QString::fromLatin1(bytes.toBase64(
                                                             QByteArray::Base64UrlEncoding |
                                                             QByteArray::OmitTrailingEquals))}};
        if (!insertEvent(database_, sessionId, generation, cursor, turnSequence,
                         QStringLiteral("transcript.large_chunk"), chunk, sentAt, error)) {
            database_.rollback();
            return -1;
        }
        QString projectionError;
        if (!updateReplayProjection(
                sessionId, generation, cursor, turnSequence,
                QStringLiteral("transcript.large_chunk"),
                QString::fromUtf8(QJsonDocument(chunk).toJson(QJsonDocument::Compact)), sentAt,
                &projectionError)) {
            database_.rollback();
            if (error != nullptr) {
                *error = projectionError;
            }
            return -1;
        }
    }

    const QJsonObject reference{{QStringLiteral("version"), 1},
                                {QStringLiteral("transfer_id"), transferId},
                                {QStringLiteral("item_id"), itemId},
                                {QStringLiteral("revision"), revision},
                                {QStringLiteral("event_type"), type},
                                {QStringLiteral("encoding"), QStringLiteral("json-utf8")},
                                {QStringLiteral("total_bytes"), totalBytes},
                                {QStringLiteral("chunk_count"), chunkCount},
                                {QStringLiteral("chunk_bytes"), cloudChunkBytes},
                                {QStringLiteral("digest_sha256"), digest}};
    const QJsonObject manifest{{QStringLiteral("item_id"), itemId},
                               {QStringLiteral("item_type"), itemType},
                               {QStringLiteral("large_event"), reference}};
    if (!insertEvent(database_, sessionId, generation, cursor, turnSequence, type, manifest, sentAt,
                     error)) {
        database_.rollback();
        return -1;
    }
    QString projectionError;
    if (!updateActivityGroups(sessionId, generation, cursor, turnSequence, type,
                              QJsonDocument::fromJson(encodedPayload).object(), &projectionError)) {
        database_.rollback();
        if (error)
            *error = projectionError;
        return -1;
    }
    if (!updateReplayProjection(
            sessionId, generation, cursor, turnSequence, type,
            QString::fromUtf8(QJsonDocument(manifest).toJson(QJsonDocument::Compact)), sentAt,
            &projectionError)) {
        database_.rollback();
        if (error != nullptr) {
            *error = projectionError;
        }
        return -1;
    }
    QSqlQuery update(database_);
    update.prepare(QStringLiteral("UPDATE sessions SET last_cursor = ?, current_turn_sequence = ? "
                                  "WHERE id = ? AND generation = ?"));
    update.addBindValue(cursor);
    update.addBindValue(turnSequence);
    update.addBindValue(sessionId);
    update.addBindValue(generation);
    if (!update.exec() || !database_.commit()) {
        database_.rollback();
        if (error != nullptr) {
            *error = update.lastError().text().isEmpty() ? database_.lastError().text()
                                                         : update.lastError().text();
        }
        return -1;
    }
    return cursor;
}

LargeEventFragment LocalJournal::largeEventFragment(const QString& sessionId,
                                                    const qint64 generation,
                                                    const QString& transferId, const qint64 offset,
                                                    const int maximumBytes) const {
    LargeEventFragment result;
    result.transferId = transferId;
    if (transferId.isEmpty() || transferId.size() > 128 || offset < 0 || maximumBytes < 1 ||
        maximumBytes > 128 * 1024) {
        result.errorCode = QStringLiteral("invalid_large_event_fragment");
        result.errorMessage = QStringLiteral("Invalid large-event fragment boundary.");
        return result;
    }
    QSqlQuery query(database_);
    // Metadata only: never select the payload column here, so a fragment read does not load the
    // whole blob. Legacy rows (total_bytes < 0) fall back to substr below.
    query.prepare(
        QStringLiteral("SELECT event_type, item_id, revision, digest_sha256, total_bytes "
                       "FROM large_event_payloads WHERE session_id = ? AND generation = ? "
                       "AND transfer_id = ?"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(transferId);
    if (!query.exec()) {
        result.errorCode = QStringLiteral("journal_error");
        result.errorMessage = query.lastError().text();
        setError(result.errorMessage);
        return result;
    }
    if (!query.next()) {
        result.errorCode = QStringLiteral("large_event_not_found");
        result.errorMessage = QStringLiteral("The large transcript item is not retained.");
        return result;
    }
    result.eventType = query.value(0).toString();
    result.itemId = query.value(1).toString();
    result.revision = query.value(2).toLongLong();
    result.digestSha256 = query.value(3).toString();
    result.totalBytes = query.value(4).toLongLong();
    const bool legacy = result.totalBytes < 0;
    if (legacy) {
        QSqlQuery length(database_);
        length.prepare(QStringLiteral("SELECT length(payload) FROM large_event_payloads WHERE "
                                      "session_id = ? AND generation = ? AND transfer_id = ?"));
        length.addBindValue(sessionId);
        length.addBindValue(generation);
        length.addBindValue(transferId);
        if (!length.exec() || !length.next()) {
            result.errorCode = QStringLiteral("journal_error");
            result.errorMessage = length.lastError().text();
            setError(result.errorMessage);
            return result;
        }
        result.totalBytes = length.value(0).toLongLong();
    }
    if (offset >= result.totalBytes) {
        result.errorCode = QStringLiteral("invalid_large_event_fragment");
        result.errorMessage = QStringLiteral("Large-event offset is out of range.");
        return result;
    }
    result.offset = offset;
    QSqlQuery bytes(database_);
    if (legacy) {
        bytes.prepare(QStringLiteral(
            "SELECT 0, substr(payload, ?, ?) FROM large_event_payloads WHERE session_id = ? AND "
            "generation = ? AND transfer_id = ?"));
        bytes.addBindValue(offset + 1);
        bytes.addBindValue(maximumBytes);
    } else {
        // The fragment may end at a stored slice boundary; callers continue from next_offset.
        bytes.prepare(QStringLiteral(
            "SELECT chunk_offset, payload FROM large_event_payload_chunks WHERE session_id = ? "
            "AND generation = ? AND transfer_id = ? AND chunk_offset <= ? "
            "ORDER BY chunk_offset DESC LIMIT 1"));
    }
    bytes.addBindValue(sessionId);
    bytes.addBindValue(generation);
    bytes.addBindValue(transferId);
    if (!legacy)
        bytes.addBindValue(offset);
    if (!bytes.exec()) {
        result.errorCode = QStringLiteral("journal_error");
        result.errorMessage = bytes.lastError().text();
        setError(result.errorMessage);
        return result;
    }
    if (bytes.next()) {
        const QByteArray slice = bytes.value(1).toByteArray();
        result.data =
            legacy ? slice : slice.mid(offset - bytes.value(0).toLongLong(), maximumBytes);
    }
    result.nextOffset = offset + result.data.size();
    result.complete = result.nextOffset == result.totalBytes;
    if (result.data.isEmpty() || result.nextOffset > result.totalBytes) {
        result.errorCode = QStringLiteral("large_event_corrupt");
        result.errorMessage = QStringLiteral("The retained large transcript item is corrupt.");
    }
    return result;
}

} // namespace ketplus::daemon
