#pragma once

#include "runner/jobs/RemoteJobRecord.h"
#include "runner/peer/PeerPorts.h"

#include <QHash>
#include <QSet>
#include <QString>

// The specs of remote jobs, held in memory only: nothing of a spec is ever written to disk or
// logged. A spec is kept per task, follows the task's record to its job id, and is dropped when
// the record ends. After a restart nothing is held, so every job answers that it has no spec.
namespace ketplus::daemon::runner::jobs {

class RemoteJobSpecs final : public peer::JobSpecSource {
  public:
    // Keeps `spec` for `taskId`; its epoch, job id and attempt are not kept. The same spec again
    // changes nothing; a different one is refused and the first stays.
    [[nodiscard]] bool store(const QString& taskId, peer::protocol::JobSpec spec);
    // Follows a record the coordinator committed: names its job, or drops the spec once it ends.
    void recorded(const RemoteJobRecord& record);

    // The task's spec with this job id and attempt; nullopt when none is held.
    [[nodiscard]] std::optional<peer::protocol::JobSpec> spec(const QString& jobId,
                                                              qint64 attempt) override;

    [[nodiscard]] bool holds(const QString& taskId) const { return specs_.contains(taskId); }
    [[nodiscard]] int size() const { return specs_.size(); }

  private:
    QHash<QString, peer::protocol::JobSpec> specs_; // by task id
    QHash<QString, QString> tasks_;                 // task id by job id
    QSet<QString> ended_;                           // tasks whose record ended
};

} // namespace ketplus::daemon::runner::jobs
