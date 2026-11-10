#include "storage/LocalJournal.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

namespace ketplus::daemon {

// ADR 0106: one row per conversation generation holds its current work report. Retention may
// drop old events, so the ledger cannot be rebuilt from them; each work_report.updated event
// carries the snapshot its turn ended with.
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
