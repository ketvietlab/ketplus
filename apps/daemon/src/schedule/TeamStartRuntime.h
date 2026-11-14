#pragma once

#include "schedule/ScheduleRunner.h"

namespace ketplus::daemon::teams {
class TeamCoordinator;
}

namespace ketplus::daemon::schedule {

// ScheduleRuntime for a team_start row: a paused team stays queued, and delivery
// is the leader's first turn. Prompt schedules are not in this store.
class TeamStartRuntime final : public ScheduleRuntime {
  public:
    explicit TeamStartRuntime(teams::TeamCoordinator* teams);

    [[nodiscard]] Inspection inspect(const ::ketplus::schedule::Schedule& schedule) const override;
    Delivery deliver(const ::ketplus::schedule::Schedule& schedule,
                     const ::ketplus::schedule::Run& run) override;
    void emitRun(const ::ketplus::schedule::Run& run) override;
    void emitChanged(const QString& change, const ::ketplus::schedule::Schedule& schedule) override;

  private:
    teams::TeamCoordinator* teams_;
};

} // namespace ketplus::daemon::schedule
