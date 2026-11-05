#pragma once
#include "atlas/AtlasTunnelCodec.h"
#include <QHash>
#include <QObject>
#include <QTcpSocket>
#include <QWebSocket>

namespace ketplus::daemon {
// Byte streams are restricted to origins owned by this Atlas process. HTTP,
// renderer WebSockets and SSE retain their original semantics inside each stream.
class AtlasLiveTunnel final : public QObject {
    Q_OBJECT
  public:
    AtlasLiveTunnel(QByteArray key, QString channel, QHash<QString, quint16> origins,
                    QObject* parent = nullptr);
    void connectRelay(const QUrl& url, const QString& bearer);
    void stop();
  signals:
    void ready();
    void closed();

  private:
    struct Stream {
        QTcpSocket* socket;
        qint64 sendCredit{65536};
        qint64 receiveCredit{65536};
    };
    void receive(const QByteArray& wire);
    bool send(const QJsonObject& header, const QByteArray& bytes = {});
    void drain(quint32 id);
    void remove(quint32 id);
    AtlasTunnelCodec codec_;
    QHash<QString, quint16> origins_;
    QHash<quint32, Stream> streams_;
    QWebSocket socket_;
    quint32 lastStream_{0};
    quint32 drainAfter_{0};
    bool stopped_{false};
};
} // namespace ketplus::daemon
