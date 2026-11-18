#pragma once

#include "runner/RunnerRelayClient.h"
#include "runner/jobs/RemoteJobSchema.h"

#include <QStringList>

// The desktop's job calls on the Relay: job.submit, job.get and job.cancel.
namespace ketplus::daemon::runner::jobs {

using JobOutcome = RunnerOutcome<RelayJob>;
using JobCallback = std::function<void(const JobOutcome&)>;

// What a remote job record needs from the Relay. RemoteJobRelay implements it on the
// RunnerRelayPort; tests replace it.
class JobRelay {
  public:
    virtual ~JobRelay() = default;
    // Whether the Relay offers runners to this machine.
    [[nodiscard]] virtual bool available() const = 0;
    virtual void submit(const QString& submitKey, const QStringList& runsOn,
                        JobCallback callback) = 0;
    virtual void get(const QString& jobId, JobCallback callback) = 0;
    virtual void cancel(const QString& jobId, const QString& reason, JobCallback callback) = 0;
};

class RemoteJobRelay final : public JobRelay {
  public:
    explicit RemoteJobRelay(RunnerRelayPort& port) : port_(port) {}

    [[nodiscard]] bool available() const override { return port_.runnerRelayAvailable(); }
    // Idempotent per submitKey: the same labels answer the existing job, also a terminal one.
    void submit(const QString& submitKey, const QStringList& runsOn, JobCallback callback) override;
    void get(const QString& jobId, JobCallback callback) override;
    // A terminal job answers its state.
    void cancel(const QString& jobId, const QString& reason, JobCallback callback) override;

  private:
    RunnerRelayPort& port_;
};

} // namespace ketplus::daemon::runner::jobs
