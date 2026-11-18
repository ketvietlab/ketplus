#pragma once

#include "runner/jobs/RemoteJobPorts.h"
#include "runner/peer/JobPeerSession.h"

#include <QHash>
#include <QString>

#include <functional>
#include <memory>

// The peer sessions of the desktop's remote jobs, one per job, and the record's way to reach
// them. Signaling and the connections report by connection id; the session owning it answers.
namespace ketplus::daemon::runner::peer {

inline constexpr int maximumSessions = 64;

class RemotePeerSessions final : public jobs::JobPeers {
  public:
    // A new session of a job, on that job's ports.
    using Factory = std::function<std::unique_ptr<JobPeerSession>(const QString& jobId)>;

    RemotePeerSessions(JobRecords& records, Factory factory);

    // A verified offer; the job's session is made on the first one. Ok lets signaling answer.
    [[nodiscard]] RecordAnswer offered(const QString& connectionId, const QString& runnerId,
                                       const QString& jobId, qint64 attempt);
    void offerEnded(const QString& connectionId);
    void channelsOpened(const QString& connectionId);
    void frameReceived(const QString& connectionId, protocol::Channel channel,
                       const QByteArray& message);
    void bulkWritable(const QString& connectionId);
    void connectionLost(const QString& connectionId);

    void timerFired(const QString& jobId, PeerTimer timer);
    void snapshotReady(const QString& jobId);
    void snapshotFailed(const QString& jobId, const QString& code);

    // jobs::JobPeers
    void cancel(const QString& taskId, const QString& runnerId) override;

    // Drops the session of a job the record ended, with its connections.
    void release(const QString& jobId);
    [[nodiscard]] JobPeerSession* session(const QString& jobId) const;
    [[nodiscard]] int size() const { return static_cast<int>(sessions_.size()); }

  private:
    [[nodiscard]] JobPeerSession* owner(const QString& connectionId) const;

    JobRecords& records_;
    Factory factory_;
    QHash<QString, std::shared_ptr<JobPeerSession>> sessions_;
};

} // namespace ketplus::daemon::runner::peer
