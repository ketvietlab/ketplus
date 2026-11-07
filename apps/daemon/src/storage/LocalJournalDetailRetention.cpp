#include "storage/LocalJournal.h"

#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

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

struct Row {
    qint64 rowid;
    QString session;
    qint64 generation;
    QString type;
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
    scan.prepare(QStringLiteral("SELECT rowid, session_id, generation, event_type FROM "
                                "replay_events WHERE rowid > ? ORDER BY rowid LIMIT ?"));
    scan.addBindValue(detailRetentionScanRow_);
    scan.addBindValue(maximumRows);
    if (!scan.exec())
        return fail(scan.lastError().text());
    QList<Row> rows;
    while (scan.next())
        rows.append({scan.value(0).toLongLong(), scan.value(1).toString(),
                     scan.value(2).toLongLong(), scan.value(3).toString()});
    if (scan.lastError().isValid())
        return fail(scan.lastError().text());
    scan.finish();

    const QString eligible = QStringLiteral(
        "r.rowid = ? AND r.turn_sequence > 0 "
        // Large manifests retain their payload ownership; this pass never edits blobs.
        "AND json_type(r.payload, '$.large_event') IS NULL "
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
    QSqlQuery remove(database_);
    remove.prepare(
        QStringLiteral("DELETE FROM replay_events AS r WHERE ") + eligible +
        QStringLiteral(
            "AND EXISTS (SELECT 1 FROM replay_events newer WHERE newer.session_id = r.session_id "
            "AND newer.generation = r.generation AND newer.turn_sequence = r.turn_sequence "
            "AND newer.event_type = r.event_type AND newer.cursor > r.cursor)"));
    QSqlQuery candidate(database_);
    // Bound parsing independently of row count. Larger payloads and manifests need
    // the separate blob-retention path; never materialize them in this idle pass.
    candidate.prepare(
        QStringLiteral("SELECT r.payload FROM replay_events AS r WHERE ") + eligible +
        QStringLiteral("AND length(CAST(r.payload AS BLOB)) <= 1048576 "
                       "AND json_extract(r.payload,'$.item.type') = 'commandExecution' "
                       "AND json_extract(r.payload,'$.item.status') IN ('completed','failed') "
                       "AND json_type(r.payload,'$.item.compacted') IS NULL"));
    QSqlQuery update(database_);
    update.prepare(QStringLiteral("UPDATE replay_events SET payload = ? WHERE rowid = ?"));
    qint64 parsedBytes = 0;
    qsizetype processed = 0;
    QSet<QPair<QString, qint64>> changed;
    qint64 removed = 0;
    for (; processed < rows.size(); ++processed) {
        const Row& row = rows.at(processed);
        if (row.type == QStringLiteral("item.completed")) {
            candidate.bindValue(0, row.rowid);
            candidate.bindValue(1, olderThan.toUTC().toString(Qt::ISODateWithMs));
            if (!candidate.exec())
                return fail(candidate.lastError().text());
            if (!candidate.next()) {
                if (candidate.lastError().isValid())
                    return fail(candidate.lastError().text());
                candidate.finish();
                continue;
            }
            const QByteArray bytes = candidate.value(0).toByteArray();
            candidate.finish();
            if (parsedBytes + bytes.size() > 1024 * 1024)
                break; // leave this candidate for the next batch
            parsedBytes += bytes.size();
            auto payload = QJsonDocument::fromJson(bytes).object();
            auto item = payload.value(QStringLiteral("item")).toObject();
            const QByteArray output =
                item.value(QStringLiteral("aggregatedOutput")).toString().toUtf8();
            if (output.size() <= 21 * 1024)
                continue;
            // Never split a UTF-8 codepoint at either retained boundary.
            qsizetype head = 4 * 1024;
            while (head > 0 && (static_cast<unsigned char>(output.at(head)) & 0xc0) == 0x80)
                --head;
            qsizetype tail = output.size() - 16 * 1024;
            while (tail < output.size() &&
                   (static_cast<unsigned char>(output.at(tail)) & 0xc0) == 0x80)
                ++tail;
            const QString notice =
                QStringLiteral("[Output trimmed · original size %1 bytes · history retention]\n")
                    .arg(output.size());
            item.insert(QStringLiteral("aggregatedOutput"),
                        notice + QString::fromUtf8(output.first(head)) +
                            QStringLiteral("\n[Middle output removed]\n") +
                            QString::fromUtf8(output.sliced(tail)));
            item.insert(
                QStringLiteral("compacted"),
                QJsonObject{
                    {QStringLiteral("at"),
                     QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                    {QStringLiteral("cutoff"), olderThan.toUTC().toString(Qt::ISODateWithMs)},
                    {QStringLiteral("original_bytes"), output.size()},
                    {QStringLiteral("retained_bytes"), head + output.size() - tail},
                    {QStringLiteral("policy"), QStringLiteral("command-output-v1")}});
            payload.insert(QStringLiteral("item"), item);
            update.bindValue(
                0, QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
            update.bindValue(1, row.rowid);
            if (!update.exec())
                return fail(update.lastError().text());
            if (update.numRowsAffected() != 1)
                return fail(QStringLiteral("Missing replay item during detail retention."));
            update.finish();
            ++removed;
            changed.insert({row.session, row.generation});
            continue;
        }
        if (row.type != QStringLiteral("usage.context") &&
            row.type != QStringLiteral("usage.limits") &&
            row.type != QStringLiteral("usage.session"))
            continue;
        remove.bindValue(0, row.rowid);
        remove.bindValue(1, olderThan.toUTC().toString(Qt::ISODateWithMs));
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
} // namespace ketplus::daemon
