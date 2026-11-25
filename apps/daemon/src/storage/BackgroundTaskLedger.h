#pragma once

#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <algorithm>
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
    QString containmentMode{"native_best_effort"}; // Set by the trusted platform runtime.
    // Normalized runner labels: the task is a remote job from its creation and never runs here.
    std::optional<QStringList> runsOn{};
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
    // The daemon gave up verifying cleanup and ended the job anyway. Kept only in the
    // internal diagnostics table and the daemon log; never in the record clients read.
    QString cleanupDiagnostic;
    bool fullScopeVerified{false};
    // Native: the job process group was verified empty by the OS. Weaker than kernel
    // verification (fullScopeVerified) and recorded as such.
    bool groupVerified{false};
    std::optional<qint64> outputSize;
    std::optional<bool> outputTruncated;
};

// The cleanup evidence an executor result carries. Native process-group evidence is a weaker
// kind than kernel (cgroup_v2) verification and never sets fullScopeVerified.
inline void applyExecutorCleanupEvidence(BackgroundTaskChange& change, const QJsonObject& event) {
    change.cleanupStatus = event.value("cleanup_status").toString();
    const bool verified = change.cleanupStatus == "verified";
    const bool kernel = event.value("containment_mode") == "cgroup_v2";
    change.fullScopeVerified = kernel && verified;
    change.groupVerified =
        !kernel && verified && event.value("cleanup_verification") == "process_group_empty";
}

// What a transition's effects need besides the change: given by the caller, never read from the
// task's record.
struct BackgroundTaskEffects final {
    bool native{false};                  // the task's containment is native best effort
    std::optional<bool> executorStopped; // the executor's state before; nullopt: not tracked
};

// Transactional persistence and slot admission. Permission and executor evidence
// come from the session runtime. This class never launches a process or inference.
class BackgroundTaskLedger final {
  public:
    // Execution slots one root owner session/generation may hold at once.
    static constexpr int rootSlotLimit = 4;
    // How long an approved job may wait in the queue before its owner is told why: half its
    // timeout, at most two minutes, so the notice always comes well before the timeout.
    static constexpr qint64 queuedNoticeAfterMs(qint64 timeoutMs) {
        return std::clamp<qint64>(timeoutMs / 2, 1, 120000);
    }
    explicit BackgroundTaskLedger(QSqlDatabase database);
    // The registry's database, for stores that keep rows beside its tasks.
    [[nodiscard]] QSqlDatabase database() const { return database_; }
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
    // Whether `owner` has a queued, starting, running or stopping job; runtime only.
    [[nodiscard]] bool ownsLiveJobs(const BackgroundTaskOwner& owner) const;
    // Runtime-only recovery scan; callers must apply authorization to every API projection.
    [[nodiscard]] QJsonObject nonterminal() const;
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
    // Owner notices (BackgroundTaskNotices.cpp): events that wake the owner while a job is
    // still nonterminal. They share the wake table and fences with completions but keep
    // their own rows, so the completion outbox stays one event per task.
    // One notice per job that has waited in the queue past queuedNoticeAfterMs, naming why.
    // `queued` in the reply says whether the job has such a notice now; idempotent.
    QJsonObject noticeQueued(const QString& taskId, qint64 nowMs);
    [[nodiscard]] QJsonObject noticePage(qint64 after = 0) const;
    QJsonObject acceptNotice(const QString& eventId, const BackgroundTaskOwner& owner);
    // Whether a notice still describes its job: its job has not moved on since.
    [[nodiscard]] bool noticeCurrent(const QJsonObject& turnRecord) const;
    // Suppresses a completion or a notice and its accepted wake.
    QJsonObject suppressCompletion(const QString& eventId, const QString& reason);
    QJsonObject finishWake(const QString& eventId, const QString& state);
    QJsonObject recoverDispatchingWakes();
    QJsonObject deferWake(const QString& eventId);
    QJsonObject prune(qint64 nowMs);
    bool outputRemoved(const QString& taskId);
    // Writes an allowed change of `record` (the task as read, at `revision`) with the effects of
    // `change`: record, slot, cleanup audit, completion outbox. Runs inside the caller's open
    // transaction and never begins, commits or rolls one back.
    QJsonObject applyTransition(const QString& taskId, QJsonObject record, qint64 revision,
                                const BackgroundTaskChange& change,
                                const BackgroundTaskEffects& effects);
    [[nodiscard]] bool wakeDispatchAllowed(const QString& eventId,
                                           const BackgroundTaskOwner& owner) const;

  private:
    QJsonObject accept(const QString& table, const QString& eventId,
                       const BackgroundTaskOwner& owner);
    // Inside applyTransition's transaction: suppressing the pending notices of a job that ends.
    QJsonObject supersedeNotices(const QString& taskId);
    QJsonObject changeImpl(const QString& taskId, qint64 expectedRevision,
                           const BackgroundTaskChange& change);
    bool migrateRetiredAttention(QString* error);
    QJsonObject task(const QString& taskId) const;
    QSqlDatabase database_;
};

} // namespace ketplus::daemon
