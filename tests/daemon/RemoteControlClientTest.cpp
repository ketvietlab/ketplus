#include "cloud-privacy/FakeTransport.h"
#include "cloud-privacy/MemoryCredentials.h"
#include "cloud/HostPlatform.h"
#include "control/ControlV2Client.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QPointer>
#include <QtTest>
using namespace ketplus::daemon;
using namespace ketplus::remote;
namespace ketplus::remote {
class RemoteControlTestAccess {
  public:
    static void transport(ControlV2Client& client, QNetworkAccessManager& transport) {
        client.transport_ = &transport;
    }
    static void paired(ControlV2Client& client) {
        client.deviceId_ = "remote-client";
        client.keyVersion_ = 1;
    }
    static void expireCredential(ControlV2Client& client) {
        client.credential_.insert("expiresAt", "2020-01-01T00:00:00.000Z");
    }
};
} // namespace ketplus::remote
namespace {
QJsonObject credential(const QString& subject = "fixture-owner") {
    return {{"accessToken", "ketplus_access_" + QString(43, 'A')},
            {"refreshToken", "ketplus_refresh_" + QString(43, 'B')},
            {"tokenType", "Bearer"},
            {"expiresIn", 900},
            {"sessionId", "fixture-session"},
            {"subject", QJsonObject{{"id", subject}}}};
}
class FixtureTransport final : public QNetworkAccessManager {
  public:
    QHash<QString, QJsonValue> replies;
    QHash<QString, int> statuses;
    QList<QByteArray> bodies;
    QList<QString> paths;
    QPointer<QNetworkReply> latestReply;

  protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* outgoing) override {
        if (operation != CustomOperation)
            qFatal("Fixture cannot open a real network connection");
        const auto path = request.url().path();
        if (!replies.contains(path))
            qFatal("Unexpected fixture request: %s", qPrintable(path));
        bodies.append(outgoing ? outgoing->readAll() : QByteArray{});
        paths.append(path);
        const auto value = replies.value(path);
        const auto body = value.isObject() ? QJsonDocument(value.toObject()).toJson()
                                           : QJsonDocument(value.toArray()).toJson();
        latestReply = new r03::Reply(request, statuses.value(path, 200), body, this);
        return latestReply;
    }
};
const QString prefix = "/native/ketplus";
const QJsonObject session{{"id", "opaque-session"},
                          {"generation", 3},
                          {"workspaceId", "workspace"},
                          {"machineId", "linux-host"}};
struct EncryptedFixture {
    CredentialStore hostStore{"fixture-host"};
    CloudCrypto host{&hostStore};
    QJsonObject hostKeys = host.publicKeys();
    QJsonObject wrapped;
    EncryptedFixture(ControlV2Client& client, FixtureTransport& network) {
        RemoteControlTestAccess::transport(client, network);
        if (!client.setCredential(credential()))
            qFatal("Invalid credential fixture");
        RemoteControlTestAccess::paired(client);
        const auto own = client.identity();
        wrapped = host.wrapSessionKey(
            "opaque-session", 3, 1,
            {{"id", "remote-client"}, {"keyVersion", 1}, {"keys", own.value("keys")}}, "linux-host",
            1);
        network.replies.insert(prefix + "/v2/mobile/sessions/opaque-session/keys",
                               QJsonArray{wrapped});
        network.replies.insert(
            prefix + "/v2/mobile/device-keys/linux-host/1",
            QJsonObject{{"deviceId", "linux-host"}, {"keyVersion", 1}, {"keys", hostKeys}});
    }
    QJsonObject event(int cursor = 1) {
        QJsonObject event{{"workspaceId", "workspace"}, {"sessionId", "opaque-session"},
                          {"sessionGeneration", 3},     {"cursor", cursor},
                          {"turnSequence", 1},          {"eventType", "assistant.delta"}};
        const auto aad = CloudCrypto::canonical({"ketplus", 2, "event", "workspace",
                                                 "opaque-session", 3, cursor, 1, "assistant.delta",
                                                 "opaque-session:3:1", 1, "linux-host", 1});
        event.insert("content", host.encrypt("{\"delta\":\"private fixture text\"}", aad,
                                             "opaque-session", 3, 1, "linux-host", 1));
        return event;
    }
};
} // namespace
class RemoteControlClientTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { r03::resetCredentials(); }
    void browserSignInReturnsOnlyTheRelayPageAndCanBeCanceled() {
        FixtureTransport network;
        ControlV2Client client(QUrl("https://synthetic.invalid/native/ketplus"));
        RemoteControlTestAccess::transport(client, network);
        network.replies.insert(
            prefix + "/v2/auth/handoffs",
            QJsonObject{{"authorizationUrl", "https://synthetic.invalid/native/ketplus/signin"}});
        bool answered = false;
        client.startSignIn([&](const QJsonValue& value, const QString& error) {
            QVERIFY(error.isEmpty());
            QCOMPARE(value.toObject().value("authorizationUrl").toString(),
                     QStringLiteral("https://synthetic.invalid/native/ketplus/signin"));
            QVERIFY(!value.toObject().contains("accessToken"));
            answered = true;
        });
        QTRY_VERIFY(answered);
        QVERIFY(!client.signedIn());
        client.cancelSignIn();
        QVERIFY(!client.signedIn());
    }
    void pairingApprovalBindsTheLocalKeyAndCancellationCannotAcceptALateTicket() {
        FixtureTransport network;
        ControlV2Client client(QUrl("https://synthetic.invalid/native/ketplus"));
        RemoteControlTestAccess::transport(client, network);
        QVERIFY(client.setCredential(credential()));
        const auto own = client.identity();
        const auto keys = own.value("keys").toObject();
        const auto digest = QString::fromLatin1(
            QCryptographicHash::hash(CloudCrypto::canonical({"ticket", "remote-client", 1,
                                                             keys.value("agreementPublicKey"),
                                                             keys.value("signingPublicKey")}),
                                     QCryptographicHash::Sha256)
                .toHex());
        QJsonObject ticket{
            {"id", "ticket"},
            {"machineId", "linux-host"},
            {"status", "claimed"},
            {"claimDigest", digest},
            {"claimedDevice", QJsonObject{{"id", "remote-client"},
                                          {"installationId", own.value("installationId")},
                                          {"platform", hostPlatform()},
                                          {"keyVersion", 1},
                                          {"status", "pending"}}}};
        network.replies.insert(prefix + "/v2/mobile/pairing/tickets/ticket/claim", ticket);
        bool answered = false;
        client.claimPairing("ticket", QString(64, 'A'), "Desktop fixture", [&](auto, auto error) {
            QVERIFY(error.isEmpty());
            answered = true;
        });
        QTRY_VERIFY(answered);
        ticket.insert("status", "approved");
        auto foreign = ticket;
        foreign.insert("machineId", "another-host");
        QVERIFY(!client.acceptPairing(foreign));
        QVERIFY(client.acceptPairing(ticket));
        answered = false;
        client.claimPairing("ticket", QString(64, 'A'), "Desktop fixture", [&](auto, auto error) {
            QVERIFY(!error.isEmpty());
            answered = true;
        });
        client.cancelPairing();
        QTRY_VERIFY(answered);
        QVERIFY(!client.acceptPairing(ticket));
    }
    void hostKeysUnwrapOnlyForTheAddressedClientAndSignedEventsAreDecoded() {
        FixtureTransport network;
        ControlV2Client client(QUrl("https://synthetic.invalid/native/ketplus"));
        EncryptedFixture fixture(client, network);
        const auto event = fixture.event();
        const QString path = prefix + "/v2/mobile/sessions/opaque-session/events";
        network.replies.insert(path, QJsonObject{{"sessionId", "opaque-session"},
                                                 {"sessionGeneration", 3},
                                                 {"afterCursor", 0},
                                                 {"nextCursor", 1},
                                                 {"events", QJsonArray{event}}});
        bool answered = false;
        client.events(session, 0, [&](auto value, auto error) {
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(value.toObject()
                         .value("events")
                         .toArray()
                         .first()
                         .toObject()
                         .value("payload")
                         .toObject()
                         .value("delta")
                         .toString(),
                     "private fixture text");
            answered = true;
        });
        QTRY_VERIFY(answered);
        auto tampered = fixture.wrapped;
        tampered.insert("recipientDeviceId", "other-client");
        network.replies.insert(prefix + "/v2/mobile/sessions/opaque-session/keys",
                               QJsonArray{tampered});
        answered = false;
        client.events(session, 0, [&](auto, auto error) {
            QVERIFY(!error.isEmpty());
            answered = true;
        });
        QTRY_VERIFY(answered);
        // A prior valid key cannot make a changed envelope acceptable.
        tampered = fixture.wrapped;
        tampered.insert("signature", QString(86, 'A'));
        network.replies.insert(prefix + "/v2/mobile/sessions/opaque-session/keys",
                               QJsonArray{tampered});
        answered = false;
        client.events(session, 0, [&](auto, auto error) {
            QVERIFY(!error.isEmpty());
            answered = true;
        });
        QTRY_VERIFY(answered);
    }
    void terminalResultsRequireTheHostSignatureAndOriginalCommandScope() {
        FixtureTransport network;
        ControlV2Client client(QUrl("https://synthetic.invalid/native/ketplus"));
        EncryptedFixture fixture(client, network);
        const QJsonObject intent{
            {"id", "result-command"},        {"idempotencyKey", "result-retry"},
            {"workspaceId", "workspace"},    {"machineId", "linux-host"},
            {"sessionId", "opaque-session"}, {"sessionGeneration", 3},
            {"kind", "session.start_turn"}};
        auto result = intent;
        result.insert("status", "completed");
        result.insert("resultCode", "ok");
        result.insert("attempt", 1);
        const auto aad = CloudCrypto::canonical(
            {"ketplus", 2, "command-result", "result-command", "workspace", "linux-host",
             "opaque-session", 3, 1, "completed", "ok", "opaque-session:3:1", 1, "linux-host", 1});
        result.insert("encryptedResult",
                      fixture.host.encrypt("{\"turnSequence\":9}", aad, "opaque-session", 3, 1,
                                           "linux-host", 1));
        const auto path = prefix + "/v2/mobile/commands/result-command";
        network.replies.insert(path, result);
        bool answered = false;
        client.command(intent, [&](auto value, auto error) {
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(value.toObject().value("result").toObject().value("turnSequence").toInt(), 9);
            QVERIFY(!value.toObject().contains("encryptedResult"));
            answered = true;
        });
        QTRY_VERIFY(answered);
        for (int mutation = 0; mutation < 3; ++mutation) {
            auto invalid = result;
            if (mutation == 0)
                invalid.insert("sessionGeneration", 4);
            if (mutation == 1)
                invalid.insert("resultCode", "changed");
            if (mutation == 2) {
                auto content = invalid.value("encryptedResult").toObject();
                content.insert("signature", QString(86, 'A'));
                invalid.insert("encryptedResult", content);
            }
            network.replies.insert(path, invalid);
            answered = false;
            client.command(intent, [&](auto value, auto error) {
                QVERIFY(!error.isEmpty());
                QVERIFY(value.isNull());
                answered = true;
            });
            QTRY_VERIFY(answered);
        }
    }
    void preparedCommandRetriesUseIdenticalCiphertextAndTheHostDecryptsIt() {
        FixtureTransport network;
        ControlV2Client client(QUrl("https://synthetic.invalid/native/ketplus"));
        EncryptedFixture fixture(client, network);
        const QJsonObject intent{{"id", "command"},
                                 {"idempotencyKey", "same-retry"},
                                 {"workspaceId", "workspace"},
                                 {"machineId", "linux-host"},
                                 {"sessionId", "opaque-session"},
                                 {"sessionGeneration", 3},
                                 {"kind", "session.start_turn"},
                                 {"capability", "session.start_turn"}};
        bool answered = false;
        QJsonObject prepared;
        client.prepareCommand(intent, {{"prompt", "private fixture prompt"}},
                              [&](auto value, auto error) {
                                  QVERIFY(error.isEmpty());
                                  prepared = value.toObject();
                                  answered = true;
                              });
        QTRY_VERIFY(answered);
        QVERIFY(!QJsonDocument(prepared).toJson().contains("private fixture prompt"));
        const auto clientSigning = CloudCrypto::fromBase64Url(
            client.identity().value("keys").toObject().value("signingPublicKey").toString());
        QString error;
        const auto decrypted = fixture.host.decrypt(
            prepared.value("content").toObject(), ControlV2Client::commandAssociatedData(prepared),
            clientSigning, "opaque-session", 3, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QJsonDocument::fromJson(decrypted).object().value("prompt").toString(),
                 "private fixture prompt");
        const auto path = prefix + "/v2/mobile/sessions/opaque-session/commands";
        network.replies.insert(path, QJsonObject{{"id", "command"}, {"status", "queued"}});
        QByteArray first;
        for (int attempt = 0; attempt < 2; ++attempt) {
            answered = false;
            client.submit(prepared, [&](auto, auto failure) {
                QVERIFY(failure.isEmpty());
                answered = true;
            });
            QTRY_VERIFY(answered);
            if (attempt == 0)
                first = network.bodies.last();
            else
                QCOMPARE(network.bodies.last(), first);
        }
    }
    void accountReplacementFencesPendingReads() {
        FixtureTransport network;
        ControlV2Client client(QUrl("https://synthetic.invalid/native/ketplus"));
        RemoteControlTestAccess::transport(client, network);
        QVERIFY(client.setCredential(credential()));
        network.replies.insert(prefix + "/v2/mobile/worktrees", QJsonArray{});
        bool answered = false;
        client.listWorktrees([&](auto value, auto error) {
            QVERIFY(!error.isEmpty());
            QVERIFY(value.isNull());
            answered = true;
        });
        QVERIFY(client.setCredential(credential("another-owner")));
        QTRY_VERIFY(answered);
    }
    void revokedRefreshFencesAlreadyPendingReads() {
        FixtureTransport network;
        ControlV2Client client(QUrl("https://synthetic.invalid/native/ketplus"));
        RemoteControlTestAccess::transport(client, network);
        QVERIFY(client.setCredential(credential()));
        network.replies.insert(prefix + "/v2/mobile/sessions", QJsonArray{});
        bool pendingAnswered = false;
        client.listSessions([&](auto value, auto error) {
            QVERIFY(!error.isEmpty());
            QVERIFY(value.isNull());
            pendingAnswered = true;
        });
        auto pending = network.latestReply;
        pending->blockSignals(true);
        RemoteControlTestAccess::expireCredential(client);
        const auto path = prefix + "/v2/auth/refresh";
        network.replies.insert(path, QJsonObject{{"code", "user_session_revoked"}});
        network.statuses.insert(path, 401);
        bool refreshAnswered = false;
        client.listWorktrees([&](auto, auto error) {
            QVERIFY(!error.isEmpty());
            refreshAnswered = true;
        });
        QTRY_VERIFY(refreshAnswered);
        QVERIFY(!pendingAnswered);
        QVERIFY(pending);
        pending->blockSignals(false);
        QVERIFY(QMetaObject::invokeMethod(pending.data(), "finished", Qt::DirectConnection));
        QTRY_VERIFY(pendingAnswered);
    }
    void signOutReportsFailureToRemovePersistedCredentials() {
        FixtureTransport network;
        ControlV2Client client(QUrl("https://synthetic.invalid/native/ketplus"));
        RemoteControlTestAccess::transport(client, network);
        QVERIFY(client.setCredential(credential()));
        network.replies.insert(prefix + "/v2/auth/logout", QJsonObject{{"ok", true}});
        r03::failRemovals = true;
        bool answered = false;
        client.signOut([&](auto, auto error) {
            QVERIFY(!error.isEmpty());
            answered = true;
        });
        QTRY_VERIFY(answered);
        client.listSessions([&](auto, auto error) { QVERIFY(!error.isEmpty()); });
        QCOMPARE(network.paths.size(), 1);
    }
    void refreshRetriesKeepOneOperationIdentityAfterTransportLoss() {
        FixtureTransport network;
        ControlV2Client client(QUrl("https://synthetic.invalid/native/ketplus"));
        RemoteControlTestAccess::transport(client, network);
        QVERIFY(client.setCredential(credential()));
        RemoteControlTestAccess::expireCredential(client);
        const auto path = prefix + "/v2/auth/refresh";
        network.replies.insert(path, QJsonObject{{"code", "temporary_failure"}});
        network.statuses.insert(path, 503);
        bool answered = false;
        client.listWorktrees([&](auto, auto error) {
            QVERIFY(!error.isEmpty());
            answered = true;
        });
        QTRY_VERIFY(answered);
        const auto first = QJsonDocument::fromJson(network.bodies.last()).object();
        network.replies.insert(path, credential());
        network.statuses.insert(path, 200);
        network.replies.insert(prefix + "/v2/mobile/worktrees", QJsonArray{});
        answered = false;
        client.listWorktrees([&](auto, auto error) {
            QVERIFY(error.isEmpty());
            answered = true;
        });
        QTRY_VERIFY(answered);
        const auto second =
            QJsonDocument::fromJson(network.bodies.at(network.bodies.size() - 2)).object();
        QCOMPARE(second.value("operationId"), first.value("operationId"));
        QCOMPARE(second.value("installationId"), first.value("installationId"));
    }
};
QTEST_GUILESS_MAIN(RemoteControlClientTest)
#include "RemoteControlClientTest.moc"
