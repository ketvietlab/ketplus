#include "FakeCodex.h"
#include "cloud-privacy/CloudPrivacyTestAccess.h"
#include "cloud-privacy/FakeTransport.h"
#include "cloud-privacy/MemoryCredentials.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
using namespace ketplus::daemon;
using Access = CloudPrivacyTestAccess;
class RelayAttachmentHostTest final : public QObject {
    Q_OBJECT
  private slots:
    void desktopEncryptedAttachmentsReachRealHostAndTurn() {
        r03::resetCredentials();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto oldData = qgetenv("KETPLUS_DAEMON_DATA_DIR");
        qputenv("KETPLUS_DAEMON_DATA_DIR", dir.filePath("data").toUtf8());
        const auto restoreData = qScopeGuard([&] {
            if (oldData.isNull())
                qunsetenv("KETPLUS_DAEMON_DATA_DIR");
            else
                qputenv("KETPLUS_DAEMON_DATA_DIR", oldData);
        });
        const QString worktree = dir.filePath("worktree");
        QVERIFY(QDir().mkpath(worktree));
        const QString executable = dir.filePath("fake-codex");
        const QString providerLog = dir.filePath("provider.log");
        QVERIFY(writeFakeCodex(executable, providerLog));
        LocalJournal journal(dir.filePath("journal.sqlite"));
        QVERIFY(journal.upsertSession(
            {"session-synthetic", worktree, "codex", "thread-synthetic", {}, "ready", 1, 0, 1}));
        CodexAdapter provider(executable);
        SessionHost host(&journal, &provider);
        r03::Transport relay;
        relay.status = 202;
        CloudControlClient client(&journal, &host);
        Access::configure(client, relay);
        auto& crypto = Access::crypto(client);
        const auto keys = crypto.publicKeys();
        Access::pinDevice(client, "desktop-attachment", 1, keys);
        relay.statusForPath.insert("/v2/device-keys/desktop-attachment/1", 200);
        relay.responseForPath.insert(
            "/v2/device-keys/desktop-attachment/1",
            QJsonDocument(QJsonObject{{"keys", keys}}).toJson(QJsonDocument::Compact));
        QString failure;
        int serial = 0;
        const auto run = [&](const QString& kind, const QJsonObject& payload) -> QJsonObject {
            const QString id = "attachment-command-" + QString::number(++serial);
            QJsonObject command{{"id", id},
                                {"workspaceId", "workspace-synthetic"},
                                {"machineId", "machine-synthetic"},
                                {"sessionId", "session-synthetic"},
                                {"sessionGeneration", 1},
                                {"kind", kind},
                                {"capability", kind},
                                {"idempotencyKey", id + "-key"},
                                {"attempt", 1},
                                {"claimToken", id + "-claim"},
                                {"content", QJsonObject{{"keyId", "session-synthetic:1:1"},
                                                        {"keyEpoch", 1},
                                                        {"senderDeviceId", "desktop-attachment"},
                                                        {"senderKeyVersion", 1}}}};
            const auto encrypted =
                crypto.encrypt(QJsonDocument(payload).toJson(QJsonDocument::Compact),
                               Access::commandAad(client, command), "session-synthetic", 1, 1,
                               "desktop-attachment", 1);
            if (encrypted.isEmpty()) {
                failure = "Command encryption failed";
                return {};
            }
            command.insert("content", encrypted);
            Access::execute(
                client,
                command); // production CloudControlCommands dispatch, not SyntheticRelay results
            const QString path = "/v2/commands/" + id + "/ack";
            int found = -1;
            if (!QTest::qWaitFor(
                    [&] {
                        for (int i = 0; i < relay.requests.size(); ++i)
                            if (relay.requests[i].url().path() == path) {
                                found = i;
                                return true;
                            }
                        return false;
                    },
                    10000)) {
                failure = "No host acknowledgement";
                return {};
            }
            const auto ack = QJsonDocument::fromJson(relay.bodies[found]).object();
            const auto content = ack.value("encryptedResult").toObject();
            const auto decoded = crypto.decrypt(
                content,
                Access::resultAad(client, command, 1, ack.value("status").toString(),
                                  ack.value("resultCode").toString(), content),
                CloudCrypto::fromBase64Url(keys.value("signingPublicKey").toString()),
                "session-synthetic", 1, &failure);
            return QJsonDocument::fromJson(decoded).object();
        };
        const auto digest = [](const QByteArray& bytes) {
            return QString::fromLatin1(
                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        };
        const QList<QPair<QString, QByteArray>> files{
            {"text/plain", QByteArray("T50_TEXT_REFERENCE\n")},
            {"image/png", QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC"
                                                 "0lEQVR42mP8/x8AAwMCAO+aWQ0AAAAASUVORK5CYII=")}};
        QJsonArray ids;
        for (const auto& file : files) {
            const QString id = "file-" + QString::number(ids.size());
            auto result = run("attachment.stage.begin", {{"attachment_id", id},
                                                         {"expected_revision", 0},
                                                         {"name", id},
                                                         {"media_type", file.first},
                                                         {"total_bytes", file.second.size()},
                                                         {"chunk_count", 1},
                                                         {"digest_sha256", digest(file.second)}});
            QVERIFY2(failure.isEmpty(), qPrintable(failure));
            QVERIFY2(result.value("ok").toBool(), QJsonDocument(result).toJson().constData());
            result =
                run("attachment.stage.chunk",
                    {{"attachment_id", id},
                     {"expected_revision", 1},
                     {"chunk_index", 0},
                     {"data", QString::fromLatin1(file.second.toBase64(
                                  QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))},
                     {"digest_sha256", digest(file.second)}});
            QVERIFY2(result.value("ok").toBool(), QJsonDocument(result).toJson().constData());
            result =
                run("attachment.stage.commit", {{"attachment_id", id}, {"expected_revision", 1}});
            QVERIFY2(result.value("ok").toBool(), QJsonDocument(result).toJson().constData());
            const auto stored = journal.attachment("session-synthetic", 1, id);
            QCOMPARE(stored.status, QStringLiteral("ready"));
            QCOMPARE(stored.bytes, file.second);
            ids.append(id);
        }
        const auto result =
            run("session.start_turn", {{"prompt", "continue"}, {"attachment_ids", ids}});
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY2(result.value("ok").toBool(), QJsonDocument(result).toJson().constData());
        QTRY_VERIFY([&] {
            QFile log(providerLog);
            return log.open(QIODevice::ReadOnly) && log.readAll().contains("turn-start:");
        }());
        // Media is journalled by the actual SessionHost, not invented by the relay fixture.
        QVERIFY(QJsonDocument(journal.eventsAfter("session-synthetic", 1, 0))
                    .toJson()
                    .contains("\"images\""));
        for (const auto& body : relay.bodies)
            QVERIFY(!body.contains("T50_TEXT_REFERENCE"));
    }
};
QTEST_GUILESS_MAIN(RelayAttachmentHostTest)
#include "RelayAttachmentHostTest.moc"
