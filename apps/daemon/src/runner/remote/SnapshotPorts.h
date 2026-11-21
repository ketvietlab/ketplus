#pragma once

#include "runner/peer/PeerPorts.h"
#include "runner/remote/SnapshotStores.h"
#include "runner/sync/SnapshotSender.h"

#include <QHash>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

// The peer sessions' snapshot port: one snapshot sender per job, in the store of its task. A
// sender starts only once the record durably took job.accepted; until then the start waits and
// the record is asked to write it again.
namespace ketplus::daemon::runner::remote {

// What one job's snapshot is: its task, its store and what the sender offers.
struct SnapshotPlan {
    QString taskId;
    QString storePath;
    sync::SyncJob job; // job id, runner, workspace key, snapshot id, warnings
};

class SnapshotPorts final : public peer::SnapshotPort {
  public:
    using Plan =
        std::function<std::optional<SnapshotPlan>(const QString& jobId, const QString& runnerId)>;
    // Whether the record durably took job.accepted from this runner; it may write it again.
    using Accepted = std::function<bool(const QString& jobId, const QString& runnerId)>;
    using BulkWritable = std::function<bool(const QString& jobId)>;
    // The sender's answer for the job's session: no code when the runner has the snapshot.
    using Answer = std::function<void(const QString& jobId, const std::optional<QString>& code)>;
    // Runs `work` on the event loop after `delayMs`, never inside the caller.
    using Defer = std::function<void(qint64 delayMs, std::function<void()> work)>;

    static constexpr qint64 firstAcceptedRetryMs = 1'000;
    static constexpr qint64 longestAcceptedRetryMs = 30'000;

    SnapshotPorts(SnapshotStores& stores, Plan plan, Accepted accepted, BulkWritable writable,
                  Answer answer, Defer defer);
    ~SnapshotPorts() override;
    SnapshotPorts(const SnapshotPorts&) = delete;
    SnapshotPorts& operator=(const SnapshotPorts&) = delete;

    void start(const QString& jobId, const QString& runnerId, const QMap<QString, QString>& held,
               Send send, SendBulk sendBulk) override;
    void receive(const QString& jobId, const peer::protocol::Frame& frame) override;
    void bulkWritable(const QString& jobId) override;
    void cancel(const QString& jobId) override;

    // The job's record ended: its sender stops and its task's snapshot is released.
    void ended(const QString& jobId);
    [[nodiscard]] bool sending(const QString& jobId) const;
    [[nodiscard]] int size() const { return static_cast<int>(jobs_.size()); }

    class Job;

  private:
    void begin(const QString& jobId, quint64 generation, qint64 retryMs);
    void answer(const QString& jobId, const std::optional<QString>& code);

    SnapshotStores& stores_;
    Plan plan_;
    Accepted accepted_;
    BulkWritable writable_;
    Answer answer_;
    Defer defer_;
    QHash<QString, std::shared_ptr<Job>> jobs_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace ketplus::daemon::runner::remote
