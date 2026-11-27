#include "runner/peer/RemotePeerSessions.h"

#include <utility>

namespace ketplus::daemon::runner::peer {

RemotePeerSessions::RemotePeerSessions(JobRecords& records, Factory factory)
    : records_(records), factory_(std::move(factory)) {}

JobPeerSession* RemotePeerSessions::session(const QString& jobId) const {
    const auto found = sessions_.constFind(jobId);
    return found == sessions_.cend() ? nullptr : found->get();
}

JobPeerSession* RemotePeerSessions::owner(const QString& connectionId) const {
    for (const auto& session : sessions_)
        if (session->owns(connectionId))
            return session.get();
    return nullptr;
}

RecordAnswer RemotePeerSessions::offered(const QString& connectionId, const QString& runnerId,
                                         const QString& jobId, const qint64 attempt) {
    const RecordAnswer mismatch{QString(protocol::toString(protocol::ErrorCode::JobMismatch))};
    if (owner(connectionId))
        return mismatch;
    JobPeerSession* found = session(jobId);
    if (!found) {
        const std::optional<RecordView> view = records_.view(jobId);
        if (!view || view->terminal || sessions_.size() >= maximumSessions)
            return mismatch;
        std::unique_ptr<JobPeerSession> made = factory_(jobId);
        if (!made || made->jobId() != jobId)
            return mismatch;
        found = made.get();
        sessions_.insert(jobId, std::shared_ptr<JobPeerSession>(std::move(made)));
    }
    return found->offered(connectionId, runnerId, attempt);
}

void RemotePeerSessions::offerEnded(const QString& connectionId) {
    if (JobPeerSession* found = owner(connectionId))
        found->offerEnded(connectionId);
}

void RemotePeerSessions::channelsOpened(const QString& connectionId) {
    if (JobPeerSession* found = owner(connectionId))
        found->channelsOpened(connectionId);
}

void RemotePeerSessions::frameReceived(const QString& connectionId, const protocol::Channel channel,
                                       const QByteArray& message) {
    if (JobPeerSession* found = owner(connectionId))
        found->frameReceived(connectionId, channel, message);
}

void RemotePeerSessions::bulkWritable(const QString& connectionId) {
    if (JobPeerSession* found = owner(connectionId))
        found->bulkWritable(connectionId);
}

void RemotePeerSessions::connectionLost(const QString& connectionId) {
    if (JobPeerSession* found = owner(connectionId))
        found->connectionLost(connectionId);
}

void RemotePeerSessions::timerFired(const QString& jobId, const PeerTimer timer) {
    if (JobPeerSession* found = session(jobId))
        found->timerFired(timer);
}

void RemotePeerSessions::snapshotReady(const QString& jobId) {
    if (JobPeerSession* found = session(jobId))
        found->snapshotReady();
}

void RemotePeerSessions::snapshotFailed(const QString& jobId, const QString& code) {
    if (JobPeerSession* found = session(jobId))
        found->snapshotFailed(code);
}

void RemotePeerSessions::cancel(const QString& taskId, const QString& runnerId) {
    if (JobPeerSession* found = session(records_.jobOf(taskId)))
        found->cancel(runnerId);
}

void RemotePeerSessions::release(const QString& jobId) {
    const std::shared_ptr<JobPeerSession> found = sessions_.take(jobId);
    if (found)
        found->close();
}

} // namespace ketplus::daemon::runner::peer
