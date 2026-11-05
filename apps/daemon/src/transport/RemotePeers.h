#pragma once

#include "transport/FrameCodec.h"
#include "transport/SendQueue.h"
#include <QJsonArray>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <functional>
#include <memory>

namespace ketplus::daemon::transport {
struct EventPage {
    QJsonArray events;
    qint64 nextCursor{0};
    qint64 generation{0};
    bool live{false};
    bool reset{false};
};

// Owns authenticated links; signaling verifies signatures before admitting one.
class RemotePeers final : public QObject {
    Q_OBJECT
  public:
    using Reader = std::function<EventPage(const QString&, qint64, qint64)>;
    explicit RemotePeers(Reader reader, QObject* parent = nullptr);
    bool accept(const QString& id, const QString& device, int keyVersion, const QJsonObject& grant,
                PeerLink* link, qint64 epoch);
    bool renew(const QString& id, const QJsonObject& grant);
    void revoke(const QString& device);
    void closeAll();
    void close(const QString& id, const QString& reason);
    bool authorized(const QString& id, const QJsonObject& command) const;
    bool send(const QString& id, QJsonObject frame, Channel channel = Channel::Control);
    void completed(const QString& id, const QString& commandId);
    void tick();
    void eventsChanged(const QString& sessionId);
    int size() const { return int(peers_.size()); }
    static bool validGrant(const QJsonObject& grant, const QString& id, const QString& device,
                           int version, qint64 now);

  signals:
    void command(const QString& connectionId, const QJsonObject& command);
    void ended(const QString& connectionId, const QString& reason);

  private:
    struct Subscription {
        qint64 generation{0};
        qint64 cursor{0};
        bool live{false};
        bool dirty{true};
    };
    struct Peer {
        QString device;
        int version{0};
        QJsonObject grant;
        QPointer<PeerLink> link;
        qint64 epoch{0};
        qint64 created{0};
        qint64 lastPing{0};
        QString pingNonce;
        int missedPongs{0};
        int nextSubscription{0};
        qint64 inputWindow{0};
        int inputFrames{0};
        bool hello{false};
        FrameCodec codec;
        SendQueue queue;
        QHash<QString, Subscription> subscriptions;
        QSet<QString> commands;
        quint64 nextTransfer{1};
    };
    void receive(const QString& id, Channel channel, const QByteArray& bytes, bool binary);
    Reader reader_;
    QHash<QString, std::shared_ptr<Peer>> peers_;
    QTimer timer_;
};
} // namespace ketplus::daemon::transport
