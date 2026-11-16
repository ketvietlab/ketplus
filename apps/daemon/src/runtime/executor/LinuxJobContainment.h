#pragma once

#include <QJsonObject>
#include <QString>

namespace ketplus::daemon::executor {
// Linux implementations are fail-closed; these functions never alter host policy.
QString linuxJobUnit(const QString& task, const QString& lease);
QString linuxBootId();
QJsonObject linuxUnitProperties(const QString& unit, QString* error = nullptr);
bool linuxContainmentAvailable(const QString& helper, QString* error);
bool linuxStartSupervisor(const QString& helper, const QString& directory, const QString& task,
                          const QString& lease, QString* error);
QJsonObject linuxCreateBoundary(const QString& task, const QString& lease, QString* error);
bool linuxBoundaryMatches(const QJsonObject& identity);
bool linuxBoundaryEmpty(const QJsonObject& identity);
bool linuxKillBoundary(const QJsonObject& identity);
bool linuxPlaceLauncher(const QJsonObject& identity, qint64 pid);
bool linuxReceiptTrusted(const QString& task, const QString& lease, const QJsonObject& receipt);
QJsonObject linuxRecoverBoundary(const QString& task, const QString& lease,
                                 const QJsonObject& receipt);
int linuxIsolationLaunch(const QJsonObject& execution, bool probe = false);
int linuxContainmentProbe(const QString& task, const QString& lease);
} // namespace ketplus::daemon::executor
