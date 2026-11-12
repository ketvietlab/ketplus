#include "DaemonClient.h"
#include "DaemonMachineBackend.h"

#include <QDir>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using ketplus::remote::DaemonClient;
using ketplus::remote::DaemonMachineBackend;
using ketplus::remote::MachineBackend;
using State = MachineBackend::ConnectionState;

namespace {

struct RecordedCall final {
    QString type;
    QJsonObject payload;
    QString sessionId;
    qint64 generation{0};
};

class MachineBackendTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        qRegisterMetaType<MachineBackend::ConnectionState>();
        // The local adapter test builds a real DaemonClient; point it at a socket nobody serves
        // so it never reaches the daemon of the Mac running the tests.
        QVERIFY(directory_.isValid());
        qputenv("KETPLUS_DAEMON_SOCKET", directory_.filePath(QStringLiteral("none.sock")).toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", directory_.filePath(QStringLiteral("token")).toUtf8());
        qputenv("KETPLUS_DAEMON_STATE", directory_.filePath(QStringLiteral("state")).toUtf8());
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
    }

    void mapsEveryOperationToTheLocalProtocol() {
        QList<RecordedCall> calls;
        DaemonMachineBackend backend(QStringLiteral("local"), MachineBackend::Route::Local,
                                     [&calls](const QString& type, const QJsonObject& payload,
                                              const QString& sessionId, const qint64 generation,
                                              const MachineBackend::Callback& callback) {
                                         calls.append({type, payload, sessionId, generation});
                                         callback({{QStringLiteral("ok"), true}}, {});
                                     });
        int answered = 0;
        const auto count = [&answered](const QJsonObject& result, const QString& error) {
            QVERIFY(error.isEmpty());
            QVERIFY(result.value(QStringLiteral("ok")).toBool());
            ++answered;
        };
        const QJsonObject window{{QStringLiteral("limit"), 40}};
        const QJsonObject page{{QStringLiteral("before_cursor"), 12}};
        const QJsonObject turn{{QStringLiteral("prompt"), QStringLiteral("Fix it")}};
        const QJsonObject steer{{QStringLiteral("prompt"), QStringLiteral("Also this")}};
        const QJsonObject accept{{QStringLiteral("decision"), QStringLiteral("accept")}};
        const QJsonObject answer{{QStringLiteral("answers"), QJsonObject{}}};
        const QJsonObject listOptions{{QStringLiteral("session_catalog_live_only"), true}};
        const QJsonObject worktrees{
            {QStringLiteral("query"), QStringLiteral("fix")},
            {QStringLiteral("offset"), 20},
            {QStringLiteral("limit"), 20},
            {QStringLiteral("repository_path"), QStringLiteral("/src/app")}};
        const QJsonObject largeEvent{{QStringLiteral("ref"), QStringLiteral("e9")},
                                     {QStringLiteral("offset"), 0}};

        int repositoryAnswers = 0;
        backend.listRepositories([&repositoryAnswers](const QJsonObject&, const QString& error) {
            QVERIFY(error.isEmpty());
            ++repositoryAnswers;
        });
        backend.listWorktrees(QStringLiteral("s1"), 3, worktrees, count);
        backend.listConversations(listOptions, count);
        backend.transcriptWindow(QStringLiteral("s1"), 3, window, count);
        backend.transcriptPage(QStringLiteral("s1"), 3, page, count);
        backend.transcriptLargeEvent(QStringLiteral("s1"), 3, largeEvent, count);
        backend.startTurn(QStringLiteral("s1"), 3, turn, count);
        backend.steerTurn(QStringLiteral("s1"), 3, steer, count);
        backend.stopTurn(QStringLiteral("s1"), 3, count);
        backend.pendingApprovals(QStringLiteral("s1"), 3, {}, count);
        backend.pendingApprovals(QStringLiteral("s1"), 3, QStringLiteral("a7"), count);
        backend.resolveApproval(QStringLiteral("s1"), 3, QStringLiteral("a1"), accept, count);
        backend.answerQuestion(QStringLiteral("s1"), 3, QStringLiteral("q1"), answer, count);

        const QList<RecordedCall> expected{
            {QStringLiteral("workspace.list"), {}, {}, -1},
            {QStringLiteral("session.worktrees"), worktrees, "s1", 3},
            {QStringLiteral("session.list"), listOptions, {}, 0},
            {QStringLiteral("session.transcript_window"), window, "s1", 3},
            {QStringLiteral("session.transcript_page"), page, "s1", 3},
            {QStringLiteral("session.transcript_large_event"), largeEvent, "s1", 3},
            {QStringLiteral("session.start_turn"), turn, "s1", 3},
            {QStringLiteral("session.steer"), steer, "s1", 3},
            {QStringLiteral("session.interrupt"), {}, "s1", 3},
            {QStringLiteral("session.pending_approvals"), {}, "s1", 3},
            {QStringLiteral("session.pending_approvals"),
             {{QStringLiteral("after_id"), QStringLiteral("a7")}},
             "s1",
             3},
            {QStringLiteral("approval.resolve"),
             {{QStringLiteral("approval_id"), QStringLiteral("a1")},
              {QStringLiteral("resolution"), accept}},
             "s1",
             3},
            {QStringLiteral("question.answer"),
             {{QStringLiteral("approval_id"), QStringLiteral("q1")},
              {QStringLiteral("resolution"), answer}},
             "s1",
             3},
        };
        QCOMPARE(repositoryAnswers, 1);
        QCOMPARE(answered + repositoryAnswers, expected.size());
        QCOMPARE(calls.size(), expected.size());
        for (qsizetype i = 0; i < expected.size(); ++i) {
            QCOMPARE(calls[i].type, expected[i].type);
            QCOMPARE(calls[i].payload, expected[i].payload);
            QCOMPARE(calls[i].sessionId, expected[i].sessionId);
            QCOMPARE(calls[i].generation, expected[i].generation);
        }
    }

    void repositoriesUseTheNeutralShape() {
        QString failure;
        DaemonMachineBackend backend(
            QStringLiteral("local"), MachineBackend::Route::Local,
            [&failure](const QString&, const QJsonObject&, const QString&, qint64,
                       const MachineBackend::Callback& callback) {
                if (!failure.isEmpty()) {
                    callback({}, failure);
                    return;
                }
                callback(
                    {{QStringLiteral("workspaces"),
                      QJsonArray{
                          QJsonObject{{QStringLiteral("path"), QStringLiteral("/Users/me/dev/app")},
                                      {QStringLiteral("available"), true}},
                          QJsonObject{{QStringLiteral("path"), QString()}},
                          QJsonObject{{QStringLiteral("path"), QStringLiteral("/Volumes/old/site")},
                                      {QStringLiteral("available"), false}}}}},
                    {});
            });
        QJsonObject result;
        backend.listRepositories(
            [&result](const QJsonObject& reply, const QString&) { result = reply; });
        const QJsonArray expected{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("/Users/me/dev/app")},
                        {QStringLiteral("label"), QStringLiteral("app")},
                        {QStringLiteral("path"), QStringLiteral("/Users/me/dev/app")},
                        {QStringLiteral("available"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("/Volumes/old/site")},
                        {QStringLiteral("label"), QStringLiteral("site")},
                        {QStringLiteral("path"), QStringLiteral("/Volumes/old/site")},
                        {QStringLiteral("available"), false}}};
        QCOMPARE(result, (QJsonObject{{QStringLiteral("repositories"), expected}}));

        failure = QStringLiteral("Daemon busy.");
        QString error;
        result = {{QStringLiteral("stale"), true}};
        backend.listRepositories([&](const QJsonObject& reply, const QString& message) {
            result = reply;
            error = message;
        });
        QCOMPARE(error, failure);
        QVERIFY(result.isEmpty());
    }

    void anSshClientKeepsItsIdentity() {
        DaemonClient client;
        DaemonMachineBackend backend(&client, QStringLiteral("SHA256:abc"),
                                     MachineBackend::Route::Ssh);
        QCOMPARE(backend.machineId(), QStringLiteral("SHA256:abc"));
        QCOMPARE(backend.route(), MachineBackend::Route::Ssh);
        // Only This Mac's route turns a missing local install into Offline.
        QCOMPARE(backend.connectionState(), State::Connecting);
    }

    void transportReadinessDrivesTheConnectionState() {
        DaemonMachineBackend backend(QStringLiteral("build-node-01"), MachineBackend::Route::Ssh,
                                     [](const QString&, const QJsonObject&, const QString&, qint64,
                                        const MachineBackend::Callback&) {});
        QSignalSpy changes(&backend, &MachineBackend::connectionStateChanged);
        QCOMPARE(backend.machineId(), QStringLiteral("build-node-01"));
        QCOMPARE(backend.route(), MachineBackend::Route::Ssh);
        QCOMPARE(backend.connectionState(), State::Connecting);

        // A failed first attempt keeps Connecting: nothing was ever connected to reconnect to.
        backend.setTransportReady(false, QStringLiteral("Host unreachable."));
        QCOMPARE(backend.connectionState(), State::Connecting);
        QCOMPARE(backend.connectionReason(), QStringLiteral("Host unreachable."));

        backend.setTransportReady(true);
        QCOMPARE(backend.connectionState(), State::Connected);
        QVERIFY(backend.connectionReason().isEmpty());

        backend.setTransportReady(false);
        backend.setTransportReady(false, QStringLiteral("Connection lost."));
        QCOMPARE(backend.connectionState(), State::Reconnecting);
        QCOMPARE(backend.connectionReason(), QStringLiteral("Connection lost."));
        // A later readiness report without a reason keeps the one already shown.
        backend.setTransportReady(false);
        QCOMPARE(backend.connectionReason(), QStringLiteral("Connection lost."));

        backend.setTransportUnavailable(QStringLiteral("Host key changed."));
        QCOMPARE(backend.connectionState(), State::Offline);
        backend.setTransportReady(true);
        QCOMPARE(backend.connectionState(), State::Connected);

        const QList<State> states{State::Connecting,   State::Connected, State::Reconnecting,
                                  State::Reconnecting, State::Offline,   State::Connected};
        QCOMPARE(changes.size(), states.size());
        for (qsizetype i = 0; i < states.size(); ++i)
            QCOMPARE(changes[i][0].value<State>(), states[i]);
    }

    void localDaemonClientSignalsMapToStates() {
        auto client = std::make_unique<DaemonClient>();
        QVERIFY(!client->isReady());
        DaemonMachineBackend backend(client.get());
        QCOMPARE(backend.machineId(), QStringLiteral("local"));
        QCOMPARE(backend.route(), MachineBackend::Route::Local);
        QCOMPARE(backend.connectionState(),
                 client->isAvailable() ? State::Connecting : State::Offline);

        QSignalSpy changes(&backend, &MachineBackend::connectionStateChanged);
        QSignalSpy events(&backend, &MachineBackend::eventReceived);

        emit client->readyChanged(true);
        QCOMPARE(backend.connectionState(), State::Connected);

        const QJsonObject event{{QStringLiteral("type"), QStringLiteral("turn.started")}};
        emit client->eventReceived(event);
        QCOMPARE(events.size(), 1);
        QCOMPARE(events[0][0].toJsonObject(), event);

        // DaemonClient drops readiness first, then reports why; it reconnects on its own.
        emit client->readyChanged(false);
        emit client->disconnected(QStringLiteral("KetPlus Daemon disconnected."));
        QCOMPARE(backend.connectionState(), State::Reconnecting);
        QCOMPARE(backend.connectionReason(), QStringLiteral("KetPlus Daemon disconnected."));

        emit client->readyChanged(true);
        QCOMPARE(backend.connectionState(), State::Connected);

        client.reset();
        QCOMPARE(backend.connectionState(), State::Offline);
        QString failure;
        backend.listRepositories(
            [&failure](const QJsonObject&, const QString& error) { failure = error; });
        QVERIFY(!failure.isEmpty());
        QVERIFY(changes.size() >= 5);
        QCOMPARE(changes.last()[0].value<State>(), State::Offline);
    }

    void localCallsReachDaemonClientQueue() {
        // Before the hello, DaemonClient queues the call as it always has; the adapter neither
        // fails nor answers it on the daemon's behalf.
        DaemonClient client;
        DaemonMachineBackend backend(&client);
        bool answered = false;
        backend.listConversations(
            {}, [&answered](const QJsonObject&, const QString&) { answered = true; });
        QVERIFY(!answered);
    }

    void aMissingClientIsOffline() {
        DaemonMachineBackend backend(static_cast<DaemonClient*>(nullptr));
        QCOMPARE(backend.connectionState(), State::Offline);
        QVERIFY(!backend.connectionReason().isEmpty());
    }

  private:
    QTemporaryDir directory_{QDir::tempPath() + QStringLiteral("/kp-machine-XXXXXX")};
};

} // namespace

QTEST_GUILESS_MAIN(MachineBackendTest)
#include "MachineBackendTest.moc"
