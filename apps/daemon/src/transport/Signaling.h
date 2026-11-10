#pragma once
#include "transport/RemotePeers.h"
#include "transport/WebRtcPeerLink.h"

namespace ketplus::daemon::transport {
class Signaling final : public QObject {
    Q_OBJECT
  public:
    using Keys = std::function<void(const QString&, int, std::function<void(const QJsonObject&)>)>;
    using Sign = std::function<QByteArray(const QByteArray&)>;
    Signaling(RemotePeers* peers, Keys keys, Sign sign, QObject* parent = nullptr);
    void configure(const QString& workspace, const QString& machine, int keyVersion, bool enabled);
    void receive(const QJsonObject& message);
    void refreshDevices(const QJsonArray& devices);
    static QByteArray offerBytes(const QJsonObject& offer);
    static QByteArray answerBytes(const QJsonObject& offer, const QString& sdp, int machineVersion,
                                  const QString& issuedAt);
    static bool verify(const QByteArray& bytes, const QByteArray& key, const QByteArray& signature);

  signals:
    void outgoing(const QJsonObject& message);
    // A paired device lost its access; everything opened for it must close.
    void deviceRevoked(const QString& deviceId);

  private:
    void offer(const QJsonObject& message);
    void finishOffer(const QString& id, const QJsonObject& keys);
    void end(const QString& id, const QString& reason);
    struct Attempt {
        QJsonObject offer;
        QPointer<WebRtcPeerLink> link;
        QList<QJsonObject> candidates;
        int remoteCandidates{0};
        int localCandidates{0};
        bool remoteComplete{false};
        bool pending{true};
        qint64 created{0};
    };
    RemotePeers* peers_;
    Keys keys_;
    Sign sign_;
    QString workspace_, machine_;
    int version_{1};
    bool enabled_{false};
    qint64 epoch_{0};
    QHash<QString, Attempt> attempts_;
    QHash<QString, qint64> used_;
    QTimer timer_;
};
} // namespace ketplus::daemon::transport
