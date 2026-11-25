#include "runner/jobs/RemoteJobTransitions.h"

#include <limits>

namespace ketplus::daemon::runner::jobs {

namespace {

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
    return false;
}

RemoteStep taskStep(const RemoteJobRecord& to) {
    RemoteStep step;
    step.kind = RemoteStep::Kind::Task;
    BackgroundTaskChange& change = step.change;
    change.state = to.state;
    change.reason = to.reason;
    change.cleanupStatus = to.cleanupStatus;
    if (to.terminal() && to.cleanupStatus == cleanup::unverified) {
        // The runner did not confirm its processes ended. The daemon ends the job anyway and
        // keeps the gap in its internal diagnostics.
        change.cleanupStatus = QStringLiteral("best_effort_complete");
        change.cleanupDiagnostic = to.reason;
    }
    if (to.exitCode && *to.exitCode >= std::numeric_limits<int>::min() &&
        *to.exitCode <= std::numeric_limits<int>::max())
        change.exitCode = int(*to.exitCode);
    change.neverLaunched = to.terminal() && to.cleanupStatus == cleanup::notRequired;
    change.cleanupPending = to.state == state::stopping && to.cleanupStatus == cleanup::pending;
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
    return value == state::stopping || isTerminalState(value);
}

RemoteStep remoteStep(const RemoteJobRecord& from, const RemoteJobRecord& to) {
    if (!isRecordedPair(to.state, to.placement))
        return {};
    if (from.terminal())
        return to.state == from.state ? RemoteStep{RemoteStep::Kind::RecordOnly, {}} : RemoteStep{};
    if (!live(from.state))
        return {};
    if (to.terminal())
        return to.cleanupStatus == cleanup::verified || to.cleanupStatus == cleanup::notRequired ||
                       to.cleanupStatus == cleanup::unverified
                   ? taskStep(to)
                   : RemoteStep{};
    return forward(from.state, to.state) ? taskStep(to) : RemoteStep{};
}

} // namespace ketplus::daemon::runner::jobs
