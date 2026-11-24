#include "storage/BackgroundTaskLedger.h"

#include <QJsonArray>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace ketplus::daemon;

namespace {
bool seedOwner(QSqlDatabase database) {
    QSqlQuery query(database);
    return query.exec(
               "CREATE TABLE IF NOT EXISTS sessions(id TEXT PRIMARY KEY,generation INTEGER)") &&
           query.exec("INSERT OR IGNORE INTO sessions VALUES('owner',1),('other',1)");
}
BackgroundTaskIntent intent() {
    return {{"owner", 1, "turn"},      "request", "process", "Build",
            QByteArray("fingerprint"), 60000,     {}};
}
QString taskId(const QJsonObject& reply) {
    return reply.value("task").toObject().value("task_id").toString();
}
QJsonObject failed(BackgroundTaskLedger& ledger, const QString& id) {
    BackgroundTaskChange change;
    change.state = "failed";
    change.reason = "spawn_failed";
    change.neverLaunched = true;
    return ledger.change(id, 1, change);
}
// A native job that runs, at revision 4.
QString runningJob(BackgroundTaskLedger& ledger) {
    const auto id = taskId(ledger.create(intent()));
    BackgroundTaskChange approval;
    approval.state = "queued";
    approval.permissionApproved = true;
    if (!ledger.change(id, 1, approval).value("ok").toBool() || taskId(ledger.claimNext()) != id)
        return {};
    BackgroundTaskChange running;
    running.state = "running";
    running.handshakeCommitted = true;
    running.executionIdentity = {{"process_birth_identity", "birth"},
                                 {"executor_lease_identity", "lease"}};
    return ledger.change(id, 3, running).value("ok").toBool() ? id : QString();
}
int count(QSqlDatabase database, const QString& sql) {
    QSqlQuery query(database);
    return query.exec(sql) && query.next() ? query.value(0).toInt() : -1;
}
QString wakeState(QSqlDatabase database, const QString& eventId) {
    QSqlQuery query(database);
    query.prepare("SELECT state FROM managed_job_wakes WHERE event_id=?");
    query.addBindValue(eventId);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}
} // namespace

class BackgroundTaskLedgerTest final : public QObject {
    Q_OBJECT

  private slots:
    void retentionCommitsTombstoneAndNeverReexecutesAnExpiredRequest() {
        QTemporaryDir directory;
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(directory.filePath("registry.sqlite"));
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            auto request = intent();
            request.kind = "agent";
            const QJsonObject defaults{{"provider", "fake"},
                                       {"model", "old-model"},
                                       {"effort", "normal"},
                                       {"cwd", directory.path()}};
            request.executionMetadata = {{"request_defaults", defaults}};
            request.requestPayload = {{"kind", "agent"},
                                      {"client_request_id", "request"},
                                      {"execution_spec", QJsonObject{{"prompt", "work"}}}};
            request.specFingerprint = backgroundTaskFingerprint(request.requestPayload, defaults);
            const auto id = taskId(ledger.create(request));
            BackgroundTaskChange denied;
            denied.state = "failed";
            denied.reason = "permission_denied";
            denied.neverLaunched = true;
            const auto terminal = ledger.change(id, 1, denied);
            const auto ended = terminal.value("task").toObject().value("ended_at").toInteger();
            const auto future = ended + 7LL * 24 * 60 * 60 * 1000 + 10000;
            QCOMPARE(ledger.prune(ended + 1000).value("pruned").toInt(), 0);
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TRIGGER fail_prune BEFORE DELETE ON managed_jobs "
                               "BEGIN SELECT RAISE(ABORT,'injected retention crash'); END"));
            QVERIFY(!ledger.prune(future).value("ok").toBool());
            QVERIFY(ledger.get(id, request.owner).value("ok").toBool());
            QVERIFY(query.exec("SELECT count(*) FROM managed_job_tombstones"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 0);
            query.finish();
            QVERIFY(query.exec("DROP TRIGGER fail_prune"));
            QCOMPARE(ledger.prune(future).value("pruned").toInt(), 1);
            QCOMPARE(ledger.get(id, request.owner).value("error_code"), "task_not_found");
            db.close();
            QVERIFY(db.open());
            BackgroundTaskLedger restarted(db);
            QVERIFY(restarted.initialize(&error));
            const auto expired = restarted.create(request);
            QCOMPARE(expired.value("error_code"), "task_expired");
            QCOMPARE(expired.value("task_id"), id);
            QVERIFY(expired.value("expired").toBool());
            QVERIFY(restarted.prune(future).value("output_cleanup").toArray().contains(id));
            QVERIFY(restarted.outputRemoved(id));
            QVERIFY(restarted.prune(future).value("output_cleanup").toArray().isEmpty());
            auto spec = request.requestPayload.value("execution_spec").toObject();
            spec.insert("model", "old-model");
            request.requestPayload.insert("execution_spec", spec);
            // Omitted and explicit accepted defaults are the same canonical spec.
            QCOMPARE(restarted.retry(request).value("error_code"), "task_expired");
            spec.insert("model", "new-model");
            request.requestPayload.insert("execution_spec", spec);
            QCOMPARE(restarted.create(request).value("error_code"), "idempotency_conflict");
            QVERIFY(restarted.nonterminal().value("tasks").toArray().isEmpty());
            auto pending = intent();
            pending.clientRequestId = "pending";
            const auto pendingId = taskId(restarted.create(pending));
            const auto pendingTerminal = failed(restarted, pendingId);
            const auto event =
                pendingTerminal.value("task").toObject().value("completion_event_id").toString();
            QCOMPARE(restarted.prune(future).value("pruned").toInt(), 0);
            QVERIFY(restarted.acceptCompletion(event, pending.owner).value("ok").toBool());
            QCOMPARE(restarted.prune(future).value("pruned").toInt(), 0);
            QVERIFY(restarted.claimWake(event, pending.owner).value("ok").toBool());
            QVERIFY(restarted.finishWake(event, "finished").value("ok").toBool());
            QCOMPARE(restarted.prune(future).value("pruned").toInt(), 1);
        }
        QSqlDatabase::removeDatabase(name);
    }

    void acceptedWakeSurvivesRestartButUncertainDispatchNeverReplays() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            const auto id = taskId(ledger.create(intent()));
            const auto ended = failed(ledger, id);
            const auto event =
                ended.value("task").toObject().value("completion_event_id").toString();
            QVERIFY(!event.isEmpty());
            QCOMPARE(ledger.deliveryPage(false).value("events").toArray().size(), 1);
            QVERIFY(ledger.acceptCompletion(event, {"owner", 1, {}}).value("ok").toBool());
            QVERIFY(ledger.recoverDispatchingWakes().value("ok").toBool());
            QCOMPARE(ledger.deliveryPage(true).value("events").toArray().size(), 1);
            QVERIFY(ledger.claimWake(event, {"owner", 1, {}}).value("ok").toBool());
            QVERIFY(ledger.wakeDispatchAllowed(event, {"owner", 1, {}}));
            QVERIFY(ledger.deferWake(event).value("ok").toBool());
            QCOMPARE(ledger.deliveryPage(true).value("events").toArray().size(), 1);
            QVERIFY(ledger.claimWake(event, {"owner", 1, {}}).value("ok").toBool());
            QVERIFY(ledger.recoverDispatchingWakes().value("ok").toBool());
            QVERIFY(!ledger.wakeDispatchAllowed(event, {"owner", 1, {}}));
            QVERIFY(ledger.deliveryPage(true).value("events").toArray().isEmpty());
            QVERIFY(ledger.acceptCompletion(event, {"owner", 1, {}}).value("duplicate").toBool());
            QVERIFY(ledger.deliveryPage(true).value("events").toArray().isEmpty());
            QSqlQuery query(db);
            QVERIFY(query.exec("UPDATE sessions SET generation=2 WHERE id='owner'"));
            QCOMPARE(ledger.get(id, {"owner", 1, {}}).value("error_code"), "stale_owner");
            QVERIFY(ledger.get(id, {"owner", 1, {}}, true).value("ok").toBool());
            QVERIFY(ledger.list({"owner", 1, {}}, {}, 50, true).value("ok").toBool());
            QCOMPARE(ledger.get(id, {"other", 1, {}}, true).value("error_code"), "task_not_found");
        }
        QSqlDatabase::removeDatabase(name);
    }

    void managedChildReservationIsAtomicAndTerminalWaitsForDescendants() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            auto request = intent();
            request.kind = "agent";
            const auto parent = taskId(ledger.create(request));
            BackgroundTaskChange approve;
            approve.state = "queued";
            approve.permissionApproved = true;
            QVERIFY(ledger.change(parent, 1, approve).value("ok").toBool());
            QCOMPARE(taskId(ledger.claimNext()), parent);
            QSqlQuery query(db);
            QVERIFY(query.exec(
                "CREATE TRIGGER fail_child BEFORE UPDATE ON managed_jobs "
                "WHEN json_extract(new.record,'$.execution_identity.child_session_id') IS NOT NULL "
                "BEGIN SELECT RAISE(ABORT,'injected child reservation failure'); END"));
            QCOMPARE(ledger.reserveChild(parent, 3).value("error_code"), "storage_error");
            QVERIFY(query.exec("SELECT count(*) FROM managed_job_children"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 0);
            query.finish();
            QVERIFY(query.exec("DROP TRIGGER fail_child"));
            const auto reserved = ledger.reserveChild(parent, 3);
            QVERIFY(reserved.value("ok").toBool());
            const auto identity =
                reserved.value("task").toObject().value("execution_identity").toObject();
            const auto childSession = identity.value("child_session_id").toString();
            QVERIFY(!childSession.isEmpty());
            const auto retry = ledger.reserveChild(parent, 4);
            QVERIFY(retry.value("reused").toBool());
            QCOMPARE(retry.value("task").toObject().value("execution_identity").toObject(),
                     identity);
            BackgroundTaskChange running;
            running.state = "running";
            running.handshakeCommitted = true;
            running.executionIdentity = identity;
            QCOMPARE(ledger.change(parent, 4, running).value("error_code"), "invalid_transition");
            query.prepare("INSERT INTO sessions VALUES(?,1)");
            query.addBindValue(childSession);
            QVERIFY(query.exec());
            QVERIFY(ledger.change(parent, 4, running).value("ok").toBool());
            auto descendant = intent();
            descendant.owner.sessionId = childSession;
            const auto childTask = taskId(ledger.create(descendant));
            QVERIFY(!childTask.isEmpty());
            BackgroundTaskChange complete;
            complete.state = "completed";
            complete.cleanupConfirmed = true;
            QCOMPARE(ledger.change(parent, 5, complete).value("error_code"), "execution_live");
            BackgroundTaskChange cancelled;
            cancelled.state = "stopping";
            cancelled.terminalTarget = "cancelled";
            QVERIFY(ledger.change(childTask, 1, cancelled).value("ok").toBool());
            cancelled.state = "cancelled";
            cancelled.cleanupConfirmed = true;
            cancelled.neverLaunched = true;
            QVERIFY(ledger.change(childTask, 2, cancelled).value("ok").toBool());
            QVERIFY(ledger.change(parent, 5, complete).value("ok").toBool());
            descendant.clientRequestId = "too-late";
            QCOMPARE(ledger.create(descendant).value("error_code"), "stale_owner");
            const auto event = ledger.get(parent, request.owner)
                                   .value("task")
                                   .toObject()
                                   .value("completion_event_id")
                                   .toString();
            QVERIFY(ledger.acceptCompletion(event, request.owner).value("ok").toBool());
            QVERIFY(ledger.claimWake(event, request.owner).value("ok").toBool());
            QVERIFY(ledger.finishWake(event, "finished").value("ok").toBool());
            QVERIFY(ledger.prune(QDateTime::currentMSecsSinceEpoch() + 8LL * 24 * 60 * 60 * 1000)
                        .value("ok")
                        .toBool());
            QCOMPARE(ledger.linkedTask(childSession, 1).value("error_code"), "task_expired");
            QCOMPARE(ledger.create(descendant).value("error_code"), "stale_owner");
        }
        QSqlDatabase::removeDatabase(name);
    }

    void attentionCountsTowardNonterminalCapAndOnlyRootSlotsBoundExecution() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            BackgroundTaskChange approve;
            approve.state = "queued";
            approve.permissionApproved = true;
            BackgroundTaskChange attention;
            attention.state = "needs_attention";
            attention.executorStopped = true;
            for (int i = 0; i < 116; ++i) {
                auto request = intent();
                request.clientRequestId = QString::number(i);
                const auto id = taskId(ledger.create(request));
                QVERIFY(!id.isEmpty());
                QVERIFY(ledger.change(id, 1, approve).value("ok").toBool());
                QCOMPARE(taskId(ledger.claimNext()), id);
                QVERIFY(ledger.change(id, 3, attention).value("ok").toBool());
            }
            QCOMPARE(ledger.create(intent()).value("error_code"), "resource_exhausted");
            auto retry = intent();
            retry.clientRequestId = "0";
            QVERIFY(ledger.create(retry).value("reused").toBool());
            QCOMPARE(ledger.nonterminal().value("tasks").toArray().size(), 116);
            QCOMPARE(ledger.list(intent().owner).value("active_count").toInt(), 0);
        }
        QSqlDatabase::removeDatabase(name);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            QSqlQuery query(db);
            for (int root = 0; root < 5; ++root) {
                const auto owner = QString::number(root);
                query.prepare("INSERT INTO sessions VALUES(?,1)");
                query.addBindValue(owner);
                QVERIFY(query.exec());
                for (int i = 0; i < 4; ++i) {
                    auto request = intent();
                    request.owner.sessionId = owner;
                    request.clientRequestId = QString::number(i);
                    const auto id = taskId(ledger.create(request));
                    BackgroundTaskChange approve;
                    approve.state = "queued";
                    approve.permissionApproved = true;
                    QVERIFY(ledger.change(id, 1, approve).value("ok").toBool());
                }
            }
            // No machine-wide slot limit: every root runs its four jobs.
            for (int i = 0; i < 20; ++i)
                QVERIFY(!taskId(ledger.claimNext()).isEmpty());
            QVERIFY(ledger.claimNext().value("task").isNull());
            QCOMPARE(ledger.list({"4", 1, {}}).value("active_count").toInt(), 4);
            QCOMPARE(ledger.list({"4", 1, {}}).value("tasks").toArray().size(), 4);
            // A fifth job of one root still waits for that root's own slot.
            auto fifth = intent();
            fifth.owner.sessionId = "4";
            fifth.clientRequestId = "fifth";
            const auto queuedId = taskId(ledger.create(fifth));
            BackgroundTaskChange approve;
            approve.state = "queued";
            approve.permissionApproved = true;
            QVERIFY(ledger.change(queuedId, 1, approve).value("ok").toBool());
            QVERIFY(ledger.claimNext().value("task").isNull());
        }
        QSqlDatabase::removeDatabase(name);
    }

    void neverStartedAttentionFailsWithoutUserAndReleasesSlot() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            BackgroundTaskChange approval;
            approval.state = "queued";
            approval.permissionApproved = true;
            BackgroundTaskChange attention;
            attention.state = "needs_attention";
            attention.executorStopped = false;
            BackgroundTaskChange neverStarted;
            neverStarted.state = "failed";
            neverStarted.reason = "handshake_failed";
            neverStarted.errorCode = "executor_never_started";
            neverStarted.neverLaunched = true;
            neverStarted.cleanupStatus = "not_required";

            // No handshake was committed, so the daemon never sent start.
            auto request = intent();
            request.clientRequestId = "never-started";
            const auto id = taskId(ledger.create(request));
            QVERIFY(ledger.change(id, 1, approval).value("ok").toBool());
            QCOMPARE(taskId(ledger.claimNext()), id);
            QVERIFY(ledger.change(id, 3, attention).value("ok").toBool());
            QCOMPARE(ledger.list(intent().owner).value("active_count").toInt(), 1);
            auto partial = neverStarted;
            partial.neverLaunched = false;
            QCOMPARE(ledger.change(id, 4, partial).value("error_code"), "invalid_transition");
            const auto result = ledger.change(id, 4, neverStarted);
            QVERIFY(result.value("ok").toBool());
            const auto task = result.value("task").toObject();
            QCOMPARE(task.value("state"), "failed");
            QCOMPARE(task.value("reason"), "handshake_failed");
            QCOMPARE(task.value("error"), "executor_never_started");
            QCOMPARE(task.value("cleanup_status"), "not_required");
            QCOMPARE(task.value("executor_stopped").toBool(), true);
            QCOMPARE(ledger.list(intent().owner).value("active_count").toInt(), 0);

            // A committed handshake may have been followed by start: only a user ends it.
            request.clientRequestId = "handshaken";
            const auto ran = taskId(ledger.create(request));
            QVERIFY(ledger.change(ran, 1, approval).value("ok").toBool());
            QCOMPARE(taskId(ledger.claimNext()), ran);
            BackgroundTaskChange running;
            running.state = "running";
            running.handshakeCommitted = true;
            running.executionIdentity = {{"process_birth_identity", "birth"},
                                         {"executor_lease_identity", "lease"}};
            QVERIFY(ledger.change(ran, 3, running).value("ok").toBool());
            QVERIFY(ledger.change(ran, 4, attention).value("ok").toBool());
            QCOMPARE(ledger.change(ran, 5, neverStarted).value("error_code"), "invalid_transition");
            QCOMPARE(ledger.list(intent().owner).value("active_count").toInt(), 1);
        }
        QSqlDatabase::removeDatabase(name);
    }

    void nativeCleanupAttentionAndUserAuditAreDurableAndQueueNoCompletion() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            const auto id = taskId(ledger.create(intent()));
            BackgroundTaskChange approval;
            approval.state = "queued";
            approval.permissionApproved = true;
            QVERIFY(ledger.change(id, 1, approval).value("ok").toBool());
            QCOMPARE(taskId(ledger.claimNext()), id);
            BackgroundTaskChange running;
            running.state = "running";
            running.handshakeCommitted = true;
            running.executionIdentity = {{"process_birth_identity", "birth"},
                                         {"executor_lease_identity", "lease"}};
            QVERIFY(ledger.change(id, 3, running).value("ok").toBool());
            BackgroundTaskChange attention;
            attention.state = "needs_attention";
            attention.exitCode = 0;
            attention.executorStopped = false;
            auto result = ledger.change(id, 4, attention);
            QVERIFY(result.value("ok").toBool());
            auto task = result.value("task").toObject();
            QCOMPARE(task.value("cleanup_status"), "unverified");
            QVERIFY(task.value("completion").isNull());
            QCOMPARE(ledger.list(intent().owner).value("active_count").toInt(), 1);
            QCOMPARE(ledger.resolveCleanup(id, 5, "local-user").value("error_code"),
                     "invalid_transition");
            attention.executorStopped = true;
            QVERIFY(ledger.change(id, 5, attention).value("ok").toBool());
            QCOMPARE(ledger.list(intent().owner).value("active_count").toInt(), 0);
            QCOMPARE(ledger.resolveCleanup(id, 6, {}).value("error_code"), "user_action_required");
            BackgroundTaskChange completed;
            completed.state = "completed";
            completed.cleanupConfirmed = true;
            QCOMPARE(ledger.change(id, 6, completed).value("error_code"), "invalid_transition");
            QSqlQuery query(db);
            QVERIFY(
                query.exec("CREATE TRIGGER fail_audit BEFORE INSERT ON managed_job_cleanup_audit "
                           "BEGIN SELECT RAISE(ABORT,'injected audit failure'); END"));
            QCOMPARE(ledger.resolveCleanup(id, 6, "local-user").value("error_code"),
                     "storage_error");
            QCOMPARE(
                ledger.get(id, intent().owner).value("task").toObject().value("revision").toInt(),
                6);
            QVERIFY(query.exec("SELECT count(*) FROM managed_job_outbox"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 0);
            query.finish();
            QVERIFY(query.exec("DROP TRIGGER fail_audit"));
            result = ledger.resolveCleanup(id, 6, "local-user");
            QVERIFY(result.value("ok").toBool());
            task = result.value("task").toObject();
            QCOMPARE(task.value("state"), "interrupted");
            QCOMPARE(task.value("reason"), "cleanup_acknowledged");
            QCOMPARE(task.value("exit_code").toInt(), 0);
            QCOMPARE(task.value("cleanup_status"), "unverified");
            QCOMPARE(task.value("completion").toObject().value("status"), "suppressed");
            QVERIFY(
                query.exec("SELECT source,user_client_id,revision FROM managed_job_cleanup_audit"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toString(), "user");
            QCOMPARE(query.value(1).toString(), "local-user");
            QCOMPARE(query.value(2).toInt(), 7);
        }
        QSqlDatabase::removeDatabase(name);
    }

    // ADR 0127 §7a: entering needs_attention queues one owner notice; another update of the
    // same incident adds none, and the user's acknowledgement supersedes its wake.
    void attentionNotifiesItsOwnerOncePerIncidentUntilTheUserAcknowledges() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            const auto id = runningJob(ledger);
            QVERIFY(!id.isEmpty());
            BackgroundTaskChange attention;
            attention.state = "needs_attention";
            attention.exitCode = 0;
            attention.executorStopped = false;
            QVERIFY(ledger.change(id, 4, attention).value("ok").toBool());
            attention.executorStopped = true;
            QVERIFY(ledger.change(id, 5, attention).value("ok").toBool());
            QCOMPARE(count(db, "SELECT count(*) FROM managed_job_notices"), 1);
            QCOMPARE(count(db, "SELECT count(*) FROM managed_job_outbox"), 0);
            const auto page = ledger.noticePage().value("events").toArray();
            QCOMPARE(page.size(), 1);
            const auto event = page.first().toObject();
            const auto eventId = event.value("event_id").toString();
            QCOMPARE(eventId, id + ":attention:5");
            QCOMPARE(event.value("owner_session_id"), "owner");
            const auto turn = event.value("turn_record").toObject();
            QCOMPARE(turn.value("event"), "needs_attention");
            QCOMPARE(turn.value("task_id"), id);
            QCOMPARE(turn.value("reason"), "cleanup_unverified");
            QCOMPARE(turn.value("cleanup_status"), "unverified");
            QCOMPARE(turn.value("exit_code").toInt(), 0);
            QVERIFY(!turn.value("manual_guidance").toString().isEmpty());
            QVERIFY(ledger.noticeCurrent(turn));

            // The same fences as a completion: a held owner does not accept it yet.
            QVERIFY(ledger.holdOwner(intent().owner, "user_stopped", &error));
            QCOMPARE(ledger.acceptNotice(eventId, intent().owner).value("error_code"),
                     "owner_held");
            QVERIFY(ledger.holdOwner(intent().owner, {}, &error));
            QCOMPARE(ledger.acceptNotice(eventId, {"other", 1, {}}).value("error_code"),
                     "stale_owner");
            const auto accepted = ledger.acceptNotice(eventId, intent().owner);
            QVERIFY(accepted.value("ok").toBool());
            QVERIFY(!accepted.value("duplicate").toBool());
            QVERIFY(ledger.acceptNotice(eventId, intent().owner).value("duplicate").toBool());
            QCOMPARE(wakeState(db, eventId), "accepted");
            QVERIFY(ledger.noticePage().value("events").toArray().isEmpty());

            QVERIFY(ledger.resolveCleanup(id, 6, "local-user").value("ok").toBool());
            QCOMPARE(wakeState(db, eventId), "suppressed");
            QVERIFY(!ledger.noticeCurrent(turn));
        }
        QSqlDatabase::removeDatabase(name);
    }

    void attentionOfACancelledJobWakesNoOne() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            const auto id = runningJob(ledger);
            QVERIFY(!id.isEmpty());
            BackgroundTaskChange stopping;
            stopping.state = "stopping";
            stopping.reason = "cancelled";
            stopping.terminalTarget = "cancelled";
            QVERIFY(ledger.change(id, 4, stopping).value("ok").toBool());
            BackgroundTaskChange attention;
            attention.state = "needs_attention";
            attention.executorStopped = true;
            QVERIFY(ledger.change(id, 5, attention).value("ok").toBool());
            QCOMPARE(count(db, "SELECT count(*) FROM managed_job_notices WHERE "
                               "status='suppressed' AND reason='cancelled'"),
                     1);
            QVERIFY(ledger.noticePage().value("events").toArray().isEmpty());
        }
        QSqlDatabase::removeDatabase(name);
    }

    // ADR 0127 §7a: an approved job still queued at queuedNoticeAfterMs gets one notice that
    // names the jobs holding its root's slots; it is stale once the job starts.
    void queuedNoticeNamesTheFullRootOnceAndGoesStaleWhenTheJobStarts() {
        QCOMPARE(BackgroundTaskLedger::queuedNoticeAfterMs(60000), 30000);
        QCOMPARE(BackgroundTaskLedger::queuedNoticeAfterMs(24LL * 60 * 60 * 1000), 120000);
        QCOMPARE(BackgroundTaskLedger::queuedNoticeAfterMs(1), 1);
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            BackgroundTaskChange approval;
            approval.state = "queued";
            approval.permissionApproved = true;
            QStringList jobs;
            for (int i = 0; i <= BackgroundTaskLedger::rootSlotLimit; ++i) {
                auto start = intent();
                start.clientRequestId = QString::number(i);
                const auto id = taskId(ledger.create(start));
                jobs.append(id);
                QVERIFY(ledger.change(id, 1, approval).value("ok").toBool());
            }
            for (int i = 0; i < BackgroundTaskLedger::rootSlotLimit; ++i)
                QCOMPARE(taskId(ledger.claimNext()), jobs[i]);
            const auto queued = jobs.last();
            const auto createdAt = ledger.get(queued, intent().owner)
                                       .value("task")
                                       .toObject()
                                       .value("created_at")
                                       .toInteger();
            const auto due = createdAt + BackgroundTaskLedger::queuedNoticeAfterMs(60000);
            auto reply = ledger.noticeQueued(queued, due - 1);
            QVERIFY(reply.value("ok").toBool());
            QVERIFY(!reply.value("queued").toBool());
            // A permission request is the user's: no notice while it is unanswered.
            auto unapproved = intent();
            unapproved.clientRequestId = "unapproved";
            const auto waiting = taskId(ledger.create(unapproved));
            QVERIFY(!ledger.noticeQueued(waiting, due + 60000).value("queued").toBool());

            reply = ledger.noticeQueued(queued, due);
            QVERIFY(reply.value("queued").toBool());
            QCOMPARE(reply.value("queue_reason"), "root_slot_limit");
            QCOMPARE(ledger.noticeQueued(queued, due + 1000).value("queue_reason"),
                     "root_slot_limit");
            QCOMPARE(count(db, "SELECT count(*) FROM managed_job_notices"), 1);
            const auto page = ledger.noticePage().value("events").toArray();
            QCOMPARE(page.size(), 1);
            QCOMPARE(page.first().toObject().value("event_id").toString(), queued + ":queued");
            const auto turn = page.first().toObject().value("turn_record").toObject();
            QCOMPARE(turn.value("event"), "queued");
            QCOMPARE(turn.value("queued_ms").toInteger(),
                     BackgroundTaskLedger::queuedNoticeAfterMs(60000));
            QStringList holders;
            for (const auto holder : turn.value("slot_holders").toArray())
                holders.append(holder.toObject().value("task_id").toString());
            QCOMPARE(holders, jobs.mid(0, BackgroundTaskLedger::rootSlotLimit));
            QVERIFY(ledger.noticeCurrent(turn));

            BackgroundTaskChange cancel;
            cancel.state = "stopping";
            cancel.cleanupPending = true;
            QVERIFY(ledger.change(jobs.first(), 3, cancel).value("ok").toBool());
            cancel.state = "cancelled";
            cancel.cleanupConfirmed = true;
            QVERIFY(ledger.change(jobs.first(), 4, cancel).value("ok").toBool());
            QCOMPARE(taskId(ledger.claimNext()), queued);
            QVERIFY(!ledger.noticeCurrent(turn));
            QCOMPARE(ledger
                         .suppressCompletion(page.first().toObject().value("event_id").toString(),
                                             "superseded")
                         .value("ok"),
                     true);
            QVERIFY(ledger.noticePage().value("events").toArray().isEmpty());
        }
        QSqlDatabase::removeDatabase(name);
    }

    void linuxTerminalRequiresPositiveCleanupEvidence() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            auto request = intent();
            request.containmentMode = "cgroup_v2";
            const auto id = taskId(ledger.create(request));
            BackgroundTaskChange change;
            change.state = "queued";
            change.permissionApproved = true;
            QVERIFY(ledger.change(id, 1, change).value("ok").toBool());
            QCOMPARE(taskId(ledger.claimNext()), id);
            change = {};
            change.state = "running";
            change.handshakeCommitted = true;
            change.executionIdentity = {{"process_birth_identity", "birth"},
                                        {"executor_lease_identity", "lease"}};
            QVERIFY(ledger.change(id, 3, change).value("ok").toBool());
            change = {};
            change.state = "completed";
            change.cleanupConfirmed = true;
            QCOMPARE(ledger.change(id, 4, change).value("error_code"), "cleanup_pending");
            change.cleanupStatus = "verified";
            QCOMPARE(ledger.change(id, 4, change).value("error_code"), "cleanup_pending");
            QVERIFY(ledger.deliveryPage(false).value("events").toArray().isEmpty());
            change.fullScopeVerified = true;
            const auto result = ledger.change(id, 4, change).value("task").toObject();
            QCOMPARE(result.value("state"), "completed");
            QCOMPARE(result.value("cleanup_status"), "verified");
        }
        QSqlDatabase::removeDatabase(name);
    }

    void nativeNormalCompletionRetainsBestEffortAssuranceAndWakes() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            const auto id = taskId(ledger.create(intent()));
            BackgroundTaskChange change;
            change.state = "queued";
            change.permissionApproved = true;
            QVERIFY(ledger.change(id, 1, change).value("ok").toBool());
            QCOMPARE(taskId(ledger.claimNext()), id);
            change = {};
            change.state = "running";
            change.handshakeCommitted = true;
            change.executionIdentity = {{"process_birth_identity", "birth"},
                                        {"executor_lease_identity", "lease"}};
            QVERIFY(ledger.change(id, 3, change).value("ok").toBool());
            change = {};
            change.state = "completed";
            change.cleanupConfirmed = true;
            change.cleanupStatus = "verified";
            QCOMPARE(ledger.change(id, 4, change).value("error_code"), "invalid_cleanup_evidence");
            change.cleanupStatus = "unverified";
            QCOMPARE(ledger.change(id, 4, change).value("error_code"), "invalid_cleanup_evidence");
            change.cleanupStatus.clear();
            const auto task = ledger.change(id, 4, change).value("task").toObject();
            QCOMPARE(task.value("state"), "completed");
            QCOMPARE(task.value("cleanup_status"), "best_effort_complete");
            QCOMPARE(task.value("completion").toObject().value("status"), "pending");
            QVERIFY(
                ledger
                    .acceptCompletion(task.value("completion_event_id").toString(), intent().owner)
                    .value("ok")
                    .toBool());
        }
        QSqlDatabase::removeDatabase(name);
    }

    void paginationIncludesMoreThanSixteenAndRefetchesChangedSnapshot() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            QStringList expected;
            for (int i = 0; i < 35; ++i) {
                auto request = intent();
                request.clientRequestId = QString::number(i);
                expected.append(taskId(ledger.create(request)));
            }
            auto page = ledger.list(intent().owner, {}, 16);
            QCOMPARE(page.value("tasks").toArray().size(), 16);
            const auto cursor = page.value("next_cursor").toString();
            QVERIFY(!cursor.isEmpty());
            QCOMPARE(ledger.list({"other", 1, {}}, cursor, 16).value("error_code"),
                     "invalid_cursor");
            QStringList actual;
            while (true) {
                for (const auto value : page.value("tasks").toArray())
                    actual.append(value.toObject().value("task_id").toString());
                if (page.value("next_cursor").isNull())
                    break;
                page = ledger.list(intent().owner, page.value("next_cursor").toString(), 16);
                QVERIFY(page.value("ok").toBool());
            }
            QCOMPARE(actual, expected);
            QVERIFY(failed(ledger, expected.last()).value("ok").toBool());
            QCOMPARE(ledger.list(intent().owner, cursor, 16).value("error_code"), "stale_cursor");
            QVERIFY(ledger.list(intent().owner).value("ok").toBool());
        }
        QSqlDatabase::removeDatabase(name);
    }

    void retryAndOwnerFenceSurviveReopen() {
        QTemporaryDir directory;
        const auto name = QUuid::createUuid().toString();
        QString id;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(directory.filePath("ledger.sqlite"));
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY2(ledger.initialize(&error), qPrintable(error));
            QVERIFY(seedOwner(db));
            const auto first = ledger.create(intent());
            QVERIFY(first.value("ok").toBool());
            id = taskId(first);
            QVERIFY(!id.isEmpty());
            QCOMPARE(taskId(ledger.create(intent())), id);
            auto conflicting = intent();
            conflicting.specFingerprint = "different";
            QCOMPARE(ledger.create(conflicting).value("error_code"), "idempotency_conflict");
            QCOMPARE(ledger.get(id, {"owner", 2, {}}).value("error_code"), "stale_owner");
            QCOMPARE(ledger.get(id, {"other", 1, {}}).value("error_code"), "task_not_found");
        }
        QSqlDatabase::removeDatabase(name);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(directory.filePath("ledger.sqlite"));
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            const auto retry = ledger.create(intent());
            QCOMPARE(taskId(retry), id);
            QVERIFY(retry.value("reused").toBool());
        }
        QSqlDatabase::removeDatabase(name);
    }

    void terminalOutboxIsAtomicAndTerminalCannotBeOverwritten() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            const auto id = taskId(ledger.create(intent()));
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TRIGGER fail_job_update BEFORE UPDATE ON managed_jobs "
                               "BEGIN SELECT RAISE(ABORT,'injected terminal failure'); END"));
            QCOMPARE(failed(ledger, id).value("error_code"), "storage_error");
            QVERIFY(query.exec("SELECT count(*) FROM managed_job_outbox"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 0);
            query.finish();
            QCOMPARE(ledger.get(id, intent().owner).value("task").toObject().value("state"),
                     "queued");
            QVERIFY(query.exec("DROP TRIGGER fail_job_update"));
            const auto result = failed(ledger, id);
            QVERIFY(result.value("ok").toBool());
            const auto record = result.value("task").toObject();
            QCOMPARE(record.value("revision").toInteger(), 2);
            QCOMPARE(record.value("completion").toObject().value("status"), "pending");
            QCOMPARE(failed(ledger, id).value("error_code"), "revision_conflict");
            BackgroundTaskChange overwrite;
            overwrite.state = "completed";
            overwrite.cleanupConfirmed = true;
            QCOMPARE(ledger.change(id, 2, overwrite).value("error_code"), "terminal_immutable");
            QVERIFY(query.exec("SELECT count(*) FROM managed_job_outbox"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 1);
        }
        QSqlDatabase::removeDatabase(name);
    }

    void cancellationRequiresCleanupAndPermissionDenialSuppressesWake() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            const auto id = taskId(ledger.create(intent()));
            BackgroundTaskChange change;
            change.state = "starting";
            QCOMPARE(ledger.change(id, 1, change).value("error_code"), "invalid_transition");
            change.state = "stopping";
            change.cleanupPending = true;
            QVERIFY(ledger.change(id, 1, change).value("ok").toBool());
            change.state = "cancelled";
            QCOMPARE(ledger.change(id, 2, change).value("error_code"), "invalid_transition");
            change.cleanupConfirmed = true;
            QVERIFY(ledger.change(id, 2, change).value("ok").toBool());
            auto denied = intent();
            denied.clientRequestId = "denied";
            const auto deniedId = taskId(ledger.create(denied));
            change = {};
            change.state = "failed";
            change.reason = "permission_denied";
            change.neverLaunched = true;
            const auto record = ledger.change(deniedId, 1, change).value("task").toObject();
            const auto completion = record.value("completion").toObject();
            QCOMPARE(completion.value("status"), "suppressed");
            QCOMPARE(completion.value("reason"), "permission_denied");
            QCOMPARE(ledger.acceptCompletion(completion.value("event_id").toString(), denied.owner)
                         .value("error_code"),
                     "completion_suppressed");
        }
        QSqlDatabase::removeDatabase(name);
    }

    void wakeAcceptanceRollbackDedupAndDispatchClaimSurviveRestart() {
        QTemporaryDir directory;
        const auto name = QUuid::createUuid().toString();
        QString eventId;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(directory.filePath("ledger.sqlite"));
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            const auto record =
                failed(ledger, taskId(ledger.create(intent()))).value("task").toObject();
            eventId = record.value("completion_event_id").toString();
            QVERIFY(!eventId.isEmpty());
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TRIGGER fail_delivery BEFORE UPDATE ON managed_job_outbox "
                               "BEGIN SELECT RAISE(ABORT,'injected acceptance failure'); END"));
            QCOMPARE(ledger.acceptCompletion(eventId, intent().owner).value("error_code"),
                     "storage_error");
            QVERIFY(query.exec("SELECT count(*) FROM managed_job_wakes"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 0);
            query.finish();
            QVERIFY(query.exec("DROP TRIGGER fail_delivery"));
            const auto accepted = ledger.acceptCompletion(eventId, intent().owner);
            QVERIFY(accepted.value("ok").toBool());
            QVERIFY(!accepted.value("duplicate").toBool());
        }
        QSqlDatabase::removeDatabase(name);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(directory.filePath("ledger.sqlite"));
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QVERIFY(ledger.acceptCompletion(eventId, intent().owner).value("duplicate").toBool());
            QCOMPARE(ledger.claimWake(eventId, {"owner", 2, {}}).value("error_code"),
                     "wake_not_dispatchable");
            QVERIFY(ledger.claimWake(eventId, intent().owner).value("ok").toBool());
        }
        QSqlDatabase::removeDatabase(name);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(directory.filePath("ledger.sqlite"));
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QVERIFY(ledger.acceptCompletion(eventId, intent().owner).value("duplicate").toBool());
            QCOMPARE(ledger.claimWake(eventId, intent().owner).value("error_code"),
                     "wake_not_dispatchable");
        }
        QSqlDatabase::removeDatabase(name);
    }

    void generationAndUserHoldFenceAcceptanceAndDispatch() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            const auto id = taskId(ledger.create(intent()));
            const auto record = failed(ledger, id).value("task").toObject();
            const auto event = record.value("completion_event_id").toString();
            QVERIFY(ledger.holdOwner(intent().owner, "permission_pending", &error));
            QCOMPARE(ledger.acceptCompletion(event, intent().owner).value("error_code"),
                     "owner_held");
            QCOMPARE(ledger.get(id, intent().owner)
                         .value("task")
                         .toObject()
                         .value("completion")
                         .toObject()
                         .value("status"),
                     "pending");
            QVERIFY(ledger.holdOwner(intent().owner, {}, &error));
            QVERIFY(ledger.acceptCompletion(event, intent().owner).value("ok").toBool());
            QVERIFY(ledger.holdOwner(intent().owner, "user_stopped", &error));
            QCOMPARE(ledger.claimWake(event, intent().owner).value("error_code"),
                     "wake_not_dispatchable");
            QVERIFY(ledger.holdOwner(intent().owner, {}, &error));
            QSqlQuery query(db);
            QVERIFY(query.exec("UPDATE sessions SET generation=2 WHERE id='owner'"));
            QCOMPARE(ledger.claimWake(event, intent().owner).value("error_code"),
                     "wake_not_dispatchable");
            QCOMPARE(ledger.acceptCompletion(event, intent().owner).value("error_code"),
                     "stale_owner");
            QCOMPARE(ledger.create(intent()).value("error_code"), "stale_owner");
        }
        QSqlDatabase::removeDatabase(name);
    }

    void stopCompletionRaceUsesOneRevisionAndRequiresBirthLeaseIdentity() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            for (bool stopFirst : {false, true}) {
                auto start = intent();
                start.clientRequestId = stopFirst ? "stop-first" : "complete-first";
                const auto id = taskId(ledger.create(start));
                BackgroundTaskChange change;
                change.state = "queued";
                change.permissionApproved = true;
                QVERIFY(ledger.change(id, 1, change).value("ok").toBool());
                QCOMPARE(taskId(ledger.claimNext()), id);
                change = {};
                change.state = "running";
                change.handshakeCommitted = true;
                change.executionIdentity = {{"pid", 123}};
                QCOMPARE(ledger.change(id, 3, change).value("error_code"), "invalid_transition");
                change.executionIdentity = {{"process_birth_identity", "fake-birth"},
                                            {"executor_lease_identity", "fake-lease"}};
                QVERIFY(ledger.change(id, 3, change).value("ok").toBool());
                BackgroundTaskChange stop;
                stop.state = "stopping";
                stop.terminalTarget = "cancelled";
                stop.cleanupPending = true;
                BackgroundTaskChange complete;
                complete.state = "completed";
                complete.cleanupConfirmed = true;
                if (stopFirst) {
                    QVERIFY(ledger.change(id, 4, stop).value("ok").toBool());
                    QCOMPARE(ledger.change(id, 4, complete).value("error_code"),
                             "revision_conflict");
                    QCOMPARE(ledger.change(id, 5, complete).value("error_code"),
                             "invalid_transition");
                    stop.state = "cancelled";
                    QVERIFY(!ledger.change(id, 5, stop).value("ok").toBool());
                    stop.cleanupConfirmed = true;
                    QVERIFY(ledger.change(id, 5, stop).value("ok").toBool());
                } else {
                    QVERIFY(ledger.change(id, 4, complete).value("ok").toBool());
                    QCOMPARE(ledger.change(id, 4, stop).value("error_code"), "revision_conflict");
                }
            }
            QSqlQuery query(db);
            QVERIFY(query.exec("SELECT count(*) FROM managed_job_outbox"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 2);
        }
        QSqlDatabase::removeDatabase(name);
    }

    void quotaClaimsFifoEligibleAndQueuesBehindFullRoot() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            QStringList firstRoot;
            BackgroundTaskChange approval;
            approval.state = "queued";
            approval.permissionApproved = true;
            for (int i = 0; i < 5; ++i) {
                auto start = intent();
                start.clientRequestId = QString::number(i);
                const auto id = taskId(ledger.create(start));
                firstRoot.append(id);
                QVERIFY(ledger.change(id, 1, approval).value("ok").toBool());
            }
            auto other = intent();
            other.owner.sessionId = "other";
            const auto otherId = taskId(ledger.create(other));
            QVERIFY(ledger.change(otherId, 1, approval).value("ok").toBool());
            for (int i = 0; i < 4; ++i)
                QCOMPARE(taskId(ledger.claimNext()), firstRoot[i]);
            QCOMPARE(taskId(ledger.claimNext()),
                     otherId); // Skip the full root; no head-of-line block.
            QVERIFY(ledger.claimNext().value("task").isNull());
            QCOMPARE(ledger.get(firstRoot.last(), intent().owner)
                         .value("task")
                         .toObject()
                         .value("state"),
                     "queued");
            const auto queued = ledger.create(intent());
            QVERIFY(queued.value("ok").toBool());
            QCOMPARE(queued.value("task").toObject().value("state"), "queued");
        }
        QSqlDatabase::removeDatabase(name);
    }

    void queueAndNonterminalCapsDoNotRejectIdempotentRetries() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            QString first;
            for (int i = 0; i < 100; ++i) {
                auto start = intent();
                start.clientRequestId = QString::number(i);
                const auto created = ledger.create(start);
                QVERIFY(created.value("ok").toBool());
                if (i == 0)
                    first = taskId(created);
            }
            QCOMPARE(ledger.create(intent()).value("error_code"), "resource_exhausted");
            auto retry = intent();
            retry.clientRequestId = "0";
            QCOMPARE(taskId(ledger.create(retry)), first);
        }
        QSqlDatabase::removeDatabase(name);
    }

    void parkedParentLetsChildClaimSameRootAndResumeReacquiresSlot() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            auto parentIntent = intent();
            parentIntent.kind = "agent";
            const auto parentId = taskId(ledger.create(parentIntent));
            BackgroundTaskChange approval;
            approval.state = "queued";
            approval.permissionApproved = true;
            QVERIFY(ledger.change(parentId, 1, approval).value("ok").toBool());
            QCOMPARE(taskId(ledger.claimNext()), parentId);
            QSqlQuery query(db);
            QVERIFY(query.exec("INSERT INTO sessions VALUES('child',1)"));
            query.prepare("INSERT INTO managed_job_children VALUES(?,'child',1)");
            query.addBindValue(parentId);
            QVERIFY(query.exec());
            BackgroundTaskChange running;
            running.state = "running";
            running.handshakeCommitted = true;
            running.executionIdentity = {{"child_session_id", "child"}, {"child_generation", 1}};
            QVERIFY(ledger.change(parentId, 3, running).value("ok").toBool());
            for (int i = 0; i < 3; ++i) {
                auto start = intent();
                start.clientRequestId = QString::number(i);
                const auto id = taskId(ledger.create(start));
                QVERIFY(ledger.change(id, 1, approval).value("ok").toBool());
                QCOMPARE(taskId(ledger.claimNext()), id);
            }
            auto childIntent = intent();
            childIntent.owner.sessionId = "child";
            const auto child = ledger.create(childIntent);
            const auto childId = taskId(child);
            QCOMPARE(child.value("task").toObject().value("root_owner_session_id"), "owner");
            QVERIFY(ledger.change(childId, 1, approval).value("ok").toBool());
            QVERIFY(ledger.claimNext().value("task").isNull());
            QVERIFY(!ledger.park(parentId, 4, false).value("ok").toBool());
            QVERIFY(ledger.park(parentId, 4, true).value("ok").toBool());
            QCOMPARE(taskId(ledger.claimNext()), childId);
            QVERIFY(ledger.readyToResume(parentId, 5).value("ok").toBool());
            QVERIFY(ledger.claimNext().value("task").isNull());
            running.executionIdentity = {{"process_birth_identity", "fake-birth"},
                                         {"executor_lease_identity", "fake-lease"}};
            QVERIFY(ledger.change(childId, 3, running).value("ok").toBool());
            BackgroundTaskChange complete;
            complete.state = "completed";
            complete.cleanupConfirmed = true;
            QVERIFY(ledger.change(childId, 4, complete).value("ok").toBool());
            const auto resumed = ledger.claimNext();
            QCOMPARE(taskId(resumed), parentId);
            QVERIFY(resumed.value("resume").toBool());
        }
        QSqlDatabase::removeDatabase(name);
    }
};

QTEST_GUILESS_MAIN(BackgroundTaskLedgerTest)
#include "BackgroundTaskLedgerTest.moc"
