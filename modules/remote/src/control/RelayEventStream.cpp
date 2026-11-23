#include "control/RelayEventStream.h"

namespace ketplus::remote {

RelayEventStream::RelayEventStream(RelayFrameLink* link, QString machineId,
                                   EventCursorStore* cursors, Options options, QObject* parent)
    : QObject(parent), link_(link), machineId_(std::move(machineId)), feed_(cursors),
      options_(options) {
    resubscribe_.setSingleShot(true);
    connect(&resubscribe_, &QTimer::timeout, this, &RelayEventStream::resubscribe);
    connect(link_, &RelayFrameLink::opened, this, &RelayEventStream::linkOpened);
    connect(link_, &RelayFrameLink::frameReceived, this, &RelayEventStream::received);
    connect(link_, &RelayFrameLink::dropped, this, &RelayEventStream::dropped);
    connect(link_, &RelayFrameLink::stateChanged, this, [this](const RelayFrameLink::State state) {
        if (state != RelayFrameLink::State::Open)
            feed_.disconnected();
    });
    if (link_->state() == RelayFrameLink::State::Open)
        linkOpened();
}

RelayEventStream::RelayEventStream(RelayFrameLink* link, QString machineId,
                                   EventCursorStore* cursors, QObject* parent)
    : RelayEventStream(link, std::move(machineId), cursors, Options{}, parent) {}

void RelayEventStream::send(const QList<QByteArray>& frames) {
    for (const QByteArray& frame : frames)
        if (!frame.isEmpty())
            link_->send(machineId_, frame);
}

void RelayEventStream::linkOpened() {
    // The relay forgets subscriptions with the socket.
    resubscribe_.stop();
    send(feed_.connected());
}

void RelayEventStream::open(const QString& sessionId, const qint64 generation,
                            const qint64 cursor) {
    send({feed_.followFrom(sessionId, generation, cursor)});
}

void RelayEventStream::follow(const QString& sessionId, const qint64 generation) {
    send({feed_.follow(sessionId, generation)});
}

void RelayEventStream::adoptWindow(const QString& sessionId, const qint64 generation,
                                   const qint64 snapshotCursor) {
    send({feed_.adoptWindow(sessionId, generation, snapshotCursor)});
}

void RelayEventStream::unfollow(const QString& sessionId) { send({feed_.unfollow(sessionId)}); }

void RelayEventStream::unfollowAll() {
    // Each conversation is told; the feed saves its cursors as it lets go.
    for (const QString& sessionId : feed_.followedConversations())
        unfollow(sessionId);
}

void RelayEventStream::eventRejected(const QString& sessionId) {
    if (feed_.requestReload(sessionId))
        emit reloadRequested(sessionId);
}

void RelayEventStream::received(const QString& machineId, const QByteArray& frame) {
    if (machineId != machineId_)
        return;
    QString error;
    auto result = feed_.receive(frame, &error);
    if (!error.isEmpty())
        return; // A refused frame changes nothing; the host's next frame or a gap recovers.
    setReachable(true);
    send(result.send);
    if (!result.events.isEmpty()) {
        drops_ = 0;
        emit eventsReady(result.sessionId, result.events);
    }
    if (result.reload)
        emit reloadRequested(result.sessionId);
}

void RelayEventStream::dropped(const QString& machineId, const QString& reason) {
    if (machineId != machineId_)
        return;
    if (reason == QLatin1String("host_offline"))
        setReachable(false);
    // Subscribing again is how a device recovers from a drop. Repeated drops (an offline host,
    // a refused subscription) wait longer each time instead of looping.
    const int shift = qMin(drops_++, 16);
    resubscribe_.start(int(qMin<qint64>(qint64(options_.initialResubscribeMilliseconds) << shift,
                                        options_.maximumResubscribeMilliseconds)));
}

void RelayEventStream::resubscribe() {
    QString error;
    send(feed_.receive(QByteArrayLiteral(R"({"type":"frames.dropped"})"), &error).send);
}

void RelayEventStream::setReachable(const bool reachable) {
    if (reachable_ == reachable)
        return;
    reachable_ = reachable;
    emit reachabilityChanged(reachable_);
}

} // namespace ketplus::remote
