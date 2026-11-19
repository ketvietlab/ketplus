#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <optional>

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
// cgroup v2 can report `populated 1` for a moment after the last member of a domain was reaped.
// Only the containment probe waits for that to settle; every other check reads once.
enum class LinuxPopulated { Empty, Populated, Unknown };
// The `populated` value of a cgroup.events text: Unknown unless exactly one line says 0 or 1.
LinuxPopulated linuxPopulatedOf(const QByteArray& events);
// Reads until `readEvents` reports Empty (true). Unknown, an unreadable file (nullopt) or still
// Populated once the monotonic deadline fixed at the call has passed answer false.
bool linuxAwaitEmpty(const std::function<std::optional<QByteArray>()>& readEvents, int deadlineMs,
                     int pollMs);
// linuxBoundaryEmpty, waiting up to `deadlineMs` while the same boundary reports populated 1.
bool linuxBoundaryEmptyWithin(const QJsonObject& identity, int deadlineMs);
constexpr int linuxProbeEmptyDeadlineMs = 1000;
constexpr int linuxProbeEmptyPollMs = 5;
// The probe's verdict once its launcher was placed and reaped: `waitStatus` is a clean exit 0 and
// then `boundaryEmptied` holds. `boundaryEmptied` runs only after such an exit.
bool linuxProbeSucceeded(bool launched, int waitStatus,
                         const std::function<bool()>& boundaryEmptied);
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
