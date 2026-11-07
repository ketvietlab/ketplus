#include "storage/LocalJournal.h"
#include <QJsonDocument>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {
bool LocalJournal::containsMediaReference(const QString& sessionId, qint64 generation,
                                          const QString& reference) const {
    if (reference.size() != 71 || !reference.startsWith("sha256:"))
        return false;
    // replay_events is the durable transcript; events is only the unacknowledged outbox.
    QSqlQuery query(database_);
    // A tool's result content, or the images a message was sent with.
    query.prepare(
        "SELECT 1 FROM replay_events, "
        "json_each(COALESCE(json_extract(replay_events.payload,'$.item.result.content'),"
        "json_extract(replay_events.payload,'$.images'))) AS media "
        "WHERE session_id=? AND generation=? AND json_extract(media.value,'$.media_ref')=? "
        "AND json_extract(media.value,'$.type')='image' LIMIT 1");
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(reference);
    return query.exec() && query.next();
}
QJsonObject LocalJournal::commandOutput(const QString& sessionId, qint64 generation,
                                        qint64 turnSequence, const QString& itemId) const {
    // Select only the requested item, inside its generation and turn, from the durable
    // replay_events (events empties once the cloud acknowledges). JSON extraction bounds
    // materialization even when a retained provider item contains a huge log.
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT event_type, substr(COALESCE(json_extract(payload,'$.item.aggregatedOutput'),"
        "json_extract(payload,'$.aggregatedOutput'),json_extract(payload,'$.output'),"
        "json_extract(payload,'$.delta'),''),-65536),"
        "length(CAST(COALESCE(json_extract(payload,'$.item.aggregatedOutput'),"
        "json_extract(payload,'$.aggregatedOutput'),json_extract(payload,'$.output'),"
        "json_extract(payload,'$.delta'),'') AS BLOB)),"
        "json_extract(payload,'$.item.exitCode'), json_extract(payload,'$.item.compacted') FROM "
        "replay_events "
        "WHERE session_id=? AND generation=? AND item_id=? AND turn_sequence=? "
        "AND (event_type='command.output_delta' OR "
        "json_extract(payload,'$.item.type')='commandExecution' OR "
        "json_extract(payload,'$.item_type')='commandExecution') ORDER BY cursor DESC LIMIT 257"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(itemId);
    query.addBindValue(turnSequence);
    if (!query.exec())
        return {{"ok", false}, {"error_code", "output_unavailable"}};
    QByteArray output;
    bool found = false;
    bool truncated = false;
    QJsonValue exitCode;
    QJsonObject compacted;
    int count = 0;
    while (query.next()) {
        found = true;
        if (++count > 256) {
            truncated = true;
            break;
        }
        const QString type = query.value(0).toString();
        const QByteArray bytes = query.value(1).toString().toUtf8();
        truncated |= query.value(2).toLongLong() > bytes.size();
        if (!query.value(3).isNull())
            exitCode = query.value(3).toInt();
        if (type != "command.output_delta" && !bytes.isEmpty()) {
            compacted = QJsonDocument::fromJson(query.value(4).toByteArray()).object();
            truncated |= !compacted.isEmpty();
            output = bytes;
            break; // Authoritative completed output supersedes streaming deltas.
        }
        if (type == "command.output_delta")
            output.prepend(bytes);
        if (output.size() > 64 * 1024) {
            truncated = true;
            break;
        }
    }
    if (!found)
        return {{"ok", false}, {"error_code", "output_unavailable"}};
    if (output.size() > 64 * 1024) {
        output = output.right(64 * 1024);
        truncated = true;
    }
    auto lines = QString::fromUtf8(output).split('\n');
    if (lines.size() > 40) {
        lines = lines.mid(lines.size() - 40);
        truncated = true;
    }
    QString text = lines.join('\n');
    if (!compacted.isEmpty() && !text.startsWith(QStringLiteral("[Output trimmed"))) {
        // Reserve one of the advertised 40 lines for the retention notice.
        if (lines.size() >= 40)
            text = lines.mid(lines.size() - 39).join('\n');
        text.prepend(
            QStringLiteral("[Output trimmed · original size %1 bytes · history retention]\n")
                .arg(compacted.value(QStringLiteral("original_bytes")).toInteger()));
    }
    return {{"ok", true},
            {"item_id", itemId},
            {"turn_sequence", turnSequence},
            {"text", text},
            {"compacted", compacted},
            {"truncated", truncated},
            {"exit_code", exitCode},
            {"maximum_bytes", 64 * 1024},
            {"maximum_lines", 40}};
}
} // namespace ketplus::daemon
