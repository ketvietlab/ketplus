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
#include <cerrno>
#include <csignal>

using namespace ketplus::daemon;

namespace ketplus::daemon {
struct ManagedTaskRuntimeTestAccess {
    static void observe(ManagedTaskRuntime& runtime, const QString& id, const QJsonObject& event) {
        runtime.observeProcess(id, event);
    }
};
} // namespace ketplus::daemon

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

    // Journals written by earlier daemons hold such jobs in needs_attention with their slot; the
    // registry ends them as interrupted when it opens.
    void aRetiredAttentionJobEndsWhenTheRegistryOpensAndReleasesItsSlot() {
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
            QSqlQuery legacy(fixture.database);
            QVERIFY(legacy.exec("UPDATE managed_jobs SET state='needs_attention',"
                                "record=json_set(CAST(record AS TEXT),'$.state','needs_attention',"
                                "'$.reason','cleanup_unverified')"));
            QCOMPARE(fixture.ledger.list(request.owner).value("active_count").toInt(), 1);

            QVERIFY2(fixture.ledger.initialize(&error), qPrintable(error));
            QCOMPARE(fixture.get(id).value("state"), "interrupted");
            QCOMPARE(fixture.get(id).value("reason"), "daemon_upgrade");
            QVERIFY2(fixture.startRuntime(&error), qPrintable(error));
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
                                         fixture.get(id).value("state") == "interrupted",
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
                                         fixture.get(id).value("state") == "interrupted",
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
    // executor_result_commit_failed. A member the supervisor recorded is stopped by the daemon's
    // retry, by its verified identity. A live group member the daemon cannot identify is never
    // signalled; after its bounded window the daemon ends the job anyway, keeps the gap in its
    // diagnostics and leaves the member alone.
    void lostSupervisorOutcomeFollowsTheJobGroup_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("group-empty") << "group-empty";
        QTest::newRow("known-member-stopped-by-retry") << "known-member";
        QTest::newRow("unidentified-member") << "unidentified-member";
        QTest::newRow("restart-between-retries") << "restart-between-retries";
    }

    void lostSupervisorOutcomeFollowsTheJobGroup() {
        QFETCH(QString, mode);
        const bool survivor = mode != "group-empty";
        const bool restartBetween = mode == "restart-between-retries";
        const bool unidentified = mode == "unidentified-member" || restartBetween;
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
            if (mode == "known-member") {
                // The supervisor has recorded the member by identity before it is lost.
                const auto recorded = [&] {
                    QFile saved(fixture.path() + "/jobs/" + id + "/executor.json");
                    if (!saved.open(QIODevice::ReadOnly))
                        return false;
                    const auto receipt = QJsonDocument::fromJson(saved.readAll()).object();
                    return receipt.value("known_processes").toArray().size() > 1;
                };
                QTRY_VERIFY_WITH_TIMEOUT(recorded(), 8000);
            }
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
            if (unidentified) {
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
            fixture.runtime->setCleanupWindow(1500);
            if (restartBetween) {
                // The daemon goes away while it is still retrying: the next one starts the
                // retries over from the persisted receipt, with the same outcome and the same
                // restraint toward the member it cannot identify.
                QTest::qWait(700);
                QVERIFY(fixture.get(id).value("state").toString() != "interrupted");
                fixture.runtime.reset();
                QVERIFY(fixture.startRuntime(&error));
                fixture.runtime->setCleanupWindow(1500);
            }
            if (unidentified) {
                QTRY_COMPARE_WITH_TIMEOUT(fixture.get(id).value("state"), "interrupted",
                                          executor::NativeJobLeaseMs + 15000);
                const auto task = fixture.get(id);
                QCOMPARE(task.value("cleanup_status"), "best_effort_complete");
                QVERIFY(!task.contains("manual_guidance"));
                QVERIFY(task.value("error").toString() != "executor_result_commit_failed");
                QCOMPARE(task.value("slot_held").toBool(), false);
                QSqlQuery diagnostics(fixture.database);
                QVERIFY(diagnostics.exec("SELECT count(*) FROM managed_job_diagnostics WHERE "
                                         "task_id='" +
                                         id + "'"));
                QVERIFY(diagnostics.next());
                QCOMPARE(diagnostics.value(0).toInt(), 1);
                // The daemon never signals a process it cannot identify.
                QFile childFile(fixture.path() + "/child");
                QVERIFY(childFile.open(QIODevice::ReadOnly));
                const auto child = childFile.readAll().trimmed().toInt();
                QVERIFY(child > 1);
                QCOMPARE(::kill(pid_t(child), 0), 0);
            } else {
                QTRY_COMPARE_WITH_TIMEOUT(fixture.get(id).value("state"), "interrupted",
                                          executor::NativeJobLeaseMs + 8000);
                const auto task = fixture.get(id);
                QCOMPARE(task.value("cleanup_status"), "verified");
                QCOMPARE(task.value("cleanup_verification"), "process_group_empty");
                QVERIFY(task.value("error").toString() != "executor_result_commit_failed");
                QCOMPARE(fixture.get(id).value("slot_held").toBool(), false);
                if (survivor) {
                    // The retry signalled the member it had verified by identity.
                    QFile childFile(fixture.path() + "/child");
                    QVERIFY(childFile.open(QIODevice::ReadOnly));
                    const auto child = pid_t(childFile.readAll().trimmed().toInt());
                    QVERIFY(child > 1);
                    QTRY_VERIFY_WITH_TIMEOUT(::kill(child, 0) != 0 && errno == ESRCH, 5000);
                }
            }
        }
        QSqlDatabase::removeDatabase(connection);
    }
#endif
    // Exit 0 or a failure the executor reported is the job's outcome, whether or not cleanup
    // could be verified: the gap goes to the diagnostics, the state does not change (ADR 0127
    // 4a). Without a reported result the job is interrupted.
    void anUnverifiedCleanupKeepsTheRealResult_data() {
        QTest::addColumn<QString>("resultState");
        QTest::addColumn<int>("exitCode");
        QTest::addColumn<QString>("expected");
        QTest::newRow("completed") << "completed" << 0 << "completed";
        QTest::newRow("failed") << "failed" << 3 << "failed";
        QTest::newRow("no-result") << "" << -1 << "interrupted";
    }

    void anUnverifiedCleanupKeepsTheRealResult() {
        QFETCH(QString, resultState);
        QFETCH(int, exitCode);
        QFETCH(QString, expected);
        QString connection;
        {
            Fixture fixture;
            connection = fixture.connection;
            QString error;
            QVERIFY(fixture.initialize(&error));
            const auto started =
                fixture.start(fixture.intent(), fixture.execution({"/bin/sh", "-c", "sleep 60"}));
            const auto id = started.value("task").toObject().value("task_id").toString();
            QTRY_COMPARE(fixture.permissionRequests, 1);
            fixture.runtime->permissionResolved(id, true);
            QTRY_COMPARE_WITH_TIMEOUT(fixture.get(id).value("state"), "running", 6000);
            const auto group = fixture.get(id)
                                   .value("execution_identity")
                                   .toObject()
                                   .value("root_identity")
                                   .toObject()
                                   .value("process_group")
                                   .toInteger();
            const auto cleanup = qScopeGuard([&] { ::kill(-pid_t(group), SIGKILL); });
            fixture.runtime->setCleanupWindow(0);
            QJsonObject receipt{{"event", "done"},
                                {"state", "needs_attention"},
                                {"cleanup_issue", "process_group_not_verified_empty"},
                                {"root_started", true},
                                {"exit_code", exitCode},
                                {"executor_stopped", true}};
            if (!resultState.isEmpty())
                receipt.insert("result_state", resultState);
            ManagedTaskRuntimeTestAccess::observe(*fixture.runtime, id, receipt);
            const auto task = fixture.get(id);
            QCOMPARE(task.value("state").toString(), expected);
            QCOMPARE(task.value("cleanup_status"), "best_effort_complete");
            QCOMPARE(task.value("slot_held").toBool(), false);
            QVERIFY(!task.contains("manual_guidance"));
            if (expected != "interrupted")
                QCOMPARE(task.value("exit_code").toInt(-1), exitCode);
            QSqlQuery diagnostics(fixture.database);
            QVERIFY(diagnostics.exec("SELECT count(*) FROM managed_job_diagnostics WHERE "
                                     "task_id='" +
                                     id + "'"));
            QVERIFY(diagnostics.next());
            QCOMPARE(diagnostics.value(0).toInt(), 1);
        }
        QSqlDatabase::removeDatabase(connection);
    }
};
QTEST_GUILESS_MAIN(ManagedTaskRuntimeTest)
#include "ManagedTaskRuntimeTest.moc"
