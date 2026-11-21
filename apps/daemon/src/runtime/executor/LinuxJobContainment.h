#pragma once

#include <QJsonObject>
#include <QString>
#include <functional>

class QObject;

namespace ketplus::daemon::executor {
// Linux implementations are fail-closed; these functions never alter host policy.
QString linuxJobUnit(const QString& task, const QString& lease);
QString linuxBootId();
QJsonObject linuxUnitProperties(const QString& unit, QString* error = nullptr);
bool linuxContainmentAvailable(const QString& helper, QString* error);
void linuxContainmentAvailableAsync(const QString& helper, QObject* owner,
                                    std::function<void(bool, QString)> done);
// The same probe, also applying a job's `mounts` and checking them from inside.
void linuxMountsAvailableAsync(const QString& helper, QObject* owner,
                               std::function<void(bool, QString)> done);
void linuxUnitPropertiesAsync(const QString& unit, QObject* owner,
                              std::function<void(QJsonObject)> done);
void linuxStartSupervisorAsync(const QString& helper, const QString& directory, const QString& task,
                               const QString& lease, QObject* owner, std::function<void()> done);
QJsonObject linuxCreateBoundary(const QString& task, const QString& lease, QString* error);
bool linuxBoundaryMatches(const QJsonObject& identity);
bool linuxBoundaryEmpty(const QJsonObject& identity);
bool linuxKillBoundary(const QJsonObject& identity);
bool linuxPlaceLauncher(const QJsonObject& identity, qint64 pid);
bool linuxReceiptTrusted(const QString& task, const QString& lease, const QJsonObject& receipt,
                         const QJsonObject& service);
QJsonObject linuxRecoverBoundary(const QString& task, const QString& lease,
                                 const QJsonObject& receipt, const QJsonObject& service);
int linuxIsolationLaunch(const QJsonObject& execution, bool probe = false);
int linuxContainmentProbe(const QString& task, const QString& lease);
int linuxContainmentProbe(const QString& task, const QString& lease, bool mounts);
} // namespace ketplus::daemon::executor
