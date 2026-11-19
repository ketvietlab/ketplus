// DaemonClient connects on a new socket for every attempt. A named-pipe socket that was aborted
// and connected again can connect on Windows without ever delivering a reply; a new socket has no
// such history. These cases run against a local fake daemon that answers every request.
#include "DaemonClient.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <memory>

namespace {

class FakeDaemon final : public QObject {
  public:
    explicit FakeDaemon(const QString& path) {
        connect(&server, &QLocalServer::newConnection, this, [this] {
            while (QLocalSocket* socket = server.nextPendingConnection()) {
                ++connections;
                current = socket;
                connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QLocalSocket::readyRead, this,
                        [socket, buffer] { answerAll(socket, *buffer); });
            }
        });
        listening = server.listen(path);
    }

    QLocalServer server;
    QPointer<QLocalSocket> current;
    bool listening{false};
    int connections{0};

  private:
    static void answerAll(QLocalSocket* socket, QByteArray& buffer) {
        buffer.append(socket->readAll());
        while (buffer.size() >= 4) {
            const quint32 size = qFromBigEndian<quint32>(buffer.constData());
            if (buffer.size() < static_cast<qsizetype>(size) + 4)
                return;
            const QJsonObject request = QJsonDocument::fromJson(buffer.mid(4, size)).object();
            buffer.remove(0, static_cast<qsizetype>(size) + 4);
            const bool hello = request.value(QStringLiteral("message_type")).toString() ==
                               QStringLiteral("daemon.hello");
            QJsonObject payload{{QStringLiteral("ok"), true}, {QStringLiteral("answered"), true}};
            if (hello) {
                payload.insert(QStringLiteral("protocol_minor"), 1);
                payload.insert(QStringLiteral("features"), QJsonArray{});
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
    }
};

// Points DaemonClient at `directory` with a valid token and without starting a real daemon.
class Environment {
  public:
    explicit Environment(const QTemporaryDir& directory)
        : socket(directory.filePath(QStringLiteral("fake.sock"))) {
        const QString tokenPath = directory.filePath(QStringLiteral("local.token"));
        QFile token(tokenPath);
        ok = token.open(QIODevice::WriteOnly) &&
             token.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) &&
             token.write(QByteArray(43, 'k')) == 43;
        token.close();
        for (const char* key : {"KETPLUS_DAEMON_SOCKET", "KETPLUS_DAEMON_TOKEN_FILE",
                                "KETPLUS_DISABLE_DAEMON_AUTOSTART"})
            saved.insert(key, qgetenv(key));
        qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", tokenPath.toUtf8());
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
    }
    ~Environment() {
        for (auto it = saved.cbegin(); it != saved.cend(); ++it)
            it.value().isNull() ? qunsetenv(it.key().constData())
                                : qputenv(it.key().constData(), it.value());
    }
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;

    QString socket;
    bool ok{false};

  private:
    QMap<QByteArray, QByteArray> saved;
};

bool callAnswered(ketplus::remote::DaemonClient& client) {
    bool answered = false;
    client.call(QStringLiteral("remote.status"), {}, {}, -1,
                [&answered](const QJsonObject& payload, const QString& error) {
                    answered =
                        error.isEmpty() && payload.value(QStringLiteral("answered")).toBool();
                });
    return QTest::qWaitFor([&answered] { return answered; }, 10'000);
}

} // namespace

class DaemonClientReconnectTest final : public QObject {
    Q_OBJECT

  private slots:
    void aLostConnectionIsReplacedByANewSocketThatStillGetsReplies() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const Environment environment(directory);
        QVERIFY(environment.ok);
        FakeDaemon daemon(environment.socket);
        QVERIFY(daemon.listening);

        ketplus::remote::DaemonClient client;
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 10'000);
        QVERIFY(callAnswered(client));
        const auto* first = client.findChild<QLocalSocket*>();
        QVERIFY(first);

        daemon.current->disconnectFromServer();
        QTRY_COMPARE_WITH_TIMEOUT(daemon.connections, 2, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 10'000);
        const auto* second = client.findChild<QLocalSocket*>();
        QVERIFY(second);
        // Not the same socket reconnected: the old one is gone and cannot call back into the
        // client.
        QVERIFY(second != first);
        QTRY_COMPARE(client.findChildren<QLocalSocket*>().size(), 1);
        QVERIFY(callAnswered(client));
    }

    void aDaemonThatAppearsAfterRefusedAttemptsGetsRepliesToo() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const Environment environment(directory);
        QVERIFY(environment.ok);

        ketplus::remote::DaemonClient client;
        // No daemon yet: each attempt fails on its own socket, and none is left behind.
        QTest::qWait(500);
        QVERIFY(!client.isReady());
        QVERIFY(client.findChildren<QLocalSocket*>().size() <= 2);

        FakeDaemon daemon(environment.socket);
        QVERIFY(daemon.listening);
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 15'000);
        QVERIFY(callAnswered(client));
        QTRY_COMPARE(client.findChildren<QLocalSocket*>().size(), 1);
    }
};

QTEST_MAIN(DaemonClientReconnectTest)
#include "DaemonClientReconnectTest.moc"
