#include "storage/LocalJournal.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {

bool LocalJournal::initializeRemoteAudit() {
    QSqlQuery query(database_);
    const QStringList statements{
        QStringLiteral("CREATE TABLE IF NOT EXISTS remote_command_audit ("
                       "command_id TEXT PRIMARY KEY, kind TEXT NOT NULL, "
                       "device_id TEXT NOT NULL, device_key_version INTEGER NOT NULL, "
                       "session_id TEXT NOT NULL, result_code TEXT NOT NULL, "
                       "recorded_at TEXT NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS remote_command_audit_device ON "
                       "remote_command_audit(device_id, recorded_at)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS remote_command_audit_recorded ON "
                       "remote_command_audit(recorded_at)")};
    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            setError(query.lastError().text());
            return false;
        }
    }
    return true;
}

bool LocalJournal::recordRemoteCommand(const RemoteCommandAudit& entry) {
    QSqlQuery query(database_);
    // Only a repeated command id is skipped; any other constraint failure is reported.
    query.prepare(QStringLiteral(
        "INSERT INTO remote_command_audit(command_id, kind, device_id, device_key_version, "
        "session_id, result_code, recorded_at) VALUES(?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(command_id) DO NOTHING"));
    // A missing field is stored empty: a null QString binds as SQL NULL.
    const auto text = [](const QString& value) {
        return value.isNull() ? QStringLiteral("") : value;
    };
    query.addBindValue(text(entry.commandId));
    query.addBindValue(text(entry.kind));
    query.addBindValue(text(entry.deviceId));
    query.addBindValue(entry.deviceKeyVersion);
    query.addBindValue(text(entry.sessionId));
    query.addBindValue(text(entry.resultCode));
    query.addBindValue(
        (entry.recordedAt.isValid() ? entry.recordedAt : QDateTime::currentDateTimeUtc())
            .toUTC()
            .toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

std::optional<QList<RemoteCommandAudit>> LocalJournal::remoteCommands(const QString& deviceId,
                                                                      const int limit) const {
    QList<RemoteCommandAudit> result;
    QSqlQuery query(database_);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral("SELECT command_id, kind, device_id, device_key_version, "
                                 "session_id, result_code, recorded_at FROM remote_command_audit "
                                 "%1 ORDER BY recorded_at DESC, rowid DESC LIMIT %2")
                      .arg(deviceId.isEmpty() ? QString() : QStringLiteral("WHERE device_id = ?"))
                      .arg(qMax(0, limit)));
    if (!deviceId.isEmpty())
        query.addBindValue(deviceId);
    if (!query.exec()) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    while (query.next()) {
        result.append({query.value(0).toString(), query.value(1).toString(),
                       query.value(2).toString(), query.value(3).toInt(), query.value(4).toString(),
                       query.value(5).toString(),
                       QDateTime::fromString(query.value(6).toString(), Qt::ISODateWithMs)});
    }
    if (query.lastError().isValid()) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    return result;
}

} // namespace ketplus::daemon
