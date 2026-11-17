#include "CloudPrivacyTestAccess.h"
#include "FakeTransport.h"
#include "MemoryCredentials.h"
#include "atlas/AtlasLiveRuntime.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"

#include <QJsonDocument>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;
using Access = CloudPrivacyTestAccess;

namespace {
const QString devicesPath = QStringLiteral("/v2/devices");
const QString keyPath = QStringLiteral("/v2/device-keys/device-old/1");
const QString ticketPath = QStringLiteral("/v2/pairing/tickets/ticket-synthetic");

struct Fixture {
    QTemporaryDir directory;
    LocalJournal journal{directory.filePath("journal.sqlite")};
    CodexAdapter provider{QStringLiteral("/nonexistent/synthetic-provider")};
    SessionHost host{&journal, &provider};
    r03::Transport transport;
    std::unique_ptr<CloudControlClient> client;
    QJsonObject device;

    explicit Fixture(bool legacy = true) {
        StoredSession session;
        session.id = "session-synthetic";
        session.worktreePath = "synthetic-worktree";
        session.adapter = "codex";
        session.threadId = "thread-synthetic";
        session.state = "ready";
        session.currentTurnSequence = 1;
        if (!journal.upsertSession(session))
            qFatal("Synthetic journal setup failed");
        if (legacy) {
            CredentialStore store;
            if (!store.write("machine-id", "machine-synthetic") ||
                !store.write("workspace-id", "workspace-synthetic") ||
                !store.write("control-url",
                             CloudControlClient::configuredControlUrl().toString().toUtf8()))
                qFatal("Synthetic enrollment setup failed");
        }
        client = std::make_unique<CloudControlClient>(&journal, &host);
        Access::configure(*client, transport);
        CredentialStore deviceStore("synthetic-device");
        device = {{"id", "device-old"},
                  {"keyVersion", 1},
                  {"status", "active"},
                  {"keys", CloudCrypto(&deviceStore).publicKeys()}};
        reply(devicesPath, QJsonArray{device});
        reply(keyPath, QJsonObject{{"keys", device.value("keys")}});
        reply("/v2/session-keys:append", QJsonObject{}, 202);
    }
    void reply(const QString& path, const QJsonValue& value, int status = 200) {
        transport.statusForPath.insert(path, status);
        transport.responseForPath.insert(path, value.isArray()
                                                   ? QJsonDocument(value.toArray()).toJson()
                                                   : QJsonDocument(value.toObject()).toJson());
    }
    void signOut() {
        client->handleLocal("remote.sign_out", {}, [](const QJsonObject&, const QString& error) {
            if (!error.isEmpty())
                qFatal("Synthetic sign-out failed");
        });
    }
    QJsonObject ticket() const {
        return {{"id", "ticket-synthetic"},
                {"status", "claimed"},
                {"claimedDevice", device},
                {"claimDigest", DevicePins::claimDigest("ticket-synthetic", device)}};
    }
    QJsonObject approval() const {
        return {{"ticket_id", "ticket-synthetic"},
                {"claim_digest", DevicePins::claimDigest("ticket-synthetic", device)}};
    }
};
} // namespace

class CloudTrustLifecycleTest final : public QObject {
    Q_OBJECT

    void reEnroll(Fixture& f, bool same = false) {
        f.reply("/v2/machines:enroll",
                QJsonObject{{"id", same ? "machine-synthetic" : "machine-new"},
                            {"workspaceId", same ? "workspace-synthetic" : "workspace-new"},
                            {"keyVersion", 1}},
                201);
        bool answered = false;
        Access::enroll(*f.client, [&](const QJsonObject&, const QString&) { answered = true; });
        // Credential refresh deliberately fails against the fake transport; enrollment itself
        // must already have completed. No real identity or provider is used.
        QTRY_VERIFY(answered);
        QCOMPARE(Access::machineId(*f.client), same ? "machine-synthetic" : "machine-new");
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void init() { r03::resetCredentials(); }

    void restoreReadFailure_data() {
        QTest::addColumn<QString>("account");
        for (const auto* field : {"control-url", "device-pins", "workspace-id", "account-name"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void restoreReadFailure() {
        QFETCH(QString, account);
        Fixture f;
        QVERIFY(Access::pins(*f.client).pin(f.device));
        const auto before = r03::credentials;
        f.client.reset();
        r03::failReadsOf = account;
        f.client = std::make_unique<CloudControlClient>(&f.journal, &f.host);
        QVERIFY(Access::machineId(*f.client).isEmpty());
        QVERIFY(Access::error(*f.client).contains("synthetic credential read failure"));
        QVERIFY(r03::credentials == before);
        // A transient recovery must not grant an enrollment or pin-adoption permission.
        r03::failReadsOf.clear();
        Access::deviceTransport(*f.client, f.transport);
        Access::distributeKeys(*f.client);
        QVERIFY(r03::credentials == before);
        f.client.reset();
        f.client = std::make_unique<CloudControlClient>(&f.journal, &f.host);
        QCOMPARE(Access::machineId(*f.client), "machine-synthetic");
        QVERIFY(Access::pins(*f.client).trusts(f.device));
    }

    void enrollmentWriteFailure_data() {
        QTest::addColumn<QString>("account");
        for (const auto* field :
             {"device-pins", "control-url", "machine-id", "workspace-id", "machine-key-version",
              "account-name", "account-username", "session-keys-workspace"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void enrollmentWriteFailure() {
        QFETCH(QString, account);
        Fixture f;
        QVERIFY(Access::pins(*f.client).pin(f.device));
        QVERIFY(!Access::crypto(*f.client).publicKeys().isEmpty());
        CredentialStore store;
        QVERIFY(store.write("session-key-synthetic", "old-content-key"));
        QVERIFY(store.write("uploaded-cursor-session-synthetic-1", "7"));
        const auto before = r03::credentials;
        f.reply(
            "/v2/machines:enroll",
            QJsonObject{{"id", "machine-new"}, {"workspaceId", "workspace-new"}, {"keyVersion", 2}},
            201);
        r03::failWritesTo = account;
        int answers = 0;
        QString failure;
        Access::enroll(*f.client,
                       [&](const QJsonObject&, const QString& error) {
                           ++answers;
                           failure = error;
                       },
                       {{"displayName", "Synthetic user"}, {"username", "synthetic"}});
        QTRY_COMPARE(answers, 1);
        QVERIFY(failure.contains("synthetic credential write failure"));
        QVERIFY(r03::credentials == before);
        QCOMPARE(Access::machineId(*f.client), "machine-synthetic");
        r03::failWritesTo.clear();
        f.client.reset();
        f.client = std::make_unique<CloudControlClient>(&f.journal, &f.host);
        QCOMPARE(Access::machineId(*f.client), "machine-synthetic");
        QVERIFY(Access::pins(*f.client).trusts(f.device));
        QCOMPARE(store.read("workspace-id"), QByteArray("workspace-synthetic"));
        QCOMPARE(store.read("session-key-synthetic"), QByteArray("old-content-key"));
    }

    void enrollmentEndsAtlasAndCredentials_data() {
        QTest::addColumn<bool>("sameIdentity");
        QTest::addColumn<bool>("holdCredentialPost");
        for (const bool same : {false, true})
            for (const bool post : {false, true})
                QTest::newRow(qPrintable(QString("same-%1-post-%2").arg(same).arg(post)))
                    << same << post;
    }
    void enrollmentEndsAtlasAndCredentials() {
        QFETCH(bool, sameIdentity);
        QFETCH(bool, holdCredentialPost);
        Fixture f;
        const QString challengePath = "/v2/daemon/challenges";
        const QString credentialPath = "/v2/daemon/credentials";
        const auto challenge = [](const QString& machine) {
            return QJsonObject{
                {"id", "challenge-synthetic"},
                {"machineId", machine},
                {"keyVersion", 1},
                {"signatureVersion", 2},
                {"signatureAlgorithm", "Ed25519"},
                {"challenge", QString::fromLatin1(CloudCrypto::base64Url(QByteArray(32, 'a')))}};
        };
        const auto credential = [](const QString& token) {
            return QJsonObject{
                {"accessToken", token},
                {"expiresAt", QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODate)}};
        };
        f.reply(challengePath, challenge("machine-synthetic"), 201);
        f.reply(credentialPath, credential("old-enrollment-token"), 201);
        f.transport.heldPaths.insert(holdCredentialPost ? credentialPath : challengePath);
        int oldAnswers = 0;
        bool oldReady = true;
        Access::credential(*f.client, [&](bool ready, const QString&) {
            ++oldAnswers;
            oldReady = ready;
        });
        QTRY_COMPARE(f.transport.heldReplies.size(), 1);
        const auto oldReply = f.transport.heldReplies.first();
        f.transport.heldReplies.clear();
        f.transport.heldPaths.clear();
        auto runtime = QPointer<AtlasLiveRuntime>(new AtlasLiveRuntime(f.client.get()));
        runtime->setProperty("device", "device-old");
        const auto fence = Access::atlasFence(*f.client, "device-old");
        const QString machine = sameIdentity ? "machine-synthetic" : "machine-new";
        f.reply(challengePath, challenge(machine), 201);
        f.reply(credentialPath, credential("new-enrollment-token"), 201);
        f.reply("/v2/machines:enroll",
                QJsonObject{{"id", machine},
                            {"workspaceId", sameIdentity ? "workspace-synthetic" : "workspace-new"},
                            {"keyVersion", 1}},
                201);
        int newAnswers = 0;
        QString failure;
        Access::enroll(*f.client, [&](const QJsonObject&, const QString& error) {
            ++newAnswers;
            failure = error;
        });
        QTRY_COMPARE(newAnswers, 1);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(oldAnswers, 1);
        QVERIFY(!oldReady);
        QTRY_VERIFY(!runtime);
        QVERIFY(!Access::atlasAccessCurrent(*f.client, "device-old", fence));
        QCOMPARE(Access::token(*f.client), "new-enrollment-token");
        const auto requests = f.transport.requests.size();
        oldReply->finish();
        QCOMPARE(Access::token(*f.client), "new-enrollment-token");
        QCOMPARE(f.transport.requests.size(), requests);
        QCOMPARE(oldAnswers, 1);
        QCOMPARE(newAnswers, 1);
    }

    void enrollmentStopsAtlasWithoutCredentialRefresh() {
        Fixture f;
        auto runtime = QPointer<AtlasLiveRuntime>(new AtlasLiveRuntime(f.client.get()));
        runtime->setProperty("device", "device-old");
        const auto fence = Access::atlasFence(*f.client, "device-old");
        reEnroll(f, true);
        QTRY_VERIFY(!runtime);
        QVERIFY(!Access::atlasAccessCurrent(*f.client, "device-old", fence));
    }
    void sameIdentityEnrollmentDropsOldHeartbeatBatch() {
        Fixture f;
        const QString path = "/v2/daemon/heartbeat";
        f.reply(path, QJsonObject{}, 202);
        f.transport.heldPaths.insert(path);
        Access::heartbeat(*f.client, {QJsonArray{}, QJsonArray{}});
        QCOMPARE(f.transport.heldReplies.size(), 1);
        reEnroll(f, true);
        const auto requests = f.transport.requests.size();
        const auto error = Access::error(*f.client);
        f.transport.release(path);
        QCOMPARE(f.transport.requests.size(), requests);
        QCOMPARE(Access::error(*f.client), error);
    }
    void senderPinReadFailureIsReportedWithoutAdoption() {
        Fixture f;
        const auto before = r03::credentials;
        r03::failReadsOf = "device-pins";
        int answers = 0;
        Access::senderKeys(*f.client, "device-old", 1,
                           [&](const QJsonObject& keys, const QString& error) {
                               ++answers;
                               QVERIFY(keys.isEmpty());
                               QVERIFY(error.contains("synthetic credential read failure"));
                           });
        QTRY_COMPARE(answers, 1);
        QVERIFY(r03::credentials == before);
        QVERIFY(Access::error(*f.client).contains("synthetic credential read failure"));
    }
    void corruptPinsAreNotLegacyEnrollment() {
        Fixture f;
        CredentialStore store;
        QVERIFY(store.write("device-pins", "not-json"));
        f.client.reset();
        const auto before = r03::credentials;
        f.client = std::make_unique<CloudControlClient>(&f.journal, &f.host);
        QVERIFY(Access::machineId(*f.client).isEmpty());
        QVERIFY(Access::error(*f.client).contains("pin record is invalid"));
        QVERIFY(r03::credentials == before);
    }

    void delayedDistribution_data() {
        QTest::addColumn<QString>("transition");
        for (const auto* name :
             {"sign-out", "new-enrollment", "same-enrollment", "machine", "workspace"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void approvalDuringSameIdentityEnrollment_data() {
        QTest::addColumn<bool>("holdClaim");
        QTest::addColumn<bool>("storageFailure");
        QTest::newRow("claim-reply") << true << false;
        QTest::newRow("approval-reply") << false << false;
        QTest::newRow("claim-reply-storage-failure") << true << true;
        QTest::newRow("approval-reply-storage-failure") << false << true;
    }
    void approvalDuringSameIdentityEnrollment() {
        QFETCH(bool, holdClaim);
        QFETCH(bool, storageFailure);
        Fixture f;
        const QString enrollmentPath = "/v2/machines:enroll";
        f.reply("/v2/auth/device/code",
                QJsonObject{{"device_code", "ketplus_device_" + QString(43, 'A')},
                            {"user_code", "A1B2C3D4"},
                            {"verification_uri_complete", "https://synthetic.invalid/approve"},
                            {"expires_in", 600},
                            {"interval", 5}});
        f.reply("/v2/auth/device/token", QJsonObject{{"accessToken", "synthetic-user-token"}});
        f.reply(enrollmentPath,
                QJsonObject{{"id", "machine-synthetic"},
                            {"workspaceId", "workspace-synthetic"},
                            {"keyVersion", 1}},
                201);
        f.transport.heldPaths.insert(enrollmentPath);
        bool started = false;
        f.client->handleLocal("remote.sign_in.device", {},
                              [&](const QJsonObject& result, const QString& error) {
                                  started = error.isEmpty() && result.value("ok").toBool();
                              });
        QTRY_VERIFY(started);
        Access::pollDevice(*f.client);
        QTRY_COMPARE(f.transport.heldReplies.size(), 1);

        // Start approval after the sign-in epoch advances, while the old identity remains.
        f.reply(ticketPath, f.ticket());
        f.reply(ticketPath + "/approve", QJsonObject{});
        const auto heldPath = holdClaim ? ticketPath : ticketPath + "/approve";
        f.transport.heldPaths.insert(heldPath);
        int answers = 0;
        QString failure;
        f.client->handleLocal("remote.pairing.approve", f.approval(),
                              [&](const QJsonObject&, const QString& error) {
                                  ++answers;
                                  failure = error;
                              });
        QTRY_COMPARE(f.transport.heldReplies.size(), 2);
        if (storageFailure)
            r03::failWritesTo = "account-name";
        f.transport.release(enrollmentPath);
        // Storage or the fake credential challenge fails; sign-in must still finish.
        QTRY_VERIFY(!Access::status(*f.client).value("sign_in_pending").toBool());
        r03::failWritesTo.clear();
        QCOMPARE(Access::machineId(*f.client), "machine-synthetic");
        QCOMPARE(Access::pins(*f.client).initialized(), !storageFailure);
        QVERIFY(!Access::pins(*f.client).trusts(f.device));
        const auto credentials = r03::credentials;
        const auto requests = f.transport.requests.size();
        f.transport.release(heldPath);
        QCOMPARE(answers, 1);
        QVERIFY(!failure.isEmpty());
        QCOMPARE(r03::credentials, credentials);
        QCOMPARE(f.transport.requests.size(), requests);
        QVERIFY(!Access::pins(*f.client).trusts(f.device));
    }
    void delayedDistribution() {
        QFETCH(QString, transition);
        Fixture f;
        f.transport.heldPaths.insert(devicesPath);
        Access::distributeKeys(*f.client);
        QCOMPARE(f.transport.heldReplies.size(), 1);
        if (transition == "machine")
            Access::scope(*f.client, "machine-new", "workspace-synthetic");
        else if (transition == "workspace")
            Access::scope(*f.client, "machine-synthetic", "workspace-new");
        else {
            f.signOut();
            if (transition != "sign-out")
                reEnroll(f, transition == "same-enrollment");
        }
        const auto credentials = r03::credentials;
        const auto requests = f.transport.requests.size();
        f.transport.release(devicesPath);
        QCOMPARE(r03::credentials, credentials);
        QCOMPARE(f.transport.requests.size(), requests);
        QVERIFY(!Access::pins(*f.client).trusts(f.device));
    }

    void migrationRequiresRestoredEnrollment_data() {
        QTest::addColumn<bool>("legacy");
        QTest::addColumn<bool>("initialized");
        QTest::newRow("legacy") << true << false;
        QTest::newRow("new-process-without-enrollment") << false << false;
        QTest::newRow("existing-empty-pins") << true << true;
    }

    void staleDistributionCannotRefreshNewEnrollment() {
        Fixture f;
        f.transport.heldPaths.insert(devicesPath);
        Access::distributeKeys(*f.client);
        QCOMPARE(f.transport.heldReplies.size(), 1);
        const auto oldReply = f.transport.heldReplies.first();
        f.signOut();
        reEnroll(f);

        auto& signaling = Access::signaling(*f.client);
        signaling.configure("workspace-new", "machine-new", 1, true);
        QSignalSpy outgoing(&signaling, &transport::Signaling::outgoing);
        const QString id = "pc_abcdefghijklmnopqrstuvwx";
        const auto now = QDateTime::currentDateTimeUtc();
        const QJsonObject grant{{"connectionId", id},
                                {"deviceId", "device-new"},
                                {"credentialEpoch", 1},
                                {"workspaceId", "workspace-new"},
                                {"machineId", "machine-new"},
                                {"ownerSubjectId", "owner-synthetic"},
                                {"capabilities", QJsonArray{}},
                                {"issuedAt", now.toString(Qt::ISODateWithMs)},
                                {"expiresAt", now.addSecs(60).toString(Qt::ISODateWithMs)}};
        // Hold key lookup before signature verification, so no WebRTC link is created.
        signaling.receive({{"type", "signal.offer"},
                           {"connectionId", id},
                           {"deviceId", "device-new"},
                           {"deviceKeyVersion", 1},
                           {"grant", grant},
                           {"issuedAt", now.toString(Qt::ISODateWithMs)},
                           {"sdp", "synthetic-pending-offer"}});
        QCOMPARE(f.transport.heldReplies.size(), 2);
        QVERIFY(outgoing.isEmpty());
        const auto credentials = r03::credentials;
        const auto requests = f.transport.requests.size();
        oldReply->finish();
        QCOMPARE(r03::credentials, credentials);
        QCOMPARE(f.transport.requests.size(), requests);
        QVERIFY(outgoing.isEmpty());
        // A stale device-list refresh would have ended this new attempt as revoked.
        signaling.receive({{"type", "signal.end"}, {"connectionId", id}});
        QCOMPARE(outgoing.size(), 1);
        QCOMPARE(outgoing.first().first().toJsonObject().value("reason"), "remote_closed");
    }
    void migrationRequiresRestoredEnrollment() {
        QFETCH(bool, legacy);
        QFETCH(bool, initialized);
        if (initialized) {
            CredentialStore store;
            QVERIFY(DevicePins(&store).reset());
        }
        Fixture f(legacy);
        QSignalSpy finished(&f.transport, &QNetworkAccessManager::finished);
        Access::distributeKeys(*f.client);
        QTRY_VERIFY(!finished.isEmpty());
        const bool adopted = legacy && !initialized;
        QCOMPARE(Access::pins(*f.client).trusts(f.device), adopted);
        QCOMPARE(f.transport.requests.size(), adopted ? 2 : 1);
        if (adopted) {
            QTRY_COMPARE(finished.size(), 2);
            // Losing pins later in this process is not another migration opportunity.
            QVERIFY(CredentialStore().remove("device-pins"));
            Access::distributeKeys(*f.client);
            QTRY_COMPARE(finished.size(), 3);
            QVERIFY(!Access::pins(*f.client).trusts(f.device));
            QCOMPARE(f.transport.requests.size(), 3);
        }
    }

    void delayedSenderLookup_data() {
        QTest::addColumn<bool>("fallback");
        QTest::addColumn<bool>("enrollAgain");
        QTest::newRow("key-sign-out") << false << false;
        QTest::newRow("key-reenroll") << false << true;
        QTest::newRow("adoption-sign-out") << true << false;
        QTest::newRow("adoption-reenroll") << true << true;
    }

    void senderFallbackMigration_data() { migrationRequiresRestoredEnrollment_data(); }
    void senderFallbackMigration() {
        QFETCH(bool, legacy);
        QFETCH(bool, initialized);
        if (initialized) {
            CredentialStore store;
            QVERIFY(DevicePins(&store).reset());
        }
        Fixture f(legacy);
        int answers = 0;
        QJsonObject accepted;
        Access::senderKeys(*f.client, "device-old", 1,
                           [&](const QJsonObject& keys, const QString&) {
                               ++answers;
                               accepted = keys;
                           });
        QTRY_COMPARE(answers, 1);
        const bool adopted = legacy && !initialized;
        QCOMPARE(!accepted.isEmpty(), adopted);
        QCOMPARE(Access::pins(*f.client).trusts(f.device), adopted);
        QCOMPARE(f.transport.requests.size(), adopted ? 2 : 1);
    }
    void delayedSenderLookup() {
        QFETCH(bool, fallback);
        QFETCH(bool, enrollAgain);
        Fixture f;
        const auto heldPath = fallback ? devicesPath : keyPath;
        f.transport.heldPaths.insert(heldPath);
        int answers = 0;
        QJsonObject accepted;
        QString failure;
        Access::senderKeys(*f.client, "device-old", 1,
                           [&](const QJsonObject& keys, const QString& error) {
                               ++answers;
                               accepted = keys;
                               failure = error;
                           });
        QTRY_COMPARE(f.transport.heldReplies.size(), 1);
        f.signOut();
        if (enrollAgain)
            reEnroll(f);
        const auto credentials = r03::credentials;
        const auto requests = f.transport.requests.size();
        f.transport.release(heldPath);
        QCOMPARE(answers, 1);
        QVERIFY(accepted.isEmpty());
        QVERIFY(!failure.isEmpty());
        QCOMPARE(r03::credentials, credentials);
        QCOMPARE(f.transport.requests.size(), requests);
        QCOMPARE(Access::cachedSenderKeys(*f.client), 0);
    }

    void delayedPairing_data() {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<bool>("enrollAgain");
        for (const auto* op : {"claim", "approve", "revoke-device", "revoke-ticket"}) {
            QTest::newRow(op) << QString::fromLatin1(op) << false;
            QTest::newRow(qPrintable(QString::fromLatin1(op) + "-reenroll"))
                << QString::fromLatin1(op) << true;
        }
    }

    void staleSenderFailureCannotAcknowledgeCommand_data() {
        QTest::addColumn<bool>("enrollAgain");
        QTest::newRow("sign-out") << false;
        QTest::newRow("reenroll") << true;
    }
    void staleSenderFailureCannotAcknowledgeCommand() {
        QFETCH(bool, enrollAgain);
        Fixture f;
        f.transport.heldPaths.insert(keyPath);
        Access::execute(*f.client, {{"id", "command-synthetic"},
                                    {"machineId", "machine-synthetic"},
                                    {"content", QJsonObject{{"senderDeviceId", "device-old"},
                                                            {"senderKeyVersion", 1}}}});
        QCOMPARE(f.transport.heldReplies.size(), 1);
        f.signOut();
        if (enrollAgain)
            reEnroll(f);
        const auto credentials = r03::credentials;
        const auto requests = f.transport.requests.size();
        f.transport.release(keyPath);
        QCOMPARE(r03::credentials, credentials);
        QCOMPARE(f.transport.requests.size(), requests);
    }
    void delayedPairing() {
        QFETCH(QString, operation);
        QFETCH(bool, enrollAgain);
        Fixture f;
        f.reply(ticketPath, f.ticket());
        f.reply(ticketPath + "/approve", QJsonObject{});
        const QString heldPath = operation == "claim"           ? ticketPath
                                 : operation == "approve"       ? ticketPath + "/approve"
                                 : operation == "revoke-device" ? devicesPath + "/device-old/revoke"
                                                                : ticketPath + "/revoke";
        if (operation.startsWith("revoke"))
            f.reply(heldPath, QJsonObject{});
        f.transport.heldPaths.insert(heldPath);
        int answers = 0;
        QString failure;
        const auto done = [&](const QJsonObject&, const QString& error) {
            ++answers;
            failure = error;
        };
        if (operation == "revoke-device")
            f.client->handleLocal("remote.device.revoke", {{"device_id", "device-old"}}, done);
        else if (operation == "revoke-ticket")
            f.client->handleLocal("remote.pairing.revoke", {{"ticket_id", "ticket-synthetic"}},
                                  done);
        else
            f.client->handleLocal("remote.pairing.approve", f.approval(), done);
        QTRY_COMPARE(f.transport.heldReplies.size(), 1);
        f.signOut();
        if (enrollAgain) {
            reEnroll(f);
            // An old revocation must not remove the new enrollment's independently approved pin.
            Access::pinDevice(*f.client, "device-old", 1, f.device.value("keys").toObject());
        }
        QSignalSpy revoked(&Access::signaling(*f.client), &transport::Signaling::deviceRevoked);
        const auto credentials = r03::credentials;
        const auto requests = f.transport.requests.size();
        f.transport.release(heldPath);
        QCOMPARE(answers, 1);
        QVERIFY(!failure.isEmpty());
        QCOMPARE(r03::credentials, credentials);
        QCOMPARE(f.transport.requests.size(), requests);
        QCOMPARE(revoked.size(), 0);
    }
};

QTEST_GUILESS_MAIN(CloudTrustLifecycleTest)
#include "CloudTrustLifecycleTest.moc"
