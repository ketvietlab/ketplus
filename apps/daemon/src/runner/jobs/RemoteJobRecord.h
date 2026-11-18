#pragma once

#include "runner/jobs/RemoteJobSchema.h"

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <optional>

// The desktop record of a job that runs on a runner, and the pure rules that move it. Values
// are the ADR 0127 registry states and cleanup statuses with the runner placements.
namespace ketplus::daemon::runner::jobs {

namespace state {
inline const QString queued = QStringLiteral("queued");
inline const QString starting = QStringLiteral("starting");
inline const QString running = QStringLiteral("running");
inline const QString stopping = QStringLiteral("stopping");
inline const QString completed = QStringLiteral("completed");
inline const QString failed = QStringLiteral("failed");
inline const QString cancelled = QStringLiteral("cancelled");
inline const QString timedOut = QStringLiteral("timed_out");
inline const QString interrupted = QStringLiteral("interrupted");
inline const QString needsAttention = QStringLiteral("needs_attention");
} // namespace state

namespace placement {
inline const QString submitting = QStringLiteral("submitting");
inline const QString relayQueued = QStringLiteral("relay_queued");
inline const QString assigned = QStringLiteral("assigned");
inline const QString syncing = QStringLiteral("syncing");
inline const QString running = QStringLiteral("running");
} // namespace placement

namespace cleanup {
inline const QString notRequired = QStringLiteral("not_required");
inline const QString pending = QStringLiteral("pending");
inline const QString verified = QStringLiteral("verified");
inline const QString unverified = QStringLiteral("unverified");
} // namespace cleanup

struct RemoteRunner {
    QString runnerId;
    QString name;
    int attempt = 0;
    friend bool operator==(const RemoteRunner&, const RemoteRunner&) = default;
};

// The worktree snapshot of a job, frozen at submit: what the desktop needs to send it after a
// restart. The runner key is derived again from the secret each time; it is never stored.
struct RemoteSnapshot {
    QString repositoryId; // canonical common git dir
    QString worktreeId;   // canonical worktree root
    QString snapshotId;   // the snapshot commit, hex
    QStringList warnings; // wire codes only
    friend bool operator==(const RemoteSnapshot&, const RemoteSnapshot&) = default;
};

struct RemoteJobRecord {
    QString taskId; // also the job's submitKey
    QStringList runsOn;
    qint64 timeoutMs = 0; // counted from job.started; a registry task always has one above 0
    QString state = state::queued;
    QString placement = placement::submitting;
    std::optional<RemoteRunner> runner;
    QString reason;
    QString cleanupStatus = cleanup::notRequired;
    QString jobId;
    QString stopReason;   // "user", "deadline" or "snapshot_failed" once stopping was entered
    QString stopCode;     // the desktop's snapshot failure code, with "snapshot_failed"
    QString dispatchedTo; // the runner the spec may have reached; never two
    std::optional<QDateTime> dispatchedAt;
    std::optional<QDateTime> lastPeerContact;
    std::optional<QDateTime> startedAt;
    std::optional<qint64> exitCode;
    std::optional<qint64> signal;
    std::optional<RemoteSnapshot> snapshot; // absent in records written before it existed
    qint64 revision = 0;

    [[nodiscard]] bool terminal() const;
    [[nodiscard]] bool dispatched() const { return !dispatchedTo.isEmpty(); }
    friend bool operator==(const RemoteJobRecord&, const RemoteJobRecord&) = default;
};

[[nodiscard]] bool isTerminalState(const QString& value);

// A job.result of the runner holding the spec, already authenticated by the peer session.
struct JobResult {
    QString state; // exited, failed, cancelled, timed_out, interrupted
    std::optional<qint64> exitCode;
    std::optional<qint64> signal;
    QString reason;
    QString cleanupStatus; // verified or unverified
};

// The record after `result`; a result without verified cleanup leaves it in needs_attention.
[[nodiscard]] RemoteJobRecord applyResult(RemoteJobRecord record, const JobResult& result);

// The record after a Relay view of its job, for a record whose spec was never dispatched.
// `runnerName` names the runner the Relay assigned. Nullopt when nothing changes.
[[nodiscard]] std::optional<RemoteJobRecord>
applyRelayJob(const RemoteJobRecord& record, const RelayJob& job, const QString& runnerName);

// The failed reason of a job.submit refusal that ends the record, or nullopt when the call is
// retried with the same key.
[[nodiscard]] std::optional<QString> submitRefusalReason(const QString& code);

// The failed reason of a job.rejected code that ends the record, or nullopt when another runner
// may still take the job.
[[nodiscard]] std::optional<QString> rejectionReason(const QString& code);

// The record ended without a spec ever reaching a runner.
[[nodiscard]] RemoteJobRecord endUndispatched(RemoteJobRecord record, const QString& terminal,
                                              const QString& reason);

} // namespace ketplus::daemon::runner::jobs
