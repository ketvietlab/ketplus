#include "CloudPrivacyTestAccess.h"
#include "FakeTransport.h"
#include "cloud/CredentialBackend.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

using namespace ketplus::daemon;
using Access = CloudPrivacyTestAccess;
namespace {
// This target links the real CredentialStore and replaces only the OS adapter.
class CountingBackend final : public CredentialBackend {
  public:
    QHash<QString, QByteArray> records;
    int reads{0};
    int writes{0};
    int delayMs{0};
    bool failRead{false};
    bool failWrite{false};
    std::function<void()> firstRead;
    Result read(const QString& service, QByteArray* value, QString* error) override {
        ++reads;
        if (auto started = std::exchange(firstRead, {}))
            started();
        if (delayMs)
            QThread::msleep(delayMs);
        if (failRead) {
            if (error)
                *error = "Synthetic backend unavailable";
            return Result::Error;
        }
        *value = records.value(service);
        return records.contains(service) ? Result::Found : Result::Missing;
    }
    bool write(const QString& service, const QByteArray& value, QString* error) override {
        ++writes;
        if (failWrite) {
            if (error)
                *error = "Synthetic backend write denied";
            return false;
        }
        records.insert(service, value);
        return true;
    }
};
std::shared_ptr<CountingBackend> backend;
QString keyAccount(const QString& id) {
    return "session-key-" +
           QString::fromLatin1(
               QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex()) +
           "-1-1";
}
struct Fixture {
    QTemporaryDir directory;
    LocalJournal journal{directory.filePath("journal.sqlite")};
    CodexAdapter provider{QStringLiteral("/nonexistent/synthetic-provider")};
    SessionHost host{&journal, &provider};
    r03::Transport transport;
    std::unique_ptr<CloudControlClient> client;
    QJsonObject device;
    QList<QJsonArray> batches;
    QHash<QString, QJsonObject> metadata;
    Fixture(int accounts, int sessions) {
        qputenv("KETPLUS_DAEMON_DATA_DIR", directory.path().toUtf8());
        backend = std::make_shared<CountingBackend>();
        client = std::make_unique<CloudControlClient>(&journal, &host);
        Access::configure(*client, transport);
        CredentialStore store;
        CredentialStore deviceStore("t15-synthetic-device");
        device = {{"id", "device-synthetic"},
                  {"keyVersion", 1},
                  {"status", "active"},
                  {"keys", CloudCrypto(&deviceStore).publicKeys()}};
        Access::pinDevice(*client, "device-synthetic", 1, device["keys"].toObject());
        auto envelope = Access::crypto(*client).wrapSessionKey("template", 1, 1, device,
                                                               "machine-synthetic", 1);
        if (!CloudCrypto::validWrappedSessionKey(envelope))
            qFatal("Synthetic wrap setup failed");
        QJsonArray snapshots;
        for (int i = 0; i < sessions; ++i) {
            const auto id = QString("session-%1").arg(i);
            StoredSession session;
            session.id = id;
            session.generation = 1;
            session.state = "ready";
            session.adapter = "codex";
            session.worktreePath = directory.path();
            if (!journal.upsertSession(session))
                qFatal("Synthetic journal setup failed");
            snapshots.append(QJsonObject{
                {"id", id}, {"generation", 1}, {"lastEventCursor", 0}, {"currentTurnSequence", 0}});
            metadata.insert(id, {{"title", "Synthetic metadata"}});
        }
        batches.append(snapshots);
        // Cached envelopes are structurally valid synthetic fixtures, not a crypto benchmark.
        if (!store.update([&](auto& values) {
                for (int i = 0; i < sessions; ++i) {
                    const auto id = QString("session-%1").arg(i);
                    values.insert(keyAccount(id), QByteArray(32, 'k'));
                    values.insert(QString("wrapped-%1-1-device-synthetic-1").arg(id),
                                  QJsonDocument(envelope).toJson(QJsonDocument::Compact));
                }
                for (int i = 0; values.size() < accounts; ++i)
                    values.insert(QString("synthetic-retained-%1").arg(i), QByteArray(420, 'w'));
            }))
            qFatal("Synthetic record setup failed");
        reply("/v2/devices", QJsonArray{device});
        reply("/v2/device-keys/device-synthetic/1", QJsonObject{{"keys", device["keys"]}});
        reply("/v2/session-keys:append", QJsonObject{}, 202);
        reply("/v2/pairing/tickets/t15",
              QJsonObject{{"id", "t15"},
                          {"status", "claimed"},
                          {"claimedDevice", device},
                          {"claimDigest", DevicePins::claimDigest("t15", device)}});
        reply("/v2/pairing/tickets/t15/approve", QJsonObject{});
        backend->reads = 0;
        backend->writes = 0;
    }
    ~Fixture() { qunsetenv("KETPLUS_DAEMON_DATA_DIR"); }
    void reply(const QString& path, const QJsonValue& body, int status = 200) {
        transport.statusForPath[path] = status;
        transport.responseForPath[path] = body.isArray() ? QJsonDocument(body.toArray()).toJson()
                                                         : QJsonDocument(body.toObject()).toJson();
    }
    bool requested(const QString& path) const {
        for (const auto& request : transport.requests)
            if (request.url().path() == path)
                return true;
        return false;
    }
};
} // namespace
namespace ketplus::daemon {
std::shared_ptr<CredentialBackend> nativeCredentialBackend() { return backend; }
} // namespace ketplus::daemon

class CredentialBatchTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void batchIsAtomicAndRefreshesBetweenOperations() {
        Fixture f(200, 2);
        CredentialStore store;
        CredentialStore concurrent;
        QString error;
        bool contended = false;
        QVERIFY(store.batch(
            [&](CredentialStore& view, QString* failure) {
                for (int i = 0; i < 100; ++i)
                    if (view.read(keyAccount("session-0"), failure).size() != 32)
                        return false;
                QProcess child;
                child.start(QCoreApplication::applicationFilePath(), {"--probe-batch-lock"});
                const bool lockedAcrossProcesses = child.waitForFinished(5000) &&
                                                   child.exitStatus() == QProcess::NormalExit &&
                                                   child.exitCode() == 0;
                if (!lockedAcrossProcesses) {
                    *failure = "Synthetic child did not observe the held lock";
                    return false;
                }
                QString busy;
                contended =
                    !concurrent.write("other-process", "uncommitted", &busy) && !busy.isEmpty();
                return view.write("batched", "one", failure);
            },
            &error));
        QVERIFY(contended);
        QCOMPARE(backend->reads, 1);
        QCOMPARE(backend->writes, 1);
        QVERIFY(concurrent.write("other-process", "committed", &error));
        QVERIFY(store.batch(
            [&](CredentialStore& view, QString* failure) {
                return view.read("other-process", failure) == "committed";
            },
            &error));
        const auto record = backend->records;
        QVERIFY(!store.batch(
            [&](CredentialStore& view, QString* failure) {
                view.write("batched", "discarded", failure);
                *failure = "Synthetic operation abort";
                return false;
            },
            &error));
        QCOMPARE(error, "Synthetic operation abort");
        QVERIFY(backend->records == record);
        backend->failWrite = true;
        QVERIFY(
            !store.batch([&](CredentialStore& view,
                             QString* failure) { return view.write("batched", "denied", failure); },
                         &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(backend->records == record);
    }
    void failedMetadataCommitDoesNotCacheUnstoredKeys() {
        Fixture f(200, 2);
        CredentialStore store;
        QVERIFY(store.remove(keyAccount("session-0")));
        backend->failWrite = true;
        QVERIFY(Access::sealMetadata(*f.client, f.batches, f.metadata).isEmpty());
        QVERIFY(!Access::error(*f.client).isEmpty());
        backend->failWrite = false;
        QVERIFY(store.read(keyAccount("session-0")).isEmpty());
        QVERIFY(!Access::sealMetadata(*f.client, f.batches, f.metadata).isEmpty());
        QCOMPARE(store.read(keyAccount("session-0")).size(), 32);
    }
    void deniedReadAndChangedPinsCannotPublishWraps() {
        Fixture f(200, 2);
        backend->failRead = true;
        Access::distributeKeys(*f.client);
        QTRY_VERIFY(!Access::error(*f.client).isEmpty());
        QVERIFY(!f.requested("/v2/session-keys:append"));
        backend->failRead = false;
        Access::distributeKeys(*f.client);
        QTRY_VERIFY(f.requested("/v2/session-keys:append"));
        QCoreApplication::processEvents();
        CredentialStore otherProcess;
        QVERIFY(DevicePins(&otherProcess).unpin("device-synthetic"));
        f.transport.requests.clear();
        f.transport.bodies.clear();
        backend->reads = 0;
        Access::distributeKeys(*f.client);
        QTRY_VERIFY(backend->reads > 0);
        QVERIFY(!f.requested("/v2/session-keys:append"));
    }
    void failedWrapCommitDoesNotApprovePairing() {
        Fixture f(200, 2);
        CredentialStore store;
        QVERIFY(store.remove("wrapped-session-0-1-device-synthetic-1"));
        backend->failWrite = true;
        bool completed = false;
        QString error;
        Access::approvePairing(*f.client, "t15", DevicePins::claimDigest("t15", f.device),
                               [&](const QJsonObject&, const QString& failure) {
                                   error = failure;
                                   completed = true;
                               });
        QTRY_VERIFY(completed);
        QVERIFY(!error.isEmpty());
        QVERIFY(!f.requested("/v2/pairing/tickets/t15/approve"));
        backend->failWrite = false;
        QVERIFY(store.read("wrapped-session-0-1-device-synthetic-1").isEmpty());
    }
    void uncachedKeysUseOneCommit_data() {
        QTest::addColumn<int>("sessions");
        QTest::newRow("two") << 2;
        QTest::newRow("twenty") << 20;
    }
    void uncachedKeysUseOneCommit() {
        QFETCH(int, sessions);
        Fixture f(200, sessions);
        CredentialStore store;
        QVERIFY(store.removeIf([](const QString& account) {
            return account.startsWith("session-key-") || account.startsWith("wrapped-");
        }));
        backend->reads = backend->writes = 0;
        QVERIFY(!Access::sealMetadata(*f.client, f.batches, f.metadata).isEmpty());
        QCOMPARE(backend->reads, 1);
        QCOMPARE(backend->writes, 1);
        backend->reads = backend->writes = 0;
        Access::distributeKeys(*f.client);
        QTRY_VERIFY(f.requested("/v2/session-keys:append"));
        QVERIFY(backend->reads <= 3);
        QCOMPARE(backend->writes, 1);
    }
    void sealingABacklogReadsKeysOnce() {
        backend = std::make_shared<CountingBackend>();
        CredentialStore store;
        CloudCrypto crypto(&store);
        QString error;
        QVERIFY(
            !crypto.encrypt("first", "aad", "session-backlog", 1, 1, "machine-synthetic", 1, &error)
                 .isEmpty());
        QVERIFY(!crypto.sign("first", &error).isEmpty());
        QVERIFY2(error.isEmpty(), qPrintable(error));
        backend->reads = 0;
        backend->writes = 0;
        // Each OS read is a locked fetch of the whole record; a backlog of a hundred events
        // read it twice per event on the daemon's only thread.
        for (int i = 0; i < 100; ++i) {
            QVERIFY(!crypto
                         .encrypt(QByteArray::number(i), "aad", "session-backlog", 1, 1,
                                  "machine-synthetic", 1, &error)
                         .isEmpty());
            QVERIFY(!crypto.sign(QByteArray::number(i), &error).isEmpty());
        }
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(backend->reads, 0);
        QCOMPARE(backend->writes, 0);
    }
    void forgottenKeysAreReadAgain() {
        backend = std::make_shared<CountingBackend>();
        CredentialStore store;
        CloudCrypto crypto(&store);
        const auto account = keyAccount("session-moved");
        QVERIFY(store.write(account, QByteArray(32, 'a')));
        QCOMPARE(crypto.sessionKey("session-moved", 1), QByteArray(32, 'a'));
        // Adopting another workspace puts that workspace's key under the same account.
        QVERIFY(store.write(account, QByteArray(32, 'b')));
        QCOMPARE(crypto.sessionKey("session-moved", 1), QByteArray(32, 'a'));
        crypto.forgetKeys();
        QCOMPARE(crypto.sessionKey("session-moved", 1), QByteArray(32, 'b'));
    }
    void signingOutForgetsKeys() {
        Fixture f(16, 1);
        auto& crypto = Access::crypto(*f.client);
        QCOMPARE(crypto.sessionKey("session-0", 1), QByteArray(32, 'k'));
        CredentialStore store;
        QVERIFY(store.write(keyAccount("session-0"), QByteArray(32, 'n')));
        bool finished = false;
        Access::signOut(*f.client, [&](const QJsonObject&, const QString&) { finished = true; });
        QTRY_VERIFY(finished);
        QCOMPARE(crypto.sessionKey("session-0", 1), QByteArray(32, 'n'));
    }
    void aBacklogUploadGivesTheEventLoopBack() {
        constexpr int sessions = 12;
        Fixture f(64, sessions);
        f.reply("/v2/events:append", QJsonObject{}, 202);
        for (int i = 0; i < sessions; ++i) {
            QString error;
            QVERIFY2(f.journal.appendEvent(QString("session-%1").arg(i), 1, "user.message",
                                           {{"text", "synthetic"}}, &error) > 0,
                     qPrintable(error));
        }
        const auto sent = [&f] {
            return int(std::count_if(
                f.transport.requests.cbegin(), f.transport.requests.cend(),
                [](const auto& request) { return request.url().path() == "/v2/events:append"; }));
        };
        // Every session's first upload reads its cursor and key from the slow store.
        backend->delayMs = 20;
        Access::uploadAll(*f.client);
        QVERIFY2(sent() < sessions, "One pass must not seal every session's backlog at once");
        QTRY_COMPARE_WITH_TIMEOUT(sent(), sessions, 60000);
    }
    void operationReads_data() {
        QTest::addColumn<int>("accounts");
        QTest::addColumn<int>("sessions");
        QTest::addColumn<QString>("operation");
        const auto sizes = qEnvironmentVariableIsSet("KETPLUS_CREDENTIAL_MEASURE")
                               ? QList<int>{2000, 8000, 20000}
                               : QList<int>{200};
        const auto live = qEnvironmentVariableIsSet("KETPLUS_CREDENTIAL_MEASURE")
                              ? QList<int>{100}
                              : QList<int>{2, 20};
        for (const auto accounts : sizes)
            for (const auto sessions : live)
                for (const auto* operation :
                     {"heartbeat_metadata", "distribution", "pairing", "command_auth"})
                    QTest::newRow(
                        qPrintable(QString("%1-%2-%3").arg(operation).arg(accounts).arg(sessions)))
                        << accounts << sessions << QString(operation);
    }
    void operationReads() {
        QFETCH(int, accounts);
        QFETCH(int, sessions);
        QFETCH(QString, operation);
        Fixture f(accounts, sessions);
        SessionHost stopHost(&f.journal, &f.provider);
        backend->reads = 0;
        backend->writes = 0;
        backend->delayMs = 2;
        QElapsedTimer elapsed;
        elapsed.start();
        double queuedStopMs = -1;
        QJsonObject stopResult;
        backend->firstRead = [&] {
            QTimer::singleShot(0, this, [&] {
                // Measure dispatch of an idle-session Stop, not provider cancellation latency.
                stopResult = stopHost.handleCommand({{"message_type", "session.interrupt"},
                                                     {"message_id", "synthetic-stop-command"},
                                                     {"session_id", "session-0"},
                                                     {"session_generation", 1},
                                                     {"payload", QJsonObject{}}},
                                                    "synthetic-stop");
                queuedStopMs = double(elapsed.nsecsElapsed()) / 1e6;
            });
        };
        QString error;
        if (operation == "heartbeat_metadata") {
            const auto sealed = Access::sealMetadata(*f.client, f.batches, f.metadata);
            QCOMPARE(sealed.size(), 1);
            QCOMPARE(sealed.front().size(), sessions);
            QVERIFY(sealed.front().at(0).toObject().contains("encryptedMetadata"));
        } else if (operation == "distribution") {
            Access::distributeKeys(*f.client);
            QTRY_VERIFY_WITH_TIMEOUT(f.requested("/v2/session-keys:append"), 120000);
        } else if (operation == "pairing") {
            bool finished = false;
            Access::approvePairing(*f.client, "t15", DevicePins::claimDigest("t15", f.device),
                                   [&](const QJsonObject&, const QString& failure) {
                                       error = failure;
                                       finished = true;
                                   });
            QTRY_VERIFY_WITH_TIMEOUT(finished, 120000);
        } else {
            bool finished = false;
            Access::senderKeys(*f.client, "device-synthetic", 1,
                               [&](const QJsonObject& keys, const QString& failure) {
                                   error = failure;
                                   finished = !keys.isEmpty();
                               });
            QTRY_VERIFY_WITH_TIMEOUT(finished, 120000);
        }
        QTRY_VERIFY_WITH_TIMEOUT(queuedStopMs >= 0, 120000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(stopResult.value("error_code").toString(), "turn_not_running");
        const auto row =
            QJsonDocument(QJsonObject{{"operation", operation},
                                      {"accounts", accounts},
                                      {"active_sessions", sessions},
                                      {"backend", "synthetic_full_record"},
                                      {"delay_ms", 2},
                                      {"backend_fetches", backend->reads},
                                      {"backend_writes", backend->writes},
                                      {"elapsed_ms", double(elapsed.nsecsElapsed()) / 1e6},
                                      {"idle_stop_dispatch_ms", queuedStopMs},
                                      {"stop_result", stopResult.value("error_code")}})
                .toJson(QJsonDocument::Compact);
        qInfo().noquote() << row;
        QVERIFY2(backend->reads <= 8, "Operation must not fetch the full record per session");
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    if (app.arguments().contains("--probe-batch-lock")) {
        backend = std::make_shared<CountingBackend>();
        CredentialStore store;
        QString error;
        const bool written = store.write("synthetic-child", "must-not-commit", &error);
        return !written && error.contains("busy") && backend->reads == 0 ? 0 : 2;
    }
    CredentialBatchTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "CredentialBatchTest.moc"
