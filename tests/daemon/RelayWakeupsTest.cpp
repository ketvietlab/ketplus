#include "cloud/RelayWakeups.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>

#include <memory>

using namespace ketplus::daemon;

namespace {

// Stands in for the relay's /v2/relay: accepts sockets and remembers how they asked.
class FakeRelay final : public QObject {
    Q_OBJECT

  public:
    explicit FakeRelay(const QStringList& protocols = {QStringLiteral("ketplus.control.v2")})
        : server_(QStringLiteral("relay"), QWebSocketServer::NonSecureMode) {
        server_.setSupportedSubprotocols(protocols);
        QVERIFY(server_.listen(QHostAddress::LocalHost));
        connect(&server_, &QWebSocketServer::newConnection, this, [this] {
            while (QWebSocket* socket = server_.nextPendingConnection()) {
                socket->setParent(this);
                authorizations.append(
                    QString::fromUtf8(socket->request().rawHeader("Authorization")));
                paths.append(socket->request().url().path());
                sockets.append(socket);
                emit accepted(socket);
            }
        });
    }

    [[nodiscard]] QUrl baseUrl() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort()));
    }

    [[nodiscard]] QWebSocket* last() const { return sockets.isEmpty() ? nullptr : sockets.last(); }

    QStringList authorizations;
    QStringList paths;
    QList<QPointer<QWebSocket>> sockets;

  signals:
    void accepted(QWebSocket* socket);

  private:
    QWebSocketServer server_;
};

std::unique_ptr<RelayWakeups> wakeups(const QUrl& base, QStringList* tokens) {
    auto result =
        std::make_unique<RelayWakeups>(base, [tokens](std::function<void(const QString&)> answer) {
            answer(tokens->isEmpty() ? QString() : tokens->takeFirst());
        });
    result->setTimings(20, 100, 25'000);
    return result;
}

} // namespace

class RelayWakeupsTest final : public QObject {
    Q_OBJECT

  private slots:
    void wakeUpUrlFollowsTheControlScheme() {
        QStringList none;
        // A relay served under a tenant path keeps that path.
        QCOMPARE(
            wakeups(QUrl(QStringLiteral("https://relay.example/native/ketplus")), &none)->url(),
            QUrl(QStringLiteral("wss://relay.example/native/ketplus/v2/relay")));
        QCOMPARE(wakeups(QUrl(QStringLiteral("http://127.0.0.1:8790")), &none)->url(),
                 QUrl(QStringLiteral("ws://127.0.0.1:8790/v2/relay")));
    }

    void connectsWithTheDaemonCredentialAndWakesOnCommands() {
        FakeRelay relay;
        QStringList tokens{QStringLiteral("daemon-token")};
        auto client = wakeups(relay.baseUrl(), &tokens);
        QSignalSpy open(client.get(), &RelayWakeups::openChanged);
        QSignalSpy wake(client.get(), &RelayWakeups::wake);
        client->start();

        QTRY_VERIFY(client->isOpen());
        QCOMPARE(open.size(), 1);
        QCOMPARE(relay.authorizations, QStringList{QStringLiteral("Bearer daemon-token")});
        QCOMPARE(relay.paths, QStringList{QStringLiteral("/v2/relay")});

        relay.last()->sendTextMessage(QStringLiteral(R"({"type":"ready","role":"daemon"})"));
        QTRY_COMPARE(wake.size(), 1);
        // Hints for the phone are not the daemon's business.
        relay.last()->sendTextMessage(QStringLiteral(R"({"type":"sessions"})"));
        relay.last()->sendTextMessage(QStringLiteral("not json"));
        relay.last()->sendTextMessage(QStringLiteral(R"({"type":"commands"})"));
        QTRY_COMPARE(wake.size(), 2);
        relay.last()->sendTextMessage(QStringLiteral(R"({"type":"resync"})"));
        QTRY_COMPARE(wake.size(), 3);

        client->stop();
        QVERIFY(!client->isOpen());
        QCOMPARE(open.size(), 2);
        QCOMPARE(open.last().first().toBool(), false);
    }

    void carriesSignalingAndReportsRevocation() {
        FakeRelay relay;
        QStringList tokens{QStringLiteral("synthetic")};
        auto client = wakeups(relay.baseUrl(), &tokens);
        QSignalSpy messages(client.get(), &RelayWakeups::message);
        QSignalSpy revoked(client.get(), &RelayWakeups::authorizationRevoked);
        QVERIFY(!client->send({{"type", "signal.answer"}}));
        client->start();
        QTRY_VERIFY(client->isOpen());
        QSignalSpy inbound(relay.last(), &QWebSocket::textMessageReceived);
        const QJsonObject answer{{"type", "signal.answer"}, {"connectionId", "synthetic"}};
        QVERIFY(client->send(answer));
        QTRY_COMPARE(inbound.size(), 1);
        QCOMPARE(QJsonDocument::fromJson(inbound.first().first().toString().toUtf8()).object(),
                 answer);
        relay.last()->sendTextMessage(
            QStringLiteral(R"({"type":"signal.offer","connectionId":"synthetic"})"));
        QTRY_COMPARE(messages.size(), 1);
        QVERIFY(!client->send({{"type", "signal.offer"}, {"sdp", QString(65 * 1024, 'x')}}));
        relay.last()->close(QWebSocketProtocol::CloseCode(4003), "revoked");
        QTRY_COMPARE(revoked.size(), 1);
        client->stop();
    }

    void expiredCredentialIsDroppedAndTheSocketReopens() {
        FakeRelay relay;
        QStringList tokens{QStringLiteral("first"), QStringLiteral("second")};
        auto client = wakeups(relay.baseUrl(), &tokens);
        QSignalSpy expired(client.get(), &RelayWakeups::credentialExpired);
        client->start();
        QTRY_VERIFY(client->isOpen());

        relay.last()->close(QWebSocketProtocol::CloseCode(RelayWakeups::closeCredentialExpired),
                            QStringLiteral("credential_expired"));
        QTRY_COMPARE(expired.size(), 1);
        QTRY_COMPARE(relay.authorizations.size(), 2);
        QCOMPARE(relay.authorizations.last(), QStringLiteral("Bearer second"));
        QTRY_VERIFY(client->isOpen());
    }

    void reconnectsWhenTheRelayGoesAway() {
        FakeRelay relay;
        QStringList tokens{QStringLiteral("a"), QStringLiteral("b")};
        auto client = wakeups(relay.baseUrl(), &tokens);
        QSignalSpy expired(client.get(), &RelayWakeups::credentialExpired);
        client->start();
        QTRY_VERIFY(client->isOpen());

        relay.last()->close(QWebSocketProtocol::CloseCodeGoingAway);
        QTRY_COMPARE(relay.sockets.size(), 2);
        QTRY_VERIFY(client->isOpen());
        QCOMPARE(expired.size(), 0);
    }

    void waitsForACredential() {
        FakeRelay relay;
        int asked = 0;
        RelayWakeups client(relay.baseUrl(), [&asked](std::function<void(const QString&)> answer) {
            answer(++asked < 3 ? QString() : QStringLiteral("late"));
        });
        client.setTimings(10, 40, 25'000);
        client.start();
        QTRY_VERIFY(client.isOpen());
        QCOMPARE(asked, 3);
        QCOMPARE(relay.authorizations, QStringList{QStringLiteral("Bearer late")});
    }

    void refusesARelayThatDoesNotSpeakTheProtocol() {
        FakeRelay relay(QStringList{});
        QStringList tokens{QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")};
        auto client = wakeups(relay.baseUrl(), &tokens);
        QSignalSpy open(client.get(), &RelayWakeups::openChanged);
        client->start();
        QTRY_VERIFY(relay.sockets.size() >= 2);
        QCOMPARE(open.size(), 0);
        client->stop();
    }
};

QTEST_GUILESS_MAIN(RelayWakeupsTest)
#include "RelayWakeupsTest.moc"
