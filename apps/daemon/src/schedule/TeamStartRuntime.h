#pragma once

#include "schedule/ScheduleRunner.h"

#include <functional>

namespace ketplus::daemon::teams {
class TeamCoordinator;
}

namespace ketplus::daemon::schedule {

// The one team_start inspection and delivery. HostScheduleRuntime and the test
// runner below both call these, so the live path is the one the tests fire.
[[nodiscard]] Inspection inspectTeamStart(const teams::TeamCoordinator* teams,
                                          const ::ketplus::schedule::Schedule& schedule);
Delivery deliverTeamStart(teams::TeamCoordinator* teams,
                          const ::ketplus::schedule::Schedule& schedule);

// ScheduleRuntime for a team_start row. Prompt schedules are not this runtime's job.
class TeamStartRuntime final : public ScheduleRuntime {
  public:
    using Changed =
        std::function<void(const QString& change, const ::ketplus::schedule::Schedule&)>;

    explicit TeamStartRuntime(teams::TeamCoordinator* teams, Changed changed = {});

    [[nodiscard]] Inspection inspect(const ::ketplus::schedule::Schedule& schedule) const override;
    Delivery deliver(const ::ketplus::schedule::Schedule& schedule,
                     const ::ketplus::schedule::Run& run) override;
    void emitRun(const ::ketplus::schedule::Run& run) override;
    void emitChanged(const QString& change, const ::ketplus::schedule::Schedule& schedule) override;

  private:
    teams::TeamCoordinator* teams_;
    Changed changed_;
};

} // namespace ketplus::daemon::schedule
