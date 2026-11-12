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
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QProcess>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>
#include <algorithm>
#include <limits>
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
    void deletedGenerationStopsInFlightCloudUpload_data() {
        QTest::addColumn<int>("status");
        QTest::newRow("413 retry") << 413;
        QTest::newRow("late 202") << 202;
    }
    void deletedGenerationStopsInFlightCloudUpload() {
        // ADR 0093: residual history awaiting purge must never be read, sealed or acknowledged.
        QFETCH(int, status);
        QTemporaryDir dir;
        const auto path = dir.filePath("deleted-upload.sqlite");
        Context c(path);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        QVERIFY2(q.exec("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE "
                        "i<3000) INSERT INTO events(session_id,generation,cursor,event_type,"
                        "payload,sent_at,turn_sequence) SELECT 'session-synthetic',1,i,"
                        "'assistant.delta','{\"text\":\"private\"}',datetime('now'),1 FROM n"),
                 qPrintable(q.lastError().text()));
        q.finish();
        c.start();
        c.transport.status = status;
        c.transport.response = R"({"acceptedThrough":[{"sessionId":"session-synthetic",)"
                               R"("sessionGeneration":1,"cursor":1}]})";
        Access::upload(*c.client);
        QCOMPARE(c.transport.bodies.size(), 1);
        QVERIFY(c.journal.deleteSession("session-synthetic", 1));
        QVERIFY(c.journal.sessions().isEmpty());
        QVERIFY(c.journal.hasPendingHistoryPurge());
        QTRY_VERIFY(Access::idle(*c.client));
        QCoreApplication::processEvents();
        QCOMPARE(c.transport.bodies.size(), 1);
        QVERIFY(q.exec("SELECT count(*) FROM cloud_upload_progress"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
        q.finish();
        Access::upload(*c.client);
        QCOMPARE(c.transport.bodies.size(), 1);
    }
    void deletedGenerationResidualIsNeverUploaded() {
        QTemporaryDir dir;
        const auto path = dir.filePath("residual-upload.sqlite");
        Context c(path);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        QVERIFY2(q.exec("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE "
                        "i<3000) INSERT INTO events(session_id,generation,cursor,event_type,"
                        "payload,sent_at,turn_sequence) SELECT 'session-synthetic',1,i,"
                        "'assistant.delta','{\"text\":\"private\"}',datetime('now'),1 FROM n"),
                 qPrintable(q.lastError().text()));
        q.finish();
        c.start();
        c.transport.status = 202;
        QVERIFY(c.journal.deleteSession("session-synthetic", 1));
        QVERIFY(c.journal.hasPendingHistoryPurge());
        QVERIFY(!c.journal.eventsAfter("session-synthetic", 1, 0).isEmpty());
        Access::upload(*c.client);
        QVERIFY(c.transport.bodies.isEmpty());
    }
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

    void failedCacheInvalidationDoesNotClaimReconciliation_data() {
        QTest::addColumn<bool>("wrongTurn");
        QTest::newRow("policy") << false;
        QTest::newRow("turn") << true;
    }
    void failedCacheInvalidationDoesNotClaimReconciliation() {
        QFETCH(bool, wrongTurn);
        QTemporaryDir dir;
        const QString path = dir.filePath("cache-failure.sqlite");
        Context c(path);
        c.item("completed");
        c.start();
        auto legacy = legacyEnvelope(c);
        if (wrongTurn)
            legacy.insert("turnSequence", 99999);
        QVERIFY(c.journal.storeCloudEvent("session-synthetic", 1, 1, legacy, 0));
        const QString name = "cloud-cache-fault-" + dir.path();
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
        const auto cleanup = qScopeGuard([&] {
            db.close();
            db = {};
            QSqlDatabase::removeDatabase(name);
        });
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec("CREATE TRIGGER ignore_cache_delete BEFORE DELETE ON cloud_events "
                           "BEGIN SELECT RAISE(IGNORE); END"));
        Access::upload(*c.client);
        QVERIFY(Access::idle(*c.client));
        QVERIFY(c.transport.bodies.isEmpty());
        QCOMPARE(c.journal.cloudEvent("session-synthetic", 1, 1), legacy);
        QCOMPARE(Access::error(*c.client), QString("Cloud cache entry was not discarded."));
        QVERIFY(query.exec("DROP TRIGGER ignore_cache_delete"));
        Access::upload(*c.client);
        if (!wrongTurn) {
            QCOMPARE(Access::error(*c.client),
                     QString("cloud_event_cache_requires_reconciliation"));
            QVERIFY(c.transport.bodies.isEmpty());
            Access::upload(*c.client);
        }
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(c.outgoing().size(), 1);
        QVERIFY(!decrypt(c, c.outgoing().first().toObject()).contains(marker.toUtf8()));
    }

    void oldUploadReceiptAfterSignOutCannotRestoreProgressOrDeleteCache() {
        QTemporaryDir dir;
        Context c(dir.filePath("stale-upload.sqlite"));
        c.item("completed");
        c.start();
        c.transport.status = 202;
        c.transport.response =
            R"({"acceptedThrough":[{"sessionId":"session-synthetic","sessionGeneration":1,"cursor":1}]})";
        Access::upload(*c.client);
        QVERIFY(!Access::idle(*c.client));
        const auto cached = c.journal.cloudEvent("session-synthetic", 1, 1);
        QVERIFY(!cached.isEmpty());
        QString error;
        c.client->handleLocal("remote.sign_out", {},
                              [&](const QJsonObject&, const QString& detail) { error = detail; });
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QTRY_VERIFY(Access::idle(*c.client));
        QVERIFY(!r03::credentials.contains(
            "dev.ketplus.daemon.control/uploaded-cursor-session-synthetic-1"));
        QVERIFY(c.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(c.journal.cloudEvent("session-synthetic", 1, 1), cached);
        QCOMPARE(c.journal.eventsAfter("session-synthetic", 1, 0).size(), 1);
    }

    void failedReceiptRemainsRetryableAfterProgress() {
        QTemporaryDir dir;
        const auto path = dir.filePath("failed-receipt.sqlite");
        Context c(path);
        c.item("completed");
        c.start();
        c.transport.status = 202;
        c.transport.response =
            R"({"acceptedThrough":[{"sessionId":"session-synthetic","sessionGeneration":1,"cursor":1}]})";
        const QString name = "reviewer-receipt-" + dir.path();
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
        const auto cleanup = qScopeGuard([&] {
            db.close();
            db = {};
            QSqlDatabase::removeDatabase(name);
        });
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec("CREATE TRIGGER ignore_receipt BEFORE UPDATE OF upload_acked ON "
                           "cloud_events WHEN NEW.upload_acked=1 BEGIN SELECT RAISE(IGNORE); END"));
        Access::uploadAll(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(Access::error(*c.client), QString("Cloud upload cache receipt was not stored."));
        QVERIFY(!r03::credentials.contains(
            "dev.ketplus.daemon.control/uploaded-cursor-session-synthetic-1"));
        const auto firstBody = c.transport.bodies.first();
        QVERIFY(query.exec("SELECT upload_acked FROM cloud_events WHERE cursor=1"));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 0);
        query.finish();
        QVERIFY(query.exec("DROP TRIGGER ignore_receipt"));
        const auto requests = c.transport.bodies.size();
        Access::uploadAll(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(c.transport.bodies.size() - requests, 1);
        QCOMPARE(c.transport.bodies.last(), firstBody);
        c.item("failed");
        c.transport.response =
            R"({"acceptedThrough":[{"sessionId":"session-synthetic","sessionGeneration":1,"cursor":2}]})";
        Access::uploadAll(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        for (int i = 0; i < 3; ++i)
            QVERIFY(c.journal.pruneAcknowledgedCloudEvents());
        qInfo() << "Progress after next successful batch"
                << r03::credentials.value(
                       "dev.ketplus.daemon.control/uploaded-cursor-session-synthetic-1");
        LocalJournal reopened(path);
        QVERIFY(reopened.isOpen());
        QVERIFY(reopened.pruneAcknowledgedCloudEvents());
        QVERIFY2(reopened.cloudEvent("session-synthetic", 1, 1).isEmpty(),
                 "Receipt failure permanently strands cache after persisted progress, retry, "
                 "subsequent ACK and reopen");
    }

    void durableReceiptRecoversAfterCredentialFailureAndRestart() {
        QTemporaryDir dir;
        const auto path = dir.filePath("receipt-restart.sqlite");
        const QString cursorAccount = "uploaded-cursor-session-synthetic-1";
        {
            Context c(path);
            c.item("completed");
            c.start();
            c.transport.status = 202;
            c.transport.response =
                R"({"acceptedThrough":[{"sessionId":"session-synthetic","sessionGeneration":1,"cursor":1}]})";
            Access::uploadAll(*c.client);
            r03::failWritesTo = cursorAccount;
            QTRY_VERIFY(Access::idle(*c.client));
            QVERIFY(!Access::error(*c.client).isEmpty());
            QVERIFY(!r03::credentials.contains("dev.ketplus.daemon.control/" + cursorAccount));
            // A crash/reopen may reclaim cache, but must not force new ciphertext
            // for an event the relay already accepted.
            QVERIFY(c.journal.pruneAcknowledgedCloudEvents());
            QVERIFY(c.journal.cloudEvent("session-synthetic", 1, 1).isEmpty());
        }
        r03::failWritesTo.clear();
        Context restarted(path);
        restarted.start();
        Access::uploadAll(*restarted.client);
        QTRY_VERIFY(Access::idle(*restarted.client));
        QVERIFY(restarted.transport.bodies.isEmpty());
        QCOMPARE(r03::credentials.value("dev.ketplus.daemon.control/" + cursorAccount),
                 QByteArray("1"));
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

    // The relay waits for a cursor this journal pruned: no retry can fill it, so the session waits
    // on the long gap cap, while another refusal keeps the shorter one.
    void cursorGapWaitsOnItsOwnCap() {
        QTemporaryDir dir;
        Context c(dir.filePath("gap.sqlite"));
        QString error;
        QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "user.message", {{"text", "first"}},
                                       &error) > 0,
                 qPrintable(error));
        c.start();
        c.transport.statusForPath.insert("/v2/events:append", 409);
        c.transport.responseForPath.insert("/v2/events:append", R"({"code":"event_cursor_gap"})");
        Access::uploadAll(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(Access::uploadWaitsOnGap(*c.client), 1);

        Access::expireUploadBackoff(*c.client);
        c.transport.responseForPath.insert("/v2/events:append", R"({"code":"stale_generation"})");
        Access::uploadAll(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(Access::uploadWaitsOnGap(*c.client), 0);
    }

    // Once the relay accepted every event of a session, later passes skip it without reading the
    // journal until it records another event.
    void acceptedSessionIsSkippedUntilItsCursorMoves() {
        QTemporaryDir dir;
        Context c(dir.filePath("known.sqlite"));
        QString error;
        QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "user.message", {{"text", "first"}},
                                       &error) > 0,
                 qPrintable(error));
        c.start();
        c.transport.statusForPath.insert("/v2/events:append", 202);
        c.transport.responseForPath.insert(
            "/v2/events:append",
            R"({"acceptedThrough":[{"sessionId":"session-synthetic","sessionGeneration":1,"cursor":1}]})");
        const auto appends = [&c] {
            int count = 0;
            for (const QNetworkRequest& request : c.transport.requests)
                count += request.url().path() == "/v2/events:append";
            return count;
        };
        Access::uploadAll(*c.client);
        QTRY_COMPARE(appends(), 1);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(Access::knownUploadedCursor(*c.client), 1);
        for (int i = 0; i < 5; ++i)
            Access::uploadAll(*c.client);
        QTest::qWait(50);
        QCOMPARE(appends(), 1);

        c.transport.responseForPath.insert(
            "/v2/events:append",
            R"({"acceptedThrough":[{"sessionId":"session-synthetic","sessionGeneration":1,"cursor":2}]})");
        QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "user.message", {{"text", "next"}},
                                       &error) > 0,
                 qPrintable(error));
        Access::uploadAll(*c.client);
        QTRY_COMPARE(appends(), 2);
        QTRY_COMPARE(Access::knownUploadedCursor(*c.client), 2);
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

    // #404 / ADR 0106: a work report reaches the relay as transcript content, redacted field by
    // field, both when it is uploaded live and when a remote client replays the restore window
    // after it reconnects. The journal is written directly, as an older daemon that stored the
    // text unredacted would have, so only the cloud projection stands between it and the relay.
    void workReportReachesTheRelayRedactedAndReplays() {
        QTemporaryDir dir;
        Context c(dir.filePath("work-report.sqlite"));
        const QJsonObject report{
            {"title", "Release"},
            {"turn", 1},
            {"version", 1},
            {"items",
             QJsonArray{QJsonObject{
                 {"key", "sign"},
                 {"group", "Release"},
                 {"title", "Sign the package"},
                 {"status", "blocked"},
                 {"evidence", "curl token=" + marker + "_EVIDENCE"},
                 {"remaining", "Run with --api-key " + marker + "_REMAINING"},
                 {"next", QJsonObject{{"action", "Send Authorization: Bearer " + marker + "_NEXT"},
                                      {"owner", "user"}}},
                 {"created_turn", 1},
                 {"updated_turn", 1}}}}};
        QString journalError;
        QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "work_report.updated", report,
                                       &journalError) > 0,
                 qPrintable(journalError));
        QVERIFY2(c.journal.appendEvent("session-synthetic", 1, "turn.completed",
                                       {{"status", "completed"}, {"work_report", report}},
                                       &journalError) > 0,
                 qPrintable(journalError));
        const auto checkReport = [](const QJsonObject& projected) {
            QCOMPARE(projected.value("title").toString(), QStringLiteral("Release"));
            const QJsonArray items = projected.value("items").toArray();
            QCOMPARE(items.size(), 1);
            const QJsonObject item = items.first().toObject();
            QCOMPARE(item.value("key").toString(), QStringLiteral("sign"));
            QCOMPARE(item.value("status").toString(), QStringLiteral("blocked"));
            QCOMPARE(item.value("next").toObject().value("owner").toString(),
                     QStringLiteral("user"));
            QVERIFY(item.value("evidence").toString().startsWith("curl"));
            QVERIFY(item.value("next").toObject().value("action").toString().startsWith("Send"));
        };

        // Live: the uploaded envelopes decrypt to the report without the secrets.
        c.start();
        Access::upload(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        const auto outgoing = c.outgoing();
        QCOMPARE(outgoing.size(), 2);
        for (int i = 0; i < outgoing.size(); ++i) {
            const auto envelope = outgoing.at(i).toObject();
            QString error;
            const auto bytes = decrypt(c, envelope, &error);
            QVERIFY2(!bytes.isEmpty(), qPrintable(error));
            QVERIFY2(!bytes.contains(marker.toUtf8()), bytes.constData());
            const auto payload = QJsonDocument::fromJson(bytes).object();
            if (i == 0) {
                QCOMPARE(envelope.value("eventType").toString(),
                         QStringLiteral("work_report.updated"));
                checkReport(payload);
            } else {
                QCOMPARE(envelope.value("eventType").toString(), QStringLiteral("turn.completed"));
                checkReport(payload.value("work_report").toObject());
            }
        }

        // Reconnect: the restore window replays the same report, projected the same way.
        QByteArray bytes;
        const QJsonObject page = restorePage(c, "restore-work-report-0001", &bytes);
        QVERIFY(page.value("ok").toBool());
        QVERIFY2(!bytes.contains(marker.toUtf8()), bytes.constData());
        bool sawReport = false;
        bool sawTurnEnd = false;
        for (const QJsonValue& value : page.value("events").toArray()) {
            const QJsonObject event = value.toObject();
            const QJsonObject payload = event.value("payload").toObject();
            if (event.value("event_type") == "work_report.updated") {
                sawReport = true;
                checkReport(payload);
            }
            if (event.value("event_type") == "turn.completed") {
                sawTurnEnd = true;
                checkReport(payload.value("work_report").toObject());
            }
        }
        QVERIFY(sawReport);
        QVERIFY(sawTurnEnd);
        // The desktop's own window keeps what the journal holds.
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

    // #125 and #429: the phone reads "Waiting for you" and the tags only from sealed metadata;
    // the plaintext snapshot the relay sees carries neither.
    void sealedMetadataCarriesWaitingAndTags() {
        StoredSession session;
        session.id = QStringLiteral("session-synthetic");
        session.worktreePath = QStringLiteral("/synthetic/worktree");
        session.adapter = QStringLiteral("claude");
        session.state = QStringLiteral("ready");
        session.generation = 1;
        QJsonObject sealed = Access::metadata(session);
        QVERIFY(!sealed.contains(QStringLiteral("awaitingReply")));
        QVERIFY(!sealed.contains(QStringLiteral("tags")));
        session.awaitingReply = true;
        session.tags = {QStringLiteral("Release"), QStringLiteral("Custom")};
        sealed =
            Access::metadata(session, {}, {{QStringLiteral("release"), QStringLiteral("#d1453b")}});
        QVERIFY(sealed.value(QStringLiteral("awaitingReply")).toBool());
        const QJsonArray tags = sealed.value(QStringLiteral("tags")).toArray();
        QCOMPARE(tags.size(), 2);
        QCOMPARE(tags.at(0).toObject().value(QStringLiteral("color")).toString(),
                 QStringLiteral("#d1453b"));
        QCOMPARE(tags.at(1).toObject().value(QStringLiteral("color")).toString(),
                 QStringLiteral("#5b6b7f"));
        const QJsonObject plain = Access::snapshots({session}).first().toObject();
        QVERIFY(!plain.contains(QStringLiteral("tags")));
        QVERIFY(!plain.contains(QStringLiteral("awaitingReply")));
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

    void pairingStatusGivesTheDaemonsCodeOnlyForAVerifiedClaim() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        CredentialStore phoneStore("status-phone");
        CredentialStore relayStore("status-relay");
        const QJsonObject phone = CloudCrypto(&phoneStore).publicKeys();
        const QJsonObject relay = CloudCrypto(&relayStore).publicKeys();
        const QString phoneDigest = QString::fromLatin1(
            QCryptographicHash::hash(QStringLiteral("[\"ticket-1\",\"phone\",1,\"%1\",\"%2\"]")
                                         .arg(phone.value("agreementPublicKey").toString(),
                                              phone.value("signingPublicKey").toString())
                                         .toUtf8(),
                                     QCryptographicHash::Sha256)
                .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        const QString ticketPath = QStringLiteral("/v2/pairing/tickets/ticket-1");
        const auto status = [&](const QJsonObject& claimedKeys, const QString& digest) {
            c.transport.statusForPath.insert(ticketPath, 200);
            c.transport.responseForPath.insert(
                ticketPath,
                QJsonDocument(QJsonObject{{"id", "ticket-1"},
                                          {"status", "claimed"},
                                          {"claimedDevice", QJsonObject{{"id", "phone"},
                                                                        {"keyVersion", 1},
                                                                        {"keys", claimedKeys}}},
                                          {"claimDigest", digest}})
                    .toJson());
            QJsonObject answer;
            bool answered = false;
            Access::pairingStatus(*c.client, "ticket-1",
                                  [&](const QJsonObject& result, const QString& error) {
                                      QVERIFY2(error.isEmpty(), qPrintable(error));
                                      answer = result;
                                      answered = true;
                                  });
            [&] { QTRY_VERIFY(answered); }();
            return answer;
        };
        QString expected = phoneDigest.toUpper();
        expected.remove('-');
        expected = expected.left(6);

        const QJsonObject verified = status(phone, phoneDigest);
        QCOMPARE(verified.value("claim_verified").toBool(), true);
        QCOMPARE(verified.value("confirmation_code").toString(), expected);
        QCOMPARE(verified.value("claim_digest").toString(), phoneDigest);
        // The relay's keys under the phone's digest: no code to compare, so nothing to approve.
        const QJsonObject substituted = status(relay, phoneDigest);
        QCOMPARE(substituted.value("claim_verified").toBool(true), false);
        QVERIFY(!substituted.contains("confirmation_code"));
        // A digest too short for a code never falls back to a fixed one.
        const QJsonObject empty = status(phone, {});
        QCOMPARE(empty.value("claim_verified").toBool(true), false);
        QVERIFY(!empty.contains("confirmation_code"));
    }

    void thePairingQrCodeNamesThisMacsKeys() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        c.transport.statusForPath.insert("/v2/pairing/tickets", 201);
        c.transport.responseForPath.insert(
            "/v2/pairing/tickets", R"({"ticket":{"id":"ticket-1","status":"open"},"secret":"s"})");
        QString code;
        bool answered = false;
        Access::createPairing(*c.client, [&](const QJsonObject& result, const QString& error) {
            QVERIFY2(error.isEmpty(), qPrintable(error));
            code = result.value("pairing_code").toString();
            answered = true;
        });
        QTRY_VERIFY(answered);
        const QJsonObject keys = Access::crypto(*c.client).publicKeys();
        const QByteArray raw =
            QByteArray::fromBase64(keys.value("signingPublicKey").toString().toLatin1(),
                                   QByteArray::Base64UrlEncoding) +
            QByteArray::fromBase64(keys.value("agreementPublicKey").toString().toLatin1(),
                                   QByteArray::Base64UrlEncoding);
        const QString fingerprint = QString::fromLatin1(
            QCryptographicHash::hash(raw, QCryptographicHash::Sha256)
                .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        const QUrlQuery query(QUrl(code).query());
        QCOMPARE(query.queryItemValue("ticket"), QStringLiteral("ticket-1"));
        QCOMPARE(query.queryItemValue("secret"), QStringLiteral("s"));
        QCOMPARE(query.queryItemValue("machine"), QStringLiteral("machine-synthetic"));
        QCOMPARE(query.queryItemValue("kv"), QStringLiteral("1"));
        QCOMPARE(query.queryItemValue("fp"), fingerprint);
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
        const auto approve = [&](const QJsonObject& claimedKeys, const QString& claimDigest,
                                 const QString& confirmed) {
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
                *c.client, "ticket-1", confirmed,
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
        QCOMPARE(approve(relay, digest(phone), digest(phone)),
                 QStringLiteral("The device claim does not match its keys."));
        // Or swaps keys and digest together after the user confirmed the phone's code.
        QCOMPARE(approve(relay, digest(relay), digest(phone)),
                 QStringLiteral("The device claim does not match its keys."));
        QCOMPARE(approvals(), 0);
        QVERIFY(!Access::pins(*c.client).trustsKeys("phone", 1, relay));

        QCOMPARE(approve(phone, digest(phone), digest(phone)), QString());
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
                challenges, QJsonDocument(QJsonObject{{"id", "challenge_1"},
                                                      {"challenge", encoded},
                                                      {"machineId", "machine-synthetic"},
                                                      {"keyVersion", 1},
                                                      {"signatureVersion", 2},
                                                      {"signatureAlgorithm", "Ed25519"}})
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

    void remoteWorktreeQuotaCountsEachRepositoryForAnHourAndNeedsFreeSpace() {
        RemoteWorktreeQuota quota;
        quota.perHour = 2;
        quota.minimumFreeBytes = 100;
        const qint64 hour = 60 * 60 * 1000;
        QVERIFY(quota.reserve("repo-a", 0, 1000).isEmpty());
        QVERIFY(quota.reserve("repo-a", 10, -1).isEmpty()); // free space unknown
        QVERIFY(!quota.reserve("repo-a", 20, 1000).isEmpty());
        QVERIFY(quota.reserve("repo-b", 20, 1000).isEmpty());
        // Low space refuses without using up the hour's allowance.
        QVERIFY(!quota.reserve("repo-b", 30, 99).isEmpty());
        QVERIFY(quota.reserve("repo-b", 40, 1000).isEmpty());
        // A creation leaves the window an hour after it happened.
        QVERIFY(quota.reserve("repo-a", hour, 1000).isEmpty());
        QVERIFY(!quota.reserve("repo-a", hour + 5, 1000).isEmpty());
        QVERIFY(quota.reserve("repo-a", hour + 10, 1000).isEmpty());
    }

    void aRedeliveredNewWorktreeAnswersWithItsFirstResultAndCreationIsBounded() {
        QTemporaryDir dir;
        const QString repository = dir.filePath("repo");
        QVERIFY(QDir().mkpath(repository));
        const auto git = [&repository](const QStringList& arguments) {
            return QProcess::execute("git", QStringList{"-C", repository, "-c", "user.name=Fixture",
                                                        "-c", "user.email=fixture@example.invalid",
                                                        "-c", "commit.gpgsign=false"} +
                                                arguments);
        };
        QCOMPARE(git({"init", "-q", "-b", "main"}), 0);
        QCOMPARE(git({"commit", "--allow-empty", "-qm", "initial"}), 0);
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        QVERIFY(c.journal.rememberWorktree(QFileInfo(repository).canonicalFilePath()));
        const QByteArray salt(32, 's');
        Access::pathSalt(*c.client, salt);
        const auto id = [&](const QByteArray& label) {
            return QString::fromLatin1(
                QMessageAuthenticationCode::hash(
                    label + '\n' + QFileInfo(repository).canonicalFilePath().toUtf8(), salt,
                    QCryptographicHash::Sha256)
                    .toHex());
        };
        CredentialStore phoneStore("worktree-phone");
        const QByteArray phone = CloudCrypto::fromBase64Url(
            CloudCrypto(&phoneStore).publicKeys().value("agreementPublicKey").toString());
        const auto create = [&](const QString& commandId, const QString& branch) {
            const QJsonObject command{
                {"id", commandId},
                {"kind", "session.create"},
                {"attempt", 1},
                {"workspaceId", "workspace-synthetic"},
                {"sessionGeneration", 0},
                {"expectedCatalogRevision", 0},
                {"expectedLifecycleRevision", 0},
                {"repositoryId", id("repository")},
                {"worktreeId", id("worktree")},
                {"content", QJsonObject{{"recipientDeviceId", "machine-synthetic"},
                                        {"recipientKeyVersion", 1},
                                        {"senderDeviceId", "worktree-phone"},
                                        {"senderKeyVersion", 1}}}};
            const QJsonObject payload{
                {"prompt", "Start here"},
                {"new_worktree", QJsonObject{{"branch", branch}, {"base", "main"}}}};
            const qsizetype before = c.transport.requests.size();
            Access::worktreeCommand(*c.client, command, payload, phone);
            QString resultCode;
            [&] {
                QTRY_VERIFY(std::any_of(
                    c.transport.requests.cbegin() + before, c.transport.requests.cend(),
                    [](const auto& request) { return request.url().path().endsWith("/ack"); }));
            }();
            for (qsizetype i = before; i < c.transport.requests.size(); ++i) {
                if (c.transport.requests.at(i).url().path() ==
                    QStringLiteral("/v2/commands/%1/ack").arg(commandId))
                    resultCode = QJsonDocument::fromJson(c.transport.bodies.at(i))
                                     .object()
                                     .value("resultCode")
                                     .toString();
            }
            return resultCode;
        };
        const auto branchExists = [&git](const QString& branch) {
            return git({"show-ref", "--verify", "--quiet", "refs/heads/" + branch}) == 0;
        };

        // The first attempt created the branch and started its conversation, but its
        // acknowledgement was lost. The redelivery answers with the journaled result.
        QCOMPARE(git({"branch", "first-attempt"}), 0);
        QVERIFY(c.journal.storeCommandResult(
            "command-redelivered", {{"ok", true}, {"session_id", "session-first-attempt"}}));
        QCOMPARE(create("command-redelivered", "first-attempt"), QStringLiteral("ok"));

        // Creation is refused, before git runs, past the hourly allowance or when space is low.
        Access::worktreeQuota(*c.client).perHour = 0;
        QCOMPARE(create("command-over-quota", "over-quota"),
                 QStringLiteral("worktree_quota_exceeded"));
        QVERIFY(!branchExists("over-quota"));
        Access::worktreeQuota(*c.client).perHour = RemoteWorktreeQuota::defaultPerHour;
        Access::worktreeQuota(*c.client).minimumFreeBytes = std::numeric_limits<qint64>::max();
        QCOMPARE(create("command-low-space", "low-space"),
                 QStringLiteral("worktree_quota_exceeded"));
        QVERIFY(!branchExists("low-space"));

        // The Mac keeps a record of which device asked for each, and how it ended (#86).
        const auto audit = c.journal.remoteCommands("worktree-phone", 10);
        QVERIFY(audit);
        QHash<QString, QString> outcomes;
        for (const RemoteCommandAudit& entry : *audit) {
            QCOMPARE(entry.kind, QStringLiteral("session.create"));
            QCOMPARE(entry.deviceKeyVersion, 1);
            outcomes.insert(entry.commandId, entry.resultCode);
        }
        QCOMPARE(outcomes.value("command-over-quota"), QStringLiteral("worktree_quota_exceeded"));
        QCOMPARE(outcomes.value("command-low-space"), QStringLiteral("worktree_quota_exceeded"));
        QCOMPARE(outcomes.value("command-redelivered"), QStringLiteral("ok"));
        // A command that created a conversation is recorded with the conversation it created.
        const auto created = std::find_if(audit->cbegin(), audit->cend(), [](const auto& entry) {
            return entry.commandId == QStringLiteral("command-redelivered");
        });
        QVERIFY(created != audit->cend());
        QCOMPARE(created->sessionId, QStringLiteral("session-first-attempt"));
    }

    // Refused commands are audited without storing a command result, so the audit's own writes
    // must keep it bounded.
    void theRemoteCommandAuditStaysBoundedByItsOwnWrites() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite"));
        const QDateTime expired =
            QDateTime::currentDateTimeUtc().addDays(-LocalJournal::remoteCommandRetentionDays - 1);
        const int written = LocalJournal::maximumRemoteCommands + 600;
        for (int row = 0; row < written; ++row) {
            QVERIFY(journal.recordRemoteCommand({QStringLiteral("command-%1").arg(row),
                                                 "turn.start",
                                                 "phone-1",
                                                 1,
                                                 {},
                                                 "sender_key_unavailable",
                                                 row < 100 ? expired : QDateTime{}}));
        }
        const auto kept = journal.remoteCommands({}, written);
        QVERIFY(kept);
        // Pruned every 256 rows: never more than 255 above the bound, and no expired row.
        QVERIFY2(kept->size() <= LocalJournal::maximumRemoteCommands + 255,
                 qPrintable(QString::number(kept->size())));
        QVERIFY(kept->size() >= LocalJournal::maximumRemoteCommands);
        QVERIFY(std::none_of(kept->cbegin(), kept->cend(), [&expired](const auto& entry) {
            return entry.recordedAt <= expired;
        }));
        QCOMPARE(kept->first().commandId, QStringLiteral("command-%1").arg(written - 1));
    }

    void remoteCommandsThatCanChangeSomethingAreAuditedOnce() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        const auto command = [](const QString& id, const QString& kind, const QString& device) {
            return QJsonObject{
                {"id", id},
                {"kind", kind},
                {"attempt", 1},
                {"sessionId", "session-synthetic"},
                {"sessionGeneration", 1},
                {"content", QJsonObject{{"senderDeviceId", device}, {"senderKeyVersion", 2}}}};
        };
        const auto acknowledge = [&c](const QJsonObject& sent, const QJsonObject& result) {
            const qsizetype before = c.transport.requests.size();
            Access::acknowledge(*c.client, sent, result);
            QTRY_VERIFY(c.transport.requests.size() > before);
        };
        const QJsonObject ok{{"ok", true}};
        acknowledge(command("command-turn", "turn.start", "phone-1"), ok);
        // A redelivery keeps the first outcome.
        acknowledge(command("command-turn", "turn.start", "phone-1"),
                    {{"ok", false}, {"error_code", "duplicate"}});
        // Reads are not recorded; a kind this build does not know is.
        acknowledge(command("command-read", "workspace.files.tree", "phone-1"), ok);
        for (const QString& kind :
             {QStringLiteral("session.output.read"), QStringLiteral("session.media.read"),
              QStringLiteral("session.pending_approvals"), QStringLiteral("session.task.output"),
              QStringLiteral("attachment.stage.chunk")})
            acknowledge(command("command-" + kind, kind, "phone-1"), ok);
        acknowledge(command("command-future", "future.kind", "phone-2"),
                    {{"ok", false}, {"error_code", "unsupported_command"}});

        const auto all = c.journal.remoteCommands({}, 10);
        QVERIFY(all);
        QCOMPARE(all->size(), 2);
        QCOMPARE(all->at(0).commandId, QStringLiteral("command-future"));
        QCOMPARE(all->at(0).resultCode, QStringLiteral("unsupported_command"));
        QCOMPARE(all->at(1).commandId, QStringLiteral("command-turn"));
        QCOMPARE(all->at(1).kind, QStringLiteral("turn.start"));
        QCOMPARE(all->at(1).deviceId, QStringLiteral("phone-1"));
        QCOMPARE(all->at(1).deviceKeyVersion, 2);
        QCOMPARE(all->at(1).sessionId, QStringLiteral("session-synthetic"));
        QCOMPARE(all->at(1).resultCode, QStringLiteral("ok"));

        // The desktop reads them over the local connection, all or one device's.
        const auto audit = [&c](const QJsonObject& payload) {
            QJsonObject reply;
            QString failure = "unanswered";
            c.client->handleLocal("remote.audit", payload,
                                  [&](const QJsonObject& value, const QString& error) {
                                      reply = value;
                                      failure = error;
                                  });
            return std::make_pair(reply, failure);
        };
        QVERIFY(c.client->handles("remote.audit"));
        auto [reply, failure] = audit({{"device_id", "phone-1"}});
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        const QJsonArray entries = reply.value("entries").toArray();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.at(0).toObject().value("command_id").toString(),
                 QStringLiteral("command-turn"));
        QCOMPARE(entries.at(0).toObject().value("device_key_version").toInt(), 2);
        QVERIFY(entries.at(0).toObject().value("session_exists").toBool());
        QCOMPARE(entries.at(0).toObject().value("session_title").toString(),
                 c.journal.sessions().first().title);
        // A deleted conversation is reported as such, so the desktop does not guess a name.
        QVERIFY(c.journal.recordRemoteCommand(
            {"command-gone", "session.delete", "phone-3", 1, "session-gone", "ok", {}}));
        const QJsonObject gone =
            audit({{"device_id", "phone-3"}}).first.value("entries").toArray().at(0).toObject();
        QVERIFY(!gone.value("session_exists").toBool());
        QVERIFY(gone.value("session_title").toString().isEmpty());
        QVERIFY(QDateTime::fromString(entries.at(0).toObject().value("recorded_at").toString(),
                                      Qt::ISODateWithMs)
                    .isValid());
        QCOMPARE(audit({{"limit", 1}}).first.value("entries").toArray().size(), 1);

        // Kept for 90 days.
        QVERIFY(c.journal.recordRemoteCommand(
            {"command-old", "turn.start", "phone-1", 2, "session-synthetic", "ok",
             QDateTime::currentDateTimeUtc().addDays(-LocalJournal::remoteCommandRetentionDays -
                                                     1)}));
        QCOMPARE(c.journal.remoteCommands({}, 10)->size(), 4);
        QVERIFY(c.journal.pruneRetention());
        QCOMPARE(c.journal.remoteCommands({}, 10)->size(), 3);

        // A failed read is reported, not answered as an empty record.
        QSqlDatabase blocker = QSqlDatabase::addDatabase("QSQLITE", "audit-blocker");
        blocker.setDatabaseName(dir.filePath("journal.sqlite"));
        QVERIFY(blocker.open());
        QVERIFY(QSqlQuery(blocker).exec(
            "ALTER TABLE remote_command_audit RENAME TO remote_command_audit_hidden"));
        const auto [unread, readFailure] = audit({});
        QVERIFY(readFailure != "unanswered");
        QVERIFY(!readFailure.isEmpty());
        QVERIFY(!unread.contains("entries"));
        QVERIFY(QSqlQuery(blocker).exec(
            "ALTER TABLE remote_command_audit_hidden RENAME TO remote_command_audit"));
        blocker.close();
        blocker = {};
        QSqlDatabase::removeDatabase("audit-blocker");
    }

    void linkedCheckoutsOfOneBareRepositoryShareTheWorktreeQuota() {
        QTemporaryDir dir;
        const auto git = [](const QString& directory, const QStringList& arguments) {
            return QProcess::execute("git", QStringList{"-C", directory, "-c", "user.name=Fixture",
                                                        "-c", "user.email=fixture@example.invalid",
                                                        "-c", "commit.gpgsign=false"} +
                                                arguments);
        };
        const QString source = dir.filePath("source");
        const QString bare = dir.filePath("repo.git");
        const QString a = dir.filePath("checkout-a");
        const QString b = dir.filePath("checkout-b");
        QVERIFY(QDir().mkpath(source));
        QCOMPARE(git(source, {"init", "-q", "-b", "main"}), 0);
        QCOMPARE(git(source, {"commit", "--allow-empty", "-qm", "initial"}), 0);
        QCOMPARE(git(dir.path(), {"clone", "-q", "--bare", source, bare}), 0);
        QCOMPARE(git(bare, {"worktree", "add", "-q", a, "main"}), 0);
        QCOMPARE(git(bare, {"worktree", "add", "-q", "-b", "other", b, "main"}), 0);
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        QVERIFY(c.journal.rememberWorktree(QFileInfo(a).canonicalFilePath()));
        QVERIFY(c.journal.rememberWorktree(QFileInfo(b).canonicalFilePath()));
        const QByteArray salt(32, 's');
        Access::pathSalt(*c.client, salt);
        CredentialStore phoneStore("bare-repository-phone");
        const QByteArray phone = CloudCrypto::fromBase64Url(
            CloudCrypto(&phoneStore).publicKeys().value("agreementPublicKey").toString());
        const auto create = [&](const QString& checkout, const QString& commandId,
                                const QString& branch) {
            const auto id = [&](const QByteArray& label) {
                return QString::fromLatin1(
                    QMessageAuthenticationCode::hash(
                        label + '\n' + QFileInfo(checkout).canonicalFilePath().toUtf8(), salt,
                        QCryptographicHash::Sha256)
                        .toHex());
            };
            const qsizetype before = c.transport.requests.size();
            Access::worktreeCommand(
                *c.client,
                {{"id", commandId},
                 {"kind", "session.create"},
                 {"attempt", 1},
                 {"workspaceId", "workspace-synthetic"},
                 {"sessionGeneration", 0},
                 {"expectedCatalogRevision", 0},
                 {"expectedLifecycleRevision", 0},
                 {"repositoryId", id("repository")},
                 {"worktreeId", id("worktree")},
                 {"content", QJsonObject{{"recipientDeviceId", "machine-synthetic"},
                                         {"recipientKeyVersion", 1}}}},
                {{"prompt", "Start here"},
                 {"new_worktree", QJsonObject{{"branch", branch}, {"base", "main"}}}},
                phone);
            const QString ack = QStringLiteral("/v2/commands/%1/ack").arg(commandId);
            QString resultCode;
            [&] {
                QTRY_VERIFY(std::any_of(
                    c.transport.requests.cbegin() + before, c.transport.requests.cend(),
                    [&ack](const auto& request) { return request.url().path() == ack; }));
            }();
            for (qsizetype i = before; i < c.transport.requests.size(); ++i) {
                if (c.transport.requests.at(i).url().path() == ack)
                    resultCode = QJsonDocument::fromJson(c.transport.bodies.at(i))
                                     .object()
                                     .value("resultCode")
                                     .toString();
            }
            return resultCode;
        };

        Access::worktreeQuota(*c.client).perHour = 1;
        QCOMPARE(create(a, "command-through-a", "through-a"), QStringLiteral("ok"));
        // The other checkout shares the repository's allowance.
        QCOMPARE(create(b, "command-through-b", "through-b"),
                 QStringLiteral("worktree_quota_exceeded"));
        QVERIFY(git(bare, {"show-ref", "--verify", "--quiet", "refs/heads/through-b"}) != 0);
    }

    void aNewEnrollmentTrustsNoDeviceUntilOneIsApproved() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        c.transport.statusForPath.insert("/v2/machines:enroll", 201);
        c.transport.responseForPath.insert(
            "/v2/machines:enroll",
            R"({"id":"machine-synthetic","workspaceId":"workspace-synthetic","keyVersion":1})");
        bool answered = false;
        Access::enroll(*c.client, [&answered](const QJsonObject&, const QString&) {
            answered = true; // the fake relay has no credential endpoint; enrollment is stored
        });
        QTRY_VERIFY(answered);
        QVERIFY(Access::pins(*c.client).initialized());

        // Phones another Mac of this workspace paired are listed, but not adopted here.
        CredentialStore otherStore("other-macs-phone");
        const QJsonObject other = CloudCrypto(&otherStore).publicKeys();
        Access::configure(*c.client, c.transport);
        c.transport.statusForPath.insert("/v2/devices", 200);
        c.transport.responseForPath.insert(
            "/v2/devices", QJsonDocument(QJsonArray{QJsonObject{{"id", "other-phone"},
                                                                {"keyVersion", 1},
                                                                {"status", "active"},
                                                                {"keys", other}}})
                               .toJson());
        const qsizetype before = c.transport.requests.size();
        Access::distributeKeys(*c.client);
        QTRY_VERIFY(
            std::any_of(c.transport.requests.cbegin() + before, c.transport.requests.cend(),
                        [](const auto& request) { return request.url().path() == "/v2/devices"; }));
        QTest::qWait(100);
        QVERIFY(std::none_of(
            c.transport.requests.cbegin() + before, c.transport.requests.cend(),
            [](const auto& request) { return request.url().path() == "/v2/session-keys:append"; }));
        QVERIFY(!Access::pins(*c.client).trustsKeys("other-phone", 1, other));
    }

    void aNewEnrollmentDropsEarlierPinsAndStoresNothingIfThePinsCannotBeWritten() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        c.transport.statusForPath.insert("/v2/machines:enroll", 201);
        c.transport.responseForPath.insert(
            "/v2/machines:enroll",
            R"({"id":"machine-new","workspaceId":"workspace-synthetic","keyVersion":1})");
        const auto storedMachineId = [] {
            return std::any_of(r03::credentials.keyBegin(), r03::credentials.keyEnd(),
                               [](const QString& key) { return key.endsWith("/machine-id"); });
        };
        QVERIFY(!storedMachineId());
        // Writing the empty pin set fails: no enrollment is stored, so a restart cannot take
        // this Mac for one enrolled before pins and adopt the relay's device list.
        r03::failWritesTo = "device-pins";
        QString failure;
        bool answered = false;
        Access::enroll(*c.client, [&](const QJsonObject&, const QString& error) {
            failure = error;
            answered = true;
        });
        QTRY_VERIFY(answered);
        QCOMPARE(failure, QStringLiteral("synthetic credential write failure"));
        QVERIFY(!storedMachineId());
        QVERIFY(!Access::pins(*c.client).initialized());
        // Nor does it take effect in memory: once storage recovers, this daemon must not sync
        // (and adopt the relay's device list) as an enrollment from before pins.
        QCOMPARE(Access::machineId(*c.client), QStringLiteral("machine-synthetic"));
        r03::failWritesTo.clear();

        // A phone approved under an earlier enrollment is not trusted by the next one.
        CredentialStore phoneStore("earlier-phone");
        const QJsonObject phone = CloudCrypto(&phoneStore).publicKeys();
        Access::pinDevice(*c.client, "earlier-phone", 1, phone);
        answered = false;
        Access::enroll(*c.client,
                       [&answered](const QJsonObject&, const QString&) { answered = true; });
        QTRY_VERIFY(answered);
        QVERIFY(storedMachineId());
        QCOMPARE(Access::machineId(*c.client), QStringLiteral("machine-new"));
        QVERIFY(Access::pins(*c.client).initialized());
        QVERIFY(!Access::pins(*c.client).trustsKeys("earlier-phone", 1, phone));
    }

    // #84: what the credential file keeps for each conversation is bounded by the journal.
    // Settings and Connections name the account that signed this Mac in (ADR 0109).
    void statusNamesTheSignedInAccountUntilSignOut() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        c.transport.statusForPath.insert("/v2/machines:enroll", 201);
        c.transport.responseForPath.insert(
            "/v2/machines:enroll",
            R"({"id":"machine-synthetic","workspaceId":"workspace-synthetic","keyVersion":1})");
        bool answered = false;
        Access::enroll(
            *c.client, [&answered](const QJsonObject&, const QString&) { answered = true; },
            QJsonObject{{"id", "subject-synthetic"},
                        {"username", "synthetic.user"},
                        {"displayName", "Synthetic User"}});
        QTRY_VERIFY(answered);
        const QString service = QStringLiteral("dev.ketplus.daemon.control/");
        QCOMPARE(r03::credentials.value(service + "account-name"), QByteArray("Synthetic User"));
        QJsonObject status;
        c.client->handleLocal(
            "remote.status", {},
            [&status](const QJsonObject& value, const QString&) { status = value; });
        const QJsonObject account = status.value("account").toObject();
        QCOMPARE(account.value("display_name").toString(), QStringLiteral("Synthetic User"));
        QCOMPARE(account.value("username").toString(), QStringLiteral("synthetic.user"));
        // The relay's subject id and tokens are not reported.
        QVERIFY(!QJsonDocument(status).toJson().contains("subject-synthetic"));

        // Stored with the enrollment, so a restart reads it back.
        QCOMPARE(r03::credentials.value(service + "account-username"),
                 QByteArray("synthetic.user"));

        QString error = "unanswered";
        c.client->handleLocal("remote.sign_out", {},
                              [&](const QJsonObject& value, const QString& detail) {
                                  error = detail;
                                  status = value;
                              });
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!status.contains("account"));
        QVERIFY(!r03::credentials.contains(service + "account-name"));
        QVERIFY(!r03::credentials.contains(service + "account-username"));
    }

    void signingOutRemovesTheEnrollmentAndEveryCursorInOneRewrite() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        const QString service = QStringLiteral("dev.ketplus.daemon.control/");
        r03::credentials.insert(service + "machine-id", "machine-synthetic");
        r03::credentials.insert(service + "workspace-id", "workspace-synthetic");
        r03::credentials.insert(service + "control-url",
                                CloudControlClient::configuredControlUrl().toString().toUtf8());
        for (int session = 0; session < 5; ++session)
            r03::credentials.insert(
                service + QStringLiteral("uploaded-cursor-gone-%1-1").arg(session), "9");
        r03::credentials.insert(service + "uploaded-cursor-session-synthetic-1", "42");
        r03::credentials.insert(service + "account-name", "Synthetic User");
        r03::credentials.insert(service + "account-username", "synthetic.user");
        CloudControlClient client(&c.journal, &c.host);
        // A restart reads the account back with the enrollment (ADR 0109).
        QJsonObject status;
        client.handleLocal("remote.status", {},
                           [&status](const QJsonObject& value, const QString&) { status = value; });
        QCOMPARE(status.value("account").toObject().value("display_name").toString(),
                 QStringLiteral("Synthetic User"));
        r03::rewrites = 0;
        QString error = "unanswered";
        client.handleLocal("remote.sign_out", {},
                           [&error](const QJsonObject&, const QString& detail) { error = detail; });
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(r03::rewrites, 1);
        const auto stored = [&service](const QString& prefix) {
            return std::any_of(
                r03::credentials.keyBegin(), r03::credentials.keyEnd(),
                [&](const QString& key) { return key.startsWith(service + prefix); });
        };
        QVERIFY(!stored("uploaded-cursor-"));
        QVERIFY(!stored("machine-id"));
        QVERIFY(!stored("account-"));
        // The workspace its content keys were made in outlives the enrollment.
        QCOMPARE(r03::credentials.value(service + "session-keys-workspace"),
                 QByteArray("workspace-synthetic"));
    }

    void deletedConversationsLoseTheirKeysWrapsAndCursors() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        const QString service = QStringLiteral("dev.ketplus.daemon.control/");
        const auto accounts = [&service](const QString& session) {
            return QStringList{service + QStringLiteral("session-key-%1-1-1")
                                             .arg(QString::fromLatin1(
                                                 QCryptographicHash::hash(
                                                     session.toUtf8(), QCryptographicHash::Sha256)
                                                     .toHex())),
                               service + QStringLiteral("wrapped-%1-1-mobile-1-1").arg(session),
                               service + QStringLiteral("uploaded-cursor-%1-1").arg(session)};
        };
        // "session" is a prefix of the live id: its entries must not pass for the live one's.
        for (const QString& gone : {QStringLiteral("session-gone"), QStringLiteral("session")}) {
            for (const QString& account : accounts(gone))
                r03::credentials.insert(account, "stale");
        }
        for (const QString& account : accounts("session-synthetic"))
            r03::credentials.insert(account, "live");
        r03::credentials.insert(service + "path-id-salt", "salt");
        // Keys and wraps set aside for a workspace this Mac left are bounded the same way.
        const QString setAside =
            QStringLiteral("workspace-keys-%1-")
                .arg(QString::fromLatin1(
                    QCryptographicHash::hash("workspace-left", QCryptographicHash::Sha256)
                        .toHex()));
        const QString goneAside = service + setAside + "wrapped-session-gone-1-mobile-1-1";
        const QString liveAside = service + setAside + "wrapped-session-synthetic-1-mobile-1-1";
        r03::credentials.insert(goneAside, "stale");
        r03::credentials.insert(liveAside, "live");

        // A conversation deleted while the daemon was not running is swept at start.
        CloudControlClient client(&c.journal, &c.host);
        client.start();
        for (const QString& gone : {QStringLiteral("session-gone"), QStringLiteral("session")}) {
            for (const QString& account : accounts(gone))
                QVERIFY2(!r03::credentials.contains(account), qPrintable(account));
        }
        for (const QString& account : accounts("session-synthetic"))
            QVERIFY2(r03::credentials.contains(account), qPrintable(account));
        QVERIFY(r03::credentials.contains(service + "path-id-salt"));
        QVERIFY(!r03::credentials.contains(goneAside));
        QVERIFY(r03::credentials.contains(liveAside));

        // One deleted now loses them as the deletion is announced.
        QVERIFY(c.journal.deleteSession("session-synthetic", 1));
        emit c.host.eventReady(
            {{"session_id", "session-synthetic"}, {"event_type", "session.deleted"}});
        for (const QString& account : accounts("session-synthetic"))
            QVERIFY2(!r03::credentials.contains(account), qPrintable(account));
        QVERIFY(r03::credentials.contains(service + "path-id-salt"));
    }

    void enrollingIntoAnotherWorkspaceMakesNewContentKeys() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        c.start();
        const auto enrollInto = [&c](const QString& workspace) {
            c.transport.statusForPath.insert("/v2/machines:enroll", 201);
            c.transport.responseForPath.insert(
                "/v2/machines:enroll",
                QStringLiteral(R"({"id":"machine-%1","workspaceId":"%1","keyVersion":1})")
                    .arg(workspace)
                    .toUtf8());
            QString failure = "unanswered";
            Access::enroll(*c.client, [&failure](const QJsonObject&, const QString& error) {
                failure = error;
            });
            QTest::qWaitFor([&failure] { return failure != "unanswered"; });
            return failure;
        };
        enrollInto("workspace-a");
        const QByteArray first = Access::crypto(*c.client).sessionKey("session-synthetic", 1);
        QCOMPARE(first.size(), 32);
        const QString wrap =
            QStringLiteral("dev.ketplus.daemon.control/wrapped-session-synthetic-1-phone-a-1");
        r03::credentials.insert(wrap, "wrapped for a phone of workspace-a");
        const QString cursor =
            QStringLiteral("dev.ketplus.daemon.control/uploaded-cursor-session-synthetic-1");
        r03::credentials.insert(cursor, "42");

        // Signing in to the same workspace again keeps them: the relay holds their wraps.
        enrollInto("workspace-a");
        QCOMPARE(Access::crypto(*c.client).sessionKey("session-synthetic", 1), first);
        QVERIFY(r03::credentials.contains(wrap));

        // Another workspace's devices never received these keys, and the old one's must not
        // read what is uploaded next. If that cannot be stored, the daemon keeps running the
        // enrollment it had, with its keys; a restart into the stored enrollment replaces them.
        r03::failWritesTo = "session-keys-workspace";
        QVERIFY(!enrollInto("workspace-b").isEmpty());
        QCOMPARE(Access::machineId(*c.client), QStringLiteral("machine-workspace-a"));
        QCOMPARE(Access::crypto(*c.client).sessionKey("session-synthetic", 1), first);
        QVERIFY(r03::credentials.contains(wrap));
        r03::failWritesTo.clear();
        {
            // This build's relay, as the enrollment would have been stored outside the test.
            r03::credentials.insert("dev.ketplus.daemon.control/control-url",
                                    CloudControlClient::configuredControlUrl().toString().toUtf8());
            CloudControlClient restarted(&c.journal, &c.host);
            QCOMPARE(Access::machineId(restarted), QStringLiteral("machine-workspace-b"));
            QVERIFY(!r03::credentials.contains(wrap));
            // Progress uploaded to workspace-a would resume workspace-b mid-history.
            QVERIFY(!r03::credentials.contains(cursor));
            QVERIFY(Access::crypto(restarted).sessionKey("session-synthetic", 1) != first);
        }
        const QByteArray second = Access::crypto(*c.client).sessionKey("session-synthetic", 1);
        QCOMPARE(second.size(), 32);
        r03::credentials.insert(wrap, "wrapped for a phone of workspace-b");

        // Returning to a workspace takes back its keys and wraps: its relay holds those wraps
        // under the same ids, and its history is encrypted with those keys.
        enrollInto("workspace-a");
        QCOMPARE(Access::machineId(*c.client), QStringLiteral("machine-workspace-a"));
        QCOMPARE(Access::crypto(*c.client).sessionKey("session-synthetic", 1), first);
        QCOMPARE(r03::credentials.value(wrap), QByteArray("wrapped for a phone of workspace-a"));
        enrollInto("workspace-b");
        QCOMPARE(Access::crypto(*c.client).sessionKey("session-synthetic", 1), second);
        QCOMPARE(r03::credentials.value(wrap), QByteArray("wrapped for a phone of workspace-b"));

        // A workspace never used here starts with no key.
        enrollInto("workspace-c");
        QVERIFY(!r03::credentials.contains(wrap));
        const QByteArray third = Access::crypto(*c.client).sessionKey("session-synthetic", 1);
        QCOMPARE(third.size(), 32);
        QVERIFY(third != first && third != second);
    }

    // sessions() reads as empty when the journal cannot be read; that is no proof that every
    // conversation was deleted.
    void aFailedJournalReadRemovesNoConversationCredentials() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        const QString service = QStringLiteral("dev.ketplus.daemon.control/");
        const QStringList accounts{service +
                                       QStringLiteral("wrapped-session-synthetic-1-mobile-1-1"),
                                   service + QStringLiteral("uploaded-cursor-session-synthetic-1")};
        for (const QString& account : accounts)
            r03::credentials.insert(account, "live");
        const QByteArray key = [&c] {
            CloudControlClient owner(&c.journal, &c.host);
            return Access::crypto(owner).sessionKey("session-synthetic", 1);
        }();
        QCOMPARE(key.size(), 32);
        {
            QSqlDatabase blocker = QSqlDatabase::addDatabase("QSQLITE", "journal-blocker");
            blocker.setDatabaseName(dir.filePath("journal.sqlite"));
            QVERIFY(blocker.open());
            QVERIFY(QSqlQuery(blocker).exec("ALTER TABLE sessions RENAME TO sessions_hidden"));
            CloudControlClient client(&c.journal, &c.host);
            client.start();
            QVERIFY(!Access::error(client).isEmpty());
            QVERIFY(QSqlQuery(blocker).exec("ALTER TABLE sessions_hidden RENAME TO sessions"));
            blocker.close();
        }
        QSqlDatabase::removeDatabase("journal-blocker");
        for (const QString& account : accounts)
            QVERIFY2(r03::credentials.contains(account), qPrintable(account));
        CloudControlClient after(&c.journal, &c.host);
        QCOMPARE(Access::crypto(after).sessionKey("session-synthetic", 1), key);
    }

    // An enrollment from before #84 records its workspace at start, so a later sign-out and
    // sign-in to another workspace still replaces its keys.
    void anEarlierEnrollmentRecordsItsWorkspaceAtStart() {
        QTemporaryDir dir;
        Context c(dir.filePath("journal.sqlite"));
        const QString service = QStringLiteral("dev.ketplus.daemon.control/");
        r03::credentials.insert(service + "machine-id", "machine-legacy");
        r03::credentials.insert(service + "workspace-id", "workspace-legacy");
        CloudControlClient client(&c.journal, &c.host);
        QCOMPARE(r03::credentials.value(service + "session-keys-workspace"),
                 QByteArray("workspace-legacy"));
    }

    void envelopesUseOnlyTheirOwnKeyEpochAndExactNonceAndTagSizes() {
        CredentialStore store("envelope-format");
        CloudCrypto crypto(&store);
        const QByteArray signing =
            CloudCrypto::fromBase64Url(crypto.publicKeys().value("signingPublicKey").toString());
        QString error;
        QVERIFY(crypto.encrypt("text", "aad", "session", 1, 2, "machine", 1, &error).isEmpty());
        QCOMPARE(error, QStringLiteral("Session key epoch 2 does not exist."));
        const QJsonObject sealed = crypto.encrypt("text", "aad", "session", 1, 1, "machine", 1);
        QCOMPARE(crypto.decrypt(sealed, "aad", signing, "session", 1), QByteArray("text"));
        // A truncated tag, signed by the real sender, is refused by its size, not by luck.
        const QByteArray nonce = CloudCrypto::fromBase64Url(sealed.value("nonce").toString());
        const QByteArray ciphertext =
            CloudCrypto::fromBase64Url(sealed.value("ciphertext").toString());
        const QByteArray tag = CloudCrypto::fromBase64Url(sealed.value("tag").toString()).left(12);
        QByteArray input = "aad";
        input.append('\0');
        input.append(nonce);
        input.append(ciphertext);
        input.append(tag);
        QJsonObject truncated = sealed;
        truncated.insert("tag", QString::fromLatin1(CloudCrypto::base64Url(tag)));
        truncated.insert("signature",
                         QString::fromLatin1(CloudCrypto::base64Url(crypto.sign(input))));
        error.clear();
        QVERIFY(crypto.decrypt(truncated, "aad", signing, "session", 1, &error).isEmpty());
        QCOMPARE(error, QStringLiteral("The encrypted envelope is malformed."));
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
