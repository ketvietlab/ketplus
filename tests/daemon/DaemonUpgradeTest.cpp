#include "ipc/LocalIpcServer.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include "FakeCodex.h"
#include "KetPlusProductVersion.h"

#include <QDir>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;

namespace {

QJsonObject request(const QString& type, const QString& id, const QJsonObject& payload = {}) {
    return {{QStringLiteral("protocol_version"), protocol::currentVersion},
            {QStringLiteral("message_id"), id},
            {QStringLiteral("message_type"), type},
            {QStringLiteral("sent_at"), protocol::timestamp()},
            {QStringLiteral("payload"), payload}};
}

class WireClient final : public QObject {
  public:
    explicit WireClient(QObject* parent = nullptr) : QObject(parent) {
        connect(&socket_, &QLocalSocket::readyRead, this, [this] { readFrames(); });
    }

    void connectTo(const QString& endpoint) { socket_.connectToServer(endpoint); }

    void send(const QJsonObject& envelope) {
        socket_.write(protocol::encodeFrame(envelope));
        socket_.flush();
    }

    [[nodiscard]] bool hasResponse(const QString& correlationId) const {
        for (const QJsonObject& message : messages_) {
            if (message.value(QStringLiteral("correlation_id")).toString() == correlationId) {
                return true;
            }
        }
        return false;
    }

    QJsonObject takeResponse(const QString& correlationId) {
        for (qsizetype index = 0; index < messages_.size(); ++index) {
            if (messages_.at(index).value(QStringLiteral("correlation_id")).toString() ==
                correlationId) {
                return messages_.takeAt(index);
            }
        }
        return {};
    }

    QLocalSocket socket_;

  private:
    void readFrames() {
        buffer_.append(socket_.readAll());
        while (true) {
            if (expected_ == 0) {
                if (buffer_.size() < 4) {
                    return;
                }
                expected_ = (static_cast<quint32>(static_cast<uchar>(buffer_[0])) << 24U) |
                            (static_cast<quint32>(static_cast<uchar>(buffer_[1])) << 16U) |
                            (static_cast<quint32>(static_cast<uchar>(buffer_[2])) << 8U) |
                            static_cast<quint32>(static_cast<uchar>(buffer_[3]));
                buffer_.remove(0, 4);
            }
            if (buffer_.size() < static_cast<qsizetype>(expected_)) {
                return;
            }
            const QJsonDocument document = QJsonDocument::fromJson(buffer_.left(expected_));
            buffer_.remove(0, expected_);
            expected_ = 0;
            if (document.isObject()) {
                messages_.append(document.object());
            }
        }
    }

    QByteArray buffer_;
    quint32 expected_{0};
    QList<QJsonObject> messages_;
};

void authenticate(WireClient& client, const QString& endpoint, const QByteArray& token) {
    client.connectTo(endpoint);
    QTRY_COMPARE_WITH_TIMEOUT(client.socket_.state(), QLocalSocket::ConnectedState, 1000);
    client.send(request(QStringLiteral("daemon.hello"), QStringLiteral("hello"),
                        {{QStringLiteral("token"), QString::fromUtf8(token)},
                         {QStringLiteral("client_id"), QStringLiteral("upgrade-test")}}));
    QTRY_VERIFY_WITH_TIMEOUT(client.hasResponse(QStringLiteral("hello")), 1000);
}

} // namespace

class DaemonUpgradeTest final : public QObject {
    Q_OBJECT

  private slots:
    void idleDaemonAdvertisesVersionAndAllowsReplacement_data() {
        QTest::addColumn<QString>("operation");
        QTest::newRow("upgrade") << QStringLiteral("daemon.prepare_upgrade");
        QTest::newRow("consented-uninstall") << QStringLiteral("daemon.stop_if_idle");
    }
    void idleDaemonAdvertisesVersionAndAllowsReplacement() {
        QFETCH(QString, operation);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        CodexAdapter adapter(QStringLiteral("/nonexistent/ketplus-codex"));
        SessionHost host(&journal, &adapter);
        const QByteArray token("local-test-token-with-at-least-32-bytes");
        LocalIpcServer server(&host, directory.filePath(QStringLiteral("daemon.sock")), token);
        QVERIFY(server.listen());
        QSignalSpy shutdown(&server, &LocalIpcServer::shutdownRequested);

        WireClient client;
        authenticate(client, server.serverName(), token);
        const QJsonObject hello = client.takeResponse(QStringLiteral("hello"))
                                      .value(QStringLiteral("payload"))
                                      .toObject();
        QCOMPARE(hello.value(QStringLiteral("daemon_version")).toString(),
                 QStringLiteral(KETPLUS_PRODUCT_VERSION));

        client.send(request(
            QStringLiteral("daemon.prepare_upgrade"), QStringLiteral("same-version"),
            {{QStringLiteral("replacement_version"), QStringLiteral(KETPLUS_PRODUCT_VERSION)}}));
        QTRY_VERIFY_WITH_TIMEOUT(client.hasResponse(QStringLiteral("same-version")), 1000);
        QCOMPARE(client.takeResponse(QStringLiteral("same-version"))
                     .value(QStringLiteral("payload"))
                     .toObject()
                     .value(QStringLiteral("error_code"))
                     .toString(),
                 QStringLiteral("invalid_replacement_version"));
        QCOMPARE(shutdown.size(), 0);

        client.send(request(operation, QStringLiteral("replace"),
                            {{QStringLiteral("replacement_version"), QStringLiteral("999.0.0")}}));
        QTRY_VERIFY_WITH_TIMEOUT(client.hasResponse(QStringLiteral("replace")), 1000);
        QVERIFY(client.takeResponse(QStringLiteral("replace"))
                    .value(QStringLiteral("payload"))
                    .toObject()
                    .value(QStringLiteral("ok"))
                    .toBool());
        QCOMPARE(shutdown.size(), 1);

        client.send(request(QStringLiteral("session.start_turn"), QStringLiteral("late-turn"),
                            {{QStringLiteral("worktree_path"), directory.path()},
                             {QStringLiteral("prompt"), QStringLiteral("too late")}}));
        QTRY_VERIFY_WITH_TIMEOUT(client.hasResponse(QStringLiteral("late-turn")), 1000);
        QCOMPARE(client.takeResponse(QStringLiteral("late-turn"))
                     .value(QStringLiteral("payload"))
                     .toObject()
                     .value(QStringLiteral("error_code"))
                     .toString(),
                 QStringLiteral("daemon_restarting"));
        QVERIFY(!host.hasActiveSessions());
    }

    void activeTurnDefersReplacement_data() {
        QTest::addColumn<QString>("operation");
        QTest::newRow("upgrade") << QStringLiteral("daemon.prepare_upgrade");
        QTest::newRow("consented-uninstall") << QStringLiteral("daemon.stop_if_idle");
    }
    void activeTurnDefersReplacement() {
        QFETCH(QString, operation);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("worktree"))));
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(executable, directory.filePath(QStringLiteral("provider.log"))));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const QByteArray token("local-test-token-with-at-least-32-bytes");
        LocalIpcServer server(&host, directory.filePath(QStringLiteral("daemon.sock")), token);
        QVERIFY(server.listen());
        QSignalSpy shutdown(&server, &LocalIpcServer::shutdownRequested);

        WireClient client;
        authenticate(client, server.serverName(), token);
        client.takeResponse(QStringLiteral("hello"));
        client.send(request(
            QStringLiteral("session.start_turn"), QStringLiteral("turn"),
            {{QStringLiteral("worktree_path"), directory.filePath(QStringLiteral("worktree"))},
             {QStringLiteral("prompt"), QStringLiteral("approve")}}));
        QTRY_VERIFY_WITH_TIMEOUT(client.hasResponse(QStringLiteral("turn")), 2000);
        QVERIFY(host.hasActiveSessions());

        client.send(request(operation, QStringLiteral("replace"),
                            {{QStringLiteral("replacement_version"), QStringLiteral("999.0.0")}}));
        QTRY_VERIFY_WITH_TIMEOUT(client.hasResponse(QStringLiteral("replace")), 1000);
        const QJsonObject payload = client.takeResponse(QStringLiteral("replace"))
                                        .value(QStringLiteral("payload"))
                                        .toObject();
        QCOMPARE(payload.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("daemon_busy"));
        QCOMPARE(shutdown.size(), 0);
        adapter.stop();
    }
};

QTEST_GUILESS_MAIN(DaemonUpgradeTest)
#include "DaemonUpgradeTest.moc"
