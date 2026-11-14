#pragma once

#include "Schedule.h"
#include "storage/ScheduleRecord.h"

#include <optional>

namespace ketplus::daemon {

// The stored row as a protocol schedule, including busy and missed policy.
[[nodiscard]] std::optional<::ketplus::schedule::Schedule>
scheduleFromRecord(const ScheduleRecord& record, ::ketplus::schedule::Failure* failure = nullptr);

} // namespace ketplus::daemon
