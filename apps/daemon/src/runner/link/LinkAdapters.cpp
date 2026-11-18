#include "runner/link/LinkAdapters.h"

#include "transport/WebRtcPeerLink.h"

#include <QTimer>

#include <algorithm>

namespace ketplus::daemon::runner::link {

using transport::WebRtcPeerLink;

WebRtcAnswerTransport::WebRtcAnswerTransport(LinkTransportEvents& events)
    : m_link(new WebRtcPeerLink) {
    WebRtcPeerLink* link = m_link;
    QObject::connect(link, &WebRtcPeerLink::localDescription, link,
                     [&events](const QString& sdp) { events.localDescription(sdp); });
    QObject::connect(link, &WebRtcPeerLink::localCandidate, link,
                     [&events](const QString& candidate, const QString& mid) {
                         events.localCandidate(candidate, mid);
                     });
    QObject::connect(link, &WebRtcPeerLink::opened, link, [&events] { events.opened(); });
    QObject::connect(link, &WebRtcPeerLink::received, link,
                     [&events](TransportChannel channel, const QByteArray& bytes, bool binary) {
                         events.received(channel, bytes, binary);
                     });
    QObject::connect(link, &WebRtcPeerLink::writable, link, [&events] { events.writable(); });
    QObject::connect(link, &WebRtcPeerLink::closed, link, [this, &events] {
        // The link is done: drop it after this signal, then report once.
        release();
        events.closed();
    });
}

WebRtcAnswerTransport::~WebRtcAnswerTransport() { close(); }

bool WebRtcAnswerTransport::answer(const QString& offerSdp, const QJsonArray& iceServers) {
    return m_link && m_link->answer(offerSdp, iceServers);
}

bool WebRtcAnswerTransport::candidate(const QString& candidate, const QString& mid) {
    return m_link && m_link->candidate(candidate, mid);
}

bool WebRtcAnswerTransport::send(const TransportChannel channel, const QByteArray& bytes,
                                 const bool binary) {
    return m_link && m_link->send({channel, bytes, binary});
}

quint64 WebRtcAnswerTransport::buffered(const TransportChannel channel) const {
    return m_link ? m_link->buffered(channel) : 0;
}

void WebRtcAnswerTransport::close() {
    if (!m_link)
        return;
    WebRtcPeerLink* link = m_link;
    QObject::disconnect(link, nullptr, link, nullptr);
    link->close();
    release();
}

void WebRtcAnswerTransport::release() {
    if (WebRtcPeerLink* link = m_link) {
        m_link.clear();
        QObject::disconnect(link, nullptr, link, nullptr);
        link->deleteLater();
    }
}

std::unique_ptr<LinkTransport> WebRtcAnswerTransportFactory::create(LinkTransportEvents& events) {
    return std::make_unique<WebRtcAnswerTransport>(events);
}

QtLinkClock::~QtLinkClock() { qDeleteAll(m_timers); }

QDateTime QtLinkClock::now() const { return QDateTime::currentDateTimeUtc(); }

void QtLinkClock::arm(const QString& connectionId, const qint64 delayMs) {
    disarm(connectionId);
    auto* timer = new QTimer;
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, timer, [this, connectionId] {
        if (QTimer* fired = m_timers.take(connectionId))
            fired->deleteLater();
        if (m_fired)
            m_fired(connectionId);
    });
    m_timers.insert(connectionId, timer);
    timer->start(static_cast<int>(std::clamp<qint64>(delayMs, 0, qint64{24} * 60 * 60 * 1000)));
}

void QtLinkClock::disarm(const QString& connectionId) {
    if (QTimer* timer = m_timers.take(connectionId)) {
        timer->stop();
        timer->deleteLater();
    }
}

} // namespace ketplus::daemon::runner::link
