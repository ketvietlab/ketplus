#include "runner/remote/RemoteJobs.h"

#include "runner/remote/RelayMessages.h"

#include <QLoggingCategory>

#include <iterator>
#include <utility>

// The Relay socket and the runner links of RemoteJobs.
namespace ketplus::daemon::runner::remote {

namespace {
Q_LOGGING_CATEGORY(lcRemoteLinks, "ketplus.runner.remote")
} // namespace

bool RemoteJobs::relayMessage(const QJsonObject& message) {
    ensureLinks();
    const RelayRoute route = relayRoute(message, [this](const QString& connectionId) {
        return links_ && links_->owns(connectionId);
    });
    switch (route) {
    case RelayRoute::Device:
        return false;
    case RelayRoute::RunnerLink:
        // A runner offer with no links (signed out, no identity) goes unanswered.
        if (links_)
            links_->signal(message);
        return true;
    case RelayRoute::JobChanged:
        coordinator_.jobChanged(message.value(QStringLiteral("jobId")).toString());
        return true;
    }
    return false;
}

void RemoteJobs::relaySocketReopened() {
    ensureLinks();
    coordinator_.reconcile();
}

void RemoteJobs::linkTimer(const QString& connectionId) {
    if (links_)
        links_->timerFired(connectionId);
}

// The links belong to one account; another account gets new links and the records stay.
void RemoteJobs::ensureLinks() {
    if (!open_)
        return;
    const QString scope = ports_.accountScope ? ports_.accountScope() : QString();
    if (links_ && scope == linksScope_)
        return;
    dropLinks();
    if (scope.isEmpty() || !ports_.makeLinks)
        return;
    links_ = ports_.makeLinks(*this);
    if (links_)
        linksScope_ = scope;
}

// The links drop their connections without a word, so the sessions hear it here.
void RemoteJobs::dropLinks() {
    if (!links_)
        return;
    std::unique_ptr<link::RunnerLinks> gone = std::move(links_);
    linksScope_.clear();
    QHash<QString, bool> taken = std::exchange(connections_, {});
    for (auto it = taken.begin(); it != taken.end();)
        it = gone->owns(it.key()) ? std::next(it) : taken.erase(it);
    gone.reset();
    for (auto it = taken.cbegin(); it != taken.cend(); ++it) {
        if (it.value())
            sessions_.connectionLost(it.key());
        else
            sessions_.offerEnded(it.key());
    }
    qCInfo(lcRemoteLinks).noquote()
        << QStringLiteral("runner links closed connections=%1").arg(taken.size());
}

peer::RecordAnswer RemoteJobs::offered(const QString& connectionId, const QString& runnerId,
                                       const QString& jobId, const qint64 attempt) {
    // The session writes output from its first frame, so its spool opens before it exists.
    if (!spools_.contains(jobId)) {
        const auto found = coordinator_.recordOfJob(jobId);
        if (found && !found->terminal() && ports_.openSpool) {
            std::unique_ptr<peer::OutputSpool> spool = ports_.openSpool(found->taskId);
            if (!spool)
                return {QStringLiteral("storage_error")};
            spools_.insert(jobId, std::shared_ptr<peer::OutputSpool>(std::move(spool)));
        }
    }
    const peer::RecordAnswer answer = sessions_.offered(connectionId, runnerId, jobId, attempt);
    if (answer.ok())
        connections_.insert(connectionId, false);
    return answer;
}

void RemoteJobs::offerEnded(const QString& connectionId) {
    connections_.remove(connectionId);
    sessions_.offerEnded(connectionId);
}

void RemoteJobs::channelsOpened(const QString& connectionId) {
    if (connections_.contains(connectionId))
        connections_.insert(connectionId, true);
    sessions_.channelsOpened(connectionId);
}

void RemoteJobs::frameReceived(const QString& connectionId, const link::FrameChannel channel,
                               const QByteArray& message) {
    sessions_.frameReceived(connectionId, channel, message);
}

void RemoteJobs::bulkWritable(const QString& connectionId) { sessions_.bulkWritable(connectionId); }

void RemoteJobs::connectionLost(const QString& connectionId) {
    connections_.remove(connectionId);
    sessions_.connectionLost(connectionId);
}

void RemoteJobs::CurrentLinks::send(const QString& connectionId,
                                    const peer::protocol::Channel channel,
                                    const QByteArray& message) {
    if (links_)
        links_->send(connectionId, channel, message);
}

void RemoteJobs::CurrentLinks::sendBulk(const QString& connectionId, const QString& transferId,
                                        const QByteArray& bytes) {
    if (links_)
        links_->sendBulk(connectionId, transferId, bytes);
}

bool RemoteJobs::CurrentLinks::bulkWritable(const QString& connectionId) const {
    return links_ && links_->bulkWritable(connectionId);
}

void RemoteJobs::CurrentLinks::close(const QString& connectionId, const QString& reason) {
    if (links_)
        links_->close(connectionId, reason);
}

} // namespace ketplus::daemon::runner::remote
