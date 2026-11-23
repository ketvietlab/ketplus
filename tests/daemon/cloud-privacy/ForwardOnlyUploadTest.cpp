// events.forward_only/1 in a heartbeat answer: the daemon stops uploading journaled events and,
// once it is withdrawn, resumes after the journal head without uploading the events in between.
#include "CloudPrivacyTestAccess.h"
#include "FakeTransport.h"
#include "MemoryCredentials.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;
using Access = CloudPrivacyTestAccess;
namespace {
const QString eventsPath = QStringLiteral("/v2/events:append");

struct Context {
    LocalJournal journal;
    CodexAdapter provider{"/nonexistent/forward-only-fake-provider"};
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
    explicit Context(const QString& path) : journal(path), host(seed(journal), &provider) {
        transport.statusForPath.insert(eventsPath, 202);
        transport.statusForPath.insert("/v2/daemon/heartbeat", 202);
        transport.responseForPath.insert("/v2/daemon/heartbeat", R"({"ok":true})");
    }
    void start() {
        client = std::make_unique<CloudControlClient>(&journal, &host);
        Access::configure(*client, transport);
    }
    qint64 append(int count = 1) {
        qint64 cursor = 0;
        for (int i = 0; i < count; ++i) {
            QString error;
            cursor = journal.appendEvent("session-synthetic", 1, "user.message",
                                         {{"text", "synthetic"}}, &error);
            if (cursor <= 0)
                qFatal("append failed: %s", qPrintable(error));
        }
        return cursor;
    }
    // The relay accepts every event a request carries.
    void acceptThrough(qint64 cursor) {
        transport.responseForPath.insert(
            eventsPath,
            QJsonDocument(QJsonObject{{"acceptedThrough",
                                       QJsonArray{QJsonObject{{"sessionId", "session-synthetic"},
                                                              {"sessionGeneration", 1},
                                                              {"cursor", cursor}}}}})
                .toJson(QJsonDocument::Compact));
    }
    void answer(bool forwardOnly) {
        QJsonArray capabilities{"transport.webrtc/1"};
        if (forwardOnly)
            capabilities.append("events.forward_only/1");
        Access::heartbeatAnswer(*client,
                                {{"compatibility", QJsonObject{{"capabilities", capabilities}}}});
    }
    // Cursors of every event uploaded so far, in order.
    QList<qint64> uploaded() const {
        QList<qint64> cursors;
        for (int i = 0; i < transport.requests.size(); ++i) {
            if (transport.requests.at(i).url().path() != eventsPath)
                continue;
            for (const QJsonValue& event :
                 QJsonDocument::fromJson(transport.bodies.at(i)).object().value("events").toArray())
                cursors.append(event.toObject().value("cursor").toInteger());
        }
        return cursors;
    }
    int requestsTo(const QString& path) const {
        return int(std::count_if(transport.requests.cbegin(), transport.requests.cend(),
                                 [&](const auto& r) { return r.url().path() == path; }));
    }
};
} // namespace

class ForwardOnlyUploadTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { r03::resetCredentials(); }

    void forwardOnlyStopsUploadsButNotHeartbeats() {
        QTemporaryDir dir;
        Context c(dir.filePath("on.sqlite"));
        c.start();
        c.append(3);
        c.transport.responseForPath.insert(
            "/v2/daemon/heartbeat",
            R"({"ok":true,"compatibility":{"capabilities":["events.forward_only/1"]}})");
        Access::heartbeat(*c.client, {QJsonArray{}});
        QTRY_VERIFY(Access::heartbeatIdle(*c.client));
        QVERIFY(Access::eventsForwardOnly(*c.client));
        Access::uploadAll(*c.client);
        Access::upload(*c.client);
        QCoreApplication::processEvents();
        QCOMPARE(c.requestsTo(eventsPath), 0);
        QCOMPARE(c.requestsTo("/v2/daemon/heartbeat"), 1);
        // The heartbeat itself does not advertise the capability.
        const auto body = QJsonDocument::fromJson(c.transport.bodies.first()).object();
        QVERIFY(!body.value("capabilities").toArray().contains("events.forward_only/1"));
        // Without the capability, the next heartbeat answer resumes uploads.
        c.transport.responseForPath.insert("/v2/daemon/heartbeat", R"({"ok":true})");
        Access::heartbeat(*c.client, {QJsonArray{}});
        QTRY_VERIFY(Access::heartbeatIdle(*c.client));
        QVERIFY(!Access::eventsForwardOnly(*c.client));
    }

    void withdrawingResumesAfterTheJournalHead() {
        QTemporaryDir dir;
        Context c(dir.filePath("withdraw.sqlite"));
        c.start();
        c.acceptThrough(2);
        c.append(2);
        Access::uploadAll(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(c.uploaded(), (QList<qint64>{1, 2}));
        c.answer(true);
        const qint64 head = c.append(3);
        Access::uploadAll(*c.client);
        QCoreApplication::processEvents();
        QCOMPARE(c.uploaded().size(), 2);
        c.answer(false);
        QCOMPARE(Access::uploadedCursor(*c.client), head);
        // Withdrawing deletes nothing: every event journaled while forward-only stays readable.
        QCOMPARE(c.journal.eventsAfter("session-synthetic", 1, 2).size(), 3);
        c.acceptThrough(head + 1);
        c.append();
        Access::uploadAll(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(c.uploaded(), (QList<qint64>{1, 2, head + 1}));
    }

    void togglingNeverUploadsAnEventTwice() {
        QTemporaryDir dir;
        Context c(dir.filePath("toggle.sqlite"));
        c.start();
        QList<qint64> expected;
        for (int round = 0; round < 4; ++round) {
            c.answer(true);
            c.answer(true);
            c.append(2);
            Access::uploadAll(*c.client);
            QCoreApplication::processEvents();
            c.answer(false);
            c.answer(false);
            const qint64 cursor = c.append();
            expected.append(cursor);
            c.acceptThrough(cursor);
            Access::uploadAll(*c.client);
            Access::uploadAll(*c.client);
            QTRY_VERIFY(Access::idle(*c.client));
        }
        QCOMPARE(c.uploaded(), expected);
    }

    void aRestartStaysForwardOnlyUntilTheNextAnswer() {
        QTemporaryDir dir;
        const auto path = dir.filePath("restart.sqlite");
        {
            Context c(path);
            c.start();
            c.answer(true);
        }
        Context c(path);
        c.start();
        const qint64 head = c.append(2);
        Access::uploadAll(*c.client);
        QCoreApplication::processEvents();
        QCOMPARE(c.requestsTo(eventsPath), 0);
        c.answer(false);
        QCOMPARE(Access::uploadedCursor(*c.client), head);
        Access::uploadAll(*c.client);
        QCoreApplication::processEvents();
        QCOMPARE(c.requestsTo(eventsPath), 0);
        QVERIFY(!r03::credentials.contains("events-forward-only"));
    }

    void withdrawingAdvancesEverySessionInOneCommit() {
        QTemporaryDir dir;
        Context c(dir.filePath("sessions.sqlite"));
        for (const QString id : {"session-second", "session-third"})
            QVERIFY(c.journal.upsertSession(
                {id, "synthetic-worktree", "codex", "thread-" + id, {}, "ready", 1, 0, 1}));
        c.start();
        c.answer(true);
        QHash<QString, qint64> heads;
        for (const QString id : {"session-synthetic", "session-second", "session-third"}) {
            QString error;
            for (int i = 0; i < 2; ++i)
                heads[id] = c.journal.appendEvent(id, 1, "user.message", {{"text", id}}, &error);
            QVERIFY2(heads[id] > 0, qPrintable(error));
        }
        const int before = r03::rewrites;
        c.answer(false);
        QCOMPARE(r03::rewrites - before, 1);
        QVERIFY(!Access::eventsForwardOnly(*c.client));
        for (auto head = heads.cbegin(); head != heads.cend(); ++head)
            QCOMPARE(Access::knownUploadedCursor(*c.client, head.key() + ":1"), head.value());
        QVERIFY(!r03::credentials.contains("events-forward-only"));
        for (auto head = heads.cbegin(); head != heads.cend(); ++head)
            QCOMPARE(Access::uploadedCursor(*c.client, head.key()), head.value());
    }

    void aFailedCommitAdvancesNothing() {
        QTemporaryDir dir;
        Context c(dir.filePath("failed.sqlite"));
        c.start();
        c.answer(true);
        c.append(2);
        r03::failWrites = true;
        c.answer(false);
        r03::failWrites = false;
        QVERIFY(Access::eventsForwardOnly(*c.client));
        QCOMPARE(Access::knownUploadedCursor(*c.client), -1);
        c.answer(false);
        QVERIFY(!Access::eventsForwardOnly(*c.client));
        QCOMPARE(Access::knownUploadedCursor(*c.client), 2);
    }

    void aRestartAfterWithdrawingKeepsEventsUntilARealAcknowledgedUpload() {
        QTemporaryDir dir;
        const auto path = dir.filePath("retention.sqlite");
        qint64 head = 0;
        {
            Context c(path);
            c.start();
            c.acceptThrough(2);
            c.append(2);
            Access::uploadAll(*c.client);
            QTRY_VERIFY(Access::idle(*c.client));
            QCOMPARE(c.uploaded(), (QList<qint64>{1, 2}));
            c.answer(true);
            head = c.append(3);
        }
        // The daemon restarts while forward-only, then the capability is withdrawn.
        Context c(path);
        c.start();
        c.answer(false);
        QCOMPARE(Access::uploadedCursor(*c.client), head);
        // Registering the cloud client must not acknowledge the head for retention: the
        // events journaled while forward-only are still served to other clients as pages.
        Access::uploadAll(*c.client);
        QCoreApplication::processEvents();
        QCOMPARE(c.requestsTo(eventsPath), 0);
        QCOMPARE(c.journal.eventsAfter("session-synthetic", 1, 2).size(), 3);
        // The next acknowledged upload moves retention on.
        c.acceptThrough(head + 1);
        c.append();
        Access::uploadAll(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(c.uploaded().last(), head + 1);
        QCOMPARE(c.journal.eventsAfter("session-synthetic", 1, 2).size(), 0);
    }

    void aChunkDoesNotReadTheCredentialStore() {
        QTemporaryDir dir;
        Context c(dir.filePath("reads.sqlite"));
        c.start();
        c.acceptThrough(1);
        c.append();
        Access::uploadAll(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        c.acceptThrough(3);
        c.append(2);
        r03::reads.clear();
        Access::uploadAll(*c.client);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(c.uploaded(), (QList<qint64>{1, 2, 3}));
        // Sealing reads keys; the upload cursor comes from what this process already knows.
        for (const QString& account : std::as_const(r03::reads))
            QVERIFY2(!account.startsWith("uploaded-cursor-"), qPrintable(account));
    }

    void anUploadInFlightAcrossForwardOnlyDoesNotRewindTheCursor() {
        QTemporaryDir dir;
        Context c(dir.filePath("inflight.sqlite"));
        c.start();
        c.append(2);
        c.acceptThrough(1);
        c.transport.heldPaths.insert(eventsPath);
        Access::uploadAll(*c.client);
        QCOMPARE(c.requestsTo(eventsPath), 1);
        c.answer(true);
        const qint64 head = c.append(2);
        c.answer(false);
        c.transport.heldPaths.clear();
        c.transport.release(eventsPath);
        QTRY_VERIFY(Access::idle(*c.client));
        QCOMPARE(Access::uploadedCursor(*c.client), head);
        Access::uploadAll(*c.client);
        QCoreApplication::processEvents();
        QCOMPARE(c.requestsTo(eventsPath), 1);
    }
};

QTEST_GUILESS_MAIN(ForwardOnlyUploadTest)
#include "ForwardOnlyUploadTest.moc"
