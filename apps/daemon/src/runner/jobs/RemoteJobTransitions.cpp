#include "runner/jobs/RemoteJobTransitions.h"

#include <limits>

namespace ketplus::daemon::runner::jobs {

namespace {

const QString acknowledged = QStringLiteral("cleanup_acknowledged");

bool live(const QString& value) {
    return value == state::queued || value == state::starting || value == state::running ||
           value == state::stopping;
}

bool forward(const QString& from, const QString& to) {
    if (from == to || to == state::stopping)
        return true;
    if (to == state::starting)
        return from == state::queued;
    if (to == state::running)
        return from == state::starting;
    if (to == state::needsAttention)
        return from == state::starting || from == state::running || from == state::stopping;
    return false;
}

RemoteStep taskStep(const RemoteJobRecord& to, const bool acknowledgement = false) {
    RemoteStep step;
    step.kind = RemoteStep::Kind::Task;
    step.acknowledgement = acknowledgement;
    BackgroundTaskChange& change = step.change;
    change.state = to.state;
    change.reason = to.reason;
    change.cleanupStatus = to.cleanupStatus;
    if (to.exitCode && *to.exitCode >= std::numeric_limits<int>::min() &&
        *to.exitCode <= std::numeric_limits<int>::max())
        change.exitCode = int(*to.exitCode);
    change.neverLaunched = to.terminal() && to.cleanupStatus == cleanup::notRequired;
    if (acknowledgement)
        change.suppressReason = acknowledged;
    return step;
}

} // namespace

bool isRecordedPair(const QString& value, const QString& where) {
    if (value == state::queued)
        return where == placement::submitting || where == placement::relayQueued;
    if (value == state::starting)
        return where == placement::relayQueued || where == placement::assigned ||
               where == placement::syncing;
    if (value == state::running)
        return where == placement::running;
    return value == state::stopping || value == state::needsAttention || isTerminalState(value);
}

RemoteStep remoteStep(const RemoteJobRecord& from, const RemoteJobRecord& to,
                      const bool acknowledgementHeld) {
    if (!isRecordedPair(to.state, to.placement))
        return {};
    if (from.terminal())
        return to.state == from.state ? RemoteStep{RemoteStep::Kind::RecordOnly, {}, false}
                                      : RemoteStep{};
    if (from.state == state::needsAttention) {
        if (to.state == state::needsAttention)
            return taskStep(to);
        if (to.state == state::interrupted && to.reason == acknowledged)
            return acknowledgementHeld ? taskStep(to, true) : RemoteStep{};
        // A late result of the runner, with its cleanup verified.
        return to.terminal() && to.cleanupStatus == cleanup::verified ? taskStep(to) : RemoteStep{};
    }
    if (!live(from.state))
        return {};
    if (to.terminal())
        return to.reason != acknowledged && (to.cleanupStatus == cleanup::verified ||
                                             to.cleanupStatus == cleanup::notRequired)
                   ? taskStep(to)
                   : RemoteStep{};
    return forward(from.state, to.state) ? taskStep(to) : RemoteStep{};
}

} // namespace ketplus::daemon::runner::jobs
