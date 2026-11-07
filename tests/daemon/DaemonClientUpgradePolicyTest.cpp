#include "DaemonClient.h"
#include "KetPlusProductVersion.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <memory>

namespace ketplus::remote {
class DaemonClientTestAccess {
  public:
    static QLocalSocket& socket(DaemonClient& client) { return client.socket_; }
};
} // namespace ketplus::remote

namespace {

class ExistingDaemon final : public QObject {
  public:
    explicit ExistingDaemon(const QString& path) {
        connect(&server, &QLocalServer::newConnection, this, [this] {
            while (QLocalSocket* socket = server.nextPendingConnection()) {
                ++connections;
                connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QLocalSocket::readyRead, this, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    while (buffer->size() >= 4) {
                        const quint32 size = qFromBigEndian<quint32>(buffer->constData());
                        if (size == 0 || size > 1024 * 1024) {
                            socket->abort();
                            return;
                        }
                        if (buffer->size() < static_cast<qsizetype>(size) + 4)
                            return;
                        const QJsonObject request =
                            QJsonDocument::fromJson(buffer->mid(4, size)).object();
                        buffer->remove(0, static_cast<qsizetype>(size) + 4);
                        const QString type =
                            request.value(QStringLiteral("message_type")).toString();
                        requests.append(type);
                        QJsonObject payload;
                        if (type == QStringLiteral("daemon.hello")) {
                            authenticated = request.value(QStringLiteral("payload"))
                                                .toObject()
                                                .value(QStringLiteral("token"))
                                                .toString() == QString(40, QLatin1Char('k'));
                            payload = {{QStringLiteral("ok"), authenticated && protocolCompatible},
                                       {QStringLiteral("daemon_version"), version},
                                       {QStringLiteral("protocol_minor"), 1},
                                       {QStringLiteral("features"),
                                        QJsonArray{QStringLiteral("fixture.capability")}}};
                        } else if (type == QStringLiteral("daemon.prepare_upgrade")) {
                            // The default policy must continue attaching when replacement is busy.
                            payload = {{QStringLiteral("ok"), false},
                                       {QStringLiteral("error_message"),
                                        QStringLiteral("fixture daemon is busy")}};
                        } else {
                            payload = {{QStringLiteral("ok"), authenticated},
                                       {QStringLiteral("sessions"), QJsonArray{}}};
                        }
                        const QByteArray body =
                            QJsonDocument(QJsonObject{{QStringLiteral("correlation_id"),
                                                       request.value(QStringLiteral("message_id"))},
                                                      {QStringLiteral("payload"), payload}})
                                .toJson(QJsonDocument::Compact);
                        QByteArray frame(4, Qt::Uninitialized);
                        qToBigEndian<quint32>(static_cast<quint32>(body.size()), frame.data());
                        frame.append(body);
                        socket->write(frame);
                    }
                });
            }
        });
        listening = server.listen(path);
    }

    QLocalServer server;
    QString version;
    QStringList requests;
    bool listening{false};
    bool authenticated{false};
    bool protocolCompatible{true};
    int connections{0};
};

} // namespace

class DaemonClientUpgradePolicyTest final : public QObject {
    Q_OBJECT

  private slots:
    void unreadTransportIsBounded_data() {
        QTest::addColumn<bool>("authenticated");
        QTest::newRow("before-hello") << false;
        QTest::newRow("after-hello") << true;
    }
    void unreadTransportIsBounded() {
        QFETCH(bool, authenticated);
        QTemporaryDir directory(QDir::tempPath() + "/kp-client-rx-XXXXXX");
        QVERIFY(directory.isValid());
        QMap<QByteArray, QByteArray> saved;
        for (const auto& key : {"KETPLUS_DAEMON_TOKEN_FILE", "KETPLUS_DAEMON_SOCKET",
                                "KETPLUS_DISABLE_DAEMON_AUTOSTART"})
            saved.insert(key, qgetenv(key));
        const auto restore = qScopeGuard([&] {
            for (auto it = saved.cbegin(); it != saved.cend(); ++it)
                it.value().isNull() ? qunsetenv(it.key().constData())
                                    : qputenv(it.key().constData(), it.value());
        });
        const auto endpoint = directory.filePath("fake.sock");
        const auto tokenPath = directory.filePath("local.token");
        QFile token(tokenPath);
        QVERIFY(token.open(QIODevice::WriteOnly));
        QVERIFY(token.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QCOMPARE(token.write(QByteArray(43, 'k')), 43);
        token.close();
        qputenv("KETPLUS_DAEMON_SOCKET", endpoint.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", tokenPath.toUtf8());
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");

        QByteArray requests;
        QJsonObject hello;
        QLocalSocket* peer = nullptr;
        QLocalServer server;
        QVERIFY(server.listen(endpoint));
        connect(&server, &QLocalServer::newConnection, &server, [&] {
            peer = server.nextPendingConnection();
            connect(peer, &QLocalSocket::readyRead, &server, [&] {
                requests.append(peer->readAll());
                if (!hello.isEmpty() || requests.size() < 4)
                    return;
                const auto size = qFromBigEndian<quint32>(requests.constData());
                if (requests.size() >= 4 + qsizetype(size))
                    hello = QJsonDocument::fromJson(requests.mid(4, size)).object();
            });
        });
        ketplus::remote::DaemonClient client;
        QSignalSpy events(&client, &ketplus::remote::DaemonClient::eventReceived);
        client.call("session.list", {}, {}, -1, [](const QJsonObject&, const QString&) {});
        QTRY_VERIFY_WITH_TIMEOUT(!hello.isEmpty(), 3000);
        QCOMPARE(hello.value("message_type").toString(), QString("daemon.hello"));
        const QJsonObject helloReply{{"correlation_id", hello.value("message_id")},
                                     {"payload", QJsonObject{{"ok", true}}}};
        const auto encode = [](const QJsonObject& value) {
            const auto body = QJsonDocument(value).toJson(QJsonDocument::Compact);
            QByteArray frame(4, Qt::Uninitialized);
            qToBigEndian<quint32>(quint32(body.size()), frame.data());
            return frame + body;
        };
        if (authenticated) {
            peer->write(encode(helloReply));
            QTRY_VERIFY(client.isReady());
        }
        auto& incoming = ketplus::remote::DaemonClientTestAccess::socket(client);
        QSignalBlocker held(&incoming);
        constexpr qsizetype frameLimit = 1024 * 1024;
        for (int index = 0; index < 3; ++index) {
            auto value = !authenticated && index == 0
                             ? helloReply
                             : QJsonObject{{"message_type", "session.event"}, {"index", index}};
            value.insert("padding", "");
            const auto overhead = QJsonDocument(value).toJson(QJsonDocument::Compact).size();
            value.insert("padding", QString(frameLimit - overhead, 'x'));
            const auto frame = encode(value);
            QCOMPARE(frame.size(), frameLimit + 4);
            QCOMPARE(peer->write(frame), frame.size());
        }
        peer->flush();
        QTRY_VERIFY_WITH_TIMEOUT(incoming.bytesAvailable() >= frameLimit, 2000);
        QTest::qWait(100);
        qInfo() << "Client unread bytes while dispatch held:" << incoming.bytesAvailable();
        QVERIFY2(incoming.bytesAvailable() <= frameLimit + 4,
                 "Client transport buffer exceeded one maximum frame plus header.");
        held.unblock();
        QVERIFY(QMetaObject::invokeMethod(&incoming, "readyRead", Qt::DirectConnection));
        const auto expected = authenticated ? 3 : 2;
        QTRY_COMPARE_WITH_TIMEOUT(events.size(), expected, 3000);
        QVERIFY(client.isReady());
        QCOMPARE(incoming.state(), QLocalSocket::ConnectedState);
        peer->write(encode({{"message_type", "session.event"}, {"index", 3}}));
        QTRY_COMPARE_WITH_TIMEOUT(events.size(), expected + 1, 1000);
    }

    void compatibleDaemon_data() {
        QTest::addColumn<QString>("version");
        QTest::addColumn<bool>("older");
        QTest::newRow("older") << QStringLiteral("0.0.1") << true;
        QTest::newRow("matching") << QStringLiteral(KETPLUS_PRODUCT_VERSION) << false;
        QTest::newRow("newer") << QStringLiteral("999.0.0") << false;
        QTest::newRow("legacy-no-version") << QString{} << false;
    }

    void compatibleDaemon() {
        QFETCH(QString, version);
        QFETCH(bool, older);
        run(version, true, older && KETPLUS_EXPECT_AUTO_UPGRADE);
    }

    void incompatibleDaemonCannotReceiveQueuedCalls() {
        run(QStringLiteral("0.0.1"), false, false);
    }

  private:
    void run(const QString& version, bool compatible, bool expectUpgrade) {
        QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/kp-client-policy-XXXXXX"));
        QVERIFY(directory.isValid());
        const QByteArray previousSocket = qgetenv("KETPLUS_DAEMON_SOCKET");
        const QByteArray previousToken = qgetenv("KETPLUS_DAEMON_TOKEN_FILE");
        const QByteArray previousAutostart = qgetenv("KETPLUS_DISABLE_DAEMON_AUTOSTART");
        const auto restore = qScopeGuard([&] {
            qputenv("KETPLUS_DAEMON_SOCKET", previousSocket);
            qputenv("KETPLUS_DAEMON_TOKEN_FILE", previousToken);
            qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", previousAutostart);
        });
        const QString tokenPath = directory.filePath(QStringLiteral("token"));
        QFile token(tokenPath);
        QVERIFY(token.open(QIODevice::WriteOnly));
        QCOMPARE(token.write(QByteArray(40, 'k')), 40);
        token.close();
        const QString socketPath = directory.filePath(QStringLiteral("daemon.sock"));
        qputenv("KETPLUS_DAEMON_SOCKET", socketPath.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", tokenPath.toUtf8());
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        ExistingDaemon daemon(socketPath);
        QVERIFY(daemon.listening);
        daemon.version = version;
        daemon.protocolCompatible = compatible;
        ketplus::remote::DaemonClient client;
        bool answered = false;
        client.call(QStringLiteral("session.list"), {}, {}, -1,
                    [&](const QJsonObject&, const QString& error) {
                        QVERIFY(error.isEmpty());
                        answered = true;
                    });
        QTRY_VERIFY(daemon.authenticated);
        if (!compatible) {
            QTRY_VERIFY(daemon.connections >= 2);
            QVERIFY(!client.isReady());
            QVERIFY(!answered);
            for (const QString& request : daemon.requests)
                QCOMPARE(request, QStringLiteral("daemon.hello"));
            return;
        }
        QTRY_VERIFY(answered);
        QVERIFY(client.isReady());
        QCOMPARE(client.protocolMinor(), 1);
        QVERIFY(client.hasFeature(QStringLiteral("fixture.capability")));
        QCOMPARE(daemon.connections, 1);
        QCOMPARE(daemon.requests.count(QStringLiteral("daemon.prepare_upgrade")),
                 expectUpgrade ? 1 : 0);
        QCOMPARE(daemon.requests.last(), QStringLiteral("session.list"));
    }
};

QTEST_GUILESS_MAIN(DaemonClientUpgradePolicyTest)
#include "DaemonClientUpgradePolicyTest.moc"
