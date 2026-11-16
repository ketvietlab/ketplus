#include "CloudPrivacyTestAccess.h"
#include "FakeTransport.h"
#include "MemoryCredentials.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"

#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;
using Access = CloudPrivacyTestAccess;

namespace {
const QString keyPath = "/v2/device-keys/device-synthetic/1";
const QString ticketPath = "/v2/pairing/tickets/ticket-synthetic";
struct Fixture {
    QTemporaryDir directory;
    LocalJournal journal{directory.filePath("journal.sqlite")};
    CodexAdapter provider{"/nonexistent/synthetic-provider"};
    static LocalJournal* seedJournal(LocalJournal& journal) {
        StoredSession session;
        session.id = "session-synthetic";
        session.worktreePath = "synthetic-worktree";
        session.adapter = "codex";
        session.state = "ready";
        session.title = "Original";
        if (!journal.upsertSession(session))
            qFatal("Synthetic session setup failed");
        return &journal;
    }
    SessionHost host{seedJournal(journal), &provider};
    r03::Transport transport;
    CloudControlClient client{&journal, &host};
    CredentialStore phoneStore{"synthetic-phone"};
    CloudCrypto phone{&phoneStore};
    QJsonObject device;
    Fixture() {
        Access::configure(client, transport);
        device = {{"id", "device-synthetic"}, {"keyVersion", 1}, {"keys", phone.publicKeys()}};
        if (!Access::pins(client).pin(device))
            qFatal("Synthetic pairing setup failed");
        reply(keyPath, {{"keys", device.value("keys")}});
        reply(ticketPath, {{"id", "ticket-synthetic"},
                           {"status", "claimed"},
                           {"claimedDevice", device},
                           {"claimDigest", DevicePins::claimDigest("ticket-synthetic", device)}});
        reply(ticketPath + "/approve", {});
        reply("/v2/devices/device-synthetic/revoke", {});
        reply("/v2/machines:enroll",
              {{"id", "machine-synthetic"},
               {"workspaceId", "workspace-synthetic"},
               {"keyVersion", 1}},
              201);
    }
    void reply(const QString& path, const QJsonObject& body, int status = 200) {
        transport.statusForPath.insert(path, status);
        transport.responseForPath.insert(path, QJsonDocument(body).toJson());
    }
    void signOut() {
        client.handleLocal("remote.sign_out", {}, [](const QJsonObject&, const QString& error) {
            if (!error.isEmpty())
                qFatal("Synthetic sign-out failed");
        });
    }
};
} // namespace

class SenderKeyCacheTest final : public QObject {
    Q_OBJECT
    void lookup(Fixture& f, bool allowed) {
        bool answered = false;
        QJsonObject keys;
        QString failure;
        Access::senderKeys(f.client, "device-synthetic", 1,
                           [&](const QJsonObject& value, const QString& error) {
                               answered = true;
                               keys = value;
                               failure = error;
                           });
        QTRY_VERIFY(answered);
        QCOMPARE(!keys.isEmpty(), allowed);
        QCOMPARE(failure.isEmpty(), allowed);
    }
    void enroll(Fixture& f) {
        bool answered = false;
        Access::enroll(f.client, [&](const QJsonObject&, const QString&) { answered = true; });
        QTRY_VERIFY(answered);
        QCOMPARE(Access::machineId(f.client), "machine-synthetic");
        // Enrollment has committed; its fake credential challenge fails. Supply a fake
        // credential for subsequent requests without changing the enrollment or pins.
        Access::configure(f.client, f.transport);
    }
    void pair(Fixture& f) {
        bool answered = false;
        QString failure;
        f.client.handleLocal(
            "remote.pairing.approve",
            {{"ticket_id", "ticket-synthetic"},
             {"claim_digest", DevicePins::claimDigest("ticket-synthetic", f.device)}},
            [&](const QJsonObject&, const QString& error) {
                answered = true;
                failure = error;
            });
        QTRY_VERIFY(answered);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void init() { r03::resetCredentials(); }

    void authorizedCacheReuse() {
        Fixture f;
        lookup(f, true);
        QCOMPARE(Access::cachedSenderKeys(f.client), 1);
        const auto requests = f.transport.requests.size();
        lookup(f, true);
        QCOMPARE(f.transport.requests.size(), requests);
    }
    void revocationClearsWarmCache_data() {
        QTest::addColumn<bool>("machine");
        QTest::newRow("device") << false;
        QTest::newRow("machine") << true;
    }
    void revocationClearsWarmCache() {
        QFETCH(bool, machine);
        Fixture f;
        lookup(f, true);
        QCOMPARE(Access::cachedSenderKeys(f.client), 1);
        if (machine)
            emit Access::wakeups(f.client).authorizationRevoked();
        else
            Access::signaling(f.client).receive(
                {{"type", "device.revoked"}, {"deviceId", "device-synthetic"}});
        QCOMPARE(Access::cachedSenderKeys(f.client), 0);
    }
    void cacheHitRechecksCurrentPin_data() {
        QTest::addColumn<QString>("change");
        for (const auto* change : {"remove", "reset", "replace", "machine", "workspace"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }
    void cacheHitRechecksCurrentPin() {
        QFETCH(QString, change);
        Fixture f;
        lookup(f, true);
        if (change == "remove")
            QVERIFY(Access::pins(f.client).unpin("device-synthetic"));
        else if (change == "reset")
            QVERIFY(Access::pins(f.client).reset());
        else if (change == "replace") {
            CredentialStore other("other-synthetic-phone");
            Access::pinDevice(f.client, "device-synthetic", 1, CloudCrypto(&other).publicKeys());
        } else {
            Access::scope(f.client, change == "machine" ? "machine-new" : "machine-synthetic",
                          change == "workspace" ? "workspace-new" : "workspace-synthetic");
            QVERIFY(Access::pins(f.client).reset());
        }
        lookup(f, false);
        QCOMPARE(Access::cachedSenderKeys(f.client), 0);
    }
    void sameWorkspaceEnrollmentRequiresPairingAgain() {
        Fixture f;
        lookup(f, true);
        f.signOut();
        QCOMPARE(Access::cachedSenderKeys(f.client), 0);
        enroll(f);
        lookup(f, false);
        pair(f);
        lookup(f, true);
    }
    void enrollmentWithoutSignOutClearsCache() {
        Fixture f;
        lookup(f, true);
        enroll(f);
        QCOMPARE(Access::cachedSenderKeys(f.client), 0);
        lookup(f, false);
    }
    void deviceSignInAndCancelClearCache() {
        Fixture f;
        lookup(f, true);
        f.reply("/v2/auth/device/code",
                {{"device_code", "ketplus_device_" + QString(43, 'A')},
                 {"user_code", "A1B2C3D4"},
                 {"verification_uri_complete", "https://synthetic.invalid/approve"},
                 {"expires_in", 600},
                 {"interval", 5}});
        bool started = false;
        f.client.handleLocal(
            "remote.sign_in.device", {},
            [&](const QJsonObject&, const QString& error) { started = error.isEmpty(); });
        QCOMPARE(Access::cachedSenderKeys(f.client), 0);
        QTRY_VERIFY(started);
        lookup(f, true);
        f.client.handleLocal("remote.sign_in.cancel", {},
                             [](const QJsonObject&, const QString&) {});
        QCOMPARE(Access::cachedSenderKeys(f.client), 0);
    }
    void encryptedCommandRequiresPairingAfterSameWorkspaceEnrollment() {
        Fixture f;
        lookup(f, true);
        const auto machineKey = CloudCrypto::fromBase64Url(
            Access::crypto(f.client).publicKeys().value("signingPublicKey").toString());
        const auto wrapped = Access::crypto(f.client).wrapSessionKey(
            "session-synthetic", 1, 1, f.device, "machine-synthetic", 1);
        QString error;
        QVERIFY2(f.phone.importSessionKey(wrapped, machineKey, "device-synthetic", 1, &error),
                 qPrintable(error));
        f.signOut();
        enroll(f);
        const auto send = [&](const QString& id, const QString& title) {
            QJsonObject command{{"id", id},
                                {"idempotencyKey", id},
                                {"attempt", 1},
                                {"claimToken", "synthetic"},
                                {"kind", "session.rename"},
                                {"capability", "session.rename"},
                                {"workspaceId", "workspace-synthetic"},
                                {"machineId", "machine-synthetic"},
                                {"sessionId", "session-synthetic"},
                                {"sessionGeneration", 1},
                                {"content", QJsonObject{{"senderDeviceId", "device-synthetic"},
                                                        {"senderKeyVersion", 1},
                                                        {"keyId", "session-synthetic:1:1"},
                                                        {"keyEpoch", 1}}}};
            command.insert(
                "content",
                f.phone.encrypt(
                    QJsonDocument(QJsonObject{{"title", title}}).toJson(QJsonDocument::Compact),
                    Access::commandAad(f.client, command), "session-synthetic", 1, 1,
                    "device-synthetic", 1, &error));
            QVERIFY2(error.isEmpty(), qPrintable(error));
            Access::execute(f.client, command);
        };
        const auto acknowledged = [&](const QString& id) {
            for (const auto& request : f.transport.requests)
                if (request.url().path() == "/v2/commands/" + id + "/ack")
                    return true;
            return false;
        };
        send("unpaired-command", "Unauthorized title");
        QTRY_VERIFY(acknowledged("unpaired-command"));
        QCOMPARE(f.journal.sessions().first().title, "Original");
        QCOMPARE(Access::cachedSenderKeys(f.client), 0);
        pair(f);
        send("paired-command", "Authorized title");
        QTRY_VERIFY(acknowledged("paired-command"));
        QCOMPARE(f.journal.sessions().first().title, "Authorized title");
    }
    void delayedLookup_data() {
        QTest::addColumn<QString>("transition");
        for (const auto* transition : {"sign-out", "sign-out-enroll", "enroll", "revoke",
                                       "relay-revoke", "machine-revoke", "pair"})
            QTest::newRow(transition) << QString::fromLatin1(transition);
    }
    void delayedLookup() {
        QFETCH(QString, transition);
        Fixture f;
        f.transport.heldPaths.insert(keyPath);
        int answers = 0;
        QString failure;
        Access::senderKeys(f.client, "device-synthetic", 1,
                           [&](const QJsonObject&, const QString& error) {
                               ++answers;
                               failure = error;
                           });
        QCOMPARE(f.transport.heldReplies.size(), 1);
        if (transition == "sign-out")
            f.signOut();
        else if (transition == "sign-out-enroll") {
            f.signOut();
            enroll(f);
            pair(f);
        } else if (transition == "enroll")
            enroll(f);
        else if (transition == "revoke") {
            bool revoked = false;
            f.client.handleLocal(
                "remote.device.revoke", {{"device_id", "device-synthetic"}},
                [&](const QJsonObject&, const QString& error) { revoked = error.isEmpty(); });
            QTRY_VERIFY(revoked);
            pair(f);
        } else if (transition == "relay-revoke") {
            Access::signaling(f.client).receive(
                {{"type", "device.revoked"}, {"deviceId", "device-synthetic"}});
        } else if (transition == "machine-revoke")
            emit Access::wakeups(f.client).authorizationRevoked();
        else
            pair(f);
        const auto requests = f.transport.requests.size();
        f.transport.release(keyPath);
        QCOMPARE(answers, 1);
        QVERIFY(!failure.isEmpty());
        QCOMPARE(Access::cachedSenderKeys(f.client), 0);
        QCOMPARE(f.transport.requests.size(), requests);
    }
};

QTEST_GUILESS_MAIN(SenderKeyCacheTest)
#include "SenderKeyCacheTest.moc"
