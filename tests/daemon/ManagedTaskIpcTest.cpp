#include "FakeCodex.h"
#include "ipc/LocalIpcServer.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/BackgroundTaskLedger.h"
#include "storage/LocalJournal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace ketplus::daemon;
namespace {
QJsonObject request(const QString& type, const QJsonObject& payload, const QString& owner = {}) {
    QJsonObject result{{"protocol_version", protocol::currentVersion},
                       {"message_id", QUuid::createUuid().toString()},
                       {"message_type", type},
                       {"sent_at", protocol::timestamp()},
                       {"payload", payload}};
    if (!owner.isEmpty()) {
        result.insert("session_id", owner);
        result.insert("session_generation", 1);
    }
    return result;
}
class Client final {
  public:
    QLocalSocket socket;
    QJsonObject call(const QJsonObject& request) {
        socket.write(protocol::encodeFrame(request));
        socket.flush();
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 6000) {
            buffer_.append(socket.readAll());
            while (buffer_.size() >= 4) {
                const auto length = (quint32(uchar(buffer_[0])) << 24) |
                                    (quint32(uchar(buffer_[1])) << 16) |
                                    (quint32(uchar(buffer_[2])) << 8) | quint32(uchar(buffer_[3]));
                if (buffer_.size() < length + 4)
                    break;
                const auto frame = QJsonDocument::fromJson(buffer_.mid(4, length)).object();
                buffer_.remove(0, length + 4);
                if (frame.value("correlation_id") == request.value("message_id"))
                    return frame.value("payload").toObject();
            }
            QTest::qWait(10);
        }
        return {};
    }

  private:
    QByteArray buffer_;
};
} // namespace

class ManagedTaskIpcTest final : public QObject {
    Q_OBJECT
  private slots:
    void authenticatedAgentUsesBoundOwnerAndCompleteRegistry() {
        QTemporaryDir directory;
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        QVERIFY(
            writeFakeCodex(directory.filePath("fake-codex"), directory.filePath("provider.log")));
        LocalJournal journal(directory.filePath("journal.sqlite"));
        for (const auto& id : {"owner", "other"}) {
            StoredSession session;
            session.id = id;
            session.worktreePath = cwd;
            session.adapter = "codex";
            session.state = "ready";
            session.executionPolicy = "full";
            QVERIFY(journal.upsertSession(session));
        }
        CodexAdapter adapter(directory.filePath("fake-codex"));
        SessionHost host(&journal, &adapter);
        const auto cleanup = qScopeGuard([&] { adapter.stop(); });
        LocalIpcServer server(&host, directory.filePath("ipc.sock"), "user-token");
        QVERIFY2(server.listen(), qPrintable(server.errorString()));
        Client unauthenticated;
        unauthenticated.socket.connectToServer(server.fullServerName());
        QTRY_COMPARE(unauthenticated.socket.state(), QLocalSocket::ConnectedState);
        const auto denied = unauthenticated.call(request("session.task.list", {}, "owner"));
        QCOMPARE(denied.value("error_code"), "authentication_required");

        Client agent;
        agent.socket.connectToServer(server.fullServerName());
        QTRY_COMPARE(agent.socket.state(), QLocalSocket::ConnectedState);
        QVERIFY(agent.call(request("daemon.hello", {{"agent_token", host.agentToken("owner")}}))
                    .value("ok")
                    .toBool());
        auto ledger = journal.backgroundTasks();
        QString last;
        for (int index = 0; index < 20; ++index) {
            BackgroundTaskIntent intent{{"owner", 1, "turn"},
                                        QString::number(index),
                                        "process",
                                        "Fixture",
                                        QByteArray::number(index),
                                        60000,
                                        {}};
            const auto created = ledger.create(intent);
            QVERIFY(created.value("ok").toBool());
            last = created.value("task").toObject().value("task_id").toString();
            BackgroundTaskChange failed;
            failed.state = "failed";
            failed.reason = "permission_denied";
            failed.neverLaunched = true;
            QVERIFY(ledger.change(last, 1, failed).value("ok").toBool());
        }
        // A forged envelope cannot select the other conversation; the server
        // derives the owner from the conversation credential instead.
        const auto page = agent.call(request("session.task.list", {{"limit", 17}}, "other"));
        QVERIFY(page.value("ok").toBool());
        QCOMPARE(page.value("tasks").toArray().size(), 17);
        QVERIFY(!page.value("next_cursor").toString().isEmpty());
        const auto tail = agent.call(request(
            "session.task.list", {{"cursor", page.value("next_cursor")}, {"limit", 17}}, "other"));
        QCOMPARE(tail.value("tasks").toArray().size(), 3);
        QCOMPARE(agent.call(request("session.task.get", {{"task_id", last}}, "other"))
                     .value("task")
                     .toObject()
                     .value("owner_session_id"),
                 "owner");
        QVERIFY(agent.call(request("session.task.stop", {{"task_id", last}}, "other"))
                    .value("ok")
                    .toBool());
        QVERIFY(agent.call(request("session.task.output", {{"task_id", last}}, "other"))
                    .value("ok")
                    .toBool());
        QCOMPARE(agent
                     .call(request("session.task.resolve_cleanup",
                                   {{"task_id", last}, {"revision", 2}}, "owner"))
                     .value("error_code"),
                 "forbidden");
        QCOMPARE(agent.call(request("storage.settings", {}, "owner")).value("error_code"),
                 "forbidden");
        Client user;
        user.socket.connectToServer(server.fullServerName());
        QTRY_COMPARE(user.socket.state(), QLocalSocket::ConnectedState);
        QVERIFY(user.call(request("daemon.hello", {{"token", "user-token"}, {"client_id", "test"}}))
                    .value("ok")
                    .toBool());
        QVERIFY(user.call(request("session.task.get", {{"task_id", last}}, "other"))
                    .value("ok")
                    .toBool() == false);
        StoredSession replacement;
        for (const auto& stored : journal.sessions())
            if (stored.id == "owner")
                replacement = stored;
        replacement.generation = 2;
        QVERIFY(journal.upsertSession(replacement));
        // Local administration retains access to the old generation; agent
        // credentials have no such grant.
        QVERIFY(user.call(request("session.task.get", {{"task_id", last}}, "owner"))
                    .value("ok")
                    .toBool());
        QVERIFY(!agent.call(request("session.task.get", {{"task_id", last}}, "owner"))
                     .value("ok")
                     .toBool());
    }
};
QTEST_GUILESS_MAIN(ManagedTaskIpcTest)
#include "ManagedTaskIpcTest.moc"
