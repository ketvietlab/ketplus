#include "storage/LocalJournal.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

namespace ketplus::daemon {

// ADR 0106: one row per conversation generation holds its current work report. Retention may
// drop old events, so the ledger cannot be rebuilt from them. turn.completed carries
// the snapshot its turn ended with; mid-turn calls update only this durable ledger.
bool LocalJournal::initializeWorkReports() {
    QSqlQuery query(database_);
    if (query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS work_reports (session_id TEXT NOT NULL, generation "
            "INTEGER NOT NULL, version INTEGER NOT NULL, turn_sequence INTEGER NOT NULL, ledger "
            "TEXT NOT NULL, updated_at TEXT NOT NULL, PRIMARY KEY(session_id, generation))")))
        return true;
    setError(query.lastError().text());
    return false;
}

// Called inside event acceptance's transaction. Agent deliveries, scheduled work and provider
// continuations do not start a direct-user request. Cursor distinguishes steers in one turn.
bool LocalJournal::advanceWorkReportRequest(const QString& sessionId, const qint64 generation,
                                            const qint64 cursor, const QString& type,
                                            const QJsonObject& payload, QString* error) {
    if ((type != QStringLiteral("user.message") && type != QStringLiteral("turn.steered")) ||
        !payload.value(QStringLiteral("origin")).toString().isEmpty() ||
        payload.contains(QStringLiteral("schedule_id")))
        return true;
    auto ledger = workReport(sessionId, generation);
    ledger.insert(QStringLiteral("request_id"), cursor);
    ledger.remove(QStringLiteral("title"));
    ledger.insert(QStringLiteral("version"),
                  ledger.value(QStringLiteral("version")).toInteger() + 1);
    return storeWorkReport(sessionId, generation, ledger, error);
}

QJsonObject LocalJournal::workReport(const QString& sessionId, const qint64 generation) const {
    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral("SELECT ledger FROM work_reports WHERE session_id = ? AND generation = ?"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec() || !query.next())
        return {};
    return QJsonDocument::fromJson(query.value(0).toString().toUtf8()).object();
}

QJsonObject LocalJournal::publishedWorkReport(const QString& sessionId,
                                              const qint64 generation) const {
    // replay_events retains terminal rows after transport acknowledgement/detail trimming.
    // A later turn without a report does not acquire an older turn's snapshot; the pane
    // continues to read the last published report with its original turn identity.
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT payload FROM replay_events WHERE session_id = ? AND generation = ? "
        "AND event_type = 'turn.completed' "
        "AND json_extract(payload, '$.status') IN ('completed','failed','interrupted','stopped') "
        "AND json_type(payload, '$.work_report') = 'object' "
        "AND json_extract(payload, '$.work_report.turn') = turn_sequence "
        "ORDER BY cursor DESC LIMIT 1"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec()) {
        setError(query.lastError().text());
        return {};
    }
    if (!query.next())
        return {};
    const auto terminal = QJsonDocument::fromJson(query.value(0).toString().toUtf8()).object();
    auto report = terminal.value(QStringLiteral("work_report")).toObject();
    report.insert(QStringLiteral("terminal_status"), terminal.value(QStringLiteral("status")));
    return report;
}

bool LocalJournal::storeWorkReport(const QString& sessionId, const qint64 generation,
                                   const QJsonObject& ledger, QString* error) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT INTO work_reports (session_id, generation, version, turn_sequence, ledger, "
        "updated_at) VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT(session_id, generation) DO UPDATE SET "
        "version = excluded.version, turn_sequence = excluded.turn_sequence, ledger = "
        "excluded.ledger, updated_at = excluded.updated_at WHERE excluded.version > version"));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(ledger.value(QStringLiteral("version")).toInteger());
    query.addBindValue(ledger.value(QStringLiteral("turn")).toInteger());
    query.addBindValue(QString::fromUtf8(QJsonDocument(ledger).toJson(QJsonDocument::Compact)));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (query.exec() && query.numRowsAffected() == 1)
        return true;
    if (error)
        *error = query.lastError().isValid()
                     ? query.lastError().text()
                     : QStringLiteral("A newer work report was stored first.");
    return false;
}

} // namespace ketplus::daemon
