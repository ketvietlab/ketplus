#include "FakeCodex.h"
#include "cloud-privacy/CloudPrivacyTestAccess.h"
#include "cloud-privacy/FakeTransport.h"
#include "cloud-privacy/MemoryCredentials.h"
#include "cloud/CloudControlSupport.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;
using Access = CloudPrivacyTestAccess;
class RelayPreparationHostTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
    QByteArray oldData_;
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("dev.ketplus.tests.relay-preparation");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        QVERIFY(QSettings().fileName().startsWith(settings_.path()));
        QVERIFY(!QSettings().contains("workspace/lastRoot"));
        oldData_ = qgetenv("KETPLUS_DAEMON_DATA_DIR");
        qputenv("KETPLUS_DAEMON_DATA_DIR", settings_.filePath("data").toUtf8());
    }
    void cleanupTestCase() {
        if (oldData_.isNull())
            qunsetenv("KETPLUS_DAEMON_DATA_DIR");
        else
            qputenv("KETPLUS_DAEMON_DATA_DIR", oldData_);
    }
    void encryptedPreparationPublishesOnlyTheCreatorKeyBeforeAcknowledgement_data() {
        QTest::addColumn<int>("routingStatus");
        QTest::addColumn<int>("keyStatus");
        QTest::addColumn<bool>("creatorAvailable");
        QTest::addColumn<bool>("cancel");
        QTest::newRow("ready") << 202 << 202 << true << false;
        QTest::newRow("cancel") << 202 << 202 << true << true;
        QTest::newRow("routing-refused") << 403 << 202 << true << false;
        QTest::newRow("keys-refused") << 202 << 503 << true << false;
        QTest::newRow("creator-unavailable") << 202 << 202 << false << false;
    }
    void encryptedPreparationPublishesOnlyTheCreatorKeyBeforeAcknowledgement() {
        QFETCH(int, routingStatus);
        QFETCH(int, keyStatus);
        QFETCH(bool, creatorAvailable);
        QFETCH(bool, cancel);
        r03::resetCredentials();
        QTemporaryDir root;
        const auto worktree = root.filePath("worktree");
        QVERIFY(QDir().mkpath(worktree));
        QCOMPARE(QProcess::execute("git", {"-C", worktree, "init", "-q", "-b", "main"}), 0);
        const auto canonical = QFileInfo(worktree).canonicalFilePath();
        LocalJournal journal(root.filePath("journal.sqlite"));
        QVERIFY(journal.rememberWorktree(canonical));
        const auto providerLog = root.filePath("provider.log");
        const auto oldInputLog = qgetenv("KETPLUS_FAKE_CODEX_LOG_INPUT");
        qputenv("KETPLUS_FAKE_CODEX_LOG_INPUT", "1");
        const auto restoreInputLog = qScopeGuard([&] {
            if (oldInputLog.isNull())
                qunsetenv("KETPLUS_FAKE_CODEX_LOG_INPUT");
            else
                qputenv("KETPLUS_FAKE_CODEX_LOG_INPUT", oldInputLog);
        });
        QVERIFY(writeFakeCodex(root.filePath("fake-codex"), providerLog));
        CodexAdapter provider(root.filePath("fake-codex"));
        SessionHost host(&journal, &provider);
        r03::Transport relay;
        relay.status = 202;
        relay.statusForPath.insert("/v2/daemon/heartbeat", routingStatus);
        relay.statusForPath.insert("/v2/devices", 200);
        relay.statusForPath.insert("/v2/session-keys:append", keyStatus);
        CloudControlClient client(&journal, &host);
        Access::configure(client, relay);
        Access::machineKeyVersion(client, 1);
        const QByteArray salt(32, 's');
        Access::pathSalt(client, salt);
        CredentialStore phoneStore("preparation-client");
        CloudCrypto phone(&phoneStore);
        const auto phoneKeys = phone.publicKeys();
        Access::pinDevice(client, "creator", 1, phoneKeys);
        Access::pinDevice(client, "other-device", 1, phoneKeys);
        QJsonArray devices{QJsonObject{
            {"id", "other-device"}, {"keyVersion", 1}, {"status", "active"}, {"keys", phoneKeys}}};
        if (creatorAvailable)
            devices.append(QJsonObject{
                {"id", "creator"}, {"keyVersion", 1}, {"status", "active"}, {"keys", phoneKeys}});
        relay.responseForPath.insert("/v2/devices", QJsonDocument(devices).toJson());
        relay.statusForPath.insert("/v2/device-keys/creator/1", 200);
        relay.responseForPath.insert("/v2/device-keys/creator/1",
                                     QJsonDocument(QJsonObject{{"keys", phoneKeys}}).toJson());
        const auto scope = cloud_control::sourceContext(canonical, salt);
        const auto hostKeys = Access::crypto(client).publicKeys();
        QJsonObject command{{"id", "prepare-command"},
                            {"machineId", "machine-synthetic"},
                            {"workspaceId", "workspace-synthetic"},
                            {"kind", "session.prepare"},
                            {"capability", "session.create"},
                            {"ownerSubjectId", "fixture-owner"},
                            {"sessionId", ""},
                            {"sessionGeneration", 0},
                            {"repositoryId", scope.value("repositoryId")},
                            {"worktreeId", scope.value("worktreeId")},
                            {"expectedCatalogRevision", 0},
                            {"expectedLifecycleRevision", 0},
                            {"attempt", 1},
                            {"claimToken", "fixture-claim"}};
        QString cryptoError;
        const auto sealed = phone.encryptSealedBound(
            QJsonDocument(QJsonObject{{"client_request_id", "draft-1"}}).toJson(),
            [&](const QJsonObject& envelope) {
                auto bound = command;
                bound.insert("content", envelope);
                return Access::commandAad(client, bound);
            },
            CloudCrypto::fromBase64Url(hostKeys.value("agreementPublicKey").toString()),
            "prepare-command", "session-create:prepare-command", "machine-synthetic", 1, "creator",
            1, &cryptoError);
        QVERIFY2(!sealed.isEmpty(), qPrintable(cryptoError));
        command.insert("content", sealed);
        Access::execute(client, command);
        const auto requestIndex = [&](const QString& path) {
            for (int i = 0; i < relay.requests.size(); ++i)
                if (relay.requests[i].url().path() == path)
                    return i;
            return -1;
        };
        QTRY_VERIFY(requestIndex("/v2/commands/prepare-command/ack") >= 0);
        const int ackIndex = requestIndex("/v2/commands/prepare-command/ack");
        const auto ack = QJsonDocument::fromJson(relay.bodies[ackIndex]).object();
        const auto content = ack.value("encryptedResult").toObject();
        const auto plaintext = phone.decryptSealedAs(
            content,
            Access::resultAad(client, command, 1, ack.value("status").toString(),
                              ack.value("resultCode").toString(), content),
            CloudCrypto::fromBase64Url(hostKeys.value("signingPublicKey").toString()),
            "prepare-command", "session-create:prepare-command", &cryptoError);
        QVERIFY2(!plaintext.isEmpty(), qPrintable(cryptoError));
        const auto result = QJsonDocument::fromJson(plaintext).object();
        const bool accepted = routingStatus == 202 && keyStatus == 202 && creatorAvailable;
        QCOMPARE(result.value("ok").toBool(), accepted);
        QVERIFY(journal.sessions().isEmpty());
        QCOMPARE(journal.sessionPreparations().size(), 1);
        QVERIFY(!QFile::exists(providerLog));
        const int routingIndex = requestIndex("/v2/daemon/heartbeat");
        QVERIFY(routingIndex >= 0 && routingIndex < ackIndex);
        QVERIFY(!relay.bodies[routingIndex].contains(canonical.toUtf8()));
        const int keyIndex = requestIndex("/v2/session-keys:append");
        if (routingStatus != 202 || !creatorAvailable) {
            QCOMPARE(keyIndex, -1);
            return;
        }
        QVERIFY(keyIndex > routingIndex && keyIndex < ackIndex);
        const auto wraps =
            QJsonDocument::fromJson(relay.bodies[keyIndex]).object().value("envelopes").toArray();
        QCOMPARE(wraps.size(), 1);
        QCOMPARE(wraps.first().toObject().value("recipientDeviceId").toString(), "creator");
        if (!accepted)
            return;
        const auto id = result.value("session_id").toString();
        QCOMPARE(wraps.first().toObject().value("sessionId").toString(), id);
        QVERIFY(phone.importSessionKey(
            wraps.first().toObject(),
            CloudCrypto::fromBase64Url(hostKeys.value("signingPublicKey").toString()), "creator", 1,
            &cryptoError));
        QVERIFY(journal.eventsAfter(id, 1, 0).isEmpty());
        int serial = 0;
        const auto run = [&](const QString& kind, const QJsonObject& payload) {
            const QString commandId = "prepared-command-" + QString::number(++serial);
            QJsonObject next{{"id", commandId},
                             {"workspaceId", "workspace-synthetic"},
                             {"machineId", "machine-synthetic"},
                             {"sessionId", id},
                             {"sessionGeneration", 1},
                             {"kind", kind},
                             {"capability", kind},
                             {"attempt", 1},
                             {"claimToken", commandId + "-claim"},
                             {"content", QJsonObject{{"keyId", id + ":1:1"},
                                                     {"keyEpoch", 1},
                                                     {"senderDeviceId", "creator"},
                                                     {"senderKeyVersion", 1}}}};
            const auto encrypted =
                phone.encrypt(QJsonDocument(payload).toJson(), Access::commandAad(client, next), id,
                              1, 1, "creator", 1, &cryptoError);
            if (encrypted.isEmpty())
                return QJsonObject{};
            next.insert("content", encrypted);
            Access::execute(client, next);
            const auto path = "/v2/commands/" + commandId + "/ack";
            if (!QTest::qWaitFor([&] { return requestIndex(path) >= 0; }, 10000)) {
                cryptoError = "No acknowledgement for the prepared session command";
                return QJsonObject{};
            }
            const auto response =
                QJsonDocument::fromJson(relay.bodies[requestIndex(path)]).object();
            const auto envelope = response.value("encryptedResult").toObject();
            const auto decoded = phone.decrypt(
                envelope,
                Access::resultAad(client, next, 1, response.value("status").toString(),
                                  response.value("resultCode").toString(), envelope),
                CloudCrypto::fromBase64Url(hostKeys.value("signingPublicKey").toString()), id, 1,
                &cryptoError);
            return QJsonDocument::fromJson(decoded).object();
        };
        const QByteArray bytes("FIRST_TURN_PRIVATE_REFERENCE\n");
        const auto digest = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        const auto check = [&](const QJsonObject& reply) {
            return cryptoError.isEmpty() && reply.value("ok").toBool();
        };
        QVERIFY(check(run("attachment.stage.begin", {{"attachment_id", "file-1"},
                                                     {"expected_revision", 0},
                                                     {"name", "reference.txt"},
                                                     {"media_type", "text/plain"},
                                                     {"total_bytes", bytes.size()},
                                                     {"chunk_count", 1},
                                                     {"digest_sha256", digest}})));
        QVERIFY(check(
            run("attachment.stage.chunk",
                {{"attachment_id", "file-1"},
                 {"expected_revision", 1},
                 {"chunk_index", 0},
                 {"data", QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding |
                                                             QByteArray::OmitTrailingEquals))},
                 {"digest_sha256", digest}})));
        QVERIFY(check(run("attachment.stage.commit",
                          {{"attachment_id", "file-1"}, {"expected_revision", 1}})));
        QCOMPARE(journal.attachment(id, 1, "file-1").bytes, bytes);
        QVERIFY(journal.sessions().isEmpty());
        QVERIFY(journal.eventsAfter(id, 1, 0).isEmpty());
        QVERIFY(!QFile::exists(providerLog));
        if (cancel) {
            QVERIFY(check(run("session.prepare.cancel", {})));
            QVERIFY(check(run("session.prepare.cancel", {})));
            QVERIFY(journal.sessions().isEmpty());
            QVERIFY(journal.attachment(id, 1, "file-1").status.isEmpty());
            QVERIFY(!QFile::exists(providerLog));
        } else {
            QVERIFY(check(run("session.start_turn", {{"prompt", "read the reference"},
                                                     {"attachment_ids", QJsonArray{"file-1"}}})));
            QTRY_VERIFY([&] {
                QFile log(providerLog);
                return log.open(QIODevice::ReadOnly) && log.readAll().contains(bytes.trimmed());
            }());
            QCOMPARE(journal.sessions().size(), 1);
            QCOMPARE(journal.sessionPreparation(id)->state, QStringLiteral("activated"));
            QCOMPARE(run("session.prepare.cancel", {}).value("error_code").toString(),
                     QStringLiteral("preparation_consumed"));
        }
        for (const auto& body : relay.bodies)
            QVERIFY(!body.contains(bytes.trimmed()));
    }
};
QTEST_GUILESS_MAIN(RelayPreparationHostTest)
#include "RelayPreparationHostTest.moc"
