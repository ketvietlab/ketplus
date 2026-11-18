#pragma once

#include "runner/jobs/RemoteJobRecord.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>

#include <optional>

// What the remote job coordinator needs from the rest of the daemon. It reads no clock, opens no
// connection and writes no file itself: every effect goes through these interfaces.
namespace ketplus::daemon::runner::jobs {

// Durable remote job records. `put` succeeds only when the stored revision is `expected`
// (0 for a new record) and then stores `record` with revision `expected + 1`.
class RecordStore {
  public:
    virtual ~RecordStore() = default;
    [[nodiscard]] virtual bool put(const RemoteJobRecord& record, qint64 expected) = 0;
    [[nodiscard]] virtual QList<RemoteJobRecord> load() const = 0;
};

class MemoryRecordStore final : public RecordStore {
  public:
    [[nodiscard]] bool put(const RemoteJobRecord& record, const qint64 expected) override {
        if (failWrites || records_.value(record.taskId).revision != expected)
            return false;
        RemoteJobRecord stored = record;
        stored.revision = expected + 1;
        records_.insert(record.taskId, stored);
        ++writes;
        return true;
    }
    [[nodiscard]] QList<RemoteJobRecord> load() const override { return records_.values(); }
    [[nodiscard]] std::optional<RemoteJobRecord> find(const QString& taskId) const {
        const auto found = records_.constFind(taskId);
        return found == records_.cend() ? std::nullopt : std::optional(*found);
    }

    bool failWrites = false;
    int writes = 0;

  private:
    QHash<QString, RemoteJobRecord> records_;
};

enum class JobTimer {
    RelayRetry, // repeat a Relay call that got no usable answer: 1 second doubling to 30
    PeerAbsent, // 20 minutes after the last peer contact once the spec was sent
    Deadline,   // startedAt + timeoutMs
};

// Time for the coordinator. Answer: RemoteJobCoordinator::timerFired. Arming an armed timer
// restarts it.
class JobClock {
  public:
    virtual ~JobClock() = default;
    [[nodiscard]] virtual QDateTime now() const = 0;
    virtual void arm(const QString& taskId, JobTimer timer, qint64 delayMs) = 0;
    virtual void disarm(const QString& taskId, JobTimer timer) = 0;
};

// The runner pins of this desktop.
class RunnerTrust {
  public:
    virtual ~RunnerTrust() = default;
    // The pinned runner's name, or nullopt when this desktop holds no active pin for it.
    [[nodiscard]] virtual std::optional<QString> pinnedName(const QString& runnerId) const = 0;
};

// The desktop side of the runner's peer session, told what the record decided.
class JobPeers {
  public:
    virtual ~JobPeers() = default;
    // Ask the runner holding the job to cancel it, when a connection is open.
    virtual void cancel(const QString& taskId, const QString& runnerId) = 0;
};

} // namespace ketplus::daemon::runner::jobs
