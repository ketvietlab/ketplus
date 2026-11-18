#include "runner/jobs/RemoteJobRecord.h"

#include <QSet>

namespace ketplus::daemon::runner::jobs {
namespace {

const QSet<QString>& relayFailedReasons() {
    static const QSet<QString> reasons{
        QStringLiteral("delivery_exhausted"), QStringLiteral("not_paired"),
        QStringLiteral("runner_removed"),     QStringLiteral("entitlement_required"),
        QStringLiteral("runner_unavailable"), QStringLiteral("no_matching_runner")};
    return reasons;
}

const QSet<QString>& snapshotCodes() {
    static const QSet<QString> codes{
        QStringLiteral("snapshot_failed"), QStringLiteral("snapshot_too_large"),
        QStringLiteral("pack_invalid"),    QStringLiteral("disk_full"),
        QStringLiteral("checkout_failed"), QStringLiteral("base_missing"),
        QStringLiteral("chunk_corrupt")};
    return codes;
}

const QString runnerUnavailable = QStringLiteral("runner_unavailable");

// What the Relay answers when it refuses a job.submit.
const QSet<QString>& relayRefusalReasons() {
    static const QSet<QString> reasons{
        QStringLiteral("no_matching_runner"),   QStringLiteral("entitlement_required"),
        QStringLiteral("queue_full"),           QStringLiteral("invalid_labels"),
        QStringLiteral("idempotency_conflict"), QStringLiteral("invalid_request")};
    return reasons;
}

// Codes this desktop, its ledger or the Relay give a meaning of their own. A runner's
// failed result that carries one ends like one whose reason is not a code.
const QSet<QString>& reservedReasons() {
    static const QSet<QString> reasons = [] {
        QSet<QString> all{QStringLiteral("permission_denied"),
                          QStringLiteral("cleanup_acknowledged"),
                          QStringLiteral("cleanup_unverified"),
                          QStringLiteral("queue_expired"),
                          QStringLiteral("expired"),
                          QStringLiteral("exit_code"),
                          QStringLiteral("user"),
                          QStringLiteral("deadline")};
        all.unite(relayFailedReasons());
        all.unite(relayRefusalReasons());
        return all;
    }();
    return reasons;
}

} // namespace

bool isTerminalState(const QString& value) {
    return value == state::completed || value == state::failed || value == state::cancelled ||
           value == state::timedOut || value == state::interrupted;
}

bool RemoteJobRecord::terminal() const { return isTerminalState(state); }

RemoteJobRecord applyResult(RemoteJobRecord record, const JobResult& result) {
    if (record.terminal())
        return record;
    QString terminal;
    QString reason;
    if (result.state == QStringLiteral("exited")) {
        const bool clean = result.exitCode == 0 && !result.signal;
        terminal = clean ? state::completed : state::failed;
        reason = clean ? QString() : QStringLiteral("exit_code");
    } else if (result.state == QStringLiteral("failed")) {
        terminal = state::failed;
        // The runner's own code is kept; one that is not a code is never stored.
        if (snapshotCodes().contains(result.reason))
            reason = QStringLiteral("snapshot_failed");
        else
            reason = isReasonCode(result.reason) && !reservedReasons().contains(result.reason)
                         ? result.reason
                         : QStringLiteral("spawn_failed");
    } else if (result.state == QStringLiteral("cancelled")) {
        terminal = state::cancelled;
        reason = QStringLiteral("user");
    } else if (result.state == QStringLiteral("timed_out")) {
        terminal = state::timedOut;
        reason = QStringLiteral("deadline");
    } else if (result.state == QStringLiteral("interrupted") && isReasonCode(result.reason)) {
        terminal = state::interrupted;
        reason = result.reason;
    } else {
        return record; // not a result this record can take
    }
    record.exitCode = result.exitCode;
    record.signal = result.signal;
    if (result.cleanupStatus != cleanup::verified) {
        record.state = state::needsAttention;
        record.reason = QStringLiteral("cleanup_unverified");
        record.cleanupStatus = cleanup::unverified;
        return record;
    }
    record.state = terminal;
    record.reason = reason;
    record.cleanupStatus = cleanup::verified;
    return record;
}

RemoteJobRecord endUndispatched(RemoteJobRecord record, const QString& terminal,
                                const QString& reason) {
    record.state = terminal;
    record.reason = reason;
    record.cleanupStatus = cleanup::notRequired;
    return record;
}

std::optional<RemoteJobRecord> applyRelayJob(const RemoteJobRecord& record, const RelayJob& job,
                                             const QString& runnerName) {
    if (record.terminal() || record.dispatched() || record.state == state::needsAttention)
        return std::nullopt;
    if (record.state == state::stopping) {
        // A user Stop waits for the Relay to end the job; nothing of it ever ran.
        if (!job.terminal())
            return std::nullopt;
        return endUndispatched(record, state::cancelled, QStringLiteral("user"));
    }
    RemoteJobRecord next = record;
    if (job.state == QStringLiteral("queued") || job.state == QStringLiteral("offered")) {
        next.placement = placement::relayQueued;
        next.runner.reset();
    } else if (job.state == QStringLiteral("acquired") && job.runnerId) {
        next.state = state::starting;
        next.placement = placement::assigned;
        next.runner = RemoteRunner{*job.runnerId, runnerName, job.attempt};
    } else if (job.state == QStringLiteral("expired")) {
        next = endUndispatched(record, state::failed, QStringLiteral("queue_expired"));
    } else if (job.state == QStringLiteral("failed")) {
        const QString reason = job.reason.value_or(QString());
        next = endUndispatched(record, state::failed,
                               relayFailedReasons().contains(reason) ? reason : runnerUnavailable);
    } else if (job.terminal()) {
        // cancelled, finished or abandoned without this desktop's spec
        next = endUndispatched(record, state::failed, runnerUnavailable);
    } else if (next.placement == placement::submitting) {
        next.placement = placement::relayQueued; // started for a spec that is not ours
    }
    if (next == record)
        return std::nullopt;
    return next;
}

std::optional<QString> submitRefusalReason(const QString& code) {
    static const QSet<QString> retried{
        QStringLiteral("network"),          QStringLiteral("relay_error"),
        QStringLiteral("invalid_response"), QStringLiteral("rate_limited"),
        QStringLiteral("not_enrolled"),     QStringLiteral("account_changed"),
        QStringLiteral("invalid_access"),   runnerUnavailable};
    if (retried.contains(code))
        return std::nullopt;
    if (relayRefusalReasons().contains(code))
        return code;
    return runnerUnavailable; // unsupported, route_refused
}

std::optional<QString> rejectionReason(const QString& code) {
    if (code == QStringLiteral("spec_invalid"))
        return QStringLiteral("spec_rejected");
    if (code == QStringLiteral("containment_unavailable") || code == runnerUnavailable)
        return code;
    return std::nullopt;
}

} // namespace ketplus::daemon::runner::jobs
