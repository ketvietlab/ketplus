#pragma once

#include "exec/ExecutorCommandRunner.h"
#include "run/RunPorts.h"

#include <memory>

// The run's CommandHost over the executor's command runner.
namespace ketplus::runner::app {

class ExecutorCommandHost final : public CommandHost {
  public:
    ExecutorCommandHost(exec::ExecutorCommandRunner::Options options, exec::Executor& executor,
                        exec::PlatformPolicy& policy);
    ~ExecutorCommandHost() override;
    ExecutorCommandHost(const ExecutorCommandHost&) = delete;
    ExecutorCommandHost& operator=(const ExecutorCommandHost&) = delete;

    [[nodiscard]] session::CommandRunner& runner() override { return *m_runner; }
    void bind(CommandEvents events) override { m_events = std::move(events); }
    [[nodiscard]] Readiness readiness() const override;
    void onReadinessChanged(Changed changed) override { m_changed = std::move(changed); }
    // Probes the platform, then cleans up.
    void cleanUpLeftovers(std::function<void(bool clean)> done) override;

  private:
    CommandEvents m_events;
    Changed m_changed;
    std::unique_ptr<exec::ExecutorCommandRunner> m_runner;
};

} // namespace ketplus::runner::app
