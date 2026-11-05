#include "atlas/AtlasLiveTunnel.h"
#include <QSignalSpy>
#include <QTcpServer>
#include <QWebSocketServer>
#include <QtTest>
using namespace ketplus::daemon;
class AtlasLiveTunnelTest final : public QObject {
    Q_OBJECT
  private slots:
    void ciphertextRejectsReplayWrongDirectionAndTampering() {
        const QByteArray key(32, char(7));
        AtlasTunnelCodec phone(key, QString(32, 'a'), false), mac(key, QString(32, 'a'), true);
        const auto wire =
            phone.seal({{{"kind", "data"}, {"stream", 1}}, QByteArray::fromHex("00ff8041")});
        AtlasTunnelFrame frame;
        QVERIFY(mac.open(wire, &frame));
        QCOMPARE(frame.body, QByteArray::fromHex("00ff8041"));
        QVERIFY(!mac.open(wire, &frame));
        QVERIFY(!phone.open(wire, &frame));
        AtlasTunnelCodec other(key, QString(32, 'b'), true);
        QVERIFY(!other.open(wire, &frame));
        AtlasTunnelCodec fresh(key, QString(32, 'a'), true);
        auto changed = wire;
        changed[changed.size() - 1] ^= 1;
        QVERIFY(!fresh.open(changed, &frame));
        QVERIFY(fresh.open(wire, &frame));
    }
    void binaryHttpFlowsAcrossCreditWindowsWithoutTruncatingEof() {
        QTcpServer origin;
        QVERIFY(origin.listen(QHostAddress::LocalHost, 0));
        QByteArray body(160000, Qt::Uninitialized);
        for (qsizetype i = 0; i < body.size(); ++i)
            body[i] = char(i % 256);
        const QByteArray expected =
            "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        connect(&origin, &QTcpServer::newConnection, this, [&] {
            auto* tcp = origin.nextPendingConnection();
            connect(tcp, &QTcpSocket::readyRead, this, [&, tcp] {
                const auto request = tcp->readAll();
                if (!request.isEmpty()) {
                    tcp->write(expected);
                    tcp->disconnectFromHost();
                }
            });
        });
        QWebSocketServer relay("isolated-atlas", QWebSocketServer::NonSecureMode);
        QVERIFY(relay.listen(QHostAddress::LocalHost, 0));
        const QByteArray key(32, char(7));
        const QString channel(32, 'a');
        AtlasTunnelCodec phone(key, channel, false);
        QWebSocket* peer = nullptr;
        QByteArray received;
        bool ended = false, verified = true;
        connect(&relay, &QWebSocketServer::newConnection, this, [&] {
            peer = relay.nextPendingConnection();
            connect(peer, &QWebSocket::binaryMessageReceived, this, [&](const QByteArray& wire) {
                AtlasTunnelFrame frame;
                if (!phone.open(wire, &frame)) {
                    verified = false;
                    return;
                }
                const auto kind = frame.header.value("kind").toString();
                if (kind == "opened")
                    peer->sendBinaryMessage(
                        phone.seal({{{"kind", "data"}, {"stream", 1}},
                                    "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"}));
                else if (kind == "data") {
                    received += frame.body;
                    peer->sendBinaryMessage(phone.seal(
                        {{{"kind", "credit"}, {"stream", 1}, {"bytes", frame.body.size()}}, {}}));
                } else if (kind == "end")
                    ended = true;
            });
            peer->sendTextMessage("{\"type\":\"ready\",\"version\":1}");
            peer->sendBinaryMessage(
                phone.seal({{{"kind", "open"}, {"stream", 1}, {"origin", "viewer"}}, {}}));
        });
        AtlasLiveTunnel tunnel(key, channel, {{"viewer", origin.serverPort()}});
        tunnel.connectRelay(
            QUrl(QString("ws://127.0.0.1:%1/v2/atlas/tunnel").arg(relay.serverPort())),
            "test-only");
        QTRY_VERIFY_WITH_TIMEOUT(ended, 10000);
        QVERIFY(verified);
        QCOMPARE(received, expected);
        QSignalSpy closed(&tunnel, &AtlasLiveTunnel::closed);
        peer->sendBinaryMessage(phone.seal(
            {{{"kind", "open"}, {"stream", 2}, {"origin", "arbitrary-local-service"}}, {}}));
        QTRY_COMPARE(closed.size(), 1);
        peer->deleteLater();
    }
};
QTEST_GUILESS_MAIN(AtlasLiveTunnelTest)
#include "AtlasLiveTunnelTest.moc"
