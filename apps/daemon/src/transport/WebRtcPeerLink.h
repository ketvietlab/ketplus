#pragma once

#include "transport/PeerLink.h"
#include <QJsonArray>
#include <memory>

namespace ketplus::daemon::transport {
// One libdatachannel peer connection in one of two roles, chosen by the first call:
// answer() for the host, which accepts the client's three channels, or offer() for the side that
// creates them. Session-host independent: it is part of the ketplus_peer_link library.
class WebRtcPeerLink final : public PeerLink {
    Q_OBJECT
  public:
    explicit WebRtcPeerLink(QObject* parent = nullptr);
    ~WebRtcPeerLink() override;
    bool answer(const QString& offer, const QJsonArray& iceServers);
    // Offerer: creates the control, events and bulk channels (ordered, reliable) and emits
    // localDescription with the offer. acceptAnswer() then requires the DTLS fingerprint the
    // caller obtained from an authenticated source; the answer's fingerprint and the DTLS
    // certificate must both match it, or the link closes before any frame passes.
    bool offer(const QJsonArray& iceServers);
    bool acceptAnswer(const QString& answer, const QString& expectedRemoteFingerprint);
    bool candidate(const QString& candidate, const QString& mid);
    // "sha-256 AB:CD:…" of this end's DTLS certificate; empty until localDescription is emitted.
    QString localFingerprint() const;
    // The canonical form of an "<algorithm> <hex pairs>" fingerprint, or empty when invalid.
    static QString normalizedFingerprint(const QString& fingerprint);
    bool send(const Packet& packet) override;
    quint64 buffered(Channel channel) const override;
    void close() override;

  signals:
    void localDescription(const QString& sdp);
    void localCandidate(const QString& candidate, const QString& mid);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ketplus::daemon::transport
