#include "storage/RemoteJobRows.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>

namespace ketplus::daemon::remote_job_rows {

bool initialize(QSqlDatabase database, QString* error) {
    QSqlQuery query(database);
    for (const char* statement :
         {"CREATE TABLE IF NOT EXISTS remote_jobs(task_id TEXT PRIMARY KEY "
          "REFERENCES managed_jobs(task_id) ON DELETE CASCADE,"
          "revision INTEGER NOT NULL CHECK(revision>=0),record BLOB,runs_on TEXT NOT NULL,"
          "timeout_ms INTEGER NOT NULL,job_id TEXT UNIQUE,dispatched_to TEXT,"
          "CHECK((revision=0)=(record IS NULL)))",
          "CREATE TRIGGER IF NOT EXISTS remote_jobs_follow_task AFTER DELETE ON managed_jobs "
          "BEGIN DELETE FROM remote_jobs WHERE task_id=old.task_id; END"}) {
        if (!query.exec(QLatin1String(statement))) {
            if (error)
                *error = query.lastError().text();
            return false;
        }
    }
    return true;
}

std::optional<Phase> phaseOf(QSqlDatabase database, const QString& taskId) {
    QSqlQuery query(database);
    query.prepare("SELECT revision FROM remote_jobs WHERE task_id=?");
    query.addBindValue(taskId);
    if (!query.exec())
        return std::nullopt;
    if (!query.next())
        return Phase::Local;
    return query.value(0).toLongLong() == 0 ? Phase::Reserved : Phase::Started;
}

bool reserve(QSqlDatabase database, const QString& taskId, const QStringList& runsOn,
             const qint64 timeoutMs, QString* error) {
    QSqlQuery query(database);
    query.prepare("INSERT INTO remote_jobs(task_id,revision,runs_on,timeout_ms) VALUES(?,0,?,?)");
    query.addBindValue(taskId);
    query.addBindValue(QString::fromUtf8(
        QJsonDocument(QJsonArray::fromStringList(runsOn)).toJson(QJsonDocument::Compact)));
    query.addBindValue(timeoutMs);
    if (query.exec())
        return true;
    if (error)
        *error = query.lastError().text();
    return false;
}

QByteArray fingerprint(const QByteArray& fingerprint, const std::optional<QStringList>& runsOn,
                       const qint64 timeoutMs) {
    if (!runsOn)
        return fingerprint;
    const QJsonObject remote{{"spec", QString::fromLatin1(fingerprint.toBase64())},
                             {"runs_on", QJsonArray::fromStringList(*runsOn)},
                             {"timeout_ms", timeoutMs}};
    return QCryptographicHash::hash(QJsonDocument(remote).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256);
}

QString localExecutionRefusal(QSqlDatabase database, const QString& taskId) {
    const auto phase = phaseOf(database, taskId);
    if (!phase)
        return QStringLiteral("storage_error");
    return *phase == Phase::Local ? QString() : QStringLiteral("invalid_state");
}

QString changeRefusal(QSqlDatabase database, const QString& taskId, const QString& state) {
    const auto phase = phaseOf(database, taskId);
    if (!phase)
        return QStringLiteral("storage_error");
    const bool running = state == QLatin1String("starting") || state == QLatin1String("running");
    if (*phase == Phase::Started || (*phase == Phase::Reserved && running))
        return QStringLiteral("invalid_state");
    return {};
}

} // namespace ketplus::daemon::remote_job_rows
