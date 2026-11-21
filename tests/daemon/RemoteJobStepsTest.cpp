// The remote job coordinator on the registry's store: each step a remote record takes moves its
// registry task through the registry's own transition effects, and the registry's own changes
// keep refusing what they refused before.
#include "RemoteJobLedgerFixture.h"
#include "RemoteJobTestFakes.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

using namespace remote_job_ledger_test;
using remote_job_test::FakeClock;
using remote_job_test::FakePeers;
using remote_job_test::FakeRelay;
using remote_job_test::FakeTrust;
using remote_job_test::job;
using remote_job_test::jobResult;
using remote_job_test::relayJob;
using remote_job_test::runnerA;

namespace {

constexpr qint64 minute = 60'000;

QStringList capturedLog;
void captureLog(QtMsgType, const QMessageLogContext&, const QString& message) {
    capturedLog.append(message);
}

QStringList taskIds(const QJsonObject& reply, const char* name) {
    QStringList ids;
    for (const QJsonValue& task : reply.value(QLatin1StringView(name)).toArray())
        ids.append(task.toObject().value("task_id").toString());
    return ids;
}
constexpr qint64 day = 24 * 60 * minute;

// The coordinator on the registry's store, for one remote task with no timeout.
struct Steps {
    LedgerFixture f;
    FakeRelay relay;
    FakeClock clock;
    FakeTrust trust;
    FakePeers peers;
    std::unique_ptr<RemoteJobCoordinator> jobs;
    QString id;

    Steps() {
        id = f.remoteTask(linuxLabels, day);
        reopenJobs();
    }
    void reopenJobs() {
        relay.calls.clear();
        jobs = std::make_unique<RemoteJobCoordinator>(relay, *f.store, clock, trust, peers);
        clock.due.clear();
        clock.target = jobs.get();
    }
    [[nodiscard]] RemoteJobRecord record() const {
        return jobs->record(id).value_or(RemoteJobRecord{});
    }
    [[nodiscard]] QString task() const { return f.state(id); }
    bool queued() {
        return jobs->start(id, linuxLabels, day).ok() && relay.answer("submit", relayJob("queued"));
    }
    bool assigned() { return queued() && jobs->offer(job, runnerA, 1).ok(); }
    bool sent() { return assigned() && jobs->dispatch(job, runnerA).ok(); }
    bool running() {
        if (!sent())
            return false;
        jobs->accepted(job, runnerA);
        jobs->started(job, runnerA);
        return record().state == state::running;
    }
    bool needsAttention() {
        if (!running())
            return false;
        jobs->peerLost(job, runnerA, clock.time);
        clock.advance(20 * minute);
        return record().state == state::needsAttention;
    }
    int outbox() {
        return f.count("SELECT count(*) FROM managed_job_outbox WHERE task_id='" + id + "'");
    }
    QJsonObject outboxRow() {
        QSqlQuery query(f.database);
        query.prepare("SELECT payload,status,reason FROM managed_job_outbox WHERE task_id=?");
        query.addBindValue(id);
        if (!query.exec() || !query.next())
            return {};
        return {{"payload", QJsonDocument::fromJson(query.value(0).toByteArray()).object()},
                {"status", query.value(1).toString()},
                {"reason", query.value(2).isNull() ? QJsonValue() : query.value(2).toString()}};
    }
};

} // namespace

class RemoteJobStepsTest final : public QObject {
    Q_OBJECT

  private slots:
    // S1 and S3: placements while queued, then a runner takes the job.
    void aQueuedJobMovesToStartingWhenARunnerTakesIt() {
        Steps s;
        QVERIFY(s.jobs->start(s.id, linuxLabels, day).ok());
        QCOMPARE(s.task(), QStringLiteral("queued"));
        QCOMPARE(s.f.task(s.id).value("placement").toString(), placement::submitting);
        QVERIFY(s.relay.answer("submit", relayJob("queued")));
        QCOMPARE(s.f.task(s.id).value("placement").toString(), placement::relayQueued);
        QVERIFY(s.jobs->offer(job, runnerA, 1).ok());
        QCOMPARE(s.task(), QStringLiteral("starting"));
        QCOMPARE(s.f.task(s.id).value("runner").toObject().value("name").toString(),
                 QStringLiteral("build-a"));
    }

    // S2 and S3: syncing, then running.
    void aStartingJobRunsWhenTheRunnerStartsIt() {
        Steps s;
        QVERIFY(s.sent());
        s.jobs->accepted(job, runnerA);
        QCOMPARE(s.f.task(s.id).value("placement").toString(), placement::syncing);
        QCOMPARE(s.task(), QStringLiteral("starting"));
        s.jobs->started(job, runnerA);
        QCOMPARE(s.task(), QStringLiteral("running"));
        QCOMPARE(s.outbox(), 0);
    }

    // S4: the Relay ends a job that no runner took.
    void anUndispatchedJobFailsWithTheRelayReason() {
        Steps s;
        QVERIFY(s.queued());
        s.jobs->jobChanged(job);
        QVERIFY(s.relay.answer("get", relayJob("expired")));
        QCOMPARE(s.task(), QStringLiteral("failed"));
        const QJsonObject task = s.f.task(s.id);
        QCOMPARE(task.value("reason").toString(), QStringLiteral("queue_expired"));
        QCOMPARE(task.value("cleanup_status").toString(), QStringLiteral("not_required"));
        QVERIFY(task.value("ended_at").toInteger() > 0);
        QCOMPARE(s.outbox(), 1);
    }

    // S5 and S6: a snapshot failure stops the job, and it ends failed.
    void aSnapshotFailureEndsTheJobFailed() {
        Steps s;
        QVERIFY(s.sent());
        s.jobs->accepted(job, runnerA);
        QCOMPARE(s.record().placement, placement::syncing);
        QVERIFY(s.jobs->snapshotFailed(job, runnerA, "disk_full").ok());
        QCOMPARE(s.task(), QStringLiteral("stopping"));
        QVERIFY(s.jobs->result(job, runnerA, jobResult("failed", {}, "disk_full")).ok());
        QCOMPARE(s.task(), QStringLiteral("failed"));
        QCOMPARE(s.f.task(s.id).value("reason").toString(), QStringLiteral("snapshot_failed"));
        QCOMPARE(s.outbox(), 1);
    }

    // X to stopping, then S6 and S7: what the runner reports after a Stop.
    void aStoppedJobEndsWithTheRunnersResult_data() {
        QTest::addColumn<QString>("result");
        QTest::addColumn<qint64>("exitCode");
        QTest::addColumn<QString>("ended");
        QTest::newRow("exit 1") << "exited" << qint64(1) << "failed";
        QTest::newRow("exit 0") << "exited" << qint64(0) << "completed";
        QTest::newRow("cancelled") << "cancelled" << qint64(-1) << "cancelled";
    }
    void aStoppedJobEndsWithTheRunnersResult() {
        QFETCH(QString, result);
        QFETCH(qint64, exitCode);
        QFETCH(QString, ended);
        Steps s;
        QVERIFY(s.running());
        QVERIFY(s.jobs->stop(s.id).ok());
        QCOMPARE(s.task(), QStringLiteral("stopping"));
        QVERIFY(s.f.task(s.id).value("cleanup_pending").toBool());
        const auto code = exitCode < 0 ? std::nullopt : std::optional(exitCode);
        QVERIFY(s.jobs->result(job, runnerA, jobResult(result, code)).ok());
        QCOMPARE(s.task(), ended);
        QVERIFY(!s.f.task(s.id).value("cleanup_pending").toBool());
        QCOMPARE(s.f.task(s.id).value("cleanup_status").toString(), QStringLiteral("verified"));
        QCOMPARE(s.outbox(), 1);
    }

    // A failed result keeps the runner's code on the task row; a reason that is not a code
    // ends spawn_failed and is written nowhere: not the registry, its JSON or the log.
    // A launch that fails before job.started ends the job from starting or syncing.
    void aFailedResultKeepsTheRunnersCode_data() {
        QTest::addColumn<QString>("from");
        QTest::addColumn<QString>("reason");
        QTest::addColumn<QString>("kept");
        for (const char* from : {"running", "starting", "syncing"}) {
            for (const char* code :
                 {"spawn_failed", "invalid_invocation", "workspace_missing", "cwd_missing",
                  "cwd_outside_workspace", "invalid_environment", "launch_too_large",
                  "runner_directory_missing", "workspace_outside_work", "runtime_directory_invalid",
                  "executor_directory_unavailable", "handshake_failed", "containment_unavailable",
                  "executor_leftover_unidentified", "timed_out", "executor_interrupted",
                  "exit_status_unknown", "expired", "a_code_not_yet_known"})
                QTest::addRow("%s %s", from, code)
                    << from << QString::fromLatin1(code) << QString::fromLatin1(code);
            // A code with a meaning of its own here ends the job like a malformed one.
            for (const char* code :
                 {"permission_denied", "cleanup_acknowledged", "cleanup_unverified",
                  "delivery_exhausted", "not_paired", "runner_removed", "entitlement_required",
                  "runner_unavailable", "no_matching_runner", "queue_full", "invalid_labels",
                  "idempotency_conflict", "invalid_request", "queue_expired", "exit_code", "user",
                  "deadline", "session_closed", "worker_restart"})
                QTest::addRow("%s reserved %s", from, code)
                    << from << QString::fromLatin1(code) << "spawn_failed";
            QTest::addRow("%s snapshot code", from) << from << "disk_full" << "snapshot_failed";
            QTest::addRow("%s empty", from) << from << "" << "spawn_failed";
            QTest::addRow("%s capitals", from) << from << "Marker_Reason" << "spawn_failed";
            QTest::addRow("%s space", from) << from << "marker reason" << "spawn_failed";
            QTest::addRow("%s newline", from) << from << "marker_reason\nnext" << "spawn_failed";
            QTest::addRow("%s html", from) << from << "<i>marker_reason</i>" << "spawn_failed";
            QTest::addRow("%s too long", from)
                << from << QString(65, QLatin1Char('m')) << "spawn_failed";
        }
    }
    void aFailedResultKeepsTheRunnersCode() {
        QFETCH(QString, from);
        QFETCH(QString, reason);
        QFETCH(QString, kept);
        Steps s;
        if (from == "running") {
            QVERIFY(s.running());
        } else {
            QVERIFY(s.sent());
            if (from == "syncing")
                s.jobs->accepted(job, runnerA);
            QCOMPARE(s.task(), QStringLiteral("starting"));
            QCOMPARE(s.record().placement,
                     from == "syncing" ? placement::syncing : placement::assigned);
        }
        capturedLog.clear();
        const auto previous = qInstallMessageHandler(captureLog);
        const bool recorded = s.jobs->result(job, runnerA, jobResult("failed", {}, reason)).ok();
        qInstallMessageHandler(previous);
        QVERIFY(recorded);
        QCOMPARE(s.task(), QStringLiteral("failed"));
        const QJsonObject row = s.f.task(s.id);
        QCOMPARE(row.value("reason").toString(), kept);
        QCOMPARE(row.value("cleanup_status").toString(), QStringLiteral("verified"));
        QCOMPARE(s.outbox(), 1);
        const QJsonObject completion = s.outboxRow();
        QCOMPARE(completion.value("status").toString(), QStringLiteral("pending"));
        QCOMPARE(completion.value("payload").toObject().value("state").toString(),
                 QStringLiteral("failed"));
        QVERIFY(completion.value("reason").isNull()); // not suppressed
        if (reason == kept || reason.isEmpty() || kept == "snapshot_failed")
            return;
        QVERIFY(row.value("reason").toString() != reason);
        if (isReasonCode(reason))
            return; // a reserved code may be logged; it is never the reason
        // The malformed string itself went nowhere.
        QVERIFY(!QJsonDocument(row).toJson().contains(reason.toUtf8()));
        QVERIFY(!QJsonDocument(completion).toJson().contains(reason.toUtf8()));
        for (const QString& line : std::as_const(capturedLog))
            QVERIFY2(!line.contains(reason), qPrintable(line));
        s.jobs.reset(); // before the store it refers to
        s.f.close();
        for (const QString& suffix : {QString(), QStringLiteral("-wal")}) {
            QFile file(s.f.path() + suffix);
            if (file.open(QIODevice::ReadOnly))
                QVERIFY(!file.readAll().contains(reason.toUtf8()));
        }
    }

    // S8 and the running results: the runner ends a job the desktop did not stop.
    void aRunningJobEndsWithTheRunnersResult_data() {
        QTest::addColumn<QString>("result");
        QTest::addColumn<QString>("ended");
        QTest::newRow("completed") << "exited" << "completed";
        QTest::newRow("cancelled") << "cancelled" << "cancelled";
        QTest::newRow("timed out") << "timed_out" << "timed_out";
        QTest::newRow("interrupted") << "interrupted" << "interrupted";
    }
    void aRunningJobEndsWithTheRunnersResult() {
        QFETCH(QString, result);
        QFETCH(QString, ended);
        Steps s;
        QVERIFY(s.running());
        const auto value = result == "exited"        ? jobResult(result, 0)
                           : result == "interrupted" ? jobResult(result, {}, "runner_restarted")
                                                     : jobResult(result);
        QVERIFY(s.jobs->result(job, runnerA, value).ok());
        QCOMPARE(s.task(), ended);
        QCOMPARE(s.outbox(), 1);
        const QJsonObject row = s.outboxRow();
        QCOMPARE(row.value("payload").toObject().value("state").toString(), ended);
        QCOMPARE(row.value("status").toString(),
                 ended == "cancelled" ? QStringLiteral("suppressed") : QStringLiteral("pending"));
    }

    // To needs_attention, and S9: a late result ends it.
    void aLateResultEndsAJobThatNeedsAttention() {
        Steps s;
        const QJsonValue stopped = s.f.task(s.id).value("executor_stopped");
        QVERIFY(s.needsAttention());
        QCOMPARE(s.task(), QStringLiteral("needs_attention"));
        QCOMPARE(s.f.task(s.id).value("cleanup_status").toString(), QStringLiteral("unverified"));
        QCOMPARE(s.f.task(s.id).value("executor_stopped"), stopped);
        QCOMPARE(s.f.task(s.id).value("slot_held").toBool(), false);
        QVERIFY(s.jobs->result(job, runnerA, jobResult("exited", 0)).ok());
        QCOMPARE(s.task(), QStringLiteral("completed"));
        QCOMPARE(s.outbox(), 1);
    }

    // S10: leaving needs_attention by acknowledgement needs one held for the task.
    void anAcknowledgementIsNeededToInterruptAJobThatNeedsAttention_data() {
        QTest::addColumn<QString>("source");
        QTest::newRow("resolve cleanup") << "user";
        QTest::newRow("runner removed") << "runner_removed";
    }
    void anAcknowledgementIsNeededToInterruptAJobThatNeedsAttention() {
        QFETCH(QString, source);
        Steps s;
        QVERIFY(s.needsAttention());
        const auto act = [&] {
            return source == "user" ? s.jobs->resolveCleanup(s.id).code
                                    : s.jobs->runnerRemoved(runnerA).code;
        };
        QCOMPARE(act(), QStringLiteral("storage_error"));
        QCOMPARE(s.task(), QStringLiteral("needs_attention"));
        QVERIFY(!s.f.store->acknowledge(s.id, source, QString()));
        QVERIFY(!s.f.store->acknowledge(s.id, "someone", "client-1"));
        QVERIFY(s.f.store->acknowledge(s.id, source, "client-1"));
        QCOMPARE(act(), QString());
        QCOMPARE(s.task(), QStringLiteral("interrupted"));
        QCOMPARE(s.f.count("SELECT count(*) FROM managed_job_cleanup_audit WHERE task_id='" + s.id +
                           "' AND source='" + source + "' AND user_client_id='client-1'"),
                 1);
        const QJsonObject row = s.outboxRow();
        QCOMPARE(row.value("status").toString(), QStringLiteral("suppressed"));
        QCOMPARE(row.value("reason").toString(), QStringLiteral("cleanup_acknowledged"));
    }

    // X to stopping before dispatch: the Relay cancels, nothing ran.
    void aJobStoppedBeforeDispatchIsCancelled() {
        Steps s;
        QVERIFY(s.queued());
        QVERIFY(s.jobs->stop(s.id).ok());
        QCOMPARE(s.task(), QStringLiteral("stopping"));
        QVERIFY(!s.f.task(s.id).value("cleanup_pending").toBool()); // nothing was dispatched
        QVERIFY(s.relay.answer("cancel", relayJob("cancelled")));
        QCOMPARE(s.task(), QStringLiteral("cancelled"));
        QCOMPARE(s.f.task(s.id).value("cleanup_status").toString(), QStringLiteral("not_required"));
        QCOMPARE(s.outboxRow().value("reason").toString(), QStringLiteral("cancelled"));
    }

    void anEndedRecordStaysEnded() {
        Steps s;
        QVERIFY(s.running());
        QVERIFY(s.jobs->result(job, runnerA, jobResult("exited", 0)).ok());
        const QJsonObject task = s.f.task(s.id);
        RemoteJobRecord again = s.record();
        QVERIFY(s.f.store->put(again, again.revision)); // the record only
        QCOMPARE(s.f.task(s.id), task);
        again.state = state::failed;
        QVERIFY(!s.f.store->put(again, again.revision + 1));
        RemoteJobRecord stopping = s.record();
        stopping.revision += 1;
        stopping.state = state::stopping;
        QVERIFY(!s.f.store->put(stopping, stopping.revision));
        QCOMPARE(s.outbox(), 1);
    }

    void anOutOfTableStepIsRefused() {
        Steps s;
        QVERIFY(s.running());
        RemoteJobRecord back = s.record();
        back.state = state::queued;
        back.placement = placement::relayQueued;
        QVERIFY(!s.f.store->put(back, back.revision));
        RemoteJobRecord mismatched = s.record();
        mismatched.placement = placement::assigned; // running is only placed running
        QVERIFY(!s.f.store->put(mismatched, mismatched.revision));
        RemoteJobRecord unverified = s.record();
        unverified.state = state::completed;
        unverified.cleanupStatus = cleanup::unverified;
        QVERIFY(!s.f.store->put(unverified, unverified.revision));
        QCOMPARE(s.task(), QStringLiteral("running"));
    }

    void aWriteThatFailsMidwayLeavesNothing() {
        Steps s;
        QVERIFY(s.running());
        const QJsonObject task = s.f.task(s.id);
        s.f.failRemoteWrites(true);
        QCOMPARE(s.jobs->result(job, runnerA, jobResult("exited", 0)).code,
                 QStringLiteral("storage_error"));
        s.f.failRemoteWrites(false);
        QCOMPARE(s.f.task(s.id), task);
        QCOMPARE(s.outbox(), 0);
        QCOMPARE(s.f.count("SELECT count(*) FROM managed_job_cleanup_audit"), 0);
    }

    void theCompletionEventHasTheLocalShape() {
        Steps s;
        QVERIFY(s.running());
        QVERIFY(s.jobs->result(job, runnerA, jobResult("exited", 1)).ok());
        const QString local = taskIdOf(s.f.ledger->create(intentFor("local", std::nullopt)));
        BackgroundTaskChange denied;
        denied.state = "failed";
        denied.reason = "spawn_failed";
        denied.neverLaunched = true;
        QVERIFY(s.f.ledger->change(local, s.f.revision(local), denied).value("ok").toBool());
        QSqlQuery query(s.f.database);
        QVERIFY(query.exec("SELECT task_id,payload FROM managed_job_outbox ORDER BY task_id"));
        QHash<QString, QJsonObject> payloads;
        while (query.next())
            payloads.insert(query.value(0).toString(),
                            QJsonDocument::fromJson(query.value(1).toByteArray()).object());
        QCOMPARE(payloads.size(), 2);
        QCOMPARE(payloads.value(s.id).keys(), payloads.value(local).keys());
        QCOMPARE(payloads.value(s.id).value("summary").toString(), QString());
        QCOMPARE(s.f.task(s.id).value("exit_code").toInt(), 1);
    }

    // The registry's own change still refuses what it refused before for a local job.
    void aLocalJobStillNeedsItsExecutionEvidence() {
        LedgerFixture f;
        const QString local = taskIdOf(f.ledger->create(intentFor("local", std::nullopt)));
        QVERIFY(f.approve(local));
        const auto refused = [&](const QString& to, const QString& reason = {}) {
            BackgroundTaskChange change;
            change.state = to;
            change.reason = reason;
            change.cleanupConfirmed = true;
            change.cleanupStatus = "verified";
            change.fullScopeVerified = true;
            return f.ledger->change(local, f.revision(local), change).value("error_code");
        };
        QCOMPARE(refused("starting"), QJsonValue("invalid_transition"));
        QCOMPARE(refused("running"), QJsonValue("invalid_transition"));
        QCOMPARE(refused("failed", "queue_expired"), QJsonValue("invalid_transition"));
        QCOMPARE(refused("completed"), QJsonValue("invalid_transition"));
        QCOMPARE(refused("timed_out"), QJsonValue("invalid_transition"));
        QCOMPARE(refused("needs_attention"), QJsonValue("invalid_transition"));
        QCOMPARE(f.state(local), QStringLiteral("queued"));
    }

    // A stopping remote row with cleanup pending, through a reopened registry: the local paths
    // leave it as it is, it holds no slot, and it ends with the cleanup fields a local job has.
    void aStoppingRowWithCleanupPendingIsLeftToItsRecord() {
        Steps s;
        QVERIFY(s.running() && s.jobs->stop(s.id).ok());
        const QJsonObject before = s.f.task(s.id);
        QCOMPARE(before.value("state").toString(), QStringLiteral("stopping"));
        QVERIFY(before.value("cleanup_pending").toBool());
        QVERIFY(!before.value("slot_held").toBool());

        QVERIFY(s.f.reopen());
        s.reopenJobs();
        s.jobs->restore();
        QCOMPARE(s.f.task(s.id), before);
        QVERIFY(!taskIds(s.f.ledger->nonterminal(), "tasks").contains(s.id));
        QCOMPARE(s.f.ledger->resolveCleanup(s.id, s.f.revision(s.id), "client")
                     .value("error_code")
                     .toString(),
                 QStringLiteral("invalid_state"));
        const QString local = taskIdOf(s.f.ledger->create(intentFor("local", std::nullopt)));
        QVERIFY(s.f.approve(local));
        QCOMPARE(s.f.ledger->claimNext().value("task").toObject().value("task_id").toString(),
                 local);
        QCOMPARE(s.f.task(s.id), before);

        QVERIFY(s.jobs->result(job, runnerA, jobResult("cancelled", {})).ok());
        const QJsonObject ended = s.f.task(s.id);
        QCOMPARE(ended.value("state").toString(), QStringLiteral("cancelled"));
        QVERIFY(!ended.value("cleanup_pending").toBool());
        QCOMPARE(ended.value("cleanup_status").toString(), QStringLiteral("verified"));
        QVERIFY(!ended.value("slot_held").toBool());
        QCOMPARE(s.f.count("SELECT count(*) FROM managed_job_cleanup_audit WHERE task_id='" + s.id +
                           "'"),
                 0);
        QCOMPARE(s.outbox(), 1);
    }

    void aRestartContinuesEachKindOfRecord_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("undispatched") << "undispatched";
        QTest::newRow("dispatched") << "dispatched";
        QTest::newRow("stopping") << "stopping";
        QTest::newRow("needs attention") << "needs_attention";
    }
    void aRestartContinuesEachKindOfRecord() {
        QFETCH(QString, kind);
        Steps s;
        if (kind == "undispatched")
            QVERIFY(s.assigned());
        else if (kind == "dispatched")
            QVERIFY(s.running());
        else if (kind == "stopping")
            QVERIFY(s.running() && s.jobs->stop(s.id).ok());
        else
            QVERIFY(s.needsAttention());
        const RemoteJobRecord before = s.record();
        QVERIFY(s.f.reopen());
        s.reopenJobs();
        s.jobs->restore();
        QCOMPARE(s.record(), before);
        QCOMPARE(s.f.store->load(), QList<RemoteJobRecord>{before});
        QCOMPARE(s.relay.pending("get"), kind == "undispatched" ? 1 : 0);
        QCOMPARE(s.relay.pending("cancel"), kind == "stopping" ? 1 : 0);
        QCOMPARE(s.clock.isArmed(s.id, JobTimer::PeerAbsent),
                 kind == "dispatched" || kind == "stopping");
    }
};

QTEST_GUILESS_MAIN(RemoteJobStepsTest)
#include "RemoteJobStepsTest.moc"
