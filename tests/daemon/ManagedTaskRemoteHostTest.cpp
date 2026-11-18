#include "FakeCodex.h"
#include "providers/CodexAdapter.h"
#include "runner/entry/RemoteJobPort.h"
#include "runtime/ManagedTaskRuntime.h"
#include "runtime/SessionHost.h"
#include "storage/BackgroundTaskLedger.h"
#include "storage/LocalJournal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace ketplus::daemon;
using runner::entry::RemoteJobPort;

namespace {

// Set in a copy of this test that runs where no local executor is installed.
constexpr auto withoutExecutor = "KETPLUS_REMOTE_HOST_TEST_WITHOUT_EXECUTOR";

QJsonObject command(SessionHost& host, const QString& type, const QJsonObject& payload = {}) {
    auto result = std::make_shared<QJsonObject>();
    host.handleCommandDeferred({{"message_id", QUuid::createUuid().toString()},
                                {"message_type", type},
                                {"session_id", "owner"},
                                {"session_generation", 1},
                                {"payload", payload}},
                               "local-test",
                               [result](const QJsonObject& reply) { *result = reply; });
    QElapsedTimer timer;
    timer.start();
    while (result->isEmpty() && timer.elapsed() < 6000)
        QTest::qWait(10);
    return *result;
}

QJsonObject process(const QString& cwd, const QString& request = "request") {
    return {{"client_request_id", request},
            {"kind", "process"},
            {"title", "Build"},
            {"timeout_ms", 60000},
            {"execution_spec", QJsonObject{{"argv", QJsonArray{"/usr/bin/printf", "local"}},
                                           {"cwd", cwd},
                                           {"env_delta", QJsonObject{}}}}};
}

QJsonObject remote(const QString& cwd, const QJsonArray& labels = {"os:linux"},
                   const QString& request = "remote") {
    QJsonObject payload = process(cwd, request);
    payload.insert("runs_on", labels);
    return payload;
}

QString idOf(const QJsonObject& reply) {
    return reply.value("task").toObject().value("task_id").toString();
}

// A remote jobs port that records what the runtime asks of it.
struct FakePort final : RemoteJobPort {
    Ports ports;
    QJsonObject refusal;
    mutable int admits = 0;
    QStringList submitted;
    QSet<QString> records; // tasks with a remote record
    QString stopCode;
    QString cleanupCode;
    QStringList stops;
    QStringList cleanups;

    void setPorts(Ports next) override { ports = std::move(next); }
    QJsonObject admit(const BackgroundTaskIntent&, const QJsonObject&) const override {
        ++admits;
        return refusal;
    }
    void submit(const QString& taskId, const QJsonObject&) override { submitted.append(taskId); }
    std::optional<QString> stop(const QString& taskId) override {
        if (!records.contains(taskId))
            return std::nullopt;
        stops.append(taskId);
        return stopCode;
    }
    std::optional<QString> resolveCleanup(const QString& taskId, const QString&) override {
        if (!records.contains(taskId))
            return std::nullopt;
        cleanups.append(taskId);
        return cleanupCode;
    }
    bool hasRecord(const QString& taskId) const override { return records.contains(taskId); }
    QJsonObject withPlacement(QJsonObject reply) const override {
        const auto place = [this](QJsonObject task) {
            if (records.contains(task.value("task_id").toString()))
                task.insert("placement", "relay_queued");
            return task;
        };
        if (reply.value("task").isObject())
            reply.insert("task", place(reply.value("task").toObject()));
        if (reply.value("tasks").isArray()) {
            QJsonArray tasks;
            for (const auto value : reply.value("tasks").toArray())
                tasks.append(place(value.toObject()));
            reply.insert("tasks", tasks);
        }
        return reply;
    }
};

struct Host {
    QTemporaryDir directory;
    QString cwd = QFileInfo(directory.path()).canonicalFilePath();
    std::unique_ptr<LocalJournal> journal;
    std::unique_ptr<CodexAdapter> adapter;
    std::unique_ptr<SessionHost> host;
    FakePort port;

    explicit Host(const QString& policy = "full") {
        const QString codex = directory.filePath(fixtureName("fake-codex"));
        if (!writeFakeCodex(codex, directory.filePath("provider.log")))
            qFatal("fake codex");
        journal = std::make_unique<LocalJournal>(directory.filePath("journal.sqlite"));
        StoredSession owner;
        owner.id = "owner";
        owner.worktreePath = cwd;
        owner.adapter = "codex";
        owner.state = "ready";
        owner.executionPolicy = policy;
        if (!journal->upsertSession(owner))
            qFatal("seed");
        adapter = std::make_unique<CodexAdapter>(codex);
        restart();
    }
    ~Host() {
        if (host)
            host->setRemoteJobs(nullptr);
        host.reset();
        adapter->stop();
    }
    void restart() {
        if (host)
            host->setRemoteJobs(nullptr);
        host.reset();
        host = std::make_unique<SessionHost>(journal.get(), adapter.get());
    }
    void attach() { host->setRemoteJobs(&port); }
    QJsonObject run(const QString& type, const QJsonObject& payload = {}) {
        return command(*host, type, payload);
    }
    QJsonObject task(const QString& id) {
        return run("session.task.get", {{"task_id", id}}).value("task").toObject();
    }
    int rows(const QString& table) {
        QSqlQuery query(journal->backgroundTasks().database());
        if (!query.exec("SELECT count(*) FROM " + table) || !query.next())
            return -1;
        return query.value(0).toInt();
    }
    int outbox(const QString& id) {
        QSqlQuery query(journal->backgroundTasks().database());
        query.prepare("SELECT count(*) FROM managed_job_outbox WHERE task_id=?");
        query.addBindValue(id);
        if (!query.exec() || !query.next())
            return -1;
        return query.value(0).toInt();
    }
    ManagedTaskRuntime* runtime() const {
        for (auto* child : host->findChildren<QObject*>())
            if (auto* found = dynamic_cast<ManagedTaskRuntime*>(child))
                return found;
        return nullptr;
    }
    // The remote jobs stored a record for the task, as a submit that reached the Relay does.
    bool recorded(const QString& id) {
        QSqlQuery query(journal->backgroundTasks().database());
        query.prepare(
            "UPDATE remote_jobs SET revision=1,record=X'7B7D' WHERE task_id=? AND revision=0");
        query.addBindValue(id);
        if (!query.exec() || query.numRowsAffected() != 1)
            return false;
        port.records.insert(id);
        return true;
    }
    // The remote jobs end the task as its runner reported it.
    bool end(const QString& id) {
        auto ledger = journal->backgroundTasks();
        auto database = ledger.database();
        const auto task = ledger.runtimeTask(id);
        BackgroundTaskChange change;
        change.state = "completed";
        change.cleanupStatus = "verified";
        change.exitCode = 0;
        if (!database.transaction())
            return false;
        if (!ledger.applyTransition(id, task, task.value("revision").toInteger(), change, {})
                 .value("ok")
                 .toBool()) {
            database.rollback();
            return false;
        }
        return database.commit();
    }
    QJsonArray approvals() {
        return run("session.pending_approvals").value("pending_approvals").toArray();
    }
    bool resolve(const QString& decision) {
        const auto pending = approvals();
        if (pending.isEmpty())
            return false;
        return run("approval.resolve",
                   {{"approval_id", pending.first().toObject().value("approval_id")},
                    {"resolution", QJsonObject{{"decision", decision}}}})
            .value("ok")
            .toBool();
    }
};

} // namespace

class ManagedTaskRemoteHostTest final : public QObject {
    Q_OBJECT
  private slots:
    // Without remote jobs a task with runs_on is refused before anything exists.
    void withoutAPortRunsOnIsRefusedBeforeAnyTask() {
        Host h("ask");
        const auto reply = h.run("session.task.start", remote(h.cwd));
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("runner_unavailable"));
        QCOMPARE(h.rows("managed_jobs"), 0);
        QCOMPARE(h.rows("remote_jobs"), 0);
        QTest::qWait(300);
        QVERIFY(h.approvals().isEmpty());
    }

    void aRefusalOfThePortCreatesNothing() {
        Host h("ask");
        h.attach();
        h.port.refusal = {
            {"ok", false}, {"error_code", "resource_exhausted"}, {"error_message", "full"}};
        const auto reply = h.run("session.task.start", remote(h.cwd));
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("resource_exhausted"));
        QCOMPARE(h.port.admits, 1);
        QCOMPARE(h.rows("managed_jobs"), 0);
        QCOMPARE(h.rows("remote_jobs"), 0);
        QTest::qWait(300);
        QVERIFY(h.approvals().isEmpty());
    }

    // Labels are checked with every task, before the port is asked.
    void badLabelsAndAgentTasksAreRefusedBeforeThePort() {
        Host h("ask");
        h.attach();
        QCOMPARE(h.run("session.task.start", remote(h.cwd, {"Bad Label"})).value("error_code"),
                 "invalid_labels");
        QCOMPARE(h.run("session.task.start", remote(h.cwd, {})).value("error_code"),
                 "invalid_labels");
        QJsonObject agent{{"client_request_id", "agent"},
                          {"kind", "agent"},
                          {"title", "Agent"},
                          {"timeout_ms", 60000},
                          {"runs_on", QJsonArray{"os:linux"}},
                          {"execution_spec", QJsonObject{{"prompt", "hello"}}}};
        QCOMPARE(h.run("session.task.start", agent).value("error_code"), "runs_on_unsupported");
        QCOMPARE(h.port.admits, 0);
        QCOMPARE(h.rows("managed_jobs"), 0);
    }

    // A task without runs_on never meets the port, and its replies have no remote fields.
    void aLocalTaskDoesNotTouchThePort() {
        Host h;
        h.attach();
        const auto started = h.run("session.task.start", process(h.cwd));
        QVERIFY(started.value("ok").toBool());
        const auto id = idOf(started);
        QTRY_COMPARE_WITH_TIMEOUT(h.task(id).value("state"), "completed", 6000);
        QCOMPARE(h.port.admits, 0);
        QVERIFY(h.port.submitted.isEmpty());
        QVERIFY(!h.task(id).contains("placement"));
        QCOMPARE(h.rows("remote_jobs"), 0);
    }

    // Approved: submitted once, and the task never runs here.
    void anApprovedRemoteTaskIsSubmittedOnceAndNeverRunsHere() {
        Host h("ask");
        h.attach();
        const auto started = h.run("session.task.start", remote(h.cwd));
        QVERIFY2(started.value("ok").toBool(),
                 qPrintable(QJsonDocument(started).toJson(QJsonDocument::Compact)));
        const auto id = idOf(started);
        QCOMPARE(h.rows("remote_jobs"), 1);
        QTRY_COMPARE(h.approvals().size(), 1);
        QVERIFY(h.port.submitted.isEmpty());
        QVERIFY(h.resolve("accept"));
        QTRY_COMPARE(h.port.submitted, QStringList{id});
        QTest::qWait(800);
        QCOMPARE(h.port.submitted, QStringList{id});
        const auto task = h.task(id);
        QCOMPARE(task.value("state").toString(), QStringLiteral("queued"));
        QVERIFY(task.value("permission_approved").toBool());
        QVERIFY(task.value("execution_identity").toObject().isEmpty());
        QCOMPARE(h.run("session.task.output", {{"task_id", id}}).value("output").toString(),
                 QString());
    }

    void aDeniedRemoteTaskIsNeverSubmitted() {
        Host h("ask");
        h.attach();
        const auto id = idOf(h.run("session.task.start", remote(h.cwd)));
        QTRY_COMPARE(h.approvals().size(), 1);
        QVERIFY(h.resolve("decline"));
        QTRY_COMPARE(h.task(id).value("state"), "failed");
        QCOMPARE(h.task(id).value("reason"), "permission_denied");
        QTest::qWait(300);
        QVERIFY(h.port.submitted.isEmpty());
    }

    // The port is cleared while the task waits for approval: approved, it fails
    // runner_unavailable without running and without the cleared port.
    void aClearedPortFailsAnApprovedTask() {
        Host h("ask");
        h.attach();
        const auto id = idOf(h.run("session.task.start", remote(h.cwd)));
        QTRY_COMPARE(h.approvals().size(), 1);
        h.host->setRemoteJobs(nullptr);
        QVERIFY(!h.port.ports.failReserved);
        QVERIFY(!h.port.ports.changed);
        QVERIFY(h.resolve("accept"));
        QTRY_COMPARE(h.task(id).value("state"), "failed");
        QCOMPARE(h.task(id).value("reason"), "spawn_failed");
        QCOMPARE(h.task(id).value("error"), "runner_unavailable");
        QVERIFY(h.task(id).value("execution_identity").toObject().isEmpty());
        QVERIFY(h.port.submitted.isEmpty());
    }

    // A submit that ends before a record exists fails the task with its code, once.
    void aFailedSubmitEndsTheTaskWithItsCode() {
        Host h;
        h.attach();
        const auto id = idOf(h.run("session.task.start", remote(h.cwd)));
        QTRY_COMPARE(h.port.submitted, QStringList{id});
        h.port.ports.failReserved(id, "snapshot_build_failed");
        QCOMPARE(h.task(id).value("state"), "failed");
        QCOMPARE(h.task(id).value("reason"), "spawn_failed");
        QCOMPARE(h.task(id).value("error"), "snapshot_build_failed");
        QVERIFY(h.task(id).value("execution_identity").toObject().isEmpty());
        const auto revision = h.task(id).value("revision").toInteger();
        h.port.ports.failReserved(id, "runner_unavailable");
        QCOMPARE(h.task(id).value("revision").toInteger(), revision);
        QCOMPARE(h.task(id).value("error"), "snapshot_build_failed");
    }

    void aStopWhileWaitingForApprovalCancelsWithoutSubmit() {
        Host h("ask");
        h.attach();
        const auto id = idOf(h.run("session.task.start", remote(h.cwd)));
        QTRY_COMPARE(h.approvals().size(), 1);
        QVERIFY(h.run("session.task.stop", {{"task_id", id}}).value("ok").toBool());
        QCOMPARE(h.task(id).value("state"), "cancelled");
        h.resolve("accept");
        QTest::qWait(300);
        QVERIFY(h.port.submitted.isEmpty());
        QCOMPARE(h.task(id).value("state"), "cancelled");
    }

    // Submitted but no record yet (the snapshot is being prepared): the runtime cancels it and
    // a late failure of the submit changes nothing.
    void aStopBeforeTheRecordCancelsAndALateResultIsDropped() {
        Host h;
        h.attach();
        const auto id = idOf(h.run("session.task.start", remote(h.cwd)));
        QTRY_COMPARE(h.port.submitted, QStringList{id});
        QVERIFY(h.run("session.task.stop", {{"task_id", id}}).value("ok").toBool());
        QCOMPARE(h.task(id).value("state"), "cancelled");
        QVERIFY(h.port.stops.isEmpty());
        h.port.ports.failReserved(id, "runner_unavailable");
        QCOMPARE(h.task(id).value("state"), "cancelled");
    }

    // With a record, stop, resolve_cleanup, get and list go to the remote jobs.
    void aRecordedTaskIsStoppedAndShownThroughThePort() {
        Host h;
        h.attach();
        const auto id = idOf(h.run("session.task.start", remote(h.cwd)));
        QTRY_COMPARE(h.port.submitted, QStringList{id});
        h.port.records.insert(id);
        QCOMPARE(h.task(id).value("placement"), "relay_queued");
        const auto listed = h.run("session.task.list").value("tasks").toArray();
        QCOMPARE(listed.size(), 1);
        QCOMPARE(listed.first().toObject().value("placement"), "relay_queued");
        const auto stopped = h.run("session.task.stop", {{"task_id", id}});
        QVERIFY(stopped.value("ok").toBool());
        QCOMPARE(h.port.stops, QStringList{id});
        QCOMPARE(h.task(id).value("state"), "queued"); // the record carries the stop
        h.port.cleanupCode = "invalid_state";
        const auto cleanup =
            h.run("session.task.resolve_cleanup", {{"task_id", id}, {"revision", 1}});
        QCOMPARE(cleanup.value("error_code"), "invalid_state");
        QCOMPARE(h.port.cleanups, QStringList{id});
    }

    // The same labels in another order are the same request; another timeout is not.
    void aRepeatedStartReturnsTheTask() {
        Host h("ask");
        h.attach();
        const auto first = h.run("session.task.start", remote(h.cwd, {"os:linux", "gpu"}));
        const auto again = h.run("session.task.start", remote(h.cwd, {"gpu", "os:linux", "gpu"}));
        QVERIFY(again.value("ok").toBool());
        QCOMPARE(idOf(again), idOf(first));
        QCOMPARE(h.rows("managed_jobs"), 1);
        auto other = remote(h.cwd, {"os:linux", "gpu"});
        other.insert("timeout_ms", 120000);
        QCOMPARE(h.run("session.task.start", other).value("error_code"), "idempotency_conflict");
        QCOMPARE(h.run("session.task.start", remote(h.cwd, {"os:linux"})).value("error_code"),
                 "idempotency_conflict");
    }

    // A reserved task interrupted by a restart ends like a local task that never launched, and
    // starting it again returns the ended task.
    void aRestartEndsAReservedTaskWithoutRunningIt() {
        Host h("ask");
        h.attach();
        const auto id = idOf(h.run("session.task.start", remote(h.cwd)));
        QTRY_COMPARE(h.approvals().size(), 1);
        h.restart();
        h.attach();
        QTRY_COMPARE(h.task(id).value("state"), "interrupted");
        QCOMPARE(h.task(id).value("reason"), "daemon_restart_before_launch");
        const auto again = h.run("session.task.start", remote(h.cwd));
        QCOMPARE(idOf(again), id);
        QCOMPARE(again.value("task").toObject().value("state"), "interrupted");
        QTest::qWait(300);
        QVERIFY(h.port.submitted.isEmpty());
    }

    // A runtime that ends while its port lives lets go of it: the port has no way back in.
    void anEndedRuntimeLeavesThePort() {
        Host h;
        h.attach();
        QVERIFY(h.port.ports.failReserved);
        QVERIFY(h.port.ports.changed);
        h.host.reset();
        QVERIFY(!h.port.ports.failReserved);
        QVERIFY(!h.port.ports.changed);
    }

    // A port replaced by another lets go of the first.
    void aReplacedPortIsLetGo() {
        Host h;
        h.attach();
        FakePort other;
        h.host->setRemoteJobs(&other);
        QVERIFY(!h.port.ports.changed);
        QVERIFY(other.ports.changed);
        h.host->setRemoteJobs(nullptr);
        QVERIFY(!other.ports.changed);
    }

    // A changed record is read again and published.
    void aChangedRecordIsReadAgain() {
        Host h;
        h.attach();
        const auto id = idOf(h.run("session.task.start", remote(h.cwd)));
        QTRY_COMPARE(h.port.submitted, QStringList{id});
        QVERIFY(h.port.ports.changed);
        h.port.ports.changed(id);
        h.port.ports.changed("unknown");
        QCOMPARE(h.task(id).value("state"), "queued");
    }

    // A submit that ends before a record exists: each code is the task's error, read by get.
    void eachSubmitFailureIsReadByGet() {
        Host h;
        h.attach();
        for (const QString code : {"runner_unavailable", "snapshot_build_failed",
                                   "repository_unsupported", "entitlement_required"}) {
            const auto id = idOf(h.run("session.task.start", remote(h.cwd, {"os:linux"}, code)));
            QVERIFY(!id.isEmpty());
            QTRY_VERIFY(h.port.submitted.contains(id));
            h.port.ports.failReserved(id, code);
            const auto task = h.task(id);
            QCOMPARE(task.value("state").toString(), QStringLiteral("failed"));
            QCOMPARE(task.value("reason").toString(), QStringLiteral("spawn_failed"));
            QCOMPARE(task.value("error").toString(), code);
            QCOMPARE(h.outbox(id), 1);
        }
    }

    // A remote job that ended is no longer followed here, and its owner hears of it once.
    void anEndedRemoteJobLeavesTheRuntimeWithOneCompletion() {
        Host h;
        h.attach();
        const auto id = idOf(h.run("session.task.start", remote(h.cwd)));
        QTRY_COMPARE(h.port.submitted, QStringList{id});
        auto* runtime = h.runtime();
        QVERIFY(runtime);
        QVERIFY(runtime->isLive(id));
        QVERIFY(runtime->isPumping());
        QVERIFY(h.recorded(id));
        QVERIFY(h.end(id));
        h.port.ports.changed(id);
        QVERIFY(!runtime->isLive(id));
        QVERIFY(!runtime->isPumping());
        h.port.ports.changed(id);
        QCOMPARE(h.task(id).value("state"), "completed");
        QCOMPARE(h.outbox(id), 1);
    }

    // Past its timeout here, a recorded remote job is left to its runner.
    void aRecordedRemoteJobIsNotTimedOutHere() {
        Host h;
        h.attach();
        auto payload = remote(h.cwd);
        payload.insert("timeout_ms", 1000);
        const auto id = idOf(h.run("session.task.start", payload));
        QTRY_COMPARE(h.port.submitted, QStringList{id});
        QVERIFY(h.recorded(id));
        QTest::qWait(1600);
        QCOMPARE(h.task(id).value("state"), "queued");
        QVERIFY(h.port.stops.isEmpty());
        QCOMPARE(h.outbox(id), 0);
    }

    // An owner that is let go stops its recorded remote job through the port, once: the
    // pump past the job's timeout adds no stop.
    void anEndedOwnerStopsItsRecordedRemoteJobOnce() {
        for (const QString reason : {"owner_invalid", "parent_job_stopped"}) {
            Host h;
            h.attach();
            auto payload = remote(h.cwd);
            payload.insert("timeout_ms", 1000);
            const auto id = idOf(h.run("session.task.start", payload));
            QTRY_COMPARE(h.port.submitted, QStringList{id});
            QVERIFY(h.recorded(id));
            auto* runtime = h.runtime();
            QVERIFY(runtime);
            runtime->cancelOwner({"owner", 1, {}}, reason);
            QCOMPARE(h.port.stops, QStringList{id});
            QTest::qWait(1600);
            QCOMPARE(h.port.stops, QStringList{id});
            QCOMPARE(h.task(id).value("state"), "queued"); // the record carries the stop
        }
    }

    // A reserved task of an ended owner, waiting for approval or for its snapshot, is
    // cancelled here and nothing reaches the port.
    void anEndedOwnerCancelsAReservedTaskHere() {
        for (const bool approved : {false, true}) {
            Host h(approved ? "full" : "ask");
            h.attach();
            const auto id = idOf(h.run("session.task.start", remote(h.cwd)));
            if (approved)
                QTRY_COMPARE(h.port.submitted, QStringList{id});
            else
                QTRY_COMPARE(h.approvals().size(), 1);
            auto* runtime = h.runtime();
            QVERIFY(runtime);
            runtime->cancelOwner({"owner", 1, {}}, "owner_invalid");
            QTRY_COMPARE(h.task(id).value("state"), "cancelled");
            QVERIFY(h.port.stops.isEmpty());
            if (!approved) {
                h.resolve("accept");
                QTest::qWait(300);
                QVERIFY(h.port.submitted.isEmpty());
            }
        }
    }

    // get and list show containment_mode remote for a remote task; a local one keeps its own.
    void containmentModeIsRemoteOnlyForARemoteTask() {
#ifdef Q_OS_LINUX
        const QString local = "cgroup_v2";
#else
        const QString local = "native_best_effort";
#endif
        Host h("ask");
        h.attach();
        const auto remoteId = idOf(h.run("session.task.start", remote(h.cwd)));
        const auto localId = idOf(h.run("session.task.start", process(h.cwd)));
        QVERIFY(!remoteId.isEmpty() && !localId.isEmpty());
        QCOMPARE(h.task(remoteId).value("containment_mode").toString(), QStringLiteral("remote"));
        QCOMPARE(h.task(localId).value("containment_mode").toString(), local);
        const auto listed = h.run("session.task.list").value("tasks").toArray();
        QCOMPARE(listed.size(), 2);
        for (const auto value : listed) {
            const auto task = value.toObject();
            QCOMPARE(task.value("containment_mode").toString(),
                     task.value("task_id") == remoteId ? QStringLiteral("remote") : local);
        }
    }

    // Where no local executor is installed: a task with runs_on still starts and is
    // submitted, a local one is refused. Runs the cases below in a copy of this test.
    void withoutALocalExecutor() {
        QTemporaryDir directory;
        const QString self = QCoreApplication::applicationFilePath();
        const QString copy = directory.filePath(QFileInfo(self).fileName());
        QVERIFY(QFile::copy(self, copy));
        QVERIFY(
            QFile::setPermissions(copy, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(!QFileInfo::exists(directory.filePath("ketplus_job_executor")));
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(withoutExecutor, "1");
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(copy, {});
        QVERIFY(child.waitForFinished(60000));
        const auto output = child.readAll();
        QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                 output.constData());
        QVERIFY2(output.contains("PASS   : WithoutExecutorTest::aRemoteTaskNeedsNoExecutor()"),
                 output.constData());
        QVERIFY2(output.contains("PASS   : WithoutExecutorTest::aLocalTaskNeedsTheExecutor()"),
                 output.constData());
        QVERIFY2(!output.contains("SKIP"), output.constData());
    }
};

// Run only by withoutALocalExecutor, from a directory without the executor.
class WithoutExecutorTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        QVERIFY(
            !QFileInfo::exists(QCoreApplication::applicationDirPath() + "/ketplus_job_executor"));
    }

    // No executor, containment check or Linux probe: the task is reserved, asked for and
    // submitted once.
    void aRemoteTaskNeedsNoExecutor() {
        Host h("ask");
        h.attach();
        const auto started = h.run("session.task.start", remote(h.cwd));
        QVERIFY2(started.value("ok").toBool(),
                 qPrintable(QJsonDocument(started).toJson(QJsonDocument::Compact)));
        const auto id = idOf(started);
        QCOMPARE(h.rows("remote_jobs"), 1);
        QCOMPARE(h.task(id).value("containment_mode"), "remote");
        QTRY_COMPARE(h.approvals().size(), 1);
        QVERIFY(h.port.submitted.isEmpty());
        QVERIFY(h.resolve("accept"));
        QTRY_COMPARE(h.port.submitted, QStringList{id});
        QTest::qWait(500);
        QCOMPARE(h.port.submitted, QStringList{id});
    }

    void aLocalTaskNeedsTheExecutor() {
        Host h("ask");
        h.attach();
        QCOMPARE(h.run("session.task.start", process(h.cwd)).value("error_code"),
                 "executor_unavailable");
        QCOMPARE(h.rows("managed_jobs"), 0);
        QCOMPARE(h.port.admits, 0);
    }
};

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (qEnvironmentVariableIsSet(withoutExecutor)) {
        WithoutExecutorTest test;
        return QTest::qExec(&test, argc, argv);
    }
    ManagedTaskRemoteHostTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ManagedTaskRemoteHostTest.moc"
