#include "storage/BackgroundTaskLedger.h"

#include <QDateTime>
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

    void acceptedWakeSurvivesRestartButUncertainDispatchNeverReplays_data() {
        QTest::addColumn<bool>("attention");
        QTest::newRow("completion") << false;
        QTest::newRow("queued-notice") << true;
    }

    void acceptedWakeSurvivesRestartButUncertainDispatchNeverReplays() {
        QFETCH(bool, attention);
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            QString id;
            QString event;
            if (attention) {
                id = taskId(ledger.create(intent()));
                BackgroundTaskChange approval;
                approval.state = "queued";
                approval.permissionApproved = true;
                QVERIFY(ledger.change(id, 1, approval).value("ok").toBool());
                QVERIFY(ledger.noticeQueued(id, QDateTime::currentMSecsSinceEpoch() + 3600000)
                            .value("queued")
                            .toBool());
                const auto notices = ledger.noticePage().value("events").toArray();
                QCOMPARE(notices.size(), 1);
                event = notices.first().toObject().value("event_id").toString();
            } else {
                id = taskId(ledger.create(intent()));
                const auto ended = failed(ledger, id).value("task").toObject();
                event = ended.value("completion_event_id").toString();
            }
            QVERIFY(!event.isEmpty());
            const auto accept = [&] {
                return attention ? ledger.acceptNotice(event, intent().owner)
                                 : ledger.acceptCompletion(event, intent().owner);
            };
            QVERIFY(accept().value("ok").toBool());
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
            QVERIFY(accept().value("duplicate").toBool());
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

    // A running native task and the executor "done" result a recovery reports for it.
    static QString runningNativeTask(BackgroundTaskLedger& ledger) {
        const auto id = taskId(ledger.create(intent()));
        BackgroundTaskChange change;
        change.state = "queued";
        change.permissionApproved = true;
        if (!ledger.change(id, 1, change).value("ok").toBool() || taskId(ledger.claimNext()) != id)
            return {};
        change = {};
        change.state = "running";
        change.handshakeCommitted = true;
        change.executionIdentity = {{"process_birth_identity", "birth"},
                                    {"executor_lease_identity", "lease"}};
        return ledger.change(id, 3, change).value("ok").toBool() ? id : QString();
    }
    static QJsonObject recoveryResult(const QString& status, const QString& verification) {
        QJsonObject event{{"event", "done"},
                          {"state", "interrupted"},
                          {"cleanup_status", status},
                          {"executor_stopped", true},
                          {"root_started", true}};
        if (!verification.isEmpty())
            event.insert("cleanup_verification", verification);
        return event;
    }

    void recoveryReportingAnEmptyProcessGroupIsAcceptedByTheLedger() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            const auto id = runningNativeTask(ledger);
            QVERIFY(!id.isEmpty());
            // verified without any evidence kind is refused.
            BackgroundTaskChange change;
            change.state = "interrupted";
            change.cleanupConfirmed = true;
            applyExecutorCleanupEvidence(change, recoveryResult("verified", {}));
            QVERIFY(!change.groupVerified);
            QCOMPARE(ledger.change(id, 4, change).value("error_code"), "invalid_cleanup_evidence");
            // The evidence mapping, not a hand-set flag, accepts a verified empty group.
            applyExecutorCleanupEvidence(change, recoveryResult("verified", "process_group_empty"));
            QVERIFY(change.groupVerified);
            QVERIFY(!change.fullScopeVerified);
            const auto task = ledger.change(id, 4, change).value("task").toObject();
            QCOMPARE(task.value("state"), "interrupted");
            QCOMPARE(task.value("cleanup_status"), "verified");
            QCOMPARE(task.value("cleanup_verification"), "process_group_empty");
            QVERIFY(task.value("containment_mode").toString() != "cgroup_v2");
            QVERIFY(task.value("error").toString() != "executor_result_commit_failed");
        }
        QSqlDatabase::removeDatabase(name);
    }

    void exhaustedCleanupEndsTheJobWithDiagnosticsAndReleasesItsSlot() {
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
            QCOMPARE(count(db, "SELECT holds_slot FROM managed_jobs"), 1);
            BackgroundTaskChange end;
            end.state = "interrupted";
            end.reason = "executor_lost";
            end.cleanupConfirmed = true;
            end.cleanupStatus = "best_effort_complete";
            end.cleanupDiagnostic = "known_process_survived_cleanup";
            end.executorStopped = true;
            const auto task = ledger.change(id, 4, end).value("task").toObject();
            QCOMPARE(task.value("state"), "interrupted");
            QCOMPARE(task.value("cleanup_status"), "best_effort_complete");
            QVERIFY(!task.contains("manual_guidance"));
            QVERIFY(!task.contains("cleanup_diagnostic"));
            QCOMPARE(task.value("completion").toObject().value("status"), "pending");
            QCOMPARE(count(db, "SELECT holds_slot FROM managed_jobs"), 0);
            QCOMPARE(count(db, "SELECT count(*) FROM managed_job_diagnostics WHERE "
                               "issue='known_process_survived_cleanup'"),
                     1);
            QCOMPARE(count(db, "SELECT count(*) FROM managed_job_notices"), 0);
        }
        QSqlDatabase::removeDatabase(name);
    }

    void aRecordRetiredAsNeedsAttentionEndsAsInterruptedWhenTheRegistryOpens() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            QString id;
            {
                BackgroundTaskLedger ledger(db);
                QString error;
                QVERIFY(ledger.initialize(&error));
                QVERIFY(seedOwner(db));
                id = runningJob(ledger);
                QVERIFY(!id.isEmpty());
            }
            // The state an earlier daemon left: parked for the user, still holding its slot.
            QSqlQuery query(db);
            QVERIFY(query.exec(
                "UPDATE managed_jobs SET state='needs_attention',"
                "record=json_set(CAST(record AS TEXT),'$.state','needs_attention','$.reason',"
                "'cleanup_unverified','$.cleanup_status','unverified','$.manual_guidance','x')"));
            QCOMPARE(count(db, "SELECT holds_slot FROM managed_jobs"), 1);
            BackgroundTaskLedger reopened(db);
            QString error;
            QVERIFY2(reopened.initialize(&error), qPrintable(error));
            const auto task = reopened.get(id, intent().owner).value("task").toObject();
            QCOMPARE(task.value("state"), "interrupted");
            QCOMPARE(task.value("reason"), "daemon_upgrade");
            QCOMPARE(task.value("cleanup_status"), "best_effort_complete");
            QVERIFY(!task.contains("manual_guidance"));
            QCOMPARE(task.value("revision").toInteger(), 5);
            QCOMPARE(count(db, "SELECT holds_slot FROM managed_jobs"), 0);
            QCOMPARE(count(db, "SELECT count(*) FROM managed_jobs WHERE state='needs_attention'"),
                     0);
            // Nobody is woken for the migration, and a later open changes nothing.
            QCOMPARE(count(db, "SELECT count(*) FROM managed_job_outbox WHERE status='pending'"),
                     0);
            BackgroundTaskLedger again(db);
            QVERIFY(again.initialize(&error));
            QCOMPARE(again.get(id, intent().owner).value("task").toObject().value("revision"), 5);
        }
        QSqlDatabase::removeDatabase(name);
    }

    void kernelEvidenceKeepsItsKindAndLiveGroupIsNotVerified() {
        QJsonObject kernel = recoveryResult("verified", "process_group_empty");
        kernel.insert("containment_mode", "cgroup_v2");
        BackgroundTaskChange change;
        applyExecutorCleanupEvidence(change, kernel);
        QVERIFY(change.fullScopeVerified);
        QVERIFY(!change.groupVerified);
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskLedger ledger(db);
            QString error;
            QVERIFY(ledger.initialize(&error));
            QVERIFY(seedOwner(db));
            const auto id = runningNativeTask(ledger);
            QVERIFY(!id.isEmpty());
            // A group that still has a live process reports unverified: never a terminal state.
            BackgroundTaskChange live;
            applyExecutorCleanupEvidence(live, recoveryResult("unverified", {}));
            QVERIFY(!live.groupVerified);
            live.state = "interrupted";
            live.cleanupConfirmed = true;
            QCOMPARE(ledger.change(id, 4, live).value("error_code"), "invalid_cleanup_evidence");
            // The daemon never reports needs_attention: it ends the job with a diagnostic.
            BackgroundTaskChange attention;
            attention.state = "needs_attention";
            attention.executorStopped = true;
            QCOMPARE(ledger.change(id, 4, attention).value("error_code"), "invalid_transition");
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
