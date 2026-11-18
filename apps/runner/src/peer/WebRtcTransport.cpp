#include "peer/WebRtcTransport.h"

#include "transport/WebRtcPeerLink.h"

namespace ketplus::runner::peer {

using daemon::transport::WebRtcPeerLink;

WebRtcTransport::WebRtcTransport(PeerTransportEvents& events) : m_link(new WebRtcPeerLink) {
    WebRtcPeerLink* link = m_link;
    QObject::connect(link, &WebRtcPeerLink::localDescription, link,
                     [&events](const QString& sdp) { events.localDescription(sdp); });
    QObject::connect(link, &WebRtcPeerLink::localCandidate, link,
                     [&events](const QString& candidate, const QString& mid) {
                         events.localCandidate(candidate, mid);
                     });
    QObject::connect(link, &WebRtcPeerLink::opened, link, [&events] { events.opened(); });
    QObject::connect(link, &WebRtcPeerLink::received, link,
                     [&events](Channel channel, const QByteArray& bytes, bool binary) {
                         events.received(channel, bytes, binary);
                     });
    QObject::connect(link, &WebRtcPeerLink::writable, link, [&events] { events.writable(); });
    QObject::connect(link, &WebRtcPeerLink::closed, link, [this, &events] {
        // The link is done: drop it after this signal, then report once.
        release();
        events.closed();
    });
}

WebRtcTransport::~WebRtcTransport() { close(); }

bool WebRtcTransport::offer(const QJsonArray& iceServers) {
    return m_link && m_link->offer(iceServers);
}

bool WebRtcTransport::acceptAnswer(const QString& sdp, const QString& fingerprint) {
    return m_link && m_link->acceptAnswer(sdp, fingerprint);
}

bool WebRtcTransport::candidate(const QString& candidate, const QString& mid) {
    return m_link && m_link->candidate(candidate, mid);
}

bool WebRtcTransport::send(const Channel channel, const QByteArray& bytes, const bool binary) {
    return m_link && m_link->send({channel, bytes, binary});
}

quint64 WebRtcTransport::buffered(const Channel channel) const {
    return m_link ? m_link->buffered(channel) : 0;
}

void WebRtcTransport::close() {
    if (!m_link)
        return;
    WebRtcPeerLink* link = m_link;
    QObject::disconnect(link, nullptr, link, nullptr);
    link->close();
    release();
}

void WebRtcTransport::release() {
    if (WebRtcPeerLink* link = m_link) {
        m_link.clear();
        QObject::disconnect(link, nullptr, link, nullptr);
        link->deleteLater();
    }
}

std::unique_ptr<PeerTransport> WebRtcTransportFactory::create(PeerTransportEvents& events) {
    return std::make_unique<WebRtcTransport>(events);
}

} // namespace ketplus::runner::peer
