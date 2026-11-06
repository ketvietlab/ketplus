#include "CloudPrivacyTestAccess.h"
#include "FakeTransport.h"
#include "MemoryCredentials.h"
#include "cloud/CloudControlBatching.h"
#include "cloud/CloudEventProjection.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <optional>

using namespace ketplus::daemon;
using Access = CloudPrivacyTestAccess;
namespace {
const QString marker = "R03_E0_SYNTHETIC_COMMAND_OUTPUT";
QString command() {
    return QString(180, QLatin1Char('x')) +
           QString::fromUtf8(" \xE6\x97\xA5\xE6\x9C\xAC \xE2\x9C\x93");
}

struct Context {
    LocalJournal journal;
    CodexAdapter provider{"/nonexistent/r03-e0-fake-provider"};
    SessionHost host;
    r03::Transport transport;
    std::unique_ptr<CloudControlClient> client;
    static LocalJournal* seed(LocalJournal& journal) {
        if (journal.sessions().isEmpty() && !journal.upsertSession({"session-synthetic",
                                                                    "synthetic-worktree",
                                                                    "codex",
                                                                    "thread-synthetic",
                                                                    {},
                                                                    "ready",
                                                                    1,
                                                                    0,
                                                                    1}))
            qFatal("Synthetic journal setup failed");
        return &journal;
    }
    explicit Context(const QString& path) : journal(path), host(seed(journal), &provider) {}
    void start() {
        client = std::make_unique<CloudControlClient>(&journal, &host);
        Access::configure(*client, transport);
    }
    void item(const QString& status, const QString& type = "commandExecution") {
        const QJsonObject value{
            {"id", status == "failed" ? "item-failed" : "item-command"},
            {"type", type},
            {"command", command()},
            {"text", type == "commandExecution" ? QString() : marker + " non-command text"},
            {"status", status},
            {"exitCode", status == "failed" ? 7 : 0},
            {"stdout", marker + " stdout"},
            {"stderr", marker + " stderr"},
            {"aggregatedOutput", marker + " aggregate"},
            {"outputRef", "fixture://" + marker},
            {"commandActions", QJsonArray{QJsonObject{{"output", marker}}}}};
        // Fake provider emits through the production adapter signal/normalizer connection.
        emit provider.notificationReceived(status == "inProgress" ? "item/started"
                                                                  : "item/completed",
                                           {{"threadId", "thread-synthetic"}, {"item", value}});
    }
    void delta() {
        emit provider.notificationReceived("item/commandExecution/outputDelta",
                                           {{"threadId", "thread-synthetic"},
                                            {"itemId", "item-command"},
                                            {"delta", marker + " delta"}});
    }
    QJsonArray outgoing(int index = 0) const {
        return QJsonDocument::fromJson(transport.bodies.at(index))
            .object()
            .value("events")
            .toArray();
    }
};

QJsonObject rawMetadata(const QJsonObject& envelope) {
    return {{"session_id", envelope.value("sessionId")},
            {"session_generation", envelope.value("sessionGeneration")},
            {"cursor", envelope.value("cursor")},
            {"turn_sequence", envelope.value("turnSequence")},
            {"event_type", envelope.value("eventType")}};
}
QByteArray decrypt(Context& c, const QJsonObject& envelope, QString* error = nullptr) {
    const auto content = envelope.value("content").toObject();
    auto& crypto = Access::crypto(*c.client);
    const auto publicKey =
        CloudCrypto::fromBase64Url(crypto.publicKeys().value("signingPublicKey").toString());
    return crypto.decrypt(content, Access::aad(*c.client, rawMetadata(envelope), content),
                          publicKey, envelope.value("sessionId").toString(),
                          envelope.value("sessionGeneration").toInteger(), error);
}
QJsonObject legacyEnvelope(Context& c) {
    const auto raw = c.journal.eventsAfter("session-synthetic", 1, 0).first().toObject();
    const QJsonObject shell{{"keyId", "session-synthetic:1:1"},
                            {"keyEpoch", 1},
                            {"senderDeviceId", "machine-synthetic"},
                            {"senderKeyVersion", 1}};
    auto content = Access::crypto(*c.client).encrypt(
        QJsonDocument(raw.value("payload").toObject()).toJson(QJsonDocument::Compact),
        Access::aad(*c.client, raw, shell), "session-synthetic", 1, 1, "machine-synthetic", 1);
    return {{"sessionId", "session-synthetic"},
            {"sessionGeneration", 1},
            {"cursor", 1},
            {"turnSequence", raw.value("turn_sequence")},
            {"eventType", raw.value("event_type")},
            {"occurredAt", raw.value("sent_at")},
            {"content", content}};
}

QJsonObject decryptRestoreUpload(Context& c, const QJsonObject& command, const QJsonObject& upload,
                                 QByteArray* serialized = nullptr) {
    QByteArray page;
    qint64 expectedOffset = 0;
    const QJsonArray responses = upload.value("responses").toArray();
    for (int index = 0; index < responses.size(); ++index) {
        const QJsonObject response = responses.at(index).toObject();
        const bool terminal = index + 1 == responses.size();
        if (response.value("responseCursor").toInteger() != index + 1 ||
            response.value("terminal").toBool() != terminal) {
            return {};
        }
        const QString digest = response.value("responseDigest").toString();
        const QJsonObject content = response.value("content").toObject();
        const QByteArray publicKey = CloudCrypto::fromBase64Url(
            Access::crypto(*c.client).publicKeys().value("signingPublicKey").toString());
        QString error;
        const QByteArray plaintext = Access::crypto(*c.client).decrypt(
            content, Access::restoreAad(*c.client, command, index + 1, terminal, digest, content),
            publicKey, command.value("sessionId").toString(),
            command.value("sessionGeneration").toInteger(), &error);
        if (plaintext.isEmpty()) {
            return {};
        }
        const QJsonObject fragment = QJsonDocument::fromJson(plaintext).object();
        const QByteArray bytes = CloudCrypto::fromBase64Url(fragment.value("data").toString());
        if (fragment.value("restore_response_version").toInt() != 1 ||
            fragment.value("encoding").toString() != "json-utf8-fragment" ||
            fragment.value("offset").toInteger(-1) != expectedOffset || bytes.isEmpty()) {
            return {};
        }
        page.append(bytes);
        expectedOffset += bytes.size();
        if (fragment.value("total_bytes").toInteger(-1) != expectedOffset && terminal) {
            return {};
        }
        if (terminal &&
            QString::fromLatin1(
                QCryptographicHash::hash(page, QCryptographicHash::Sha256).toHex()) != digest) {
            return {};
        }
    }
    if (serialized != nullptr) {
        *serialized = page;
    }
    return QJsonDocument::fromJson(page).object();
}

QJsonObject restoreCommand(Context& c, const QString& requestId) {
    return {{"id", requestId},
            {"ownerSubjectId", "owner-synthetic"},
            {"workspaceId", "workspace-synthetic"},
            {"machineId", "machine-synthetic"},
            {"sessionId", "session-synthetic"},
            {"sessionGeneration", 1},
            {"credentialEpoch", 1},
            {"snapshotCursor", c.journal.sessions().first().lastCursor},
            {"requestedByteCap", 512 * 1024},
            {"deadlineAt", QDateTime::currentDateTimeUtc().addSecs(300).toString(Qt::ISODate)},
            {"attempt", 1},
            {"claimToken", "claim-" + requestId}};
}

// Runs a root restore request and returns the decrypted, reassembled page.
QJsonObject restorePage(Context& c, const QString& requestId, QByteArray* serialized) {
    const QString responsePath =
        QStringLiteral("/v2/restore-requests/%1/responses:append").arg(requestId);
    const QString ackPath = QStringLiteral("/v2/commands/%1/ack").arg(requestId);
    c.transport.statusForPath.insert(responsePath, 202);
    c.transport.responseForPath.insert(responsePath, R"({"acceptedThrough":128})");
    c.transport.statusForPath.insert(ackPath, 200);
    c.transport.responseForPath.insert(ackPath, R"({"status":"completed"})");
    c.transport.requests.clear();
    c.transport.bodies.clear();
    const QJsonObject command = restoreCommand(c, requestId);
    Access::restore(*c.client, command,
                    {{"before_cursor", 0}, {"turn_limit", 3}, {"event_limit", 128}});
    QElapsedTimer timer;
    timer.start();
    const auto hasPath = [&](const QString& path) {
        return std::any_of(c.transport.requests.cbegin(), c.transport.requests.cend(),
                           [&](const auto& request) { return request.url().path() == path; });
    };
    while (timer.elapsed() < 5000 && !hasPath(ackPath))
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const auto index = std::distance(
        c.transport.requests.cbegin(),
        std::find_if(c.transport.requests.cbegin(), c.transport.requests.cend(),
                     [&](const auto& request) { return request.url().path() == responsePath; }));
    if (index >= c.transport.bodies.size())
        return {};
    return decryptRestoreUpload(
        c, command, QJsonDocument::fromJson(c.transport.bodies.at(index)).object(), serialized);
}
} // namespace

class CloudPrivacyTest final : public QObject {
    Q_OBJECT
  private slots:
    void mobileForkReachesProviderInsteadOfGenericUnknownOperation() {
        QTemporaryDir dir;
        Context c(dir.filePath("fork.sqlite"));
        c.start();
        auto& crypto = Access::crypto(*c.client);
        const QJsonObject keys = crypto.publicKeys();
        Access::pinDevice(*c.client, "mobile-fork", 1, keys);
        c.transport.statusForPath.insert("/v2/device-keys/mobile-fork/1", 200);
        c.transport.responseForPath.insert(
            "/v2/device-keys/mobile-fork/1",
            QJsonDocument(QJsonObject{{"keys", keys}}).toJson(QJsonDocument::Compact));
        QJsonObject command{{"id", "mobile-fork-command"},
                            {"workspaceId", "workspace-synthetic"},
                            {"machineId", "machine-synthetic"},
                            {"sessionId", "session-synthetic"},
                            {"sessionGeneration", 1},
                            {"kind", "session.fork"},
                            {"capability", "session.fork"},
                            {"idempotencyKey", "mobile-fork-command-key"},
                            {"attempt", 1},
                            {"claimToken", "fork-claim"}};
        const QJsonObject shell{{"keyId", "session-synthetic:1:1"},
                                {"keyEpoch", 1},
                                {"senderDeviceId", "mobile-fork"},
                                {"senderKeyVersion", 1}};
        command.insert("content", shell);
        const QJsonObject content = crypto.encrypt(
            QJsonDocument(QJsonObject{{"fork_session_id", "fork-copy"}})
                .toJson(QJsonDocument::Compact),
            Access::commandAad(*c.client, command), "session-synthetic", 1, 1, "mobile-fork", 1);
        QVERIFY(!content.isEmpty());
        command.insert("content", content);
        Access::execute(*c.client, command);
        const QString ackPath = "/v2/commands/mobile-fork-command/ack";
        QTRY_VERIFY_WITH_TIMEOUT(
            std::any_of(c.transport.requests.cbegin(), c.transport.requests.cend(),
                        [&](const auto& request) { return request.url().path() == ackPath; }),
            10000);
        const auto ack =
            std::find_if(c.transport.requests.cbegin(), c.transport.requests.cend(),
                         [&](const auto& request) { return request.url().path() == ackPath; });
        const QJsonObject body =
            QJsonDocument::fromJson(
                c.transport.bodies.at(std::distance(c.transport.requests.cbegin(), ack)))
                .object();
        // The fake provider executable cannot run; proving we reached its fork path costs no quota.
        QCOMPARE(body.value("resultCode").toString(), QStringLiteral("provider_error"));
        QCOMPARE(c.journal.sessions().size(), 1);
        QCOMPARE(c.journal.sessions().first().threadId, QStringLiteral("thread-synthetic"));
    }
    void init() { r03::resetCredentials(); }

    void worktreeScopedSealedCommandRoundTripsWithoutASessionKey() {
        QTemporaryDir dir;
        Context c(dir.filePath(QStringLiteral("sealed-create.sqlite")));
        c.start();
        auto& crypto = Access::crypto(*c.client);
        QString error;
        const QJsonObject keys = crypto.publicKeys(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QByteArray signing =
            CloudCrypto::fromBase64Url(keys.value(QStringLiteral("signingPublicKey")).toString());
        const QByteArray agreement =
            CloudCrypto::fromBase64Url(keys.value(QStringLiteral("agreementPublicKey")).toString());
        const QByteArray aad = QByteArrayLiteral("session.create/workspace/repository/worktree");
        const QByteArray plaintext = QByteArrayLiteral("{\"prompt\":\"create safely\"}");
        const QJsonObject content = crypto.encryptSealed(
            plaintext, aad, agreement, QStringLiteral("create-request-0001"),
            QStringLiteral("mobile-1"), 1, QStringLiteral("machine-1"), 1, &error);
        QVERIFY2(!content.isEmpty(), qPrintable(error));
        QCOMPARE(content.value(QStringLiteral("recipientDeviceId")).toString(),
                 QStringLiteral("mobile-1"));
        QCOMPARE(content.value(QStringLiteral("ephemeralPublicKey")).toString().size(), 43);
        QCOMPARE(crypto.decryptSealed(content, aad, signing, QStringLiteral("create-request-0001"),
                                      &error),
                 plaintext);
        QVERIFY(crypto
                    .decryptSealed(content, aad + 'x', signing,
                                   QStringLiteral("create-request-0001"), &error)
                    .isEmpty());
    }

    void signOutClearsControlIdentityWithoutDeletingJournal() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        QJsonObject result;
        QString error;
        c.client->handleLocal("remote.sign_out", {},
                              [&result, &error](const QJsonObject& value, const QString& detail) {
                                  result = value;
                                  error = detail;
                              });
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!result.value("configured").toBool());
        QVERIFY(result.value("machine_id").toString().isEmpty());
        QCOMPARE(c.journal.sessions().size(), 1);
    }

    void controlRequestsStayOffHttp2() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.item("completed");
        c.start();
        Access::upload(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QVERIFY(!c.transport.requests.isEmpty());
        // Qt allows HTTP/2 unless a request says otherwise.
        for (const auto& request : std::as_const(c.transport.requests))
            QVERIFY(!request.attribute(QNetworkRequest::Http2AllowedAttribute, true).toBool());
    }

    void freshUploadDecryptsToCommandMetadataOnly() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.item("inProgress");
        c.delta();
        c.item("completed");
        c.item("failed");
        const auto raw = c.journal.eventsAfter("session-synthetic", 1, 0);
        QCOMPARE(raw.size(), 4);
        QVERIFY(QJsonDocument(raw).toJson().contains(marker.toUtf8()));
        QCOMPARE(raw.at(1).toObject().value("event_type").toString(), "command.output_delta");
        QCOMPARE(raw.at(1).toObject().value("payload").toObject().value("delta").toString(),
                 marker + " delta");
        c.start();
        Access::upload(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(c.transport.bodies.size(), 1);
        QCOMPARE(QJsonDocument::fromJson(c.transport.bodies.first())
                     .object()
                     .value("machineId")
                     .toString(),
                 "machine-synthetic");
        QCOMPARE(c.transport.requests.first().rawHeader("Authorization"),
                 QByteArray("Bearer synthetic-test-token"));
        const auto outgoing = c.outgoing();
        QCOMPARE(outgoing.size(), 4);
        for (int i = 0; i < outgoing.size(); ++i) {
            const auto envelope = outgoing.at(i).toObject();
            QCOMPARE(envelope.value("sessionId").toString(), "session-synthetic");
            QCOMPARE(envelope.value("sessionGeneration").toInteger(), 1);
            QCOMPARE(envelope.value("cursor").toInteger(), i + 1);
            QCOMPARE(envelope.value("turnSequence").toInteger(), 1);
            QCOMPARE(envelope.value("eventType"), raw.at(i).toObject().value("event_type"));
            const auto content = envelope.value("content").toObject();
            QCOMPARE(content.value("keyId").toString(), "session-synthetic:1:1");
            QCOMPARE(content.value("senderDeviceId").toString(), "machine-synthetic");
            QCOMPARE(content.value("keyEpoch").toInt(), 1);
            QCOMPARE(content.value("senderKeyVersion").toInt(), 1);
            QCOMPARE(envelope, c.journal.cloudEvent("session-synthetic", 1, i + 1));
            QString error;
            const auto bytes = decrypt(c, envelope, &error);
            QVERIFY2(!bytes.isEmpty(), qPrintable(error));
            QVERIFY(!bytes.contains(marker.toUtf8()));
            const auto actual = QJsonDocument::fromJson(bytes).object();
            const auto original = raw.at(i).toObject().value("payload").toObject();
            QCOMPARE(actual.value("item_id"), original.value("item_id"));
            QCOMPARE(actual.value("item_id").toString(), i == 3 ? "item-failed" : "item-command");
            if (i == 1) {
                QCOMPARE(actual.keys(), QStringList{"item_id"});
            } else {
                QCOMPARE(actual.keys(),
                         (QStringList{"command", "item", "item_id", "item_type", "status"}));
                QCOMPARE(actual.value("command").toString(), command());
                QCOMPARE(actual.value("status"), original.value("status"));
                QCOMPARE(actual.value("status").toString(), i == 0   ? "inProgress"
                                                            : i == 2 ? "completed"
                                                                     : "failed");
                QCOMPARE(actual.value("item_type").toString(), "commandExecution");
                const auto item = actual.value("item").toObject();
                QCOMPARE(item.keys(), (QStringList{"command", "exitCode", "id", "status", "type"}));
                QCOMPARE(item.value("command").toString(), command());
                QCOMPARE(item.value("id"), actual.value("item_id"));
                QCOMPARE(item.value("type").toString(), "commandExecution");
                QCOMPARE(item.value("status"), original.value("item").toObject().value("status"));
                QCOMPARE(item.value("exitCode").toInt(), i == 3 ? 7 : 0);
            }
        }
        QCOMPARE(c.journal.eventsAfter("session-synthetic", 1, 0), raw);
    }

    void retryAndReopenUseSameDurableEnvelope() {
        QTemporaryDir dir;
        const auto path = dir.filePath("retry.sqlite");
        QByteArray firstBody;
        QJsonArray raw;
        {
            Context c(path);
            c.item("completed");
            c.start();
            raw = c.journal.eventsAfter("session-synthetic", 1, 0);
            Access::upload(*c.client);
            QTRY_VERIFY(Access::idle(*c.client));
            firstBody = c.transport.bodies.first();
            // The retry waits after the failure; once the wait is over it is the same envelope.
            Access::expireUploadBackoff(*c.client);
            Access::upload(*c.client);
            QTRY_VERIFY(Access::idle(*c.client));
            QCOMPARE(c.transport.bodies.size(), 2);
            QCOMPARE(c.transport.bodies.last(), firstBody);
            QCOMPARE(c.journal.eventsAfter("session-synthetic", 1, 0), raw);
        }
        Context reopened(path);
        reopened.start();
        Access::upload(*reopened.client);
        QTRY_VERIFY(Access::idle(*reopened.client));
        QCOMPARE(reopened.transport.bodies.first(), firstBody);
        QVERIFY(
            !decrypt(reopened, reopened.outgoing().first().toObject()).contains(marker.toUtf8()));
        QCOMPARE(reopened.journal.eventsAfter("session-synthetic", 1, 0), raw);
        reopened.transport.status = 202;
        reopened.transport.response =
            R"({"acceptedThrough":[{"sessionId":"wrong-session","sessionGeneration":1,"cursor":1},{"sessionId":"session-synthetic","sessionGeneration":2,"cursor":1}]})";
        Access::expireUploadBackoff(*reopened.client);
        Access::upload(*reopened.client);
        QTRY_VERIFY(Access::idle(*reopened.client));
        QCOMPARE(reopened.journal.eventsAfter("session-synthetic", 1, 0), raw);
        QVERIFY(!reopened.journal.cloudEvent("session-synthetic", 1, 1).isEmpty());
        reopened.transport.response =
            R"({"acceptedThrough":[{"sessionId":"session-synthetic","sessionGeneration":1,"cursor":1}]})";
        Access::upload(*reopened.client);
        QTRY_VERIFY(Access::idle(*reopened.client));
        QVERIFY(reopened.journal.cloudEvent("session-synthetic", 1, 1).isEmpty());
        QVERIFY(reopened.journal.eventsAfter("session-synthetic", 1, 0).isEmpty());
        QCOMPARE(reopened.journal.sessions().first().lastCursor, 1);
    }

    void acceptedThroughIsClampedToHighestUploadedCursor() {
        QTemporaryDir dir;
        Context c(dir.filePath("clamp.sqlite"));
        c.item("completed");
        c.start();
        const QJsonArray raw = c.journal.eventsAfter("session-synthetic", 1, 0);
        QVERIFY(!raw.isEmpty());
        const qint64 highest = raw.last().toObject().value("cursor").toInteger();
        c.transport.status = 202;
        c.transport.response =
            R"({"acceptedThrough":[{"sessionId":"session-synthetic","sessionGeneration":1,"cursor":999999}]})";
        Access::upload(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(c.transport.bodies.size(), 1);
        const QJsonArray sent = c.outgoing();
        QCOMPARE(sent.last().toObject().value("cursor").toInteger(), highest);
        QCOMPARE(r03::credentials.value(
                     "dev.ketplus.daemon.control/uploaded-cursor-session-synthetic-1"),
                 QByteArray::number(highest));
        QCOMPARE(c.journal.sessions().first().lastCursor, highest);
        QCOMPARE(Access::error(*c.client), QString("cloud_event_ack_beyond_upload"));
    }

    void wrappedSessionKeyRejectsMalformedEnvelopes() {
        QTemporaryDir dir;
        Context c(dir.filePath("wrap.sqlite"));
        c.start();
        auto& crypto = Access::crypto(*c.client);
        const QJsonObject keys = crypto.publicKeys();
        const QJsonObject recipient{{"id", "device-synthetic"}, {"keyVersion", 1}, {"keys", keys}};
        QString error;
        const QJsonObject envelope = crypto.wrapSessionKey("session-synthetic", 1, 1, recipient,
                                                           "machine-synthetic", 1, &error);
        QVERIFY2(CloudCrypto::validWrappedSessionKey(envelope), qPrintable(error));
        QCOMPARE(CloudCrypto::fromBase64Url(envelope.value("ciphertext").toString()).size(), 32);

        // A recipient with a bad agreement key must not yield a signed envelope.
        QJsonObject badKeys = keys;
        badKeys.insert("agreementPublicKey", QString::fromLatin1(CloudCrypto::base64Url("short")));
        QJsonObject badRecipient = recipient;
        badRecipient.insert("keys", badKeys);
        QVERIFY(
            crypto.wrapSessionKey("session-synthetic", 1, 1, badRecipient, "machine-synthetic", 1)
                .isEmpty());

        const auto tampered = [&envelope](const char* field, const QByteArray& bytes) {
            QJsonObject copy = envelope;
            copy.insert(QLatin1String(field), QString::fromLatin1(CloudCrypto::base64Url(bytes)));
            return copy;
        };
        QVERIFY(!CloudCrypto::validWrappedSessionKey(tampered("ciphertext", QByteArray(31, 'x'))));
        QVERIFY(!CloudCrypto::validWrappedSessionKey(tampered("ciphertext", QByteArray(48, 'x'))));
        QVERIFY(!CloudCrypto::validWrappedSessionKey(tampered("nonce", QByteArray(16, 'x'))));
        QVERIFY(!CloudCrypto::validWrappedSessionKey(tampered("tag", QByteArray(12, 'x'))));
        QVERIFY(!CloudCrypto::validWrappedSessionKey(tampered("signature", {})));
        QVERIFY(!CloudCrypto::validWrappedSessionKey(
            tampered("ephemeralPublicKey", QByteArray(31, 'x'))));
        QVERIFY(!CloudCrypto::validWrappedSessionKey({}));
    }

    void legacyCachePrivacyViolationReproduced() {
        QTemporaryDir dir;
        Context c(dir.filePath("legacy.sqlite"));
        c.item("completed");
        c.start();
        const auto legacy = legacyEnvelope(c);
        QVERIFY(!legacy.value("content").toObject().isEmpty());
        QVERIFY(c.journal.storeCloudEvent("session-synthetic", 1, 1, legacy, 0));
        Access::upload(*c.client);
        QVERIFY(Access::idle(*c.client));
        QVERIFY(c.transport.bodies.isEmpty());
        QCOMPARE(Access::error(*c.client),
                 QStringLiteral("cloud_event_cache_requires_reconciliation"));
        QVERIFY(c.journal.cloudEvent("session-synthetic", 1, 1).isEmpty());
        Access::upload(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(c.outgoing().size(), 1);
        const auto plaintext = decrypt(c, c.outgoing().first().toObject());
        QVERIFY(!plaintext.isEmpty());
        QVERIFY(!plaintext.contains(marker.toUtf8()));
    }

    void freshCryptoFailureAndMissingScopeDoNotUpload() {
        QTemporaryDir dir;
        Context c(dir.filePath("failure.sqlite"));
        c.item("completed");
        c.start();
        Access::upload(*c.client, "wrong-session", 1);
        Access::upload(*c.client, "session-synthetic", 2);
        QVERIFY(c.transport.bodies.isEmpty());
        r03::failWrites = true;
        Access::upload(*c.client);
        QVERIFY(c.transport.bodies.isEmpty());
        QVERIFY(Access::idle(*c.client));
        QVERIFY(!Access::error(*c.client).isEmpty());
        QVERIFY(c.journal.cloudEvent("session-synthetic", 1, 1).isEmpty());
        QCOMPARE(c.journal.eventsAfter("session-synthetic", 1, 0).size(), 1);
        r03::failWrites = false;
        CredentialStore store;
        QVERIFY(store.write("signing-private-key", "synthetic-invalid-signing-key"));
        Access::upload(*c.client);
        QVERIFY(c.transport.bodies.isEmpty());
        QVERIFY(c.journal.cloudEvent("session-synthetic", 1, 1).isEmpty());
    }

    void decryptRejectsTamperAndWrongScopeGeneration() {
        QTemporaryDir dir;
        Context c(dir.filePath("tamper.sqlite"));
        c.item("completed");
        c.start();
        Access::upload(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        const auto valid = c.outgoing().first().toObject();
        QVERIFY(!decrypt(c, valid).isEmpty());
        Access::workspace(*c.client, "wrong-workspace");
        QString wrongWorkspaceError;
        QVERIFY(decrypt(c, valid, &wrongWorkspaceError).isEmpty());
        QVERIFY(!wrongWorkspaceError.isEmpty());
        Access::workspace(*c.client, "workspace-synthetic");
        for (const QString field : {QString("sessionId"), QString("sessionGeneration"),
                                    QString("ciphertext"), QString("signature")}) {
            auto changed = valid;
            if (field == "sessionId")
                changed.insert(field, "wrong-session");
            else if (field == "sessionGeneration")
                changed.insert(field, 2);
            else {
                auto content = changed.value("content").toObject();
                auto bytes = CloudCrypto::fromBase64Url(content.value(field).toString());
                bytes[0] = bytes[0] ^ 1;
                content.insert(field, QString::fromLatin1(CloudCrypto::base64Url(bytes)));
                changed.insert("content", content);
            }
            QString error;
            QVERIFY(decrypt(c, changed, &error).isEmpty());
            QVERIFY(!error.isEmpty());
        }
    }

    void poisonedCacheScopeAndTamperAreForwarded() {
        for (const QString mode : {QString("scope"), QString("generation"), QString("tamper")}) {
            r03::resetCredentials();
            QTemporaryDir dir;
            Context c(dir.filePath("poison.sqlite"));
            c.item("completed");
            c.start();
            auto cached = legacyEnvelope(c);
            if (mode == "scope")
                cached.insert("sessionId", "other-session");
            else if (mode == "generation")
                cached.insert("sessionGeneration", 2);
            else {
                auto content = cached.value("content").toObject();
                content.insert("signature", "tampered");
                cached.insert("content", content);
            }
            QVERIFY(c.journal.storeCloudEvent("session-synthetic", 1, 1, cached));
            Access::upload(*c.client);
            QVERIFY(Access::idle(*c.client));
            QVERIFY(c.transport.bodies.isEmpty());
            QCOMPARE(Access::error(*c.client),
                     QStringLiteral("cloud_event_cache_requires_reconciliation"));
            QVERIFY(c.journal.cloudEvent("session-synthetic", 1, 1).isEmpty());
            Access::upload(*c.client);
            QTRY_VERIFY(Access::idle(*c.client));
            QCOMPARE(c.outgoing().size(), 1);
            const auto plaintext = decrypt(c, c.outgoing().first().toObject());
            QVERIFY(!plaintext.isEmpty());
            QVERIFY(!plaintext.contains(marker.toUtf8()));
        }
    }

    void embeddedUrlsLoseQueriesWithoutQuadraticScans() {
        QCOMPARE(sanitizeCloudText("open https://example.test/a?token=x#code=y now"),
                 QStringLiteral("open https://example.test/a now"));
        // The scheme starts at the first letter of the run before "://", as before.
        QCOMPARE(sanitizeCloudText("12git+ssh://host/repo?ref=secret"),
                 QStringLiteral("12git+ssh://host/repo"));
        QCOMPARE(sanitizeCloudText("x=1 ftp://a/b?q=1 'http://c/d?e=f'"),
                 QStringLiteral("x=1 ftp://a/b 'http://c/d'"));
        QCOMPARE(sanitizeCloudText("ratio 3:1 and a://"), QStringLiteral("ratio 3:1 and a://"));
        // CTest runs this suite without PCRE2 JIT, as a hardened-runtime daemon may; a
        // backtracking scan of this run took minutes.
        const QString run = QString(3 * 1024 * 1024, 'm') + QStringLiteral(" https://h/p?t=s");
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(sanitizeCloudText(run).right(12), QStringLiteral(" https://h/p"));
        QVERIFY2(timer.elapsed() < 10000, qPrintable(QString::number(timer.elapsed())));
    }

    void nonCommandPlaintextRemainsUnchanged() {
        QTemporaryDir dir;
        Context c(dir.filePath("non-command.sqlite"));
        c.item("completed", "agentMessage");
        c.item("failed", "mcpToolCall");
        c.start();
        const auto raw = c.journal.eventsAfter("session-synthetic", 1, 0);
        Access::upload(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        const auto outgoing = c.outgoing();
        QCOMPARE(outgoing.size(), 2);
        const auto agentMessage = decrypt(c, outgoing.at(0).toObject());
        QCOMPARE(agentMessage, QJsonDocument(raw.at(0).toObject().value("payload").toObject())
                                   .toJson(QJsonDocument::Compact));
        QVERIFY(agentMessage.contains(marker.toUtf8()));
        // mcpToolCall has an explicit metadata-only projection (ADR 0008): tool identity and
        // status survive, raw output does not.
        const auto mcp = decrypt(c, outgoing.at(1).toObject());
        QVERIFY(!mcp.isEmpty());
        QVERIFY(!mcp.contains(marker.toUtf8()));
        QCOMPARE(QJsonDocument::fromJson(mcp).object().value("item").toObject().value("type"),
                 QJsonValue("mcpToolCall"));
    }

    void streamedTextCarriesAnOpaqueItemKeyTheCompletionSupersedes() {
        QTemporaryDir dir;
        Context c(dir.filePath("item-key.sqlite"));
        c.item("inProgress");
        c.delta();
        c.delta();
        c.item("completed");
        c.start();
        Access::upload(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        const auto outgoing = c.outgoing();
        QStringList types;
        for (const auto& value : outgoing)
            types << value.toObject().value("eventType").toString();
        // The two chunks were journaled as one event.
        QCOMPARE(types, QStringList({"item.started", "command.output_delta", "item.completed"}));
        const auto started = outgoing.at(0).toObject();
        const auto delta = outgoing.at(1).toObject();
        const auto completed = outgoing.at(2).toObject();
        QVERIFY(!started.contains("itemKey"));
        const QString key = delta.value("itemKey").toString();
        QVERIFY(QRegularExpression("^[A-Za-z0-9_-]{16,64}$").match(key).hasMatch());
        QVERIFY(!key.contains("item-command"));
        QVERIFY(!delta.contains("supersedesDeltas"));
        QCOMPARE(completed.value("itemKey").toString(), key);
        QCOMPARE(completed.value("supersedesDeltas"), QJsonValue(true));
        // Plaintext metadata only: the signed content decrypts as before.
        QVERIFY(!decrypt(c, completed).isEmpty());
    }

    void encryptedLargeEventEnvelopesStayBelowBackendCap() {
        QTemporaryDir dir;
        Context c(dir.filePath("large-envelope.sqlite"));
        const QString text(3 * 1024 * 1024, QChar(0x20ac));
        const QJsonObject item{{"id", "large-agent"}, {"type", "agentMessage"}, {"text", text}};
        QString error;
        const qint64 manifestCursor = c.journal.appendEvent(
            "session-synthetic", 1, "item.completed",
            {{"item_id", "large-agent"}, {"item_type", "agentMessage"}, {"item", item}}, &error);
        QVERIFY2(manifestCursor > 1, qPrintable(error));
        const auto raw = c.journal.eventsAfter("session-synthetic", 1, 0, 100);
        QVERIFY(!raw.isEmpty());
        QVERIFY(raw.last().toObject().value("cursor").toInteger() < manifestCursor);

        c.start();
        Access::upload(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        // The chunks do not all fit one request: the batch stops below the relay's request cap.
        QVERIFY(c.transport.bodies.first().size() < 1024 * 1024);
        const auto outgoing = c.outgoing();
        QVERIFY(!outgoing.isEmpty());
        QVERIFY(outgoing.size() < raw.size());
        for (int i = 0; i < outgoing.size(); ++i) {
            const auto envelope = outgoing.at(i).toObject();
            QVERIFY(QJsonDocument(envelope).toJson(QJsonDocument::Compact).size() <
                    protocol::maximumEventPayloadBytes);
            QCOMPARE(decrypt(c, envelope),
                     QJsonDocument(raw.at(i).toObject().value("payload").toObject())
                         .toJson(QJsonDocument::Compact));
            QCOMPARE(envelope.value("eventType").toString(), "transcript.large_chunk");
        }
        const auto reference = c.journal.transcriptWindow("session-synthetic", 1)
                                   .events.last()
                                   .toObject()
                                   .value("payload")
                                   .toObject()
                                   .value("large_event")
                                   .toObject();
        QCOMPARE(manifestCursor, reference.value("chunk_count").toInt() + 1);
        QVERIFY(reference.value("total_bytes").toInteger() >= 8 * 1024 * 1024);
    }

    void eventBatchStaysBelowTheRelayRequestLimit() {
        QTemporaryDir dir;
        Context c(dir.filePath("request-limit.sqlite"));
        QString error;
        // A full local page (under the journal's raw page bound) grows past the relay's 1 MiB
        // request limit once every event is encrypted, encoded and signed.
        for (int i = 0; i < 100; ++i)
            QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "user.message",
                                           {{"text", QString(7'200, QLatin1Char('a' + i % 26))}},
                                           &error) > 0,
                     qPrintable(error));
        QCOMPARE(c.journal.eventsAfter("session-synthetic", 1, 0, 100).size(), 100);
        c.start();
        Access::upload(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(c.transport.bodies.size(), 1);
        QVERIFY(c.transport.bodies.first().size() < maximumEventUploadBytes + 1024);
        const auto sent = c.outgoing();
        QVERIFY(sent.size() > 1);
        QVERIFY(sent.size() < 100);
        for (int i = 0; i < sent.size(); ++i)
            QCOMPARE(sent.at(i).toObject().value("cursor").toInteger(), i + 1);
    }

    // A session the relay refused in the heartbeat (another enrollment owns it, or it is behind
    // the stored generation) keeps its events local; the others still upload.
    void sessionsTheHeartbeatRefusedAreNotUploaded() {
        QTemporaryDir dir;
        Context c(dir.filePath("heartbeat-refused.sqlite"));
        QVERIFY(c.journal.upsertSession({"session-refused",
                                         "synthetic-worktree",
                                         "codex",
                                         "thread-refused",
                                         {},
                                         "ready",
                                         1,
                                         0,
                                         1}));
        QString error;
        for (const QString session : {"session-synthetic", "session-refused"})
            QVERIFY2(
                c.journal.appendEvent(session, 1, "user.message", {{"text", session}}, &error) > 0,
                qPrintable(error));
        c.start();
        c.transport.statusForPath.insert("/v2/daemon/heartbeat", 202);
        c.transport.responseForPath.insert(
            "/v2/daemon/heartbeat",
            R"({"ok":true,"accepted":true,"rejectedSessions":[{"sessionId":"session-refused","code":"session_owned_by_other_machine"}]})");
        c.transport.statusForPath.insert("/v2/events:append", 202);
        c.transport.responseForPath.insert("/v2/events:append", R"({"acceptedThrough":[]})");
        Access::heartbeat(*c.client, {QJsonArray{}, QJsonArray{}});
        const auto uploads = [&] {
            QStringList sessions;
            for (int i = 0; i < c.transport.requests.size(); ++i) {
                if (c.transport.requests.at(i).url().path() != "/v2/events:append")
                    continue;
                for (const QJsonValue& event : c.outgoing(i))
                    sessions.append(event.toObject().value("sessionId").toString());
            }
            return sessions;
        };
        QTRY_VERIFY(uploads().contains("session-synthetic"));
        QTRY_VERIFY(Access::idle(*c.client));
        QVERIFY(!uploads().contains("session-refused"));
    }

    void batchTheRelayRefusesAsTooLargeIsHalvedNotRepeated() {
        QTemporaryDir dir;
        Context c(dir.filePath("refused.sqlite"));
        QString error;
        for (int i = 0; i < 10; ++i)
            QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "user.message",
                                           {{"text", QString("message %1").arg(i)}}, &error) > 0,
                     qPrintable(error));
        c.start();
        c.transport.statusForPath.insert("/v2/events:append", 413);
        c.transport.responseForPath.insert("/v2/events:append", R"({"code":"request_too_large"})");
        Access::upload(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QTRY_COMPARE(c.transport.bodies.size(), 4);
        QList<qsizetype> sizes;
        for (int i = 0; i < c.transport.bodies.size(); ++i)
            sizes.append(c.outgoing(i).size());
        QCOMPARE(sizes, (QList<qsizetype>{10, 5, 2, 1}));
        // Each retry starts from the same cursor: nothing was accepted.
        for (int i = 0; i < c.transport.bodies.size(); ++i)
            QCOMPARE(c.outgoing(i).first().toObject().value("cursor").toInteger(), 1);
        QCOMPARE(Access::error(*c.client), QString("request_too_large"));

        // Once the relay accepts again, batches grow back from the smaller size. The relay
        // acknowledges everything; each acknowledgement is clamped to what the batch carried.
        // A batch of one it still refused waits like any other refusal before it is sent again.
        c.transport.statusForPath.insert("/v2/events:append", 202);
        c.transport.responseForPath.insert(
            "/v2/events:append",
            R"({"acceptedThrough":[{"sessionId":"session-synthetic","sessionGeneration":1,"cursor":10}]})");
        Access::expireUploadBackoff(*c.client);
        Access::upload(*c.client);
        QTRY_COMPARE(c.transport.bodies.size(), 8);
        QTRY_VERIFY(Access::idle(*c.client));
        sizes.clear();
        for (int i = 4; i < c.transport.bodies.size(); ++i)
            sizes.append(c.outgoing(i).size());
        QCOMPARE(sizes, (QList<qsizetype>{1, 2, 4, 3}));
        QVERIFY(c.journal.eventsAfter("session-synthetic", 1, 0).isEmpty());
    }

    void refusedUploadWaitsInsteadOfRepeatingOnEveryEvent() {
        QTemporaryDir dir;
        Context c(dir.filePath("stale.sqlite"));
        QString error;
        QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "user.message", {{"text", "first"}},
                                       &error) > 0,
                 qPrintable(error));
        c.start();
        // The relay no longer takes this session, as after a phone deleted it.
        c.transport.statusForPath.insert("/v2/events:append", 400);
        c.transport.responseForPath.insert("/v2/events:append", R"({"code":"stale_generation"})");
        const auto appends = [&c] {
            int count = 0;
            for (const QNetworkRequest& request : c.transport.requests)
                count += request.url().path() == "/v2/events:append";
            return count;
        };
        Access::uploadAll(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(appends(), 1);
        QCOMPARE(Access::error(*c.client), QString("stale_generation"));

        // Every event a running turn journals starts another upload; none reaches the relay.
        for (int i = 0; i < 20; ++i) {
            QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "assistant.delta",
                                           {{"text", QString::number(i)}}, &error) > 0,
                     qPrintable(error));
            Access::uploadAll(*c.client);
        }
        QTest::qWait(50);
        QCOMPARE(appends(), 1);

        // A heartbeat that gets through does not end a refusal's wait.
        c.transport.statusForPath.insert("/v2/daemon/heartbeat", 202);
        c.transport.responseForPath.insert("/v2/daemon/heartbeat",
                                           R"({"ok":true,"accepted":true})");
        Access::heartbeat(*c.client, {QJsonArray{}});
        // The heartbeat starts its uploads, then reads the devices to distribute keys to.
        QTRY_VERIFY(std::any_of(
            c.transport.requests.cbegin(), c.transport.requests.cend(),
            [](const QNetworkRequest& request) { return request.url().path() == "/v2/devices"; }));
        QCOMPARE(appends(), 1);

        // Once the wait is over the session is sent again, and acceptance ends the waiting.
        Access::expireUploadBackoff(*c.client);
        c.transport.statusForPath.insert("/v2/events:append", 202);
        c.transport.responseForPath.insert(
            "/v2/events:append",
            R"({"acceptedThrough":[{"sessionId":"session-synthetic","sessionGeneration":1,"cursor":21}]})");
        Access::uploadAll(*c.client);
        QTRY_COMPARE(appends(), 2);
        QTRY_VERIFY(Access::idle(*c.client));
        QVERIFY(c.journal.eventsAfter("session-synthetic", 1, 0).isEmpty());
        QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "user.message", {{"text", "next"}},
                                       &error) > 0,
                 qPrintable(error));
        Access::uploadAll(*c.client);
        QTRY_COMPARE(appends(), 3);
    }

    void deletionHeartbeatRetriesUntilTheExactOperationIsAcknowledged() {
        QTemporaryDir dir;
        Context c(dir.filePath("deleted.sqlite"));
        QVERIFY(c.journal.deleteSession("session-synthetic", 1));
        c.start();
        c.transport.statusForPath.insert("/v2/daemon/heartbeat", 202);
        c.transport.responseForPath.insert("/v2/daemon/heartbeat", "{}");
        Access::heartbeat(*c.client, {QJsonArray{}});
        QTRY_VERIFY(Access::heartbeatIdle(*c.client));
        const auto request = QJsonDocument::fromJson(c.transport.bodies.first()).object();
        const auto deletion = request.value("deletions").toArray().first().toObject();
        QCOMPARE(deletion.value("sessionId").toString(), "session-synthetic");
        QCOMPARE(c.journal.deletionWork(64, true).size(), 1);
        auto receipt = deletion;
        receipt.insert("operationId", "wrong-operation");
        c.transport.responseForPath.insert(
            "/v2/daemon/heartbeat",
            QJsonDocument(QJsonObject{{"acceptedDeletions", QJsonArray{receipt}}}).toJson());
        Access::heartbeat(*c.client, {QJsonArray{}});
        QTRY_VERIFY(c.transport.bodies.size() >= 2);
        QTRY_VERIFY(Access::heartbeatIdle(*c.client));
        QCOMPARE(c.journal.deletionWork(64, true).size(), 1);
        receipt.insert("operationId", deletion.value("operationId"));
        c.transport.responseForPath.insert(
            "/v2/daemon/heartbeat",
            QJsonDocument(QJsonObject{{"acceptedDeletions", QJsonArray{receipt}}}).toJson());
        Access::heartbeat(*c.client, {QJsonArray{}});
        QTRY_VERIFY(c.journal.deletionWork().isEmpty());
    }
    void unreachableRelayWaitsUntilAHeartbeatGetsThrough() {
        QTemporaryDir dir;
        Context c(dir.filePath("unreachable.sqlite"));
        QString error;
        QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "user.message", {{"text", "first"}},
                                       &error) > 0,
                 qPrintable(error));
        c.start();
        c.transport.statusForPath.insert("/v2/events:append", 502);
        c.transport.responseForPath.insert("/v2/events:append", "<html>Bad Gateway</html>");
        const auto appends = [&c] {
            int count = 0;
            for (const QNetworkRequest& request : c.transport.requests)
                count += request.url().path() == "/v2/events:append";
            return count;
        };
        Access::uploadAll(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        for (int i = 0; i < 5; ++i)
            Access::uploadAll(*c.client);
        QTest::qWait(50);
        QCOMPARE(appends(), 1);

        // The relay answers a heartbeat again: the upload goes out without waiting longer.
        c.transport.statusForPath.insert("/v2/daemon/heartbeat", 202);
        c.transport.responseForPath.insert("/v2/daemon/heartbeat",
                                           R"({"ok":true,"accepted":true})");
        c.transport.statusForPath.insert("/v2/events:append", 202);
        c.transport.responseForPath.insert(
            "/v2/events:append",
            R"({"acceptedThrough":[{"sessionId":"session-synthetic","sessionGeneration":1,"cursor":1}]})");
        Access::heartbeat(*c.client, {QJsonArray{}});
        QTRY_COMPARE(appends(), 2);
        QTRY_VERIFY(Access::idle(*c.client));
        QVERIFY(c.journal.eventsAfter("session-synthetic", 1, 0).isEmpty());
    }

    void tooLargeFromAProxyIsHalvedEvenWithoutJson() {
        QTemporaryDir dir;
        Context c(dir.filePath("proxy-refused.sqlite"));
        QString error;
        for (int i = 0; i < 10; ++i)
            QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "user.message",
                                           {{"text", QString("message %1").arg(i)}}, &error) > 0,
                     qPrintable(error));
        c.start();
        // A tunnel or proxy refuses the body before the relay sees it, with its own HTML page.
        c.transport.statusForPath.insert("/v2/events:append", 413);
        c.transport.responseForPath.insert(
            "/v2/events:append", "<html><body>413 Request Entity Too Large</body></html>");
        Access::upload(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QTRY_COMPARE(c.transport.bodies.size(), 4);
        QList<qsizetype> sizes;
        for (int i = 0; i < c.transport.bodies.size(); ++i)
            sizes.append(c.outgoing(i).size());
        QCOMPARE(sizes, (QList<qsizetype>{10, 5, 2, 1}));
        QCOMPARE(Access::error(*c.client), QString("request_too_large"));
    }

    void restoreWindowUploadsBoundedEncryptedFragmentsBeforeTerminalAck() {
        QTemporaryDir dir;
        Context c(dir.filePath("restore.sqlite"));
        c.item("completed", "agentMessage");
        c.start();
        const QString requestId = "restore-request-0001";
        const QString responsePath =
            QStringLiteral("/v2/restore-requests/%1/responses:append").arg(requestId);
        const QString ackPath = QStringLiteral("/v2/commands/%1/ack").arg(requestId);
        c.transport.statusForPath.insert(responsePath, 202);
        c.transport.responseForPath.insert(responsePath, R"({"acceptedThrough":1})");
        c.transport.statusForPath.insert(ackPath, 200);
        c.transport.responseForPath.insert(ackPath, R"({"status":"completed"})");
        const qint64 snapshot = c.journal.sessions().first().lastCursor;
        const QString deadline = QDateTime::currentDateTimeUtc().addSecs(300).toString(Qt::ISODate);
        const QJsonObject command{{"id", requestId},
                                  {"ownerSubjectId", "owner-synthetic"},
                                  {"workspaceId", "workspace-synthetic"},
                                  {"machineId", "machine-synthetic"},
                                  {"sessionId", "session-synthetic"},
                                  {"sessionGeneration", 1},
                                  {"credentialEpoch", 1},
                                  {"snapshotCursor", snapshot},
                                  {"requestedByteCap", 512 * 1024},
                                  {"deadlineAt", deadline},
                                  {"attempt", 2},
                                  {"claimToken", "claim-token-synthetic"}};
        const QJsonObject direct = c.host.handleCommand(
            {{"protocol_version", 0},
             {"message_id", requestId},
             {"message_type", "session.transcript_window"},
             {"session_id", "session-synthetic"},
             {"session_generation", 1},
             {"sent_at", protocol::timestamp()},
             {"payload",
              QJsonObject{{"before_cursor", 0}, {"turn_limit", 3}, {"event_limit", 128}}}},
            "cloud-control-v2");
        QVERIFY2(direct.value("ok").toBool(),
                 qPrintable(QString::fromUtf8(QJsonDocument(direct).toJson())));
        QTRY_VERIFY(Access::idle(*c.client));
        c.transport.requests.clear();
        c.transport.bodies.clear();
        Access::restore(*c.client, command,
                        {{"before_cursor", 0}, {"turn_limit", 3}, {"event_limit", 128}});
        QTRY_VERIFY(
            std::any_of(c.transport.requests.cbegin(), c.transport.requests.cend(),
                        [&](const auto& request) { return request.url().path() == ackPath; }));
        const auto responseIndex =
            std::distance(c.transport.requests.cbegin(),
                          std::find_if(c.transport.requests.cbegin(), c.transport.requests.cend(),
                                       [&](const auto& request) {
                                           return request.url().path() == responsePath;
                                       }));
        const auto ackIndex = std::distance(
            c.transport.requests.cbegin(),
            std::find_if(c.transport.requests.cbegin(), c.transport.requests.cend(),
                         [&](const auto& request) { return request.url().path() == ackPath; }));
        QVERIFY(responseIndex < ackIndex);
        const QJsonObject upload =
            QJsonDocument::fromJson(c.transport.bodies.at(responseIndex)).object();
        QCOMPARE(upload.value("attempt").toInt(), 2);
        QCOMPARE(upload.value("claimToken").toString(), "claim-token-synthetic");
        const QJsonArray responses = upload.value("responses").toArray();
        QVERIFY(!responses.isEmpty());
        qint64 expectedOffset = 0;
        QByteArray restored;
        for (int index = 0; index < responses.size(); ++index) {
            const QJsonObject response = responses.at(index).toObject();
            QCOMPARE(response.value("responseCursor").toInt(), index + 1);
            const bool terminal = index + 1 == responses.size();
            QCOMPARE(response.value("terminal").toBool(), terminal);
            const QString digest = response.value("responseDigest").toString();
            const QJsonObject content = response.value("content").toObject();
            const QByteArray publicKey = CloudCrypto::fromBase64Url(
                Access::crypto(*c.client).publicKeys().value("signingPublicKey").toString());
            QString error;
            const QByteArray plaintext = Access::crypto(*c.client).decrypt(
                content,
                Access::restoreAad(*c.client, command, index + 1, terminal, digest, content),
                publicKey, "session-synthetic", 1, &error);
            QVERIFY2(!plaintext.isEmpty(), qPrintable(error));
            const QJsonObject fragment = QJsonDocument::fromJson(plaintext).object();
            QCOMPARE(fragment.value("offset").toInteger(), expectedOffset);
            const QByteArray bytes = CloudCrypto::fromBase64Url(fragment.value("data").toString());
            restored.append(bytes);
            expectedOffset += bytes.size();
            if (terminal)
                QCOMPARE(
                    QString::fromLatin1(
                        QCryptographicHash::hash(restored, QCryptographicHash::Sha256).toHex()),
                    digest);
        }
        const QJsonObject page = QJsonDocument::fromJson(restored).object();
        QVERIFY(page.value("ok").toBool());
        QCOMPARE(page.value("snapshot_cursor").toInteger(), snapshot);

        const QByteArray firstUpload = c.transport.bodies.at(responseIndex);
        c.transport.requests.clear();
        c.transport.bodies.clear();
        QJsonObject retry = command;
        retry.insert("attempt", 3);
        retry.insert("claimToken", "claim-token-retry");
        Access::restore(*c.client, retry,
                        {{"before_cursor", 0}, {"turn_limit", 3}, {"event_limit", 128}});
        QTRY_VERIFY(
            std::any_of(c.transport.requests.cbegin(), c.transport.requests.cend(),
                        [&](const auto& request) { return request.url().path() == ackPath; }));
        const auto retryResponseIndex =
            std::distance(c.transport.requests.cbegin(),
                          std::find_if(c.transport.requests.cbegin(), c.transport.requests.cend(),
                                       [&](const auto& request) {
                                           return request.url().path() == responsePath;
                                       }));
        const QJsonObject retryUpload =
            QJsonDocument::fromJson(c.transport.bodies.at(retryResponseIndex)).object();
        QCOMPARE(retryUpload.value("attempt").toInt(), 3);
        QCOMPARE(retryUpload.value("claimToken").toString(), "claim-token-retry");
        QCOMPARE(retryUpload.value("responses"),
                 QJsonDocument::fromJson(firstUpload).object().value("responses"));

        c.transport.requests.clear();
        c.transport.bodies.clear();
        Access::machineKeyVersion(*c.client, 2);
        QJsonObject rotated = command;
        rotated.insert("attempt", 4);
        rotated.insert("claimToken", "claim-token-after-key-rotation");
        Access::restore(*c.client, rotated,
                        {{"before_cursor", 0}, {"turn_limit", 3}, {"event_limit", 128}});
        QTRY_VERIFY(
            std::any_of(c.transport.requests.cbegin(), c.transport.requests.cend(),
                        [&](const auto& request) { return request.url().path() == ackPath; }));
        const auto rotatedResponseIndex =
            std::distance(c.transport.requests.cbegin(),
                          std::find_if(c.transport.requests.cbegin(), c.transport.requests.cend(),
                                       [&](const auto& request) {
                                           return request.url().path() == responsePath;
                                       }));
        const QJsonArray rotatedResponses =
            QJsonDocument::fromJson(c.transport.bodies.at(rotatedResponseIndex))
                .object()
                .value("responses")
                .toArray();
        QVERIFY(rotatedResponses != retryUpload.value("responses").toArray());
        QCOMPARE(rotatedResponses.first()
                     .toObject()
                     .value("content")
                     .toObject()
                     .value("senderKeyVersion")
                     .toInt(),
                 2);
    }

    void cachedEnvelopesFromPolicyVersionOneAreReprojected() {
        QCOMPARE(LocalJournal::currentCloudProjectionPolicyVersion, 2);
        QTemporaryDir dir;
        Context c(dir.filePath("policy-v1.sqlite"));
        c.item("completed");
        c.start();
        // A correctly projected envelope cached under policy 1 is still rebuilt under policy 2.
        const auto raw = c.journal.eventsAfter("session-synthetic", 1, 0).first().toObject();
        const QJsonObject shell{{"keyId", "session-synthetic:1:1"},
                                {"keyEpoch", 1},
                                {"senderDeviceId", "machine-synthetic"},
                                {"senderKeyVersion", 1}};
        const QJsonObject content = Access::crypto(*c.client).encrypt(
            QJsonDocument(projectEventPayloadForCloud(raw.value("event_type").toString(),
                                                      raw.value("payload").toObject()))
                .toJson(QJsonDocument::Compact),
            Access::aad(*c.client, raw, shell), "session-synthetic", 1, 1, "machine-synthetic", 1);
        const QJsonObject cached{{"sessionId", "session-synthetic"},
                                 {"sessionGeneration", 1},
                                 {"cursor", 1},
                                 {"turnSequence", raw.value("turn_sequence")},
                                 {"eventType", raw.value("event_type")},
                                 {"occurredAt", raw.value("sent_at")},
                                 {"content", content}};
        QVERIFY(c.journal.storeCloudEvent("session-synthetic", 1, 1, cached, 1));
        Access::upload(*c.client);
        QVERIFY(c.transport.bodies.isEmpty());
        QCOMPARE(Access::error(*c.client),
                 QStringLiteral("cloud_event_cache_requires_reconciliation"));
        Access::upload(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(c.outgoing().size(), 1);
        QCOMPARE(c.journal.cloudEventProjectionPolicyVersion("session-synthetic", 1, 1), 2);
    }

    void restoreWindowPagesAreCloudProjected() {
        QTemporaryDir dir;
        Context c(dir.filePath("restore-projected.sqlite"));
        c.item("completed"); // command item with stdout/stderr/aggregatedOutput markers
        const QString base64 = QString::fromLatin1(
            QByteArray("\x89PNG\r\n\x1a\n" + QByteArray(64, 'P') + marker.toUtf8()).toBase64());
        const QJsonObject mcp{
            {"id", "mcp-call"},
            {"type", "mcpToolCall"},
            {"server", "ketplus_browser"},
            {"tool", "browser_act"},
            {"status", "completed"},
            {"arguments", QJsonObject{{"url", "https://staging.example.com/login?token=" + marker},
                                      {"text", marker + "_TYPED_PASSWORD"},
                                      {"selector", "#password"}}},
            {"result",
             QJsonObject{
                 {"structuredContent", QJsonObject{{"secret", marker + "_STRUCTURED"}}},
                 {"content",
                  QJsonArray{
                      QJsonObject{{"type", "text"},
                                  {"text", QString(1024, 'x') + marker + "_LONG_TEXT"}},
                      QJsonObject{{"type", "image"}, {"data", base64}, {"mimeType", "image/png"}},
                      QJsonObject{{"type", "resource"},
                                  {"resource",
                                   QJsonObject{{"mimeType", "image/png"}, {"blob", base64}}}}}}}},
            {"error", QJsonObject{{"message", QString(512, 'e') + marker + "_ERROR"}}}};
        QString journalError;
        QVERIFY2(c.journal.appendEvent(
                     "session-synthetic", 1, "item.completed",
                     {{"item_id", "mcp-call"}, {"item_type", "mcpToolCall"}, {"item", mcp}},
                     &journalError) > 0,
                 qPrintable(journalError));
        c.start();
        QByteArray bytes;
        const QJsonObject page = restorePage(c, "restore-projected-0001", &bytes);
        QVERIFY(page.value("ok").toBool());
        QVERIFY2(!bytes.contains(marker.toUtf8()), bytes.constData());
        QVERIFY(!bytes.contains(base64.left(24).toLatin1()));
        QVERIFY(!bytes.contains("structuredContent"));
        bool sawCommand = false;
        bool sawMcp = false;
        for (const QJsonValue& value : page.value("events").toArray()) {
            const QJsonObject item =
                value.toObject().value("payload").toObject().value("item").toObject();
            if (item.value("type") == "commandExecution" && item.value("status") == "completed") {
                sawCommand = true;
                QCOMPARE(item.value("command").toString(), command());
                QCOMPARE(item.value("exitCode").toInt(), 0);
            }
            if (item.value("type") == "mcpToolCall") {
                sawMcp = true;
                QCOMPARE(item.value("tool").toString(), "browser_act");
                QVERIFY(item.value("arguments_summary")
                            .toString()
                            .contains("url=https://staging.example.com/login"));
                QVERIFY(item.value("arguments_summary").toString().contains("selector=#password"));
                QVERIFY(!item.contains("arguments"));
            }
        }
        QVERIFY(sawCommand);
        QVERIFY(sawMcp);
        // The desktop-local window of the same journal is unprojected.
        const auto local = c.journal.transcriptWindow("session-synthetic", 1);
        QVERIFY(QJsonDocument(local.events).toJson().contains(marker.toUtf8()));
    }

    void largeMcpItemRestoresInlineProjectedWithoutRawChunks() {
        QTemporaryDir dir;
        Context c(dir.filePath("restore-large-mcp.sqlite"));
        const QJsonObject item{
            {"id", "large-mcp"},
            {"type", "mcpToolCall"},
            {"server", "other_server"},
            {"tool", "dump"},
            {"status", "completed"},
            {"result",
             QJsonObject{{"content", QJsonArray{QJsonObject{
                                         {"type", "text"},
                                         {"text", QString(3 * 1024 * 1024, 'm') + marker}}}}}}};
        QString journalError;
        QVERIFY2(c.journal.appendEvent(
                     "session-synthetic", 1, "item.completed",
                     {{"item_id", "large-mcp"}, {"item_type", "mcpToolCall"}, {"item", item}},
                     &journalError) > 0,
                 qPrintable(journalError));
        const QJsonObject manifest = c.journal.transcriptWindow("session-synthetic", 1)
                                         .events.last()
                                         .toObject()
                                         .value("payload")
                                         .toObject()
                                         .value("large_event")
                                         .toObject();
        QVERIFY(!manifest.isEmpty());
        c.start();
        QByteArray bytes;
        const QJsonObject page = restorePage(c, "restore-large-mcp-0001", &bytes);
        QVERIFY(page.value("ok").toBool());
        QVERIFY(!bytes.contains(QByteArray(4096, 'm')));
        QVERIFY(!bytes.contains(marker.toUtf8()));
        QVERIFY(!bytes.contains(manifest.value("transfer_id").toString().toUtf8()));
        const QJsonArray events = page.value("events").toArray();
        QVERIFY(!events.isEmpty());
        const QJsonObject last = events.last().toObject().value("payload").toObject();
        QVERIFY(!last.contains("large_event"));
        QCOMPARE(last.value("item").toObject().value("tool").toString(), "dump");
        for (const QJsonValue& value : events)
            QVERIFY(value.toObject().value("event_type").toString() != "transcript.large_chunk");

        // A continuation for the raw bytes of that item is refused before any response upload.
        const QString requestId = "restore-large-mcp-0002";
        const QString responsePath =
            QStringLiteral("/v2/restore-requests/%1/responses:append").arg(requestId);
        const QString ackPath = QStringLiteral("/v2/commands/%1/ack").arg(requestId);
        c.transport.statusForPath.insert(ackPath, 200);
        c.transport.responseForPath.insert(ackPath, R"({"status":"completed"})");
        c.transport.requests.clear();
        QJsonObject continuation = restoreCommand(c, requestId);
        continuation.insert("continuationOfRequestId", "restore-large-mcp-0001");
        continuation.insert("continuationSequence", 1);
        continuation.insert("continuationDigest", QString(64, 'a'));
        Access::restore(
            *c.client, continuation,
            {{"large_event", QJsonObject{{"version", 1},
                                         {"transfer_id", manifest.value("transfer_id")},
                                         {"item_id", manifest.value("item_id")},
                                         {"revision", manifest.value("revision")},
                                         {"digest_sha256", manifest.value("digest_sha256")},
                                         {"total_bytes", manifest.value("total_bytes")},
                                         {"offset", 0}}}});
        QTRY_VERIFY(
            std::any_of(c.transport.requests.cbegin(), c.transport.requests.cend(),
                        [&](const auto& request) { return request.url().path() == ackPath; }));
        QVERIFY(std::none_of(
            c.transport.requests.cbegin(), c.transport.requests.cend(),
            [&](const auto& request) { return request.url().path() == responsePath; }));
    }

    void restoreWindowRejectsAnotherWorkspaceBeforeReadingTheJournal() {
        QTemporaryDir dir;
        Context c(dir.filePath("restore-workspace.sqlite"));
        c.item("completed", "agentMessage");
        c.start();
        const QString requestId = QStringLiteral("restore-wrong-workspace-0001");
        const QString responsePath =
            QStringLiteral("/v2/restore-requests/%1/responses:append").arg(requestId);
        const QString ackPath = QStringLiteral("/v2/commands/%1/ack").arg(requestId);
        c.transport.statusForPath.insert(ackPath, 200);
        c.transport.responseForPath.insert(ackPath, R"({"status":"rejected"})");
        const QJsonObject request{
            {"id", requestId},
            {"ownerSubjectId", "owner-synthetic"},
            {"workspaceId", "workspace-other"},
            {"machineId", "machine-synthetic"},
            {"sessionId", "session-synthetic"},
            {"sessionGeneration", 1},
            {"credentialEpoch", 1},
            {"snapshotCursor", c.journal.sessions().first().lastCursor},
            {"requestedByteCap", 512 * 1024},
            {"deadlineAt", QDateTime::currentDateTimeUtc().addSecs(300).toString(Qt::ISODate)},
            {"attempt", 1},
            {"claimToken", "claim-wrong-workspace"}};

        Access::restore(*c.client, request,
                        {{"before_cursor", 0}, {"turn_limit", 3}, {"event_limit", 128}});
        QTRY_VERIFY(std::any_of(c.transport.requests.cbegin(), c.transport.requests.cend(),
                                [&](const auto& reply) { return reply.url().path() == ackPath; }));
        QVERIFY(
            std::none_of(c.transport.requests.cbegin(), c.transport.requests.cend(),
                         [&](const auto& reply) { return reply.url().path() == responsePath; }));
        const auto ack =
            std::find_if(c.transport.requests.cbegin(), c.transport.requests.cend(),
                         [&](const auto& reply) { return reply.url().path() == ackPath; });
        const auto bodyIndex = std::distance(c.transport.requests.cbegin(), ack);
        const QJsonObject body = QJsonDocument::fromJson(c.transport.bodies.at(bodyIndex)).object();
        QCOMPARE(body.value("resultCode").toString(), QStringLiteral("transcript_unavailable"));
    }

    void restoreOlderWindowBindsProjectionAndServesATrailingHead() {
        QTemporaryDir dir;
        Context c(dir.filePath("restore-projection.sqlite"));
        for (int index = 0; index < 6; ++index) {
            QString error;
            QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "user.message",
                                           {{"text", QStringLiteral("prompt-%1").arg(index)}},
                                           &error) > 0,
                     qPrintable(error));
            c.item("completed", "agentMessage");
        }
        c.start();
        const qint64 snapshot = c.journal.sessions().first().lastCursor;
        const QString deadline = QDateTime::currentDateTimeUtc().addSecs(300).toString(Qt::ISODate);
        const auto command = [&](const QString& id, const qint64 head) {
            return QJsonObject{{"id", id},
                               {"ownerSubjectId", "owner-synthetic"},
                               {"workspaceId", "workspace-synthetic"},
                               {"machineId", "machine-synthetic"},
                               {"sessionId", "session-synthetic"},
                               {"sessionGeneration", 1},
                               {"credentialEpoch", 1},
                               {"snapshotCursor", head},
                               {"requestedByteCap", 512 * 1024},
                               {"deadlineAt", deadline},
                               {"attempt", 1},
                               {"claimToken", "claim-projection"}};
        };
        const auto prepareTransport = [&](const QString& id) {
            const QString responsePath =
                QStringLiteral("/v2/restore-requests/%1/responses:append").arg(id);
            const QString ackPath = QStringLiteral("/v2/commands/%1/ack").arg(id);
            c.transport.statusForPath.insert(responsePath, 202);
            c.transport.responseForPath.insert(responsePath, R"({"acceptedThrough":16})");
            c.transport.statusForPath.insert(ackPath, 200);
            c.transport.responseForPath.insert(ackPath, R"({"status":"completed"})");
            c.transport.requests.clear();
            c.transport.bodies.clear();
        };

        const QString requestId = QStringLiteral("restore-projection-0001");
        prepareTransport(requestId);
        const QJsonObject validCommand = command(requestId, snapshot);
        Access::restore(*c.client, validCommand,
                        {{"before_cursor", snapshot}, {"turn_limit", 3}, {"event_limit", 128}});
        const QString responsePath =
            QStringLiteral("/v2/restore-requests/%1/responses:append").arg(requestId);
        QTRY_VERIFY(
            std::any_of(c.transport.requests.cbegin(), c.transport.requests.cend(),
                        [&](const auto& request) { return request.url().path() == responsePath; }));
        const auto response =
            std::find_if(c.transport.requests.cbegin(), c.transport.requests.cend(),
                         [&](const auto& request) { return request.url().path() == responsePath; });
        const QJsonObject upload =
            QJsonDocument::fromJson(
                c.transport.bodies.at(std::distance(c.transport.requests.cbegin(), response)))
                .object();
        const QJsonObject page = decryptRestoreUpload(c, validCommand, upload);
        QVERIFY(page.value("ok").toBool());
        QCOMPARE(page.value("snapshot_cursor").toInteger(), snapshot);
        QVERIFY(page.value("projection_revision").toInteger(-1) >= 0);
        QVERIFY(!page.value("events").toArray().isEmpty());

        QString error;
        QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "assistant.delta",
                                       {{"item_id", "race"}, {"delta", "partial"}}, &error) > 0,
                 qPrintable(error));
        const qint64 beforeCompaction = c.journal.sessions().first().lastCursor;
        QVERIFY2(c.journal.appendEvent(
                     "session-synthetic", 1, "item.completed",
                     {{"item",
                       QJsonObject{{"id", "race"}, {"type", "agentMessage"}, {"text", "final"}}}},
                     &error) > beforeCompaction,
                 qPrintable(error));

        const QString racedId = QStringLiteral("restore-projection-0002");
        prepareTransport(racedId);
        const QJsonObject racedCommand = command(racedId, beforeCompaction);
        Access::restore(
            *c.client, racedCommand,
            {{"before_cursor", beforeCompaction}, {"turn_limit", 3}, {"event_limit", 128}});
        // The relay's head trails a running turn: the window is served as of that head and
        // holds nothing after it.
        const QString racedPath =
            QStringLiteral("/v2/restore-requests/%1/responses:append").arg(racedId);
        QTRY_VERIFY(
            std::any_of(c.transport.requests.cbegin(), c.transport.requests.cend(),
                        [&](const auto& request) { return request.url().path() == racedPath; }));
        const auto racedResponse =
            std::find_if(c.transport.requests.cbegin(), c.transport.requests.cend(),
                         [&](const auto& request) { return request.url().path() == racedPath; });
        const QJsonObject racedPage = decryptRestoreUpload(
            c, racedCommand,
            QJsonDocument::fromJson(
                c.transport.bodies.at(std::distance(c.transport.requests.cbegin(), racedResponse)))
                .object());
        QVERIFY(racedPage.value("ok").toBool());
        QCOMPARE(racedPage.value("snapshot_cursor").toInteger(), beforeCompaction);
        for (const QJsonValue& event : racedPage.value("events").toArray())
            QVERIFY(event.toObject().value("cursor").toInteger() <= beforeCompaction);
    }

    void restoreContinuationAadUsesCanonicalEmptyRootTupleAndFencesChildren() {
        QTemporaryDir dir;
        Context c(dir.filePath("continuation-aad.sqlite"));
        c.start();
        const QJsonObject content{{"keyId", "session-synthetic:1:1"},
                                  {"keyEpoch", 1},
                                  {"senderDeviceId", "mobile-synthetic"},
                                  {"senderKeyVersion", 1}};
        QJsonObject root{{"id", "restore-root-0001"},
                         {"ownerSubjectId", "owner-synthetic"},
                         {"workspaceId", "workspace-synthetic"},
                         {"machineId", "machine-synthetic"},
                         {"sessionId", "session-synthetic"},
                         {"sessionGeneration", 1},
                         {"idempotencyKey", "restore-root-idempotency-0001"},
                         {"capability", "transcript.restore_window"},
                         {"kind", "transcript.restore_window"},
                         {"content", content},
                         {"credentialEpoch", 1},
                         {"snapshotCursor", 88},
                         {"requestedByteCap", 512 * 1024},
                         {"deadlineAt", "2026-09-10T10:10:00.000Z"}};
        const QByteArray rootAad = Access::commandAad(*c.client, root);
        QVERIFY(rootAad.endsWith(
            ",1,\"owner-synthetic\",88,524288,\"\",0,\"\",\"2026-09-10T10:10:00.000Z\"]"));
        QJsonObject explicitRoot = root;
        explicitRoot.insert("continuationOfRequestId", QJsonValue::Null);
        explicitRoot.insert("continuationSequence", 0);
        explicitRoot.insert("continuationDigest", QJsonValue::Null);
        QCOMPARE(Access::commandAad(*c.client, explicitRoot), rootAad);

        QJsonObject child = root;
        child.insert("continuationOfRequestId", "restore-root-0001");
        child.insert("continuationSequence", 1);
        child.insert("continuationDigest", QString(64, QLatin1Char('a')));
        const QByteArray childAad = Access::commandAad(*c.client, child);
        QVERIFY(childAad != rootAad);

        QString error;
        const QJsonObject encrypted = Access::crypto(*c.client).encrypt(
            QByteArrayLiteral("{\"large_event\":{}}"), childAad, "session-synthetic", 1, 1,
            "machine-synthetic", 1, &error);
        QVERIFY2(!encrypted.isEmpty(), qPrintable(error));
        QJsonObject tampered = child;
        tampered.insert("continuationSequence", 2);
        const QByteArray publicKey = CloudCrypto::fromBase64Url(
            Access::crypto(*c.client).publicKeys().value("signingPublicKey").toString());
        QVERIFY(Access::crypto(*c.client)
                    .decrypt(encrypted, Access::commandAad(*c.client, tampered), publicKey,
                             "session-synthetic", 1, &error)
                    .isEmpty());
    }

    void eightMiBLargeItemRestoresThroughBoundedChainedContinuationRequests() {
        QTemporaryDir dir;
        Context c(dir.filePath("continuation-8mib.sqlite"));
        const QString text(8 * 1024 * 1024, QLatin1Char('L'));
        const QJsonObject item{{"id", "large-agent"}, {"type", "agentMessage"}, {"text", text}};
        const QJsonObject originalPayload{
            {"item_id", "large-agent"}, {"item_type", "agentMessage"}, {"item", item}};
        QString journalError;
        QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "item.completed", originalPayload,
                                       &journalError) > 0,
                 qPrintable(journalError));
        c.start();
        const qint64 snapshot = c.journal.sessions().first().lastCursor;
        const QString deadline = QDateTime::currentDateTimeUtc().addSecs(300).toString(Qt::ISODate);
        const auto commandFor = [&](const int sequence, const QString& parent,
                                    const QString& parentDigest) {
            const QString id =
                QStringLiteral("restore-continuation-%1").arg(sequence, 4, 10, QLatin1Char('0'));
            return QJsonObject{{"id", id},
                               {"ownerSubjectId", "owner-synthetic"},
                               {"workspaceId", "workspace-synthetic"},
                               {"machineId", "machine-synthetic"},
                               {"sessionId", "session-synthetic"},
                               {"sessionGeneration", 1},
                               {"credentialEpoch", 1},
                               {"snapshotCursor", snapshot},
                               {"requestedByteCap", 512 * 1024},
                               {"continuationOfRequestId", parent},
                               {"continuationSequence", sequence},
                               {"continuationDigest", parentDigest},
                               {"deadlineAt", deadline},
                               {"attempt", 1},
                               {"claimToken", QStringLiteral("claim-%1").arg(sequence)}};
        };
        const auto execute = [&](const QJsonObject& command, const QJsonObject& payload) {
            const QString responsePath = QStringLiteral("/v2/restore-requests/%1/responses:append")
                                             .arg(command.value("id").toString());
            const QString ackPath =
                QStringLiteral("/v2/commands/%1/ack").arg(command.value("id").toString());
            c.transport.statusForPath.insert(responsePath, 202);
            c.transport.responseForPath.insert(responsePath, R"({"acceptedThrough":128})");
            c.transport.statusForPath.insert(ackPath, 200);
            c.transport.responseForPath.insert(ackPath, R"({"status":"completed"})");
            c.transport.requests.clear();
            c.transport.bodies.clear();
            Access::restore(*c.client, command, payload);
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 5000 &&
                   std::none_of(
                       c.transport.requests.cbegin(), c.transport.requests.cend(),
                       [&](const auto& request) { return request.url().path() == ackPath; })) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            }
            const auto responseIndex =
                std::distance(c.transport.requests.cbegin(),
                              std::find_if(c.transport.requests.cbegin(),
                                           c.transport.requests.cend(), [&](const auto& request) {
                                               return request.url().path() == responsePath;
                                           }));
            if (responseIndex >= c.transport.bodies.size()) {
                return QJsonObject{};
            }
            return QJsonDocument::fromJson(c.transport.bodies.at(responseIndex)).object();
        };

        QJsonObject rootCommand = commandFor(0, {}, {});
        const QJsonObject rootUpload =
            execute(rootCommand, {{"before_cursor", 0}, {"turn_limit", 3}, {"event_limit", 128}});
        QVERIFY(!rootUpload.isEmpty());
        QByteArray rootBytes;
        const QJsonObject rootPage = decryptRestoreUpload(c, rootCommand, rootUpload, &rootBytes);
        QVERIFY(rootPage.value("ok").toBool());
        const QJsonArray rootEvents = rootPage.value("events").toArray();
        QVERIFY(!rootEvents.isEmpty());
        const QJsonObject manifest = rootEvents.last()
                                         .toObject()
                                         .value("payload")
                                         .toObject()
                                         .value("large_event")
                                         .toObject();
        QVERIFY(!manifest.isEmpty());
        QCOMPARE(manifest.value("revision").toInteger(), snapshot);

        QByteArray restored;
        qint64 offset = 0;
        QString parent = rootCommand.value("id").toString();
        QString parentDigest = rootUpload.value("responses")
                                   .toArray()
                                   .last()
                                   .toObject()
                                   .value("responseDigest")
                                   .toString();
        int sequence = 1;
        while (offset < manifest.value("total_bytes").toInteger()) {
            QJsonObject command = commandFor(sequence, parent, parentDigest);
            if (sequence == 1) {
                command.insert("requestedByteCap", 4096);
            }
            const QJsonObject selector{{"version", 1},
                                       {"transfer_id", manifest.value("transfer_id")},
                                       {"item_id", manifest.value("item_id")},
                                       {"revision", manifest.value("revision")},
                                       {"digest_sha256", manifest.value("digest_sha256")},
                                       {"total_bytes", manifest.value("total_bytes")},
                                       {"offset", offset}};
            const QJsonObject upload = execute(command, {{"large_event", selector}});
            QVERIFY(!upload.isEmpty());
            const QJsonArray responses = upload.value("responses").toArray();
            QVERIFY(!responses.isEmpty());
            QVERIFY(responses.size() <= 128);
            qint64 encryptedBytes = 0;
            for (const QJsonValue& value : responses) {
                encryptedBytes +=
                    CloudCrypto::fromBase64Url(
                        value.toObject().value("content").toObject().value("ciphertext").toString())
                        .size();
            }
            QVERIFY(encryptedBytes <= command.value("requestedByteCap").toInteger());
            if (sequence == 1) {
                QJsonObject retry = command;
                retry.insert("attempt", 2);
                retry.insert("claimToken", "claim-retry-same-key");
                const QJsonObject retryUpload = execute(retry, {{"large_event", selector}});
                QCOMPARE(retryUpload.value("responses"), upload.value("responses"));

                Access::machineKeyVersion(*c.client, 2);
                QJsonObject rotated = command;
                rotated.insert("attempt", 3);
                rotated.insert("claimToken", "claim-retry-rotated-key");
                const QJsonObject rotatedUpload = execute(rotated, {{"large_event", selector}});
                QVERIFY(rotatedUpload.value("responses") != upload.value("responses"));
                QCOMPARE(rotatedUpload.value("responses")
                             .toArray()
                             .first()
                             .toObject()
                             .value("content")
                             .toObject()
                             .value("senderKeyVersion")
                             .toInt(),
                         2);
                Access::machineKeyVersion(*c.client, 1);
            }
            const QJsonObject fragment = decryptRestoreUpload(c, command, upload);
            QCOMPARE(fragment.value("large_event_fragment_version").toInt(), 1);
            QCOMPARE(fragment.value("offset").toInteger(), offset);
            QCOMPARE(fragment.value("transfer_id"), manifest.value("transfer_id"));
            QCOMPARE(fragment.value("item_id"), manifest.value("item_id"));
            QCOMPARE(fragment.value("revision"), manifest.value("revision"));
            QCOMPARE(fragment.value("digest_sha256"), manifest.value("digest_sha256"));
            QCOMPARE(fragment.value("total_bytes"), manifest.value("total_bytes"));
            const QByteArray data = CloudCrypto::fromBase64Url(fragment.value("data").toString());
            QVERIFY(!data.isEmpty());
            restored.append(data);
            offset = fragment.value("next_offset").toInteger(-1);
            QCOMPARE(fragment.value("complete").toBool(),
                     offset == manifest.value("total_bytes").toInteger());
            parent = command.value("id").toString();
            parentDigest = responses.last().toObject().value("responseDigest").toString();
            ++sequence;
            QVERIFY(sequence < 4096);
        }
        QCOMPARE(restored.size(), manifest.value("total_bytes").toInteger());
        QCOMPARE(QString::fromLatin1(
                     QCryptographicHash::hash(restored, QCryptographicHash::Sha256).toHex()),
                 manifest.value("digest_sha256").toString());
        QCOMPARE(restored, QJsonDocument(originalPayload).toJson(QJsonDocument::Compact));
        QVERIFY(sequence > 2);
    }

    void restoreContinuationRejectsMixedShapesAndSelectorConflicts() {
        QTemporaryDir dir;
        Context c(dir.filePath("continuation-negative.sqlite"));
        const QJsonObject item{{"id", "large-negative"},
                               {"type", "agentMessage"},
                               {"text", QString(1024 * 1024, QLatin1Char('N'))}};
        QString journalError;
        QVERIFY2(c.journal.appendEvent(
                     "session-synthetic", 1, "item.completed",
                     {{"item_id", "large-negative"}, {"item_type", "agentMessage"}, {"item", item}},
                     &journalError) > 0,
                 qPrintable(journalError));
        c.start();
        const qint64 snapshot = c.journal.sessions().first().lastCursor;
        const QJsonObject manifest = c.journal.transcriptWindow("session-synthetic", 1)
                                         .events.last()
                                         .toObject()
                                         .value("payload")
                                         .toObject()
                                         .value("large_event")
                                         .toObject();
        QVERIFY(!manifest.isEmpty());
        const QString deadline = QDateTime::currentDateTimeUtc().addSecs(300).toString(Qt::ISODate);
        QJsonObject command{{"id", "restore-negative-0000"},
                            {"ownerSubjectId", "owner-synthetic"},
                            {"workspaceId", "workspace-synthetic"},
                            {"machineId", "machine-synthetic"},
                            {"sessionId", "session-synthetic"},
                            {"sessionGeneration", 1},
                            {"credentialEpoch", 1},
                            {"snapshotCursor", snapshot},
                            {"requestedByteCap", 512 * 1024},
                            {"continuationOfRequestId", "restore-parent-0000"},
                            {"continuationSequence", 1},
                            {"continuationDigest", QString(64, QLatin1Char('d'))},
                            {"deadlineAt", deadline},
                            {"attempt", 1},
                            {"claimToken", "claim-negative"}};
        const QJsonObject validSelector{{"version", 1},
                                        {"transfer_id", manifest.value("transfer_id")},
                                        {"item_id", manifest.value("item_id")},
                                        {"revision", manifest.value("revision")},
                                        {"digest_sha256", manifest.value("digest_sha256")},
                                        {"total_bytes", manifest.value("total_bytes")},
                                        {"offset", 0}};
        int caseNumber = 0;
        const auto rejected = [&](QJsonObject candidate, const QJsonObject& payload) {
            candidate.insert(
                "id",
                QStringLiteral("restore-negative-%1").arg(++caseNumber, 4, 10, QLatin1Char('0')));
            candidate.insert("claimToken", QStringLiteral("claim-negative-%1").arg(caseNumber));
            const QString ackPath =
                QStringLiteral("/v2/commands/%1/ack").arg(candidate.value("id").toString());
            c.transport.statusForPath.insert(ackPath, 200);
            c.transport.responseForPath.insert(ackPath, R"({"status":"rejected"})");
            c.transport.requests.clear();
            c.transport.bodies.clear();
            Access::restore(*c.client, candidate, payload);
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 5000 &&
                   std::none_of(
                       c.transport.requests.cbegin(), c.transport.requests.cend(),
                       [&](const auto& request) { return request.url().path() == ackPath; })) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            }
            const auto ack =
                std::find_if(c.transport.requests.cbegin(), c.transport.requests.cend(),
                             [&](const auto& request) { return request.url().path() == ackPath; });
            if (ack == c.transport.requests.cend()) {
                return QString{};
            }
            return QJsonDocument::fromJson(
                       c.transport.bodies.at(std::distance(c.transport.requests.cbegin(), ack)))
                .object()
                .value("resultCode")
                .toString();
        };

        QJsonObject root = command;
        root.remove("continuationOfRequestId");
        root.insert("continuationSequence", 0);
        root.remove("continuationDigest");
        QCOMPARE(rejected(root, {{"large_event", validSelector}}),
                 QString("invalid_restore_continuation"));
        QCOMPARE(rejected(command, {{"large_event", validSelector}, {"before_cursor", snapshot}}),
                 QString("invalid_restore_continuation"));

        QJsonObject badOffset = validSelector;
        badOffset.insert("offset", -1);
        QCOMPARE(rejected(command, {{"large_event", badOffset}}),
                 QString("invalid_restore_continuation"));
        QJsonObject badDigest = validSelector;
        badDigest.insert("digest_sha256", QString(64, QLatin1Char('e')));
        QCOMPARE(rejected(command, {{"large_event", badDigest}}),
                 QString("restore_continuation_conflict"));
        QJsonObject badItem = validSelector;
        badItem.insert("item_id", "another-item");
        QCOMPARE(rejected(command, {{"large_event", badItem}}),
                 QString("restore_continuation_conflict"));
        QJsonObject badRevision = validSelector;
        badRevision.insert("revision", manifest.value("revision").toInteger() - 1);
        QCOMPARE(rejected(command, {{"large_event", badRevision}}),
                 QString("restore_continuation_conflict"));
        QJsonObject overflow = validSelector;
        overflow.insert("total_bytes", 32 * 1024 * 1024 + 1);
        QCOMPARE(rejected(command, {{"large_event", overflow}}),
                 QString("invalid_restore_continuation"));

        QJsonObject sequenceOverflow = command;
        sequenceOverflow.insert("continuationSequence", 4097);
        QCOMPARE(rejected(sequenceOverflow, {{"large_event", validSelector}}),
                 QString("transcript_unavailable"));
    }
    void idleSessionAfterAFailedOrInterruptedTurnIsReportedReady() {
        const auto reported = [](const QString& state) {
            StoredSession session;
            session.id = QStringLiteral("session-synthetic");
            session.worktreePath = QStringLiteral("/synthetic/worktree");
            session.adapter = QStringLiteral("claude");
            session.state = state;
            session.generation = 1;
            return Access::snapshots({session})
                .first()
                .toObject()
                .value(QStringLiteral("state"))
                .toString();
        };
        QCOMPARE(reported(QStringLiteral("failed")), QStringLiteral("ready"));
        QCOMPARE(reported(QStringLiteral("interrupted")), QStringLiteral("ready"));
        QCOMPARE(reported(QStringLiteral("running")), QStringLiteral("running"));
        QCOMPARE(reported(QStringLiteral("recovering")), QStringLiteral("unreachable"));
    }

    // "ready" after a failure hides how the turn ended; the sealed metadata keeps it.
    void sealedMetadataCarriesTheLastTurnOutcome() {
        const auto outcome = [](const QString& state, const qint64 turns) {
            StoredSession session;
            session.id = QStringLiteral("session-synthetic");
            session.worktreePath = QStringLiteral("/synthetic/worktree");
            session.adapter = QStringLiteral("claude");
            session.state = state;
            session.generation = 1;
            session.currentTurnSequence = turns;
            return Access::metadata(session).value(QStringLiteral("lastTurnOutcome"));
        };
        QVERIFY(outcome(QStringLiteral("ready"), 0).isNull());
        QCOMPARE(outcome(QStringLiteral("ready"), 2).toString(), QStringLiteral("completed"));
        QCOMPARE(outcome(QStringLiteral("exited"), 2).toString(), QStringLiteral("completed"));
        QCOMPARE(outcome(QStringLiteral("failed"), 2).toString(), QStringLiteral("failed"));
        QCOMPARE(outcome(QStringLiteral("interrupted"), 2).toString(),
                 QStringLiteral("interrupted"));
        QVERIFY(outcome(QStringLiteral("running"), 2).isNull());
    }

    // ADR 0027: the phone shows the conversation number, which the relay never reads.
    void sealedMetadataCarriesTheConversationNumber() {
        StoredSession session;
        session.id = QStringLiteral("session-synthetic");
        session.worktreePath = QStringLiteral("/synthetic/worktree");
        session.adapter = QStringLiteral("codex");
        session.generation = 1;
        const QJsonObject sender{{QStringLiteral("number"), QStringLiteral("1093")},
                                 {QStringLiteral("title"), QStringLiteral("Sidebar")},
                                 {QStringLiteral("kind"), QStringLiteral("request")}};
        const QJsonObject sealed =
            Access::metadata(session, {{session.id,
                                        {{QStringLiteral("number"), QStringLiteral("4821")},
                                         {QStringLiteral("agentTurn"), sender}}}});
        QCOMPARE(sealed.value(QStringLiteral("number")).toString(), QStringLiteral("4821"));
        QCOMPARE(sealed.value(QStringLiteral("agentTurn")).toObject(), sender);
        QVERIFY(!Access::metadata(session).contains(QStringLiteral("number")));
        const QJsonObject snapshot = Access::snapshots({session}).first().toObject();
        QVERIFY(!snapshot.contains(QStringLiteral("number")));
        QVERIFY(!snapshot.contains(QStringLiteral("agentTurn")));
    }

    // An enrollment issued by another relay is neither presented to this build's relay nor
    // erased by signing out of it.
    void enrollmentFromAnotherRelayIsKeptButNotUsed() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        const QString service = QStringLiteral("dev.ketplus.daemon.control/");
        r03::credentials.insert(service + "machine-id", "machine-elsewhere");
        r03::credentials.insert(service + "workspace-id", "workspace-elsewhere");
        r03::credentials.insert(service + "control-url", "https://other-relay.invalid");
        r03::credentials.insert(service + "uploaded-cursor-session-synthetic-1", "42");
        CloudControlClient client(&c.journal, &c.host);
        const QJsonObject status = Access::status(client);
        QVERIFY(!status.value("configured").toBool());
        QVERIFY(status.value("machine_id").toString().isEmpty());
        QCOMPARE(status.value("enrolled_control_url").toString(),
                 QStringLiteral("https://other-relay.invalid"));
        QVERIFY(status.value("last_error").toString().contains("other-relay.invalid"));

        QString error;
        client.handleLocal("remote.sign_out", {},
                           [&error](const QJsonObject&, const QString& detail) { error = detail; });
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(r03::credentials.value(service + "machine-id"), QByteArray("machine-elsewhere"));
        QCOMPARE(r03::credentials.value(service + "control-url"),
                 QByteArray("https://other-relay.invalid"));
        QCOMPARE(r03::credentials.value(service + "uploaded-cursor-session-synthetic-1"),
                 QByteArray("42"));
    }

    void enrollmentWithoutARecordedRelayAdoptsThisBuildsRelay() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        const QString service = QStringLiteral("dev.ketplus.daemon.control/");
        r03::credentials.insert(service + "machine-id", "machine-legacy");
        r03::credentials.insert(service + "workspace-id", "workspace-legacy");
        r03::credentials.insert(service + "uploaded-cursor-session-synthetic-1", "42");
        CloudControlClient client(&c.journal, &c.host);
        QCOMPARE(Access::status(client).value("machine_id").toString(),
                 QStringLiteral("machine-legacy"));
        QCOMPARE(QString::fromUtf8(r03::credentials.value(service + "control-url")),
                 CloudControlClient::configuredControlUrl().toString());

        // Signing out of the relay it belongs to forgets it, upload progress included.
        client.handleLocal("remote.sign_out", {}, [](const QJsonObject&, const QString&) {});
        for (const QString& account :
             {"machine-id", "workspace-id", "control-url", "uploaded-cursor-session-synthetic-1"})
            QVERIFY2(!r03::credentials.contains(service + account), qPrintable(account));
    }

    void senderKeysAreReadOncePerCacheWindow() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        const QString path = QStringLiteral("/v2/device-keys/mobile-1/1");
        const QJsonObject senderKeys = Access::crypto(*c.client).publicKeys();
        Access::pinDevice(*c.client, "mobile-1", 1, senderKeys);
        c.transport.statusForPath.insert(path, 200);
        c.transport.responseForPath.insert(
            path, QJsonDocument(QJsonObject{{"keys", senderKeys}}).toJson(QJsonDocument::Compact));
        c.transport.statusForPath.insert(QStringLiteral("/v2/device-keys/mobile-1/2"), 503);
        const auto reads = [&c](const QString& keyPath) {
            return std::count_if(
                c.transport.requests.cbegin(), c.transport.requests.cend(),
                [&](const auto& request) { return request.url().path() == keyPath; });
        };
        QList<std::pair<QJsonObject, QString>> answers;
        const auto keys = [&](const int version) {
            Access::senderKeys(*c.client, QStringLiteral("mobile-1"), version,
                               [&answers](const QJsonObject& value, const QString& error) {
                                   answers.append({value, error});
                               });
        };
        keys(1);
        QTRY_COMPARE(answers.size(), 1);
        QCOMPARE(answers.last().first.value("signingPublicKey"),
                 senderKeys.value("signingPublicKey"));
        keys(1);
        QCOMPARE(answers.size(), 2);
        QCOMPARE(answers.last().first, answers.first().first);
        QCOMPARE(reads(path), 1);

        Access::expireSenderKeys(*c.client);
        keys(1);
        QTRY_COMPARE(answers.size(), 3);
        QCOMPARE(reads(path), 2);

        // A failed read is not remembered.
        keys(2);
        QTRY_COMPARE(answers.size(), 4);
        QVERIFY(!answers.last().second.isEmpty());
        QCOMPARE(Access::cachedSenderKeys(*c.client), 1);
    }

    void onlyDevicesPinnedHereReceiveKeysOrSendCommands() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        CredentialStore phoneStore("pinned-phone");
        CredentialStore intruderStore("relay-intruder");
        const QJsonObject phone = CloudCrypto(&phoneStore).publicKeys();
        const QJsonObject intruder = CloudCrypto(&intruderStore).publicKeys();
        const auto row = [](const QString& id, const QJsonObject& keys) {
            return QJsonObject{{"id", id}, {"keyVersion", 1}, {"status", "active"}, {"keys", keys}};
        };
        const auto recipients = [&c] {
            QStringList ids;
            for (qsizetype index = 0; index < c.transport.requests.size(); ++index) {
                if (c.transport.requests.at(index).url().path() != "/v2/session-keys:append")
                    continue;
                ids.clear();
                for (const auto& envelope : QJsonDocument::fromJson(c.transport.bodies.at(index))
                                                .object()
                                                .value("envelopes")
                                                .toArray())
                    ids.append(envelope.toObject().value("recipientDeviceId").toString());
            }
            return ids;
        };
        const auto distribute = [&](const QJsonArray& devices) {
            const qsizetype before = c.transport.requests.size();
            c.transport.statusForPath.insert("/v2/devices", 200);
            c.transport.responseForPath.insert("/v2/devices", QJsonDocument(devices).toJson());
            c.transport.statusForPath.insert("/v2/session-keys:append", 202);
            Access::distributeKeys(*c.client);
            QTRY_VERIFY(std::any_of(c.transport.requests.cbegin() + before,
                                    c.transport.requests.cend(), [](const auto& request) {
                                        return request.url().path() == "/v2/session-keys:append";
                                    }));
        };

        // The first device list after the upgrade is adopted: existing pairings keep working.
        QVERIFY(!Access::pins(*c.client).initialized());
        distribute({row("phone", phone)});
        QCOMPARE(recipients(), QStringList{"phone"});
        QVERIFY(Access::pins(*c.client).initialized());

        // A device the relay adds later, or the phone's id with substituted keys, gets nothing.
        distribute({row("phone", phone), row("intruder", intruder)});
        QCOMPARE(recipients(), QStringList{"phone"});
        QVERIFY(!Access::pins(*c.client).trusts(row("phone", intruder)));

        QList<std::pair<QJsonObject, QString>> answers;
        for (const auto& [id, keys] :
             {std::pair{QString("intruder"), intruder}, std::pair{QString("phone"), intruder},
              std::pair{QString("phone"), phone}}) {
            const QString path = QStringLiteral("/v2/device-keys/%1/1").arg(id);
            c.transport.statusForPath.insert(path, 200);
            c.transport.responseForPath.insert(
                path, QJsonDocument(QJsonObject{{"keys", keys}}).toJson(QJsonDocument::Compact));
            const qsizetype expected = answers.size() + 1;
            Access::senderKeys(*c.client, id, 1,
                               [&answers](const QJsonObject& value, const QString& error) {
                                   answers.append({value, error});
                               });
            QTRY_COMPARE(answers.size(), expected);
            Access::expireSenderKeys(*c.client);
        }
        QTRY_COMPARE(answers.size(), 3);
        QCOMPARE(answers.at(0).second, QStringLiteral("The sender is not paired with this Mac."));
        QCOMPARE(answers.at(1).second, QStringLiteral("The sender is not paired with this Mac."));
        QVERIFY(answers.at(2).second.isEmpty());
        QCOMPARE(answers.at(2).first.value("signingPublicKey"), phone.value("signingPublicKey"));
    }

    void approvalRecomputesTheClaimDigestAndPinsTheDevice() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        QVERIFY(Access::pins(*c.client).adopt({})); // pins exist, nothing pinned yet
        CredentialStore phoneStore("claiming-phone");
        CredentialStore relayStore("relay-substitute");
        const QJsonObject phone = CloudCrypto(&phoneStore).publicKeys();
        const QJsonObject relay = CloudCrypto(&relayStore).publicKeys();
        // The relay's and the phone's formula, written out independently of DevicePins.
        const auto digest = [](const QJsonObject& keys) {
            const QByteArray json = QStringLiteral("[\"ticket-1\",\"phone\",1,\"%1\",\"%2\"]")
                                        .arg(keys.value("agreementPublicKey").toString(),
                                             keys.value("signingPublicKey").toString())
                                        .toUtf8();
            return QString::fromLatin1(
                QCryptographicHash::hash(json, QCryptographicHash::Sha256)
                    .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        };
        const QString ticketPath = QStringLiteral("/v2/pairing/tickets/ticket-1");
        const auto approve = [&](const QJsonObject& claimedKeys, const QString& claimDigest) {
            c.transport.statusForPath.insert(ticketPath, 200);
            c.transport.responseForPath.insert(
                ticketPath,
                QJsonDocument(QJsonObject{{"id", "ticket-1"},
                                          {"status", "claimed"},
                                          {"claimedDevice", QJsonObject{{"id", "phone"},
                                                                        {"keyVersion", 1},
                                                                        {"status", "pending"},
                                                                        {"keys", claimedKeys}}},
                                          {"claimDigest", claimDigest}})
                    .toJson(QJsonDocument::Compact));
            c.transport.statusForPath.insert(ticketPath + "/approve", 200);
            std::optional<QString> result;
            Access::approvePairing(
                *c.client, "ticket-1",
                [&result](const QJsonObject&, const QString& error) { result = error; });
            [&] { QTRY_VERIFY(result.has_value()); }();
            return result.value_or(QStringLiteral("no answer"));
        };
        const auto approvals = [&] {
            return std::count_if(c.transport.requests.cbegin(), c.transport.requests.cend(),
                                 [&](const auto& request) {
                                     return request.url().path() == ticketPath + "/approve";
                                 });
        };

        // The relay substitutes its own keys but keeps the digest the phone shows.
        QCOMPARE(approve(relay, digest(phone)),
                 QStringLiteral("The device claim does not match its keys."));
        QCOMPARE(approvals(), 0);
        QVERIFY(!Access::pins(*c.client).trustsKeys("phone", 1, relay));

        QCOMPARE(approve(phone, digest(phone)), QString());
        QCOMPARE(approvals(), 1);
        QVERIFY(Access::pins(*c.client).trustsKeys("phone", 1, phone));
        QVERIFY(!Access::pins(*c.client).trustsKeys("phone", 1, relay));
        QVERIFY(Access::pins(*c.client).unpin("phone"));
        QVERIFY(!Access::pins(*c.client).trustsKeys("phone", 1, phone));
    }

    void machineSignsOnlyAThirtyTwoByteChallenge() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        const QString challenges = QStringLiteral("/v2/daemon/challenges");
        const QString credentials = QStringLiteral("/v2/daemon/credentials");
        c.transport.statusForPath.insert(challenges, 201);
        c.transport.statusForPath.insert(credentials, 503);
        const auto requested = [&c](const QString& path) {
            return std::count_if(c.transport.requests.cbegin(), c.transport.requests.cend(),
                                 [&](const auto& request) { return request.url().path() == path; });
        };
        const auto attempt = [&](const QString& encoded) {
            c.transport.responseForPath.insert(
                challenges,
                QJsonDocument(QJsonObject{{"id", "challenge_1"}, {"challenge", encoded}})
                    .toJson(QJsonDocument::Compact));
            std::optional<std::pair<bool, QString>> result;
            Access::credential(*c.client, [&result](const bool ready, const QString& error) {
                result = std::pair{ready, error};
            });
            [&] { QTRY_VERIFY(result.has_value()); }();
            return result.value_or(std::pair{true, QString()});
        };
        const auto encode = [](const QByteArray& bytes) {
            return QString::fromLatin1(CloudCrypto::base64Url(bytes));
        };
        // A relay asking for a host signature over something the host vouches for: a WebRTC
        // answer carrying the relay's own SDP, or bytes of the wrong length.
        const QByteArray answer =
            CloudCrypto::canonical({"ketplus", 2, "webrtc-answer", "pc_synthetic",
                                    "workspace-synthetic", "machine-synthetic", 1, "mobile-1",
                                    "offer-hash", "2026-11-06T00:00:00.000Z", "relay-sdp-hash"});
        for (const QString& forged :
             {encode(answer), encode(QByteArray(33, 'k')), encode(QByteArray(31, 'k')),
              encode(QByteArray(32, 'k')) + QStringLiteral("=="), QString()}) {
            const auto [ready, error] = attempt(forged);
            QVERIFY(!ready);
            QCOMPARE(error, QStringLiteral("The relay sent an invalid machine challenge."));
        }
        QCOMPARE(requested(credentials), 0);

        // The relay's real challenge is signed and sent.
        const auto [ready, error] = attempt(encode(QByteArray(32, 'k')));
        QVERIFY(!ready); // this fake credential endpoint is down
        QVERIFY(error != QStringLiteral("The relay sent an invalid machine challenge."));
        QCOMPARE(requested(credentials), 1);
    }

    void commandPollWaitsAfterFailuresUntilAWakeUp() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        const QString poll = QStringLiteral("/v2/commands:poll");
        c.transport.statusForPath.insert(poll, 503);
        const auto polls = [&c, &poll] {
            return std::count_if(c.transport.requests.cbegin(), c.transport.requests.cend(),
                                 [&](const auto& request) { return request.url().path() == poll; });
        };
        Access::pollCommands(*c.client);
        QTRY_VERIFY(!Access::commandPollInFlight(*c.client));
        QCOMPARE(polls(), 1);
        QVERIFY(Access::commandPollWaiting(*c.client));
        // Timer ticks during the wait send nothing.
        Access::pollCommands(*c.client);
        Access::pollCommands(*c.client);
        QCOMPARE(polls(), 1);

        Access::expireCommandPollBackoff(*c.client);
        Access::pollCommands(*c.client);
        QTRY_VERIFY(!Access::commandPollInFlight(*c.client));
        QCOMPARE(polls(), 2);
        QCOMPARE(Access::commandPollFailures(*c.client), 2);

        // A wake-up comes from the relay: poll at once.
        c.transport.statusForPath.insert(poll, 200);
        c.transport.responseForPath.insert(poll, R"({"commands":[]})");
        Access::wakeCommandPoll(*c.client);
        QTRY_VERIFY(!Access::commandPollInFlight(*c.client));
        QCOMPARE(polls(), 3);
        QCOMPARE(Access::commandPollFailures(*c.client), 0);
        Access::pollCommands(*c.client);
        QTRY_VERIFY(!Access::commandPollInFlight(*c.client));
        QCOMPARE(polls(), 4);
    }

    void replyLostAfterItsHeaderIsAFailure() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        // The relay answered 200, then the transfer timed out before the body arrived. Taken as
        // success, a command poll's leased commands vanished without an error.
        const QString lost = QStringLiteral("/v2/device-keys/mobile-1/1");
        c.transport.statusForPath.insert(lost, 200);
        c.transport.responseForPath.insert(lost, R"({"keys":{"signingPublicKey":"synthetic"}})");
        c.transport.failureForPath.insert(lost, QNetworkReply::OperationCanceledError);
        // The relay's own refusal still arrives as its status, not as a transport failure.
        const QString refused = QStringLiteral("/v2/device-keys/mobile-1/2");
        c.transport.statusForPath.insert(refused, 409);
        c.transport.failureForPath.insert(refused, QNetworkReply::ContentConflictError);
        QList<std::pair<QJsonObject, QString>> answers;
        for (const int version : {1, 2})
            Access::senderKeys(*c.client, QStringLiteral("mobile-1"), version,
                               [&answers](const QJsonObject& value, const QString& error) {
                                   answers.append({value, error});
                               });
        QTRY_COMPARE(answers.size(), 2);
        QVERIFY(answers.at(0).first.isEmpty());
        QCOMPARE(answers.at(0).second, QStringLiteral("synthetic failure"));
        QCOMPARE(answers.at(1).second, QStringLiteral("The sender key is unavailable."));
        QCOMPARE(Access::cachedSenderKeys(*c.client), 0);
    }
};
QTEST_GUILESS_MAIN(CloudPrivacyTest)
#include "CloudPrivacyTest.moc"
