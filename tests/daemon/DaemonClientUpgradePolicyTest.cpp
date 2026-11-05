#include "DaemonClient.h"
#include "KetPlusProductVersion.h"

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
