// Real links on this machine: the runner's connector against a desktop answerer through an
// in-process Relay, and the Relay socket adapter against local WebSocket and HTTP servers.
#include "RunnerPeerTest.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingSignatures.h"
#include "peer/QtPeerClock.h"
#include "peer/QtRelaySocket.h"
#include "peer/WebRtcTransport.h"
#include "transport/FrameCodec.h"
#include "transport/SendQueue.h"
#include "transport/WebRtcPeerLink.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>

namespace runner_peer_test {
namespace {

namespace cc = ketplus::control_crypto;
using ketplus::daemon::transport::FrameCodec;
using ketplus::daemon::transport::Packet;
using ketplus::daemon::transport::SendQueue;
using ketplus::daemon::transport::WebRtcPeerLink;

const QString kId = QString::fromLatin1(vectors::connectionId);

// The Relay and the desktop in one: answers signal.start, checks the runner's signed offer,
// answers it with a real link and a signed answer, and passes candidates both ways.
class LoopRelay final : public SignalSender {
  public:
    explicit LoopRelay(std::function<void(const QJsonObject&)> deliver)
        : m_deliver(std::move(deliver)) {}

    [[nodiscard]] bool relayReady() const override { return true; }
    bool sendSignal(const QJsonObject& message) override {
        const QJsonObject copy = message;
        QTimer::singleShot(0, &m_context, [this, copy] { handle(copy); });
        return true;
    }

    std::function<QString(QString)> rewriteAnswer = [](QString sdp) { return sdp; };
    bool offerVerified = false;
    QStringList ends;
    std::unique_ptr<WebRtcPeerLink> desktop;
    QList<Packet> desktopReceived;

  private:
    void post(const QJsonObject& message) {
        QTimer::singleShot(0, &m_context, [this, message] { m_deliver(message); });
    }

    void handle(const QJsonObject& message) {
        const QString type = message.value("type").toString();
        if (type == "signal.start") {
            post({{"type", "signal.started"},
                  {"connectionId", kId},
                  {"iceServers", QJsonArray()},
                  {"expiresAt",
                   QDateTime::currentDateTimeUtc().addSecs(60).toString(Qt::ISODateWithMs)}});
        } else if (type == "signal.offer") {
            offer(message);
        } else if (type == "signal.candidate" && desktop) {
            desktop->candidate(message.value("candidate").toString(),
                               message.value("sdpMid").toString());
        } else if (type == "signal.end") {
            ends.append(message.value("reason").toString());
        }
    }

    void offer(const QJsonObject& message) {
        pairing::OfferPayload payload{message.value("connectionId").toString(),
                                      QString::fromLatin1(vectors::workspaceId),
                                      QString::fromLatin1(vectors::machineId),
                                      QString::fromLatin1(vectors::runnerId),
                                      message.value("runnerKeyVersion").toInt(),
                                      message.value("jobId").toString(),
                                      message.value("attempt").toInt(),
                                      message.value("issuedAt").toString(),
                                      message.value("sdp").toString()};
        offerVerified = !pairing::verify(
            payload, cc::fromBase64Url(message.value("signature").toString()),
            {QByteArray::fromHex(vectors::runnerPublicKey), vectors::runnerKeyVersion},
            QDateTime::currentDateTimeUtc());
        if (!offerVerified)
            return;
        const QString offerSdp = payload.sdp;
        desktop = std::make_unique<WebRtcPeerLink>();
        QObject::connect(desktop.get(), &WebRtcPeerLink::localDescription, desktop.get(),
                         [this, offerSdp](const QString& sdp) {
                             QJsonObject answer =
                                 signedAnswer(kId, offerSdp, sdp, QDateTime::currentDateTimeUtc());
                             answer["sdp"] = rewriteAnswer(sdp);
                             post(answer);
                         });
        QObject::connect(
            desktop.get(), &WebRtcPeerLink::localCandidate, desktop.get(),
            [this](const QString& candidate, const QString& mid) {
                post({{"type", "signal.candidate"},
                      {"connectionId", kId},
                      {"candidate", candidate.isEmpty() ? QJsonValue() : QJsonValue(candidate)},
                      {"sdpMid", mid}});
            });
        QObject::connect(desktop.get(), &WebRtcPeerLink::received, desktop.get(),
                         [this](Channel channel, const QByteArray& bytes, bool binary) {
                             desktopReceived.append({channel, bytes, binary});
                         });
        desktop->answer(offerSdp, {});
    }

    std::function<void(const QJsonObject&)> m_deliver;
    QObject m_context;
};

struct Loop {
    Loop() { channels.attach(connector); }
    void request() {
        const PeerRequest request{QString::fromLatin1(vectors::jobId), QStringLiteral("lease_1"), 1,
                                  QString::fromLatin1(vectors::machineId)};
        channels.setRequest(request);
        connector.request(request);
    }

    RecordingChannelListener listener;
    LoopRelay relay{[this](const QJsonObject& message) { connector.signal(message); }};
    QtPeerClock clock{[this](PeerTimer timer) { connector.timerFired(timer); }};
    WebRtcTransportFactory transports;
    FakeSigner signer;
    FakePins pins;
    PeerChannels channels{clock, listener};
    PeerConnector connector{relay, clock, transports, signer, pins, channels};
};

QByteArray pattern(const qsizetype size) {
    QByteArray bytes(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i)
        bytes[i] = static_cast<char>(i * 13 + 5);
    return bytes;
}

class RecordingSocketEvents final : public RelaySocketEvents {
  public:
    void socketOpened() override { events.append(QStringLiteral("opened")); }
    void socketText(const QString& text) override { texts.append(text); }
    void socketClosed(int code) override { events.append(QStringLiteral("closed:%1").arg(code)); }
    void socketRefused(int status) override {
        events.append(QStringLiteral("refused:%1").arg(status));
    }

    QStringList events;
    QStringList texts;
};

// A Relay endpoint that speaks the control subprotocol, or none.
struct RelayServer {
    explicit RelayServer(bool withProtocol = true) {
        if (withProtocol)
            server.setSupportedSubprotocols({QStringLiteral("ketplus.control.v2")});
        QObject::connect(&server, &QWebSocketServer::newConnection, &server, [this] {
            while (QWebSocket* socket = server.nextPendingConnection()) {
                authorization = socket->request().rawHeader("Authorization");
                version = socket->request().rawHeader("X-KetPlus-Control-Version");
                path = socket->requestUrl().path();
                QObject::connect(socket, &QWebSocket::textMessageReceived, socket,
                                 [this](const QString& text) { received.append(text); });
                peer = socket;
            }
        });
        server.listen(QHostAddress::LocalHost);
    }
    [[nodiscard]] QUrl base() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/control").arg(server.serverPort()));
    }

    QWebSocketServer server{QStringLiteral("relay"), QWebSocketServer::NonSecureMode};
    QPointer<QWebSocket> peer;
    QByteArray authorization;
    QByteArray version;
    QString path;
    QStringList received;
};

} // namespace

void RunnerPeerLoopbackTest::runnerAndDesktopExchangeFramesAndATransfer() {
    Loop loop;
    loop.request();
    QTRY_VERIFY_WITH_TIMEOUT(loop.connector.isOpen(), 15'000);
    QVERIFY(loop.relay.offerVerified);
    QCOMPARE(loop.listener.events, QStringList{"opened " + kId});
    WebRtcPeerLink& desktop = *loop.relay.desktop;

    // Runner to desktop on control and events.
    loop.channels.send(protocol::Channel::Control, "{\"type\":\"hello\"}");
    loop.channels.send(protocol::Channel::Events, "{\"type\":\"event\"}");
    QTRY_COMPARE_WITH_TIMEOUT(loop.relay.desktopReceived.size(), 2, 5'000);
    QCOMPARE(loop.relay.desktopReceived[0].channel, Channel::Control);
    QCOMPARE(loop.relay.desktopReceived[0].bytes, QByteArray("{\"type\":\"hello\"}"));
    QVERIFY(!loop.relay.desktopReceived[0].binary);
    QCOMPARE(loop.relay.desktopReceived[1].channel, Channel::Events);

    // Desktop to runner: frames, then a 4 MiB transfer through the production send queue.
    QVERIFY(desktop.send({Channel::Control, "{\"type\":\"job\"}", false}));
    QVERIFY(desktop.send({Channel::Events, "{\"type\":\"tick\"}", false}));
    QTRY_COMPARE_WITH_TIMEOUT(loop.listener.frames.size(), 2, 5'000);
    QCOMPARE(loop.listener.frames[0].first, protocol::Channel::Control);
    QCOMPARE(loop.listener.frames[0].second, QByteArray("{\"type\":\"job\"}"));
    QCOMPARE(loop.listener.frames[1].first, protocol::Channel::Events);

    const QByteArray bytes = pattern(4 * 1024 * 1024);
    SendQueue queue;
    QVERIFY(queue.append(FrameCodec::encodeTransfer(77, bytes)));
    QObject::connect(&desktop, &WebRtcPeerLink::writable, &desktop, [&] { queue.flush(desktop); });
    queue.flush(desktop);
    QTRY_COMPARE_WITH_TIMEOUT(loop.listener.transfers.size(), 1, 30'000);
    QCOMPARE(loop.listener.transfers[0].first, quint64(77));
    QVERIFY(loop.listener.transfers[0].second == bytes);
    QCOMPARE(loop.listener.events, QStringList{"opened " + kId});

    // The desktop leaving is reported once.
    QObject::disconnect(&desktop, nullptr, &desktop, nullptr);
    desktop.close();
    QTRY_COMPARE_WITH_TIMEOUT(loop.listener.events.size(), 2, 10'000);
    QCOMPARE(loop.listener.events.last(), QStringLiteral("lost peer_closed"));
    QTest::qWait(200);
    QCOMPARE(loop.listener.events.size(), 2);
}

void RunnerPeerLoopbackTest::aTamperedAnswerNeverOpens() {
    Loop loop;
    loop.relay.rewriteAnswer = [](QString sdp) {
        static const QRegularExpression print(
            QStringLiteral("a=fingerprint:sha-256 ([0-9A-F]{2})"));
        return sdp.replace(print, QStringLiteral("a=fingerprint:sha-256 00"));
    };
    loop.request();
    QTRY_VERIFY_WITH_TIMEOUT(!loop.listener.events.isEmpty(), 15'000);
    QCOMPARE(loop.listener.events.first(), QStringLiteral("failed invalid_signature"));
    QTRY_COMPARE_WITH_TIMEOUT(loop.relay.ends, QStringList{QStringLiteral("invalid_signature")},
                              5'000);
    QTest::qWait(500);
    QVERIFY(!loop.connector.isOpen());
    QVERIFY(!loop.listener.events.contains("opened " + kId));
    loop.connector.close(QStringLiteral("done"));
}

void RunnerPeerLoopbackTest::relaySocketOpensAndReadsReady() {
    RelayServer relay;
    RecordingSocketEvents events;
    QtRelaySocket socket(relay.base(), events);
    QVERIFY(!socket.send({{"type", "early"}}));
    socket.open(QStringLiteral("TOKEN-SECRET-1"));
    QTRY_COMPARE_WITH_TIMEOUT(events.events, QStringList{QStringLiteral("opened")}, 5'000);
    QCOMPARE(relay.authorization, QByteArray("Bearer TOKEN-SECRET-1"));
    QCOMPARE(relay.version, QByteArray("2"));
    QCOMPARE(relay.path, QStringLiteral("/control/v2/relay"));
    QVERIFY(relay.peer);
    relay.peer->sendTextMessage(
        QStringLiteral(R"({"type":"ready","role":"runner","heartbeatSeconds":25})"));
    QTRY_COMPARE_WITH_TIMEOUT(events.texts.size(), 1, 5'000);
    QCOMPARE(QJsonDocument::fromJson(events.texts[0].toUtf8()).object().value("role").toString(),
             QStringLiteral("runner"));
    QVERIFY(socket.send({{"type", "signal.start"}}));
    QTRY_COMPARE_WITH_TIMEOUT(relay.received.size(), 1, 5'000);
    QVERIFY(!socket.send({{"type", "big"}, {"pad", QString(64 * 1024, 'x')}}));

    // A second open replaces the first socket; the first reports nothing more.
    socket.open(QStringLiteral("TOKEN-SECRET-2"));
    QTRY_COMPARE_WITH_TIMEOUT(events.events.size(), 2, 5'000);
    QCOMPARE(events.events.last(), QStringLiteral("opened"));
    QCOMPARE(relay.authorization, QByteArray("Bearer TOKEN-SECRET-2"));
    socket.close();
    QTest::qWait(200);
    QCOMPARE(events.events.size(), 2);
}

void RunnerPeerLoopbackTest::relaySocketReportsRefusals_data() {
    QTest::addColumn<QByteArray>("response");
    QTest::addColumn<int>("status");
    QTest::newRow("401") << QByteArray("HTTP/1.1 401 Unauthorized\r\n") << 401;
    QTest::newRow("401 bearer") << QByteArray("HTTP/1.1 401 Unauthorized\r\n"
                                              "WWW-Authenticate: Bearer\r\n")
                                << 401;
    QTest::newRow("403") << QByteArray("HTTP/1.1 403 Forbidden\r\n") << 403;
    QTest::newRow("503") << QByteArray("HTTP/1.1 503 Service Unavailable\r\n") << 503;
    QTest::newRow("closed port") << QByteArray() << 0;
    QTest::newRow("no subprotocol") << QByteArray("websocket") << 0;
}

void RunnerPeerLoopbackTest::relaySocketReportsRefusals() {
    QFETCH(QByteArray, response);
    QFETCH(int, status);
    RecordingSocketEvents events;
    std::unique_ptr<RelayServer> websocket;
    QTcpServer http;
    QUrl base;
    if (response == "websocket") {
        websocket = std::make_unique<RelayServer>(false);
        base = websocket->base();
    } else {
        QVERIFY(http.listen(QHostAddress::LocalHost));
        base = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(http.serverPort()));
        if (response.isEmpty()) {
            http.close();
        } else {
            QObject::connect(&http, &QTcpServer::newConnection, &http, [&http, response] {
                QTcpSocket* client = http.nextPendingConnection();
                QObject::connect(client, &QTcpSocket::readyRead, client, [client, response] {
                    client->readAll();
                    client->write(response + "Content-Length: 0\r\nConnection: close\r\n\r\n");
                    client->disconnectFromHost();
                });
            });
        }
    }
    QtRelaySocket socket(base, events);
    socket.open(QStringLiteral("TOKEN-SECRET-3"));
    QTRY_COMPARE_WITH_TIMEOUT(events.events.size(), 1, 5'000);
    QCOMPARE(events.events, QStringList{QStringLiteral("refused:%1").arg(status)});
    QTest::qWait(100);
    QCOMPARE(events.events.size(), 1);
}

void RunnerPeerLoopbackTest::relaySocketReportsCloseCodes_data() {
    QTest::addColumn<int>("code");
    QTest::newRow("4001") << 4001;
    QTest::newRow("4003") << 4003;
    QTest::newRow("1000") << 1000;
    QTest::newRow("1001") << 1001;
    QTest::newRow("abort") << 0;
}

void RunnerPeerLoopbackTest::relaySocketReportsCloseCodes() {
    QFETCH(int, code);
    RelayServer relay;
    RecordingSocketEvents events;
    QtRelaySocket socket(relay.base(), events);
    socket.open(QStringLiteral("TOKEN-SECRET-4"));
    QTRY_COMPARE_WITH_TIMEOUT(events.events.size(), 1, 5'000);
    QVERIFY(relay.peer);
    if (code == 0)
        relay.peer->abort();
    else
        relay.peer->close(static_cast<QWebSocketProtocol::CloseCode>(code));
    QTRY_COMPARE_WITH_TIMEOUT(events.events.size(), 2, 5'000);
    QCOMPARE(events.events.last(), QStringLiteral("closed:%1").arg(code == 0 ? 1006 : code));
}

void RunnerPeerLoopbackTest::nothingSecretWasLogged() {
    // Runs last: every line logged by every test in this binary, debug lines included.
    const QStringList lines = logLines();
    QVERIFY(!lines.isEmpty());
    static const QRegularExpression signature(QStringLiteral("[A-Za-z0-9_-]{86}"));
    for (const QString& line : lines) {
        for (const char* marker :
             {tokenMarker, turnMarker, "a=fingerprint", "candidate:", "v=0", "Bearer"})
            QVERIFY2(!line.contains(QLatin1StringView(marker)), marker);
        QVERIFY2(!signature.match(line).hasMatch(), "a signature-sized value was logged");
    }
}

} // namespace runner_peer_test
