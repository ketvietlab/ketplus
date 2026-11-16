#pragma once

#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>
#include <optional>

namespace ketplus::daemon {

// The host supplies this identity after authentication, never from execution payloads.
struct BackgroundTaskOwner final {
    QString sessionId;
    qint64 generation{0};
    QString turnId;
};

struct BackgroundTaskIntent final {
    BackgroundTaskOwner owner;
    QString clientRequestId;
    QString kind;
    QString title;
    QByteArray specFingerprint;
    qint64 timeoutMs{0};
    QJsonObject executionMetadata;
    QJsonObject requestPayload{};
};

QByteArray backgroundTaskFingerprint(QJsonObject request, const QJsonObject& defaults);

struct BackgroundTaskChange final {
    QString state;
    QString reason;
    QString summary;
    QString terminalTarget;
    QJsonObject executionIdentity;
    bool permissionApproved{false};
    bool handshakeCommitted{false};
    bool cleanupConfirmed{false};
    bool neverLaunched{false};
    bool cleanupPending{false};
    QString suppressReason;
    QString cleanupStatus;
    std::optional<bool> executorStopped;
    std::optional<int> exitCode;
    QString errorCode;
    bool fullScopeVerified{false};
    std::optional<qint64> outputSize;
    std::optional<bool> outputTruncated;
};

// Transactional persistence and slot admission. Permission and executor evidence
// come from the session runtime. This class never launches a process or inference.
class BackgroundTaskLedger final {
  public:
    explicit BackgroundTaskLedger(QSqlDatabase database);
    bool initialize(QString* error);
    QJsonObject create(const BackgroundTaskIntent& intent);
    // Retry lookup precedes infrastructure/quota checks; it never creates a job.
    [[nodiscard]] QJsonObject retry(const BackgroundTaskIntent& intent) const;
    [[nodiscard]] QJsonObject get(const QString& taskId, const BackgroundTaskOwner& owner,
                                  bool localAdministration = false) const;
    [[nodiscard]] QJsonObject list(const BackgroundTaskOwner& owner, const QString& cursor = {},
                                   int limit = 50, bool localAdministration = false);
    QJsonObject change(const QString& taskId, qint64 expectedRevision,
                       const BackgroundTaskChange& change);
    QJsonObject claimNext();
    QJsonObject park(const QString& taskId, qint64 expectedRevision, bool noInferenceRunning);
    QJsonObject readyToResume(const QString& taskId, qint64 expectedRevision);
    // Persist the recoverable executor address before spawning its supervisor.
    QJsonObject registerExecutor(const QString& taskId, qint64 expectedRevision,
                                 const QJsonObject& identity);
    // Reserve an immutable child address before creating its conversation/inference.
    QJsonObject reserveChild(const QString& taskId, qint64 expectedRevision);
    // Runtime-only recovery scan; callers must apply authorization to every API projection.
    [[nodiscard]] QJsonObject nonterminal() const;
    // The IPC layer permits this only for an authenticated local user action.
    QJsonObject resolveCleanup(const QString& taskId, qint64 expectedRevision,
                               const QString& userClientId);
    // Commits completion dedup and its queued owner-turn record in one transaction.
    QJsonObject acceptCompletion(const QString& eventId, const BackgroundTaskOwner& owner);
    // Runtime sets durable Stop/pause/decision fences; only explicit eligibility clears them.
    bool holdOwner(const BackgroundTaskOwner& owner, const QString& reason, QString* error);
    // A dispatching inference is never claimed again after restart.
    QJsonObject claimWake(const QString& eventId, const BackgroundTaskOwner& owner);
    // Runtime-only scans/links; never expose these without the IPC owner/grant checks.
    [[nodiscard]] QJsonObject runtimeTask(const QString& taskId) const { return task(taskId); }
    [[nodiscard]] QJsonObject linkedTask(const QString& childSessionId, qint64 generation) const;
    [[nodiscard]] QJsonObject deliveryPage(bool acceptedWakes, qint64 after = 0) const;
    QJsonObject suppressCompletion(const QString& eventId, const QString& reason);
    QJsonObject finishWake(const QString& eventId, const QString& state);
    QJsonObject recoverDispatchingWakes();
    QJsonObject deferWake(const QString& eventId);
    QJsonObject prune(qint64 nowMs);
    bool outputRemoved(const QString& taskId);
    [[nodiscard]] bool wakeDispatchAllowed(const QString& eventId,
                                           const BackgroundTaskOwner& owner) const;

  private:
    QJsonObject changeImpl(const QString& taskId, qint64 expectedRevision,
                           const BackgroundTaskChange& change, const QString& userClientId);
    QJsonObject task(const QString& taskId) const;
    QSqlDatabase database_;
};

} // namespace ketplus::daemon
