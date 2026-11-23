#include "control/ControlV2Client.h"
#include "control/RelayEventStream.h"
#include "control/RelayMachineBackend.h"

#include <QTimer>

namespace ketplus::remote {

bool RelayMachineBackend::hostReachable() const {
    if (!stream_ || !link_)
        return true;
    return stream_->hostReachable() && link_->state() == RelayFrameLink::State::Open;
}

void RelayMachineBackend::listCapabilities() {
    if (!client_)
        return;
    capabilityRetry_->stop();
    const auto epoch = generation_;
    client_->syncCapabilities(
        ControlV2Client::desktopCapabilities(), [self = QPointer<RelayMachineBackend>(this),
                                                 epoch](const QJsonValue&, const QString& error) {
            if (!self)
                return;
            if (error.isEmpty()) {
                self->capabilityFailures_ = 0;
                return;
            }
            // The set is sent again with the next connection too; this keeps trying meanwhile.
            qWarning("Listing this device's capabilities failed: %s", qUtf8Printable(error));
            if (epoch != self->generation_ || self->fatal(error))
                return;
            const int wait = int(qMin<qint64>(qint64(self->options_.capabilityRetryMilliseconds)
                                                  << qMin(self->capabilityFailures_++, 16),
                                              self->options_.maximumCapabilityRetryMilliseconds));
            self->capabilityRetry_->start(wait);
        });
}

void RelayMachineBackend::updateReachability() {
    const bool reachable = hostReachable();
    if (reachable == reachableReported_)
        return;
    reachableReported_ = reachable;
    emit hostReachabilityChanged(reachable);
}

void RelayMachineBackend::watch(const QString& sessionId, const qint64 cursor) {
    if (watched_.contains(sessionId))
        return;
    watched_.insert(sessionId, cursor);
    if (stream_)
        beginStreaming(sessionId, cursor);
}

void RelayMachineBackend::applyForwardOnly(const bool forwardOnly) {
    if (forwardOnly) {
        events_->stop();
        if (stream_ || !client_)
            return;
        link_ = client_->frameLink();
        stream_ = new RelayEventStream(link_, relayMachineId_, client_->eventCursors(), {}, this);
        connect(stream_, &RelayEventStream::eventsReady, this,
                &RelayMachineBackend::deliverForwarded);
        connect(stream_, &RelayEventStream::reloadRequested, this,
                &RelayMachineBackend::reloadConversation);
        connect(stream_, &RelayEventStream::reachabilityChanged, this,
                [this] { updateReachability(); });
        connect(link_, &RelayFrameLink::stateChanged, this, [this] { updateReachability(); });
        connect(link_, &RelayFrameLink::authorizationRevoked, this,
                [this] { failed(QStringLiteral("Account changed.")); });
        link_->start();
        for (auto it = watched_.cbegin(); it != watched_.cend(); ++it)
            beginStreaming(it.key(), it.value());
        updateReachability();
        return;
    }
    events_->start();
    if (!stream_)
        return;
    // The host stores events again, from the head at the moment it did: what it forwarded in
    // between is read through each conversation's newest window.
    stream_->flush();
    stream_->unfollowAll();
    delete std::exchange(stream_, nullptr);
    signing_.clear();
    updateReachability();
    for (auto it = watched_.cbegin(); it != watched_.cend(); ++it)
        reloadConversation(it.key());
}

void RelayMachineBackend::beginStreaming(const QString& sessionId, const qint64 cursor) {
    const auto row = sessions_.value(sessionId);
    const auto generation = row.value("generation").toInteger();
    if (row.isEmpty() || generation < 1 || !client_)
        return;
    client_->eventKeys(row,
                       [self = QPointer<RelayMachineBackend>(this), epoch = generation_, sessionId,
                        cursor, generation](const QByteArray& signing, const QString& error) {
                           if (!self || epoch != self->generation_ || !self->stream_)
                               return;
                           if (!error.isEmpty()) {
                               self->failed(error);
                               return;
                           }
                           self->signing_.insert(sessionId, signing);
                           self->stream_->open(sessionId, generation, cursor);
                       });
}

void RelayMachineBackend::deliverForwarded(const QString& sessionId, const QJsonArray& events) {
    if (!stream_)
        return;
    const auto row = sessions_.value(sessionId);
    const auto signing = signing_.value(sessionId);
    if (row.isEmpty() || signing.isEmpty() || !client_) {
        // The feed already moved its cursor past these events: without the key to open them,
        // the conversation reloads its window rather than lose them.
        stream_->eventRejected(sessionId);
        return;
    }
    for (const auto& item : events) {
        QString error;
        const auto opened = client_->openEvent(row, signing, item.toObject(), &error);
        if (opened.isEmpty()) {
            // Only the reason is logged, never the event.
            qWarning("A forwarded event could not be opened: %s", qUtf8Printable(error));
            stream_->eventRejected(sessionId);
            return;
        }
        emit eventReceived(relayLocalEvent(opened));
        if (watched_.contains(sessionId))
            watched_.insert(sessionId, opened.value("cursor").toInteger());
    }
}

void RelayMachineBackend::reloadConversation(const QString& sessionId) {
    const auto row = sessions_.value(sessionId);
    if (row.isEmpty() || !client_ || !watched_.contains(sessionId))
        return;
    const auto epoch = generation_;
    client_->transcriptWindow(
        row, {},
        [self = QPointer<RelayMachineBackend>(this), epoch, sessionId, row](const QJsonValue& value,
                                                                            const QString& error) {
            if (!self || epoch != self->generation_)
                return;
            if (!error.isEmpty()) {
                if (self->fatal(error)) {
                    self->failed(error);
                    return;
                }
                // Tried again after a wait that doubles up to a ceiling, for as long as the
                // conversation stays open.
                const int failures = self->reloadFailures_.value(sessionId, 0);
                self->reloadFailures_.insert(sessionId, failures + 1);
                const int wait = int(qMin<qint64>(qint64(self->options_.reloadRetryMilliseconds)
                                                      << qMin(failures, 16),
                                                  self->options_.maximumReloadRetryMilliseconds));
                QTimer::singleShot(wait, self, [self, sessionId] {
                    if (self)
                        self->reloadConversation(sessionId);
                });
                return;
            }
            self->reloadFailures_.remove(sessionId);
            const auto window = value.toObject();
            const auto snapshot = window.value("snapshot_cursor").toInteger();
            self->watched_.insert(sessionId, snapshot);
            if (self->stream_)
                self->stream_->open(sessionId, row.value("generation").toInteger(), snapshot);
            emit self->conversationReloaded(sessionId, window);
        });
}

} // namespace ketplus::remote
