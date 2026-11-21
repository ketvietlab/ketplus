#pragma once

#include "runner/peer/PeerPorts.h"
#include "runner/remote/SnapshotPorts.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

// The frozen worktree of each remote task: where its snapshot is kept and what names it to a
// runner. Specs get their runner keys from here when they are sent, and snapshot senders their
// plan. Repository and worktree ids are local paths: they only enter key digests and store names,
// and are never logged or sent.
namespace ketplus::daemon::runner::remote {

struct TaskSnapshot {
    QString repositoryId; // canonical common Git directory
    QString worktreeId;   // canonical worktree root
    QString storePath;
    QString snapshotId;
    QStringList warnings; // wire codes only
};

class TaskSnapshots final : public peer::JobSpecSource {
  public:
    // The runner a job's spec and snapshot go to, and the task of a job.
    struct JobOwner {
        QString taskId;
        QString runnerId;
    };
    using Owner = std::function<std::optional<JobOwner>(const QString& jobId)>;
    // The desktop runner secret; nullopt when it cannot be read.
    using Secret = std::function<std::optional<QByteArray>()>;

    TaskSnapshots(peer::JobSpecSource& specs, Owner owner, Secret secret);

    void put(const QString& taskId, TaskSnapshot snapshot);
    void remove(const QString& taskId);
    [[nodiscard]] std::optional<TaskSnapshot> find(const QString& taskId) const;
    [[nodiscard]] QStringList tasks() const { return snapshots_.keys(); }

    // peer::JobSpecSource: the held spec with the runner's repository and workspace keys.
    [[nodiscard]] std::optional<peer::protocol::JobSpec> spec(const QString& jobId,
                                                              qint64 attempt) override;
    // What the job's snapshot sender offers to `runnerId`.
    [[nodiscard]] std::optional<SnapshotPlan> plan(const QString& jobId, const QString& runnerId);

  private:
    struct Keys {
        QString repositoryKey;
        QString workspaceKey;
    };
    [[nodiscard]] std::optional<Keys> keys(const TaskSnapshot& snapshot, const QString& runnerId);

    peer::JobSpecSource& specs_;
    Owner owner_;
    Secret secret_;
    QHash<QString, TaskSnapshot> snapshots_; // by task id
};

} // namespace ketplus::daemon::runner::remote
