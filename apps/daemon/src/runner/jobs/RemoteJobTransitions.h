#pragma once

#include "runner/jobs/RemoteJobRecord.h"
#include "storage/BackgroundTaskLedger.h"

// How a stored remote job record moves its registry task. Closed: a step not listed is
// refused and nothing is written.
namespace ketplus::daemon::runner::jobs {

struct RemoteStep {
    enum class Kind {
        Refused,
        RecordOnly, // a terminal record again: the task does not change
        Task,       // the task takes `change` through the registry's transition effects
    };
    Kind kind = Kind::Refused;
    BackgroundTaskChange change;
};

// Whether a record's state and placement go together.
[[nodiscard]] bool isRecordedPair(const QString& state, const QString& placement);
// The step from `from` (a reserved task reads as queued and submitting) to `to`.
[[nodiscard]] RemoteStep remoteStep(const RemoteJobRecord& from, const RemoteJobRecord& to);

} // namespace ketplus::daemon::runner::jobs
