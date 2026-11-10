#include "protocol/DaemonProtocol.h"
#include "storage/LocalJournal.h"
#include "storage/ReplayDetailCompactor.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <optional>

namespace ketplus::daemon {
namespace {
// Maintenance yields to another writer instead of spending the normal busy timeout on
// the daemon thread. Restore the connection policy on success and every failure path.
class NoBusyWait final {
  public:
    explicit NoBusyWait(const QSqlDatabase& database) : database_(database) {
        QSqlQuery query(database_);
        if (query.exec(QStringLiteral("PRAGMA busy_timeout")) && query.next()) {
            timeout_ = query.value(0).toInt();
            query.finish();
            ready_ = query.exec(QStringLiteral("PRAGMA busy_timeout=0"));
        }
    }
    ~NoBusyWait() {
        if (timeout_ >= 0) {
            QSqlQuery query(database_);
            query.exec(QStringLiteral("PRAGMA busy_timeout=%1").arg(timeout_));
        }
    }
    bool ready() const { return ready_; }

  private:
    QSqlDatabase database_;
    int timeout_{-1};
    bool ready_{false};
};

// A final tool snapshot replaces presentation, but unknown start-only fields may still
// contain useful data. Only remove a subset, allowing empty known progress placeholders.
bool supersededStart(const QJsonObject& start, const QJsonObject& completed) {
    const auto first = start.value("item").toObject();
    const auto last = completed.value("item").toObject();
    const auto type = first.value("type").toString();
    if ((type != "commandExecution" && type != "mcpToolCall" && type != "dynamicToolCall" &&
         type != "fileChange") ||
        first.value("id").toString().isEmpty() || first.value("id") != last.value("id") ||
        first.value("type") != last.value("type") ||
        (last.value("status") != "completed" && last.value("status") != "failed") ||
        first.contains("compacted") || last.contains("compacted"))
        return false;
    for (auto it = start.begin(); it != start.end(); ++it)
        if (it.key() != "item" && it.value() != completed.value(it.key()))
            return false;
    for (auto it = first.begin(); it != first.end(); ++it) {
        if (it.key() == "status" || it.value() == last.value(it.key()))
            continue;
        const auto value = it.value();
        const bool empty = value.isNull() || (value.isString() && value.toString().isEmpty()) ||
                           (value.isArray() && value.toArray().isEmpty()) ||
                           (value.isObject() && value.toObject().isEmpty());
        const auto field = it.key();
        const bool progress =
            type == "commandExecution"
                ? field == "aggregatedOutput" || field == "exitCode" || field == "durationMs"
            : type == "fileChange" ? field == "changes"
                                   : field == "arguments" || field == "result" ||
                                         field == "error" || field == "contentItems";
        if (!empty || !progress)
            return false;
    }
    return true;
}

// The caller holds the retention transaction. Never hydrate legacy/oversize blobs or
// allocate an unbounded corrupt chunk; the existing pass budget includes these bytes.
std::optional<QByteArray> boundedLargePayload(const QSqlDatabase& database, const QString& session,
                                              qint64 generation, const QJsonObject& manifest,
                                              qint64 total, QString* error) {
    const auto ref = manifest.value("large_event").toObject();
    if (manifest.size() != 3 || ref.size() != 10 || ref.value("version") != QJsonValue(1) ||
        ref.value("encoding") != "json-utf8" || ref.value("event_type") != "item.completed" ||
        ref.value("total_bytes") != QJsonValue(total) ||
        ref.value("chunk_bytes") != QJsonValue(96 * 1024) ||
        ref.value("chunk_count") != QJsonValue((total + 96 * 1024 - 1) / (96 * 1024)))
        return std::nullopt;
    QSqlQuery metadata(database);
    metadata.prepare(
        "SELECT item_id,revision,digest_sha256 FROM large_event_payloads WHERE "
        "session_id=? AND generation=? AND transfer_id=? AND event_type='item.completed' "
        "AND total_bytes=?");
    metadata.addBindValue(session);
    metadata.addBindValue(generation);
    metadata.addBindValue(ref.value("transfer_id").toString());
    metadata.addBindValue(total);
    if (!metadata.exec()) {
        *error = metadata.lastError().text();
        return std::nullopt;
    }
    if (!metadata.next())
        return std::nullopt;
    const auto itemId = metadata.value(0).toString();
    const auto digest = metadata.value(2).toString();
    if (itemId.isEmpty() || manifest.value("item_id") != itemId || ref.value("item_id") != itemId ||
        ref.value("revision") != QJsonValue(metadata.value(1).toLongLong()) ||
        ref.value("digest_sha256") != digest)
        return std::nullopt;
    metadata.finish();
    QSqlQuery chunks(database);
    chunks.prepare(
        "SELECT chunk_offset,CASE WHEN length(CAST(payload AS BLOB))<=131072 "
        "THEN CAST(payload AS BLOB) ELSE NULL END FROM large_event_payload_chunks "
        "WHERE session_id=? AND generation=? AND transfer_id=? ORDER BY chunk_offset LIMIT ?");
    chunks.addBindValue(session);
    chunks.addBindValue(generation);
    chunks.addBindValue(ref.value("transfer_id").toString());
    chunks.addBindValue(ref.value("chunk_count").toInteger() + 1); // one extra detects surplus
    if (!chunks.exec()) {
        *error = chunks.lastError().text();
        return std::nullopt;
    }
    QByteArray bytes;
    while (chunks.next()) {
        const auto chunk = chunks.value(1).toByteArray();
        if (chunks.value(0).toLongLong() != bytes.size() || chunk.isEmpty() ||
            chunk.size() > total - bytes.size())
            return std::nullopt;
        bytes += chunk;
    }
    if (chunks.lastError().isValid()) {
        *error = chunks.lastError().text();
        return std::nullopt;
    }
    if (bytes.size() != total ||
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != digest.toLatin1())
        return std::nullopt;
    const auto payload = QJsonDocument::fromJson(bytes).object();
    const auto item = payload.value("item").toObject();
    if (payload.contains("large_event") || item.value("id") != itemId ||
        item.value("type") != manifest.value("item_type"))
        return std::nullopt;
    return bytes;
}

// A generated image or tool result is read whole to drop its inline pixels; larger ones stay.
constexpr qint64 maximumImagePayloadBytes = 16 * 1024 * 1024;
// Chunk bytes the image lane may probe or read in one pass: one tool payload probed and read.
constexpr qint64 maximumImageLaneBytesPerPass = 2 * maximumImagePayloadBytes;

struct Row {
    qint64 rowid;
    QString session;
    qint64 generation;
    QString type;
    qint64 turn;
};
} // namespace

std::optional<qint64> LocalJournal::pruneReplayDetails(const QDateTime& olderThan,
                                                       const int maximumRows) {
    setError({});
    if (!olderThan.isValid() || olderThan > QDateTime::currentDateTimeUtc() || maximumRows < 1 ||
        maximumRows > 256) {
        setError(QStringLiteral("Invalid detail retention boundary."));
        return std::nullopt;
    }
    NoBusyWait noWait(database_);
    if (!noWait.ready() || !database_.transaction()) {
        setError(database_.lastError().text());
        return std::nullopt;
    }
    const auto fail = [&](const QString& error) -> std::optional<qint64> {
        database_.rollback();
        setError(error);
        return std::nullopt;
    };

    // Bound rows examined, not just rows deleted: even a journal with no eligible samples
    // makes progress without rescanning all its history on each timer tick. A restart or
    // a completed sweep starts again; recent/unacknowledged rows are reconsidered later.
    QSqlQuery scan(database_);
    scan.prepare(QStringLiteral("SELECT rowid, session_id, generation, event_type, "
                                "turn_sequence FROM replay_events WHERE rowid > ? "
                                "ORDER BY rowid LIMIT ?"));
    scan.addBindValue(detailRetentionScanRow_);
    scan.addBindValue(maximumRows);
    if (!scan.exec())
        return fail(scan.lastError().text());
    QList<Row> rows;
    while (scan.next())
        rows.append({scan.value(0).toLongLong(), scan.value(1).toString(),
                     scan.value(2).toLongLong(), scan.value(3).toString(),
                     scan.value(4).toLongLong()});
    if (scan.lastError().isValid())
        return fail(scan.lastError().text());
    scan.finish();

    const QString turnEligible = QStringLiteral(
        "r.rowid = ? AND r.turn_sequence > 0 "
        "AND EXISTS (SELECT 1 FROM sessions s WHERE s.id = r.session_id "
        "AND s.generation = r.generation AND s.state IN ('ready','failed','interrupted','exited')) "
        // Count actual newer turns, not sequence arithmetic (forks can leave gaps).
        "AND (SELECT count(*) FROM (SELECT DISTINCT t.turn_sequence FROM replay_events t "
        "WHERE t.session_id = r.session_id AND t.generation = r.generation "
        "AND t.turn_sequence > r.turn_sequence LIMIT 20)) = 20 "
        // The entire turn must be acknowledged and old, not only the sample being removed.
        "AND NOT EXISTS (SELECT 1 FROM events e WHERE e.session_id = r.session_id "
        "AND e.generation = r.generation AND e.turn_sequence = r.turn_sequence) "
        "AND (SELECT julianday(t.sent_at) FROM replay_events t WHERE t.session_id = r.session_id "
        "AND t.generation = r.generation AND t.turn_sequence = r.turn_sequence "
        "ORDER BY t.cursor DESC LIMIT 1) < julianday(?) "
        "AND (SELECT t.event_type FROM replay_events t WHERE t.session_id = r.session_id "
        "AND t.generation = r.generation AND t.turn_sequence = r.turn_sequence "
        "AND t.event_type IN ('turn.started','turn.completed') ORDER BY t.cursor DESC LIMIT 1) "
        "= 'turn.completed' "
        // Conservative until retention owns approval resolution: preserve every turn that
        // contains a question/approval, even when it may already have been answered.
        "AND NOT EXISTS (SELECT 1 FROM replay_events t WHERE t.session_id = r.session_id "
        "AND t.generation = r.generation AND t.turn_sequence = r.turn_sequence "
        "AND (t.event_type LIKE 'approval.%' OR t.event_type LIKE 'question.%')) ");
    // turnEligible depends only on the row's turn. A batch's rows mostly share a few turns, so
    // the gate below evaluates it once per turn, and the lanes run only for rows that passed
    // it: evaluating it per row and per lane made a sweep of a million-row journal take many
    // minutes. Nothing in this transaction changes a turn's eligibility (rows removed here
    // always have a later row in the same turn, and turn, question and approval rows stay),
    // so the lanes check only the row itself.
    const QString rowEligible = QStringLiteral("r.rowid = ? ");
    const QString eligible = rowEligible + "AND json_type(r.payload, '$.large_event') IS NULL ";
    QSqlQuery turnGate(database_);
    turnGate.prepare(QStringLiteral("SELECT 1 FROM replay_events AS r WHERE ") + turnEligible);
    QHash<QPair<QString, QPair<qint64, qint64>>, bool> turnEligibility;
    std::optional<QString> gateError;
    const auto turnIsEligible = [&](const Row& row) {
        if (row.turn <= 0)
            return false;
        const auto key = qMakePair(row.session, qMakePair(row.generation, row.turn));
        if (const auto known = turnEligibility.constFind(key); known != turnEligibility.cend())
            return *known;
        turnGate.bindValue(0, row.rowid);
        turnGate.bindValue(1, olderThan.toUTC().toString(Qt::ISODateWithMs));
        if (!turnGate.exec()) {
            gateError = turnGate.lastError().text();
            return false;
        }
        const bool result = turnGate.next();
        turnGate.finish();
        turnEligibility.insert(key, result);
        return result;
    };
    QSqlQuery largeCandidate(database_);
    largeCandidate.prepare(
        "SELECT r.payload,p.total_bytes FROM replay_events r JOIN large_event_payloads p ON "
        "p.session_id=r.session_id AND p.generation=r.generation AND p.item_id=r.item_id AND "
        "p.transfer_id=json_extract(r.payload,'$.large_event.transfer_id') WHERE " +
        rowEligible +
        "AND p.total_bytes>0 AND p.total_bytes<=1048576 AND length(CAST(r.payload AS BLOB))<=16384 "
        // Requiring the whole session to be acknowledged keeps orphan cleanup bounded even
        // when another turn has a huge raw backlog. This lane deliberately defers that session.
        "AND NOT EXISTS (SELECT 1 FROM events e WHERE e.session_id=r.session_id AND "
        "e.generation=r.generation) "
        "AND json_extract(r.payload,'$.item_type') IN "
        "('commandExecution','mcpToolCall','dynamicToolCall','reasoning','fileChange')");
    // Generated images and tool results with inline images are larger than the shared lane
    // allows: one per pass, read whole.
    QSqlQuery imageCandidate(database_);
    imageCandidate.prepare(
        "SELECT r.payload,p.total_bytes FROM replay_events r JOIN large_event_payloads p ON "
        "p.session_id=r.session_id AND p.generation=r.generation AND p.item_id=r.item_id AND "
        "p.transfer_id=json_extract(r.payload,'$.large_event.transfer_id') WHERE " +
        rowEligible +
        "AND p.total_bytes>0 AND p.total_bytes<=? AND length(CAST(r.payload AS BLOB))<=16384 "
        "AND NOT EXISTS (SELECT 1 FROM events e WHERE e.session_id=r.session_id AND "
        "e.generation=r.generation) "
        "AND json_extract(r.payload,'$.item_type') IN "
        "('imageGeneration','mcpToolCall','dynamicToolCall')");
    // A tool result qualifies only if a chunk holds an image block; larger text-only results
    // stay outside every lane (ADR 0047). The probe reads the payload's chunks, so it is
    // charged to the pass's image budget before it runs, whatever it finds.
    QSqlQuery imageProbe(database_);
    imageProbe.prepare(
        "SELECT 1 FROM large_event_payload_chunks WHERE session_id=? AND generation=? AND "
        "transfer_id=? AND (instr(CAST(payload AS BLOB),CAST('\"type\":\"image\"' AS BLOB))>0 "
        "OR instr(CAST(payload AS BLOB),CAST('\"type\":\"inputImage\"' AS BLOB))>0) LIMIT 1");
    QSqlQuery remove(database_);
    remove.prepare(
        QStringLiteral("DELETE FROM replay_events AS r WHERE ") + eligible +
        QStringLiteral(
            "AND EXISTS (SELECT 1 FROM replay_events newer WHERE newer.session_id = r.session_id "
            "AND newer.generation = r.generation AND newer.turn_sequence = r.turn_sequence "
            "AND newer.event_type = r.event_type AND newer.cursor > r.cursor)"));
    QSqlQuery candidate(database_);
    // Ordinary payloads and the bounded manifest lane share the same parsing budget.
    candidate.prepare(
        QStringLiteral("SELECT r.payload FROM replay_events AS r WHERE ") + eligible +
        QStringLiteral(
            "AND length(CAST(r.payload AS BLOB)) <= 1048576 "
            "AND json_extract(r.payload,'$.item.type') IN "
            "('commandExecution','mcpToolCall','dynamicToolCall','reasoning','fileChange',"
            "'imageGeneration') "
            "AND (json_type(r.payload,'$.item.compacted') IS NULL OR "
            "(json_extract(r.payload,'$.item.compacted.policy')='tool-result-text-v1' "
            "AND json_type(r.payload,'$.item.compacted.arguments') IS NULL "
            "AND json_type(r.payload,'$.item.arguments_excerpt') IS NULL "
            "AND length(CAST(json_extract(r.payload,'$.item.arguments') AS BLOB))>5120) OR "
            "(json_extract(r.payload,'$.item.compacted.policy') IN "
            "('tool-result-text-v1','tool-arguments-v1') "
            "AND json_type(r.payload,'$.item.compacted.structured_result') IS NULL "
            "AND json_type(r.payload,'$.item.result.structuredContent_excerpt') IS NULL "
            "AND length(CAST(json_extract(r.payload,'$.item.result.structuredContent') AS BLOB))"
            ">5120) OR "
            // Already trimmed, but its inline images predate this policy (ADR 0099). A textual
            // prefilter: the compactor decides and leaves anything else unchanged.
            "(json_type(r.payload,'$.item.compacted.inline_images') IS NULL "
            "AND (instr(r.payload,'\"type\":\"image\"')>0 OR "
            "instr(r.payload,'\"type\":\"inputImage\"')>0)))"));
    QSqlQuery startCandidate(database_);
    // The next retained row must be this item's same-turn completion. Keeping
    // interleaved starts preserves the first-seen ordering used by every reader.
    startCandidate.prepare(
        QStringLiteral(
            "SELECT r.payload,c.payload FROM replay_events AS r JOIN replay_events c "
            "ON c.session_id=r.session_id AND c.generation=r.generation AND "
            "c.turn_sequence=r.turn_sequence AND c.cursor=(SELECT t.cursor FROM replay_events t "
            "WHERE t.session_id=r.session_id AND t.generation=r.generation AND "
            "t.cursor>r.cursor ORDER BY t.cursor LIMIT 1) "
            "WHERE ") +
        eligible +
        QStringLiteral(
            "AND r.item_id<>'' AND c.item_id=r.item_id AND c.event_type='item.completed' "
            "AND json_extract(r.payload,'$.item.type') IN "
            "('commandExecution','mcpToolCall','dynamicToolCall','fileChange') "
            "AND json_type(c.payload,'$.large_event') IS NULL "
            "AND length(CAST(r.payload AS BLOB))+length(CAST(c.payload AS BLOB))<=1048576"));
    QSqlQuery removeStart(database_);
    removeStart.prepare("DELETE FROM replay_events WHERE rowid=? AND event_type='item.started'");
    QSqlQuery update(database_);
    update.prepare(QStringLiteral("UPDATE replay_events SET payload = ? WHERE rowid = ?"));
    qint64 parsedBytes = 0;
    bool imageRead = false;
    // Decoding an image for its preview costs the same whatever its stored size, so one pass
    // decodes at most one, inline or large (ADR 0101).
    bool previewDecoded = false;
    qint64 imageLaneBytes = 0; // chunk bytes probed or read by the image lane this pass
    qsizetype processed = 0;
    QSet<QPair<QString, qint64>> changed;
    qint64 removed = 0;
    for (; processed < rows.size(); ++processed) {
        const Row& row = rows.at(processed);
        if (row.type != QStringLiteral("item.started") &&
            row.type != QStringLiteral("item.completed") &&
            row.type != QStringLiteral("usage.context") &&
            row.type != QStringLiteral("usage.limits") &&
            row.type != QStringLiteral("usage.session"))
            continue;
        if (!turnIsEligible(row)) {
            if (gateError)
                return fail(*gateError);
            continue;
        }
        if (row.type == QStringLiteral("item.started")) {
            startCandidate.bindValue(0, row.rowid);
            if (!startCandidate.exec())
                return fail(startCandidate.lastError().text());
            if (!startCandidate.next()) {
                if (startCandidate.lastError().isValid())
                    return fail(startCandidate.lastError().text());
                startCandidate.finish();
                continue;
            }
            const auto first = startCandidate.value(0).toByteArray();
            const auto last = startCandidate.value(1).toByteArray();
            startCandidate.finish();
            if (parsedBytes + first.size() + last.size() > 1024 * 1024)
                break;
            parsedBytes += first.size() + last.size();
            if (!supersededStart(QJsonDocument::fromJson(first).object(),
                                 QJsonDocument::fromJson(last).object()))
                continue;
            removeStart.bindValue(0, row.rowid);
            if (!removeStart.exec())
                return fail(removeStart.lastError().text());
            if (removeStart.numRowsAffected() != 1)
                return fail(QStringLiteral("Missing replay start during detail retention."));
            removeStart.finish();
            ++removed;
            changed.insert({row.session, row.generation});
            continue;
        }
        if (row.type == QStringLiteral("item.completed")) {
            candidate.bindValue(0, row.rowid);
            if (!candidate.exec())
                return fail(candidate.lastError().text());
            QByteArray bytes;
            QString transfer;
            bool imageBytes = false; // counted against the per-pass image allowance instead
            if (candidate.next()) {
                bytes = candidate.value(0).toByteArray();
                candidate.finish();
            } else {
                if (candidate.lastError().isValid())
                    return fail(candidate.lastError().text());
                candidate.finish();
                largeCandidate.bindValue(0, row.rowid);
                if (!largeCandidate.exec())
                    return fail(largeCandidate.lastError().text());
                QSqlQuery* lane = &largeCandidate;
                if (!largeCandidate.next()) {
                    if (largeCandidate.lastError().isValid())
                        return fail(largeCandidate.lastError().text());
                    largeCandidate.finish();
                    imageCandidate.bindValue(0, row.rowid);
                    imageCandidate.bindValue(1, maximumImagePayloadBytes);
                    if (!imageCandidate.exec())
                        return fail(imageCandidate.lastError().text());
                    if (!imageCandidate.next()) {
                        if (imageCandidate.lastError().isValid())
                            return fail(imageCandidate.lastError().text());
                        imageCandidate.finish();
                        continue;
                    }
                    lane = &imageCandidate;
                }
                const bool image = lane == &imageCandidate;
                const auto manifestBytes = lane->value(0).toByteArray();
                const auto total = lane->value(1).toLongLong();
                lane->finish();
                const auto cost = manifestBytes.size() + total;
                if (image) {
                    const auto manifest = QJsonDocument::fromJson(manifestBytes).object();
                    const bool tool = manifest.value("item_type") != "imageGeneration";
                    if (!tool && generatedImages_ == JournalStorageOptions::GeneratedImages::Full)
                        continue; // "Keep full image": do not even read the pixels
                    // A tool payload may be probed and then read: both must fit. The first
                    // candidate always fits, so every pass makes progress.
                    if (imageRead ||
                        imageLaneBytes + (tool ? 2 : 1) * total > maximumImageLaneBytesPerPass)
                        break; // the next pass takes it
                    if (tool) {
                        imageLaneBytes += total;
                        imageProbe.bindValue(0, row.session);
                        imageProbe.bindValue(1, row.generation);
                        imageProbe.bindValue(2, manifest.value("large_event")
                                                    .toObject()
                                                    .value("transfer_id")
                                                    .toString());
                        if (!imageProbe.exec())
                            return fail(imageProbe.lastError().text());
                        const bool hasImage = imageProbe.next();
                        if (imageProbe.lastError().isValid())
                            return fail(imageProbe.lastError().text());
                        imageProbe.finish();
                        if (!hasImage)
                            continue;
                    }
                    imageLaneBytes += total;
                    imageRead = true;
                } else {
                    if (cost > 1024 * 1024)
                        continue; // never stall on an individually oversized item
                    if (parsedBytes + cost > 1024 * 1024)
                        break;
                    parsedBytes += manifestBytes.size();
                }
                const auto manifest = QJsonDocument::fromJson(manifestBytes).object();
                QString readError;
                const auto hydrated = boundedLargePayload(database_, row.session, row.generation,
                                                          manifest, total, &readError);
                if (!readError.isEmpty())
                    return fail(readError);
                if (!hydrated) {
                    if (!image)
                        parsedBytes += total;
                    continue;
                }
                bytes = *hydrated;
                transfer = manifest.value("large_event").toObject().value("transfer_id").toString();
                imageBytes = image;
            }
            if (!imageBytes) {
                if (parsedBytes + bytes.size() > 1024 * 1024)
                    break; // leave this candidate for the next batch
                parsedBytes += bytes.size();
            }
            auto payload = QJsonDocument::fromJson(bytes).object();
            const auto original = payload.value(QStringLiteral("item")).toObject();
            // "Keep full image" leaves generated pictures alone; tool results are unaffected.
            if (generatedImages_ == JournalStorageOptions::GeneratedImages::Full &&
                original.value(QStringLiteral("type")) == QStringLiteral("imageGeneration"))
                continue;
            const bool previews =
                imagePreview_ &&
                generatedImages_ != JournalStorageOptions::GeneratedImages::Caption;
            // A preview decodes the whole picture, so its cost does not follow the stored size.
            // Once one item has decoded, a later item that wants a preview waits for the next
            // pass; an item's own images (at most four) are decoded together.
            bool decodedHere = false;
            bool deferred = false;
            const auto item = compactCompletedReplayItem(
                original, olderThan,
                previews ? ImagePreview([&, session = row.session](const QByteArray& image) {
                    if (previewDecoded && !decodedHere) {
                        deferred = true;
                        return QJsonObject();
                    }
                    decodedHere = true;
                    return imagePreview_(image, session);
                })
                         : ImagePreview());
            if (deferred)
                break; // the next pass takes it
            previewDecoded = previewDecoded || decodedHere;
            if (!item)
                continue;
            payload.insert(QStringLiteral("item"), *item);
            const auto compacted = QJsonDocument(payload).toJson(QJsonDocument::Compact);
            // Unknown/duplicated top-level detail must not be discarded to make the output fit.
            if (!transfer.isEmpty() && (compacted.size() > protocol::maximumEventPayloadBytes ||
                                        compacted.size() >= bytes.size()))
                continue;
            update.bindValue(0, QString::fromUtf8(compacted));
            update.bindValue(1, row.rowid);
            if (!update.exec())
                return fail(update.lastError().text());
            if (update.numRowsAffected() != 1)
                return fail(QStringLiteral("Missing replay item during detail retention."));
            update.finish();
            if (!transfer.isEmpty() &&
                !pruneUnreferencedLargePayloads(row.session, row.generation, {transfer}))
                return fail(error_);
            ++removed;
            changed.insert({row.session, row.generation});
            continue;
        }
        if (row.type != QStringLiteral("usage.context") &&
            row.type != QStringLiteral("usage.limits") &&
            row.type != QStringLiteral("usage.session"))
            continue;
        remove.bindValue(0, row.rowid);
        if (!remove.exec())
            return fail(remove.lastError().text());
        if (remove.numRowsAffected() > 0) {
            removed += remove.numRowsAffected();
            changed.insert({row.session, row.generation});
        }
        remove.finish();
    }
    for (const auto& session : changed) {
        QSqlQuery revision(database_);
        revision.prepare(
            QStringLiteral("UPDATE replay_projection_state SET revision = revision + 1 "
                           "WHERE session_id = ? AND generation = ?"));
        revision.addBindValue(session.first);
        revision.addBindValue(session.second);
        if (!revision.exec())
            return fail(revision.lastError().text());
        if (revision.numRowsAffected() != 1)
            return fail(QStringLiteral("Missing replay projection state during detail retention."));
    }
    if (!database_.commit())
        return fail(database_.lastError().text());
    if (processed < rows.size())
        detailRetentionScanRow_ =
            processed > 0 ? rows.at(processed - 1).rowid : detailRetentionScanRow_;
    else
        detailRetentionScanRow_ = rows.size() < maximumRows ? 0 : rows.last().rowid;
    return removed;
}
QJsonObject LocalJournal::turnDetailTrimmed(const QString& sessionId, qint64 generation,
                                            qint64 turnSequence) const {
    // Read from the compaction markers retention left on the turn's completions, so the count
    // always matches what the replay still holds. A diff counts each file it trimmed.
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT json_extract(payload,'$.item.type'),json_extract(payload,'$.item.compacted'),"
        "(SELECT count(*) FROM json_each(payload,'$.item.changes') "
        "WHERE json_type(value,'$.compacted')='object') "
        "FROM replay_events WHERE session_id=? AND generation=? AND turn_sequence=? AND "
        "event_type='item.completed' AND json_type(payload,'$.item.compacted')='object'"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(turnSequence);
    if (!query.exec())
        return {};
    const auto freed = [](const QJsonObject& marker) {
        return qMax<qint64>(0, marker.value("original_bytes").toInteger() -
                                   marker.value("retained_bytes").toInteger());
    };
    qint64 commands = 0, diffs = 0, results = 0, inputs = 0, thoughts = 0, bytes = 0, days = 0;
    while (query.next()) {
        const auto type = query.value(0).toString();
        const auto marker = QJsonDocument::fromJson(query.value(1).toByteArray()).object();
        const auto policy = marker.value("policy").toString();
        if (type == QStringLiteral("commandExecution") && policy == "command-output-v1") {
            ++commands;
            bytes += freed(marker);
        } else if (type == QStringLiteral("fileChange") && policy == "file-diff-v1") {
            diffs += query.value(2).toLongLong();
            bytes += freed(marker); // the item's marker sums its files
        } else if (type == QStringLiteral("reasoning") && policy == "reasoning-content-v1") {
            ++thoughts;
            bytes += freed(marker);
        } else if (type == QStringLiteral("mcpToolCall") ||
                   type == QStringLiteral("dynamicToolCall")) {
            // The item's marker is the result text's, or a copy of the one part it trimmed;
            // the arguments and the structured result keep their own.
            const auto structured = marker.value("structured_result").toObject();
            const auto arguments = marker.value("arguments").toObject();
            const bool text = policy == "tool-result-text-v1";
            if (text)
                bytes += freed(marker);
            if (!structured.isEmpty())
                bytes += freed(structured);
            if (!arguments.isEmpty())
                bytes += freed(arguments);
            results += text || !structured.isEmpty();
            inputs += !arguments.isEmpty();
        } else
            continue; // a policy this count does not know yet
        // The retention age in force: the cutoff is that many days before the compaction.
        const auto at = QDateTime::fromString(marker.value("at").toString(), Qt::ISODateWithMs);
        const auto cutoff =
            QDateTime::fromString(marker.value("cutoff").toString(), Qt::ISODateWithMs);
        if (days == 0 && at.isValid() && cutoff.isValid() && cutoff < at)
            days = qRound64(double(cutoff.secsTo(at)) / 86400.0);
    }
    if (commands + diffs + results + inputs + thoughts == 0)
        return {};
    QJsonObject trimmed{{"commands", commands},  {"diffs", diffs},       {"tool_results", results},
                        {"tool_inputs", inputs}, {"thoughts", thoughts}, {"bytes", bytes}};
    if (days > 0)
        trimmed.insert("older_than_days", days);
    return trimmed;
}
} // namespace ketplus::daemon
