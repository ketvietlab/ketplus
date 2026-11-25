#include "runtime/ManagedTaskRuntime.h"
#include "runtime/executor/NativeJobLease.h"
#include "storage/BackgroundTaskLedger.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QScopeGuard>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <csignal>

using namespace ketplus::daemon;

namespace {
class Fixture final {
  public:
    QTemporaryDir directory;
    QString connection{QUuid::createUuid().toString()};
    QSqlDatabase database{QSqlDatabase::addDatabase("QSQLITE", connection)};
    BackgroundTaskLedger ledger{database};
    std::unique_ptr<ManagedTaskRuntime> runtime;
    int permissionRequests{0};
    int updates{0};
    ~Fixture() {
        runtime.reset();
        database.close();
        // Ledger retains a handle until its destructor, so defer removeDatabase
        // until after this fixture is destroyed through the test's scope below.
    }
    QString path() const { return QFileInfo(directory.path()).canonicalFilePath(); }
    bool initialize(QString* error,
                    const QString& executor = QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH)) {
        return openJournal(error) && startRuntime(error, executor);
    }
    bool openJournal(QString* error) {
        database.setDatabaseName(directory.filePath("journal.sqlite"));
        if (!database.open() || !ledger.initialize(error))
            return false;
        QSqlQuery query(database);
        return query.exec("CREATE TABLE sessions(id TEXT PRIMARY KEY,generation INTEGER)") &&
               query.exec("INSERT INTO sessions VALUES('owner',1),('other',1)");
    }
    bool startRuntime(QString* error,
                      const QString& executor = QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH)) {
        runtime = std::make_unique<ManagedTaskRuntime>(ledger, path() + "/jobs", executor);
        runtime->setCallbacks(
            [this](const QJsonObject&) { ++updates; },
            [this](const QJsonObject&, const QJsonObject&) { ++permissionRequests; }, {}, {});
        return runtime->initialize(error);
    }
    BackgroundTaskIntent intent(const QString& key = "start") const {
        return {{"owner", 1, "turn"}, key,   "process",        "Fixture",
                key.toUtf8(),         60000, {{"cwd", path()}}};
    }
    QJsonObject start(const BackgroundTaskIntent& intent, const QJsonObject& execution) {
        QJsonObject reply;
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        deadline.start(10000);
        runtime->start(intent, execution, [&](QJsonObject result) {
            reply = std::move(result);
            loop.quit();
        });
        if (reply.isEmpty())
            loop.exec();
        return reply;
    }
    QJsonObject execution(const QJsonArray& argv) const {
        return {{"argv", argv}, {"cwd", path()}, {"env_delta", QJsonObject{}}};
    }
    QJsonObject get(const QString& id) const {
        return ledger.get(id, {"owner", 1, {}}).value("task").toObject();
    }
};
} // namespace

class ManagedTaskRuntimeTest final : public QObject {
    Q_OBJECT
  private slots:
    void closingIntermediateChildCancelsItsWholeSubtreeWithoutStoppingRoot() {
        QString connection;
        {
            Fixture fixture;
            connection = fixture.connection;
            QString error;
            QVERIFY(fixture.initialize(&error));
            QStringList stopped;
            fixture.runtime->setCallbacks(
                {},
                [&](const QJsonObject& task, const QJsonObject&) {
                    fixture.runtime->permissionResolved(task.value("task_id").toString(), true);
                },
                [&](const QJsonObject& task, const QJsonObject&) {
                    const auto id = task.value("task_id").toString();
                    const auto child = "child-" + id;
                    QSqlQuery query(fixture.database);
                    query.prepare("INSERT INTO sessions VALUES(?,1)");
                    query.addBindValue(child);
                    QVERIFY(query.exec());
                    query.prepare("INSERT INTO managed_job_children VALUES(?,?,1)");
                    query.addBindValue(id);
                    query.addBindValue(child);
                    QVERIFY(query.exec());
                    BackgroundTaskChange running;
                    running.state = "running";
                    running.handshakeCommitted = true;
                    running.executionIdentity = {{"child_session_id", child},
                                                 {"child_generation", 1}};
                    fixture.runtime->childChanged(id, running);
                },
                [&](const QJsonObject& task) { stopped.append(task.value("task_id").toString()); });
            auto request = fixture.intent();
            request.kind = "agent";
            const auto root = fixture.start(request, {{"prompt", "Fake child"}})
                                  .value("task")
                                  .toObject()
                                  .value("task_id")
                                  .toString();
            QTRY_COMPARE(fixture.get(root).value("state"), "running");
            request.owner.sessionId = "child-" + root;
            request.clientRequestId = "nested";
            request.specFingerprint = "nested";
            const auto middle = fixture.start(request, {{"prompt", "Fake nested child"}})
                                    .value("task")
                                    .toObject()
                                    .value("task_id")
                                    .toString();
            QTRY_COMPARE(
                fixture.ledger.get(middle, request.owner).value("task").toObject().value("state"),
                "running");
            const auto middleOwner = request.owner;
            request.owner.sessionId = "child-" + middle;
            request.clientRequestId = "leaf";
            request.specFingerprint = "leaf";
            const auto leaf = fixture.start(request, {{"prompt", "Fake leaf"}})
                                  .value("task")
                                  .toObject()
                                  .value("task_id")
                                  .toString();
            QTRY_COMPARE(
                fixture.ledger.get(leaf, request.owner).value("task").toObject().value("state"),
                "running");
            fixture.runtime->cancelOwner(middleOwner, "session_closed");
            QVERIFY(stopped.contains(middle));
            QVERIFY(stopped.contains(leaf));
            QVERIFY(!stopped.contains(root));
            QCOMPARE(fixture.get(root).value("state"), "running");
            QCOMPARE(
                fixture.ledger.get(middle, middleOwner).value("task").toObject().value("state"),
                "stopping");
            QCOMPARE(
                fixture.ledger.get(leaf, request.owner).value("task").toObject().value("state"),
                "stopping");
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void terminalCommitFailureRetainsReceiptAndRetriesWithoutRelaunch() {
        QString connection;
        {
            Fixture fixture;
            connection = fixture.connection;
            QString error;
            QVERIFY(fixture.initialize(&error));
            QSqlQuery query(fixture.database);
            QVERIFY(query.exec("CREATE TRIGGER fail_terminal BEFORE UPDATE ON managed_jobs "
                               "WHEN new.state='completed' BEGIN "
                               "SELECT RAISE(ABORT,'injected terminal write failure'); END"));
            const auto start = fixture.start(
                fixture.intent(),
                fixture.execution({"/bin/sh", "-c", "printf X >> once; printf result"}));
            const auto id = start.value("task").toObject().value("task_id").toString();
            QTRY_COMPARE(fixture.permissionRequests, 1);
            fixture.runtime->permissionResolved(id, true);
            const auto receiptPath = fixture.path() + "/jobs/" + id + "/executor.json";
            const auto executorFinished = [&] {
                QFile file(receiptPath);
                return file.open(QIODevice::ReadOnly) &&
                       QJsonDocument::fromJson(file.readAll()).object().value("state") ==
                           "completed";
            };
            QTRY_VERIFY_WITH_TIMEOUT(executorFinished(), 5000);
            QTest::qWait(300); // At least one terminal write/retry has been refused.
            QCOMPARE(fixture.get(id).value("state"), "running");
            QVERIFY(query.exec("SELECT count(*) FROM managed_job_outbox"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 0);
            query.finish();
            QVERIFY(query.exec("DROP TRIGGER fail_terminal"));
            QTRY_COMPARE_WITH_TIMEOUT(fixture.get(id).value("state"), "completed", 3000);
            QVERIFY(query.exec("SELECT count(*) FROM managed_job_outbox"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 1);
            query.finish();
            QFile marker(fixture.path() + "/once");
            QVERIFY(marker.open(QIODevice::ReadOnly));
            QCOMPARE(marker.readAll(), QByteArray("X"));
            QCOMPARE(fixture.runtime->output(id, fixture.intent().owner).value("output"), "result");
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void runningStopIsIdempotentAndWaitsForExecutorCleanup() {
        QString connection;
        {
            Fixture fixture;
            connection = fixture.connection;
            QString error;
            QVERIFY(fixture.initialize(&error));
            const auto first =
                fixture.start(fixture.intent(), fixture.execution({"/bin/sleep", "60"}));
            const auto id = first.value("task").toObject().value("task_id").toString();
            QTRY_COMPARE(fixture.permissionRequests, 1);
            fixture.runtime->permissionResolved(id, true);
            QTRY_COMPARE_WITH_TIMEOUT(fixture.get(id).value("state"), "running", 3000);
            QCOMPARE(fixture.runtime->stop(id, {"other", 1, {}}).value("error_code"),
                     "task_not_found");
            const auto stop = fixture.runtime->stop(id, fixture.intent().owner);
            QCOMPARE(stop.value("task").toObject().value("state"), "stopping");
            const auto stoppingRevision = stop.value("task").toObject().value("revision");
            QCOMPARE(fixture.runtime->stop(id, fixture.intent().owner)
                         .value("task")
                         .toObject()
                         .value("revision"),
                     stoppingRevision);
            QTRY_COMPARE_WITH_TIMEOUT(fixture.get(id).value("state"), "cancelled", 5000);
            QCOMPARE(fixture.get(id).value("cleanup_status"), "best_effort_complete");
            QCOMPARE(fixture.get(id).value("output_size").toInteger(), 0);
            QCOMPARE(fixture.get(id).value("completion").toObject().value("status"), "suppressed");
            const auto terminalRevision = fixture.get(id).value("revision");
            QCOMPARE(fixture.runtime->stop(id, fixture.intent().owner)
                         .value("task")
                         .toObject()
                         .value("revision"),
                     terminalRevision);
            QCOMPARE(fixture.ledger.list(fixture.intent().owner).value("active_count").toInt(), 0);
        }
        QSqlDatabase::removeDatabase(connection);
    }

    // A supervisor that dies before its first receipt (a bundle that cannot load QSQLITE did)
    // launched nothing. The job fails and frees its slot instead of waiting for a user.
    void supervisorThatNeverStartsFailsTheJobAndReleasesItsSlot() {
        QString connection;
        {
            Fixture fixture;
            connection = fixture.connection;
            QString error;
            QVERIFY2(fixture.initialize(&error, QStringLiteral("/usr/bin/false")),
                     qPrintable(error));
            const auto request = fixture.intent();
            const auto reply = fixture.start(request, fixture.execution({"/usr/bin/true"}));
            QVERIFY(reply.value("ok").toBool());
            const auto id = reply.value("task").toObject().value("task_id").toString();
            QTRY_COMPARE(fixture.permissionRequests, 1);
            fixture.runtime->permissionResolved(id, true);
            QTRY_COMPARE_WITH_TIMEOUT(fixture.get(id).value("state"), "failed",
                                      ketplus::daemon::executor::NativeJobLeaseMs + 5000);
            const auto task = fixture.get(id);
            QCOMPARE(task.value("reason"), "handshake_failed");
            QCOMPARE(task.value("error"), "executor_never_started");
            QCOMPARE(task.value("cleanup_status"), "not_required");
            QCOMPARE(fixture.ledger.list(request.owner).value("active_count").toInt(), 0);
        }
        QSqlDatabase::removeDatabase(connection);
    }

    // A stop that lands before the client notices the dead supervisor moves the job to
    // stopping. The never-started report must end it at that target, not leave it stopping.
    void stopBeforeNeverStartedReportEndsAtItsTargetAndReleasesSlot() {
        QString connection;
        {
            Fixture fixture;
            connection = fixture.connection;
            QString error;
            QVERIFY2(fixture.initialize(&error, QStringLiteral("/usr/bin/false")),
                     qPrintable(error));
            const auto request = fixture.intent();
            const auto reply = fixture.start(request, fixture.execution({"/usr/bin/true"}));
            QVERIFY(reply.value("ok").toBool());
            const auto id = reply.value("task").toObject().value("task_id").toString();
            QTRY_COMPARE(fixture.permissionRequests, 1);
            fixture.runtime->permissionResolved(id, true);
            QTRY_COMPARE_WITH_TIMEOUT(fixture.get(id).value("state"), "starting", 3000);
            const auto stopped = fixture.runtime->stop(id, request.owner, false);
            QVERIFY(stopped.value("ok").toBool());
            QCOMPARE(fixture.get(id).value("state"), "stopping");
            QTRY_COMPARE_WITH_TIMEOUT(fixture.get(id).value("state"), "cancelled",
                                      ketplus::daemon::executor::NativeJobLeaseMs + 5000);
            const auto task = fixture.get(id);
            QCOMPARE(task.value("error"), "executor_never_started");
            QCOMPARE(task.value("cleanup_status"), "not_required");
            QCOMPARE(fixture.ledger.list(request.owner).value("active_count").toInt(), 0);
        }
        QSqlDatabase::removeDatabase(connection);
    }

    // Journals written by affected builds hold such jobs in needs_attention with their slot.
    void restartEndsNeverStartedAttentionJobAndReleasesItsSlot() {
        QString connection;
        {
            Fixture fixture;
            connection = fixture.connection;
            QString error;
            QVERIFY2(fixture.openJournal(&error), qPrintable(error));
            const auto request = fixture.intent();
            const auto id =
                fixture.ledger.create(request).value("task").toObject().value("task_id").toString();
            BackgroundTaskChange approval;
            approval.state = "queued";
            approval.permissionApproved = true;
            QVERIFY(fixture.ledger.change(id, 1, approval).value("ok").toBool());
            const auto claimed = fixture.ledger.claimNext().value("task").toObject();
            QCOMPARE(claimed.value("task_id").toString(), id);
            const auto directory = fixture.path() + "/jobs/" + id;
            QVERIFY(QDir().mkpath(directory));
            QVERIFY(QFile::setPermissions(directory,
                                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
            const auto registered = fixture.ledger.registerExecutor(
                id, claimed.value("revision").toInteger(),
                {{"directory", directory},
                 {"executor_lease_identity", QUuid::createUuid().toString(QUuid::WithoutBraces)}});
            QVERIFY(registered.value("ok").toBool());
            BackgroundTaskChange attention;
            attention.state = "needs_attention";
            attention.executorStopped = false;
            attention.errorCode = "executor_connection_lost";
            const auto revision = registered.value("task").toObject().value("revision").toInteger();
            QVERIFY(fixture.ledger.change(id, revision, attention).value("ok").toBool());
            QCOMPARE(fixture.ledger.list(request.owner).value("active_count").toInt(), 1);

            QVERIFY2(fixture.startRuntime(&error), qPrintable(error));
            QTRY_COMPARE_WITH_TIMEOUT(fixture.get(id).value("state"), "failed",
                                      ketplus::daemon::executor::NativeJobLeaseMs + 5000);
            QCOMPARE(fixture.get(id).value("reason"), "handshake_failed");
            QCOMPARE(fixture.ledger.list(request.owner).value("active_count").toInt(), 0);
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void runtimeRestartReconnectsLiveSupervisorWithoutReplayingSideEffect() {
        QString connection;
        {
            Fixture fixture;
            connection = fixture.connection;
            QString error;
            QVERIFY(fixture.initialize(&error));
            const auto first =
                fixture.start(fixture.intent(),
                              fixture.execution({"/bin/sh", "-c", "printf X >> once; sleep 2"}));
            const auto id = first.value("task").toObject().value("task_id").toString();
            QTRY_COMPARE(fixture.permissionRequests, 1);
            fixture.runtime->permissionResolved(id, true);
            QTRY_COMPARE_WITH_TIMEOUT(fixture.get(id).value("state"), "running", 3000);
            fixture.runtime.reset();
            fixture.runtime =
                std::make_unique<ManagedTaskRuntime>(fixture.ledger, fixture.path() + "/jobs",
                                                     QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH));
            fixture.runtime->setCallbacks(
                {}, [&](const QJsonObject&, const QJsonObject&) { ++fixture.permissionRequests; },
                {}, {});
            QVERIFY(fixture.runtime->initialize(&error));
            QTRY_VERIFY_WITH_TIMEOUT(fixture.get(id).value("state") == "completed" ||
                                         fixture.get(id).value("state") == "needs_attention",
                                     6000);
            QFile receipt(fixture.path() + "/jobs/" + id + "/executor.json");
            QVERIFY(receipt.open(QIODevice::ReadOnly));
            QVERIFY2(fixture.get(id).value("state") == "completed", receipt.readAll().constData());
            QCOMPARE(fixture.permissionRequests, 1);
            QFile marker(fixture.path() + "/once");
            QVERIFY(marker.open(QIODevice::ReadOnly));
            QCOMPARE(marker.readAll(), QByteArray("X"));
            QCOMPARE(fixture.get(id).value("cleanup_status"), "best_effort_complete");
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void intentCommitsBeforePermissionAndLaunchThenOutputIsOwnerScoped() {
        QString connection;
        {
            Fixture fixture;
            connection = fixture.connection;
            QString error;
            QVERIFY2(fixture.initialize(&error), qPrintable(error));
            const auto request = fixture.intent();
            const auto spec = fixture.execution({"/usr/bin/printf", "hello"});
            const auto reply = fixture.start(request, spec);
            QVERIFY(reply.value("ok").toBool());
            const auto id = reply.value("task").toObject().value("task_id").toString();
            QCOMPARE(fixture.get(id).value("state"), "queued");
            QCOMPARE(fixture.permissionRequests, 0);
            QVERIFY(!QFileInfo::exists(fixture.path() + "/jobs/" + id + "/executor.json"));
            QTRY_COMPARE(fixture.permissionRequests, 1);
            QCOMPARE(fixture.get(id).value("state"), "queued");
            QCOMPARE(fixture.ledger.list(request.owner).value("active_count").toInt(), 0);
            fixture.runtime->permissionResolved(id, true);
            QTRY_VERIFY_WITH_TIMEOUT(fixture.get(id).value("state") == "completed" ||
                                         fixture.get(id).value("state") == "needs_attention",
                                     10000);
            QVERIFY2(fixture.get(id).value("state") == "completed",
                     qPrintable(QString::fromUtf8(QJsonDocument(fixture.get(id)).toJson())));
            QCOMPARE(fixture.get(id).value("cleanup_status"), "best_effort_complete");
            QCOMPARE(fixture.get(id).value("completion").toObject().value("status"), "pending");
            const auto output = fixture.runtime->output(id, request.owner);
            QVERIFY(output.value("ok").toBool());
            QCOMPARE(output.value("output"), "hello");
            QCOMPARE(fixture.get(id).value("output_size").toInteger(), 5);
            QCOMPARE(fixture.runtime->output(id, {"other", 1, {}}).value("error_code"),
                     "task_not_found");
            const auto retry = fixture.start(request, spec);
            QCOMPARE(retry.value("task").toObject().value("task_id"), id);
            QVERIFY(retry.value("reused").toBool());
            QCOMPARE(fixture.permissionRequests, 1);
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void deniedPermissionAndQueuedStopNeverLaunch() {
        QString connection;
        {
            Fixture fixture;
            connection = fixture.connection;
            QString error;
            QVERIFY(fixture.initialize(&error));
            const auto spec = fixture.execution({"/bin/sleep", "60"});
            const auto first = fixture.start(fixture.intent(), spec);
            const auto id = first.value("task").toObject().value("task_id").toString();
            QTRY_COMPARE(fixture.permissionRequests, 1);
            fixture.runtime->permissionResolved(id, false);
            QCOMPARE(fixture.get(id).value("state"), "failed");
            QCOMPARE(fixture.get(id).value("completion").toObject().value("status"), "suppressed");
            QVERIFY(!QFileInfo::exists(fixture.path() + "/jobs/" + id));
            const auto second = fixture.start(fixture.intent("second"), spec);
            const auto nextId = second.value("task").toObject().value("task_id").toString();
            QVERIFY(fixture.runtime->stop(nextId, fixture.intent().owner).value("ok").toBool());
            QCOMPARE(fixture.get(nextId).value("state"), "cancelled");
            const auto revision = fixture.get(nextId).value("revision");
            QVERIFY(fixture.runtime->stop(nextId, fixture.intent().owner).value("ok").toBool());
            QCOMPARE(fixture.get(nextId).value("revision"), revision);
            fixture.runtime->permissionResolved(nextId, true);
            QTest::qWait(300);
            QCOMPARE(fixture.get(nextId).value("state"), "cancelled");
            QCOMPARE(fixture.permissionRequests, 1);
            QVERIFY(!QFileInfo::exists(fixture.path() + "/jobs/" + nextId));
        }
        QSqlDatabase::removeDatabase(connection);
    }

    // ADR 0127 §7a: a job held behind its root's full slots tells its owner once, and why,
    // well before its timeout; the notice is superseded when the job ends.
    void aJobQueuedBehindFullRootSlotsNotifiesItsOwnerOnceBeforeItsTimeout() {
        QString connection;
        {
            Fixture fixture;
            connection = fixture.connection;
            QString error;
            QVERIFY(fixture.initialize(&error));
            fixture.runtime->setCallbacks(
                {},
                [&](const QJsonObject& task, const QJsonObject&) {
                    fixture.runtime->permissionResolved(task.value("task_id").toString(), true);
                },
                [&](const QJsonObject& task, const QJsonObject&) {
                    const auto id = task.value("task_id").toString();
                    const auto child = "child-" + id;
                    QSqlQuery query(fixture.database);
                    query.prepare("INSERT INTO sessions VALUES(?,1)");
                    query.addBindValue(child);
                    QVERIFY(query.exec());
                    query.prepare("INSERT INTO managed_job_children VALUES(?,?,1)");
                    query.addBindValue(id);
                    query.addBindValue(child);
                    QVERIFY(query.exec());
                    BackgroundTaskChange running;
                    running.state = "running";
                    running.handshakeCommitted = true;
                    running.executionIdentity = {{"child_session_id", child},
                                                 {"child_generation", 1}};
                    fixture.runtime->childChanged(id, running);
                },
                [](const QJsonObject&) {});
            QStringList holders;
            for (int i = 0; i < BackgroundTaskLedger::rootSlotLimit; ++i) {
                auto request = fixture.intent("holder" + QString::number(i));
                request.kind = "agent";
                request.timeoutMs = 60000;
                const auto id = fixture.start(request, {{"prompt", "Fake child"}})
                                    .value("task")
                                    .toObject()
                                    .value("task_id")
                                    .toString();
                QTRY_COMPARE(fixture.get(id).value("state"), "running");
                holders.append(id);
            }
            auto request = fixture.intent("queued");
            request.kind = "agent";
            request.timeoutMs = 3000;
            const auto queued = fixture.start(request, {{"prompt", "Fake queued child"}})
                                    .value("task")
                                    .toObject()
                                    .value("task_id")
                                    .toString();
            QTRY_VERIFY(fixture.get(queued).value("permission_approved").toBool());
            QCOMPARE(fixture.get(queued).value("state"), "queued");
            const auto notices = [&](const QString& status) {
                QSqlQuery query(fixture.database);
                query.prepare("SELECT count(*) FROM managed_job_notices WHERE task_id=? "
                              "AND kind='queued' AND status=?");
                query.addBindValue(queued);
                query.addBindValue(status);
                return query.exec() && query.next() ? query.value(0).toInt() : -1;
            };
            QTRY_COMPARE_WITH_TIMEOUT(notices("pending"), 1, 2500);
            // Still queued: the notice came before the timeout, not with it.
            QCOMPARE(fixture.get(queued).value("state"), "queued");
            const auto page = fixture.ledger.noticePage().value("events").toArray();
            QCOMPARE(page.size(), 1);
            const auto turn = page.first().toObject().value("turn_record").toObject();
            QCOMPARE(turn.value("event"), "queued");
            QCOMPARE(turn.value("queue_reason"), "root_slot_limit");
            QCOMPARE(turn.value("slot_limit").toInt(), BackgroundTaskLedger::rootSlotLimit);
            QStringList named;
            for (const auto holder : turn.value("slot_holders").toArray())
                named.append(holder.toObject().value("task_id").toString());
            QCOMPARE(named, holders);
            QVERIFY(fixture.ledger.noticeCurrent(turn));
            QTRY_COMPARE_WITH_TIMEOUT(fixture.get(queued).value("state"), "timed_out", 3000);
            QCOMPARE(notices("pending"), 0);
            QCOMPARE(notices("suppressed"), 1);
            QVERIFY(!fixture.ledger.noticeCurrent(turn));
        }
        QSqlDatabase::removeDatabase(connection);
    }
#ifdef Q_OS_MACOS
    // A lost supervisor whose job group is verified empty ends interrupted with verified
    // cleanup, through the runtime's own event path (observeProcess) and with no
    // executor_result_commit_failed. A live, unaccounted group member keeps needs_attention.
    void lostSupervisorOutcomeFollowsTheJobGroup_data() {
        QTest::addColumn<bool>("survivor");
        QTest::newRow("group-empty") << false;
        QTest::newRow("live-unaccounted-member") << true;
    }

    void lostSupervisorOutcomeFollowsTheJobGroup() {
        QFETCH(bool, survivor);
        QString connection;
        {
            Fixture fixture;
            connection = fixture.connection;
            QString error;
            QVERIFY(fixture.initialize(&error));
            const auto script = survivor ? QStringLiteral("sleep 60 & echo $! > child; wait")
                                         : QStringLiteral("exec sleep 60");
            const auto started =
                fixture.start(fixture.intent(), fixture.execution({"/bin/sh", "-c", script}));
            const auto id = started.value("task").toObject().value("task_id").toString();
            QTRY_COMPARE(fixture.permissionRequests, 1);
            fixture.runtime->permissionResolved(id, true);
            QTRY_COMPARE_WITH_TIMEOUT(fixture.get(id).value("state"), "running", 6000);
            if (survivor)
                QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(fixture.path() + "/child"), 3000);
            const auto identity = fixture.get(id).value("execution_identity").toObject();
            const auto root = identity.value("root_identity").toObject();
            const auto supervisor = identity.value("supervisor_identity").toObject();
            QVERIFY(root.value("pid").toInteger() > 1);
            QVERIFY(supervisor.value("pid").toInteger() > 1);
            const auto group = root.value("process_group").toInteger();
            const auto cleanup = qScopeGuard([&] { ::kill(-pid_t(group), SIGKILL); });
            fixture.runtime.reset();
            QCOMPARE(::kill(pid_t(supervisor.value("pid").toInteger()), SIGKILL), 0);
            QCOMPARE(::kill(pid_t(root.value("pid").toInteger()), SIGKILL), 0);
            if (survivor) {
                // The surviving member is absent from the persisted receipt, so recovery
                // cannot signal it: only the group listing can reveal it.
                const auto receiptPath = fixture.path() + "/jobs/" + id + "/executor.json";
                QFile receipt(receiptPath);
                QVERIFY(receipt.open(QIODevice::ReadOnly));
                auto saved = QJsonDocument::fromJson(receipt.readAll()).object();
                receipt.close();
                saved.insert("known_processes", QJsonArray{});
                QVERIFY(receipt.open(QIODevice::WriteOnly | QIODevice::Truncate));
                receipt.write(QJsonDocument(saved).toJson());
                receipt.close();
            }
            QVERIFY(fixture.startRuntime(&error));
            if (survivor) {
                QTRY_COMPARE_WITH_TIMEOUT(fixture.get(id).value("state"), "needs_attention",
                                          executor::NativeJobLeaseMs + 8000);
                QVERIFY(fixture.get(id).value("cleanup_status") != "verified");
            } else {
                QTRY_COMPARE_WITH_TIMEOUT(fixture.get(id).value("state"), "interrupted",
                                          executor::NativeJobLeaseMs + 8000);
                const auto task = fixture.get(id);
                QCOMPARE(task.value("cleanup_status"), "verified");
                QCOMPARE(task.value("cleanup_verification"), "process_group_empty");
                QVERIFY(task.value("error").toString() != "executor_result_commit_failed");
            }
        }
        QSqlDatabase::removeDatabase(connection);
    }
#endif
};
QTEST_GUILESS_MAIN(ManagedTaskRuntimeTest)
#include "ManagedTaskRuntimeTest.moc"
