#pragma once

#include "runner/jobs/RemoteJobCoordinator.h"
#include "runner/peer/PeerPorts.h"

// The peer sessions' view of the remote job records: every call goes to the coordinator, which
// stays the only authority on a job's state.
namespace ketplus::daemon::runner::remote {

class CoordinatorRecords final : public peer::JobRecords {
  public:
    explicit CoordinatorRecords(jobs::RemoteJobCoordinator& coordinator)
        : coordinator_(coordinator) {}

    [[nodiscard]] std::optional<peer::RecordView> view(const QString& jobId) const override;
    [[nodiscard]] peer::RecordAnswer offer(const QString& jobId, const QString& runnerId,
                                           qint64 attempt) override;
    [[nodiscard]] peer::RecordAnswer dispatch(const QString& jobId,
                                              const QString& runnerId) override;
    void accepted(const QString& jobId, const QString& runnerId) override;
    void started(const QString& jobId, const QString& runnerId) override;
    void rejected(const QString& jobId, const QString& runnerId, const QString& code) override;
    [[nodiscard]] peer::RecordAnswer snapshotFailed(const QString& jobId, const QString& runnerId,
                                                    const QString& code) override;
    [[nodiscard]] peer::RecordAnswer result(const QString& jobId, const QString& runnerId,
                                            const peer::protocol::JobResult& result) override;
    void peerOpened(const QString& jobId, const QString& runnerId) override;
    void peerLost(const QString& jobId, const QString& runnerId,
                  const QDateTime& lastContact) override;
    [[nodiscard]] QString jobOf(const QString& taskId) const override;

  private:
    jobs::RemoteJobCoordinator& coordinator_;
};

// The view a peer session reads of a record.
[[nodiscard]] peer::RecordView recordView(const jobs::RemoteJobRecord& record);

} // namespace ketplus::daemon::runner::remote
