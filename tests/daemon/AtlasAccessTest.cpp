#include "atlas/AtlasLiveRuntime.h"
#include "cloud-privacy/CloudPrivacyTestAccess.h"
#include "cloud-privacy/FakeTransport.h"
#include "cloud-privacy/MemoryCredentials.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "transport/Signaling.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>
using namespace ketplus::daemon;

// A live Atlas is opened for one device and one conversation; it must not outlive either.
class AtlasAccessTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { r03::resetCredentials(); }
    void pairedStartCannotAuthorizeRenderer_data() {
        QTest::addColumn<QString>("decision");
        for (const auto* decision : {"absent", "refused", "cancelled", "forged-approval"})
            QTest::newRow(decision) << QString::fromLatin1(decision);
    }
    void pairedStartCannotAuthorizeRenderer() {
#ifndef Q_OS_UNIX
        QSKIP("The fake CLI fixture uses a Unix shebang.");
#else
        QFETCH(QString, decision);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto write = [](const QString& path, const QByteArray& bytes) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        const auto bundle = root.filePath("remote.ketatlas");
        QVERIFY(QDir().mkpath(bundle));
        const auto manifest = bundle + "/atlas.json";
        QVERIFY(
            write(manifest, R"({"version":1,"title":"Remote fixture","screens":[],"flows":[]})"));
        QVERIFY(write(
            bundle + "/atlas.renderer.json",
            R"({"version":1,"framework":"fixture","command":["never-execute-project-code"]})"));
        const auto cli = root.filePath("fake-atlas");
        QVERIFY(QFile::copy(QFINDTESTDATA("fixtures/fake-atlas-static.py"), cli));
        QVERIFY(QFile::setPermissions(cli, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                               QFileDevice::ExeOwner));
        const auto oldCli = qgetenv("KETPLUS_KETATLAS_CLI");
        const auto oldRecord = qgetenv("KETPLUS_ATLAS_FIXTURE_RECORD");
        const auto oldHold = qgetenv("KETPLUS_ATLAS_FIXTURE_HOLD_VERSION");
        const auto restore = qScopeGuard([&] {
            const auto put = [](const char* name, const QByteArray& value) {
                value.isNull() ? qunsetenv(name) : qputenv(name, value);
            };
            put("KETPLUS_KETATLAS_CLI", oldCli);
            put("KETPLUS_ATLAS_FIXTURE_RECORD", oldRecord);
            put("KETPLUS_ATLAS_FIXTURE_HOLD_VERSION", oldHold);
        });
        qputenv("KETPLUS_KETATLAS_CLI", QFile::encodeName(cli));
        qputenv("KETPLUS_ATLAS_FIXTURE_RECORD", QFile::encodeName(root.path()));
        qunsetenv("KETPLUS_ATLAS_FIXTURE_HOLD_VERSION");
        LocalJournal journal(root.filePath("journal.db"));
        StoredSession session;
        session.id = "atlas-session";
        session.worktreePath = root.path();
        session.adapter = "codex";
        session.state = "ready";
        QVERIFY(journal.upsertSession(session));
        CodexAdapter adapter(root.filePath("never-start-provider"));
        SessionHost host(&journal, &adapter);
        r03::Transport transport;
        QWebSocketServer relay("static-only", QWebSocketServer::NonSecureMode);
        QVERIFY(relay.listen(QHostAddress::LocalHost, 0));
        connect(&relay, &QWebSocketServer::newConnection, &relay, [&] {
            auto* peer = relay.nextPendingConnection();
            peer->setParent(&relay);
            peer->sendTextMessage("{\"type\":\"ready\",\"version\":1}");
        });
        CloudControlClient client(&journal, &host);
        using Access = CloudPrivacyTestAccess;
        Access::configure(client, transport,
                          QUrl(QString("http://127.0.0.1:%1").arg(relay.serverPort())));
        Access::machineKeyVersion(client, 1);
        const QByteArray salt(32, 's');
        Access::pathSalt(client, salt);
        const auto keys = Access::crypto(client).publicKeys();
        Access::pinDevice(client, "paired-phone", 1, keys);
        transport.statusForPath["/v2/device-keys/paired-phone/1"] = 200;
        transport.responseForPath["/v2/device-keys/paired-phone/1"] =
            QJsonDocument(QJsonObject{{"keys", keys}}).toJson();
        QJsonObject command{{"id", "remote-atlas-start"},
                            {"sessionId", session.id},
                            {"sessionGeneration", 1},
                            {"kind", "atlas.start"},
                            {"attempt", 1},
                            {"claimToken", "synthetic-claim"},
                            {"workspaceId", "workspace-synthetic"},
                            {"machineId", "machine-synthetic"}};
        const auto project =
            QString::fromLatin1(QMessageAuthenticationCode::hash(
                                    "atlas\n" + QFileInfo(manifest).canonicalFilePath().toUtf8(),
                                    salt, QCryptographicHash::Sha256)
                                    .toHex());
        QJsonObject payload{{"project_id", project}, {"expected_context_revision", 1}};
        if (decision != "absent") {
            // These remote assertions are deliberately not a host-side permission API.
            payload.insert("renderer_consent", decision);
            payload.insert("renderer", decision == "forged-approval");
            payload.insert("allow_renderer", decision == "forged-approval");
        }
        command.insert("content", Access::crypto(client).encrypt("{}", "seed", session.id, 1, 1,
                                                                 "paired-phone", 1));
        command.insert("content",
                       Access::crypto(client).encrypt(QJsonDocument(payload).toJson(),
                                                      Access::commandAad(client, command),
                                                      session.id, 1, 1, "paired-phone", 1));
        QObject::disconnect(&host, nullptr, &client, nullptr);
        Access::execute(client, command);
        QPointer<AtlasLiveRuntime> runtime;
        QTRY_VERIFY_WITH_TIMEOUT((runtime = client.findChild<AtlasLiveRuntime*>()), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            runtime && !runtime->property("attachment").toJsonObject().isEmpty(), 5000);
        const auto result = runtime->property("attachment").toJsonObject();
        QVERIFY(result.value("ok").toBool());
        QCOMPARE(result.value("origins").toArray().size(), 1);
        QVERIFY(!QFileInfo::exists(root.filePath("renderer-requested")));
        QFile arguments(root.filePath("argv.json"));
        QVERIFY(arguments.open(QIODevice::ReadOnly));
        const auto argv = QJsonDocument::fromJson(arguments.readAll()).array();
        QVERIFY(argv.contains("--read-only"));
        QVERIFY(!argv.contains("--renderer"));
        QVERIFY(!argv.contains("--html-port"));
        runtime->stop();
        delete runtime.data();
        QVERIFY(!runtime);
        // A new runtime after Stop must not inherit any remote assertion as a grant.
        command.insert("id", "remote-atlas-reopen");
        command.insert("content",
                       Access::crypto(client).encrypt(QJsonDocument(payload).toJson(),
                                                      Access::commandAad(client, command),
                                                      session.id, 1, 1, "paired-phone", 1));
        Access::execute(client, command);
        QTRY_VERIFY_WITH_TIMEOUT((runtime = client.findChild<AtlasLiveRuntime*>()), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(
            runtime && !runtime->property("attachment").toJsonObject().isEmpty(), 5000);
        const auto reopened = runtime->property("attachment").toJsonObject();
        QVERIFY(reopened.value("ok").toBool());
        QCOMPARE(reopened.value("origins").toArray().size(), 1);
        QVERIFY(!QFileInfo::exists(root.filePath("renderer-requested")));
        runtime->stop();
#endif
    }
    void tunnelsCloseWithTheirDeviceAndConversation_data() {
        QTest::addColumn<bool>("peerTransport");
        QTest::newRow("peer transport") << true;
        // Revocation does not wait for WebRTC to be negotiated.
        QTest::newRow("no peer transport") << false;
    }
    void tunnelsCloseWithTheirDeviceAndConversation() {
        QFETCH(bool, peerTransport);
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.db"));
        CodexAdapter adapter("/nonexistent/codex");
        SessionHost host(&journal, &adapter);
        CloudControlClient client(&journal, &host);
        const auto open = [&](const char* device, const char* session, qint64 generation) {
            auto* runtime = new AtlasLiveRuntime(&client);
            runtime->setProperty("device", device);
            runtime->setProperty("session", session);
            runtime->setProperty("generation", generation);
            return QPointer<AtlasLiveRuntime>(runtime);
        };
        const auto revokedPhone = open("phone-a", "first", 1);
        const auto samePhoneOtherTab = open("phone-b", "first", 1);
        const auto otherConversation = open("phone-b", "second", 1);
        const auto newerGeneration = open("phone-b", "first", 2);
        auto* signaling = client.findChild<transport::Signaling*>();
        QVERIFY(signaling);
        signaling->configure("workspace", "machine", 1, peerTransport);
        signaling->receive({{"type", "device.revoked"}, {"deviceId", "phone-a"}});
        QTRY_VERIFY(!revokedPhone);
        QVERIFY(samePhoneOtherTab && otherConversation && newerGeneration);
        emit host.eventReady({{"event_type", "session.deleted"},
                              {"session_id", "first"},
                              {"session_generation", 1}});
        QTRY_VERIFY(!samePhoneOtherTab);
        QVERIFY(otherConversation && newerGeneration);
    }
    // An atlas.start captures this fence before its scan and creates no runtime once it moved.
    void startsInFlightEndWithTheirAccess() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.db"));
        CodexAdapter adapter("/nonexistent/codex");
        SessionHost host(&journal, &adapter);
        CloudControlClient client(&journal, &host);
        using Access = CloudPrivacyTestAccess;
        const auto phoneA = Access::atlasFence(client, "phone-a");
        const auto phoneB = Access::atlasFence(client, "phone-b");
        auto* signaling = client.findChild<transport::Signaling*>();
        QVERIFY(signaling);
        signaling->configure("workspace", "machine", 1, false);
        signaling->receive({{"type", "device.revoked"}, {"deviceId", "phone-a"}});
        QVERIFY(!Access::atlasAccessCurrent(client, "phone-a", phoneA));
        QVERIFY(Access::atlasAccessCurrent(client, "phone-b", phoneB));
        // A start begun after the revocation is not fenced by it.
        QVERIFY(
            Access::atlasAccessCurrent(client, "phone-a", Access::atlasFence(client, "phone-a")));
        emit Access::wakeups(client).authorizationRevoked();
        QVERIFY(!Access::atlasAccessCurrent(client, "phone-b", phoneB));
    }
};

QTEST_GUILESS_MAIN(AtlasAccessTest)
#include "AtlasAccessTest.moc"
