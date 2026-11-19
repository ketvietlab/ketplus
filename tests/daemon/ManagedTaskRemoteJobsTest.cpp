#include "ManagedTaskRemoteJoined.h"

#include <QSqlError>
#include <QSqlQuery>

using namespace managed_remote_test;

namespace {

constexpr qint64 minute = 60 * 1000;
constexpr qint64 hour = 60 * minute;

// The keys of a task as a reply shows it, without the ones each task has its own value for.
QJsonObject comparable(QJsonObject task) {
    for (const char* key : {"task_id", "client_request_id", "created_at", "updated_at", "queued_at",
                            "title", "spec_fingerprint"})
        task.remove(key);
    return task;
}

} // namespace

class ManagedTaskRemoteJobsTest final : public QObject {
    Q_OBJECT

  private slots:
    // The runner's output reaches the task's output, and its result ends the task once.
    void theRunnersOutputAndResultEndTheTaskOnce() {
        Joined j;
        const QString id = j.queued();
        QVERIFY(!id.isEmpty());
        j.e->assign();
        RunnerEnd end = j.e->connect(1);
        QVERIFY(j.e->play(end, [&] { return j.e->resultAcked; }));
        QVERIFY(j.until([&] { return j.state(id) == "completed"; }));
        QCOMPARE(j.task(id).value("exit_code").toInt(), 0);
        QCOMPARE(j.output(id), QStringLiteral("built\n"));
        // A remote task's output answers in the shape a local task's does.
        const QJsonObject remoteOutput = j.run("session.task.output", {{"task_id", id}});
        const auto local = j.run("session.task.start", j.process(QStringLiteral("local")));
        QVERIFY2(local.value("ok").toBool(), QJsonDocument(local).toJson());
        const QJsonObject localOutput =
            j.run("session.task.output",
                  {{"task_id", local.value("task").toObject().value("task_id").toString()}});
        QCOMPARE(remoteOutput.keys(), localOutput.keys());
        for (const QString& key : localOutput.keys())
            QCOMPARE(remoteOutput.value(key).type(), localOutput.value(key).type());
        QVERIFY(j.until([&] { return j.completions(id) == 1; }));
        QTest::qWait(600); // the bridge polls the outbox every 500 ms
        QCOMPARE(j.completions(id), 1);
        QCOMPARE(j.rows("managed_jobs"), 2); // with the local task
        QCOMPARE(j.e->h.relay.sent(QStringLiteral("submit")), 1);
        QVERIFY(!j.runtime()->isLive(id));
    }

    // Twenty minutes without the runner: needs_attention with the remote guidance; the user's
    // acknowledgement needs the task's revision and ends the task once.
    void aRunnerLostNeedsAttentionUntilTheUserResolvesIt() {
        Joined j;
        std::unique_ptr<RunnerEnd> end;
        const QString id = j.running(end, 2 * hour);
        QVERIFY(!id.isEmpty());
        end->lose();
        j.e->h.clock.advance(20 * minute);
        QVERIFY(j.until([&] { return j.state(id) == "needs_attention"; }));
        const QJsonObject attention = j.task(id);
        QCOMPARE(attention.value("cleanup_status").toString(), QStringLiteral("unverified"));
        QCOMPARE(attention.value("manual_guidance").toString(),
                 runner::entry::remoteManualGuidance());
        QCOMPARE(j.completions(id), 0);

        const qint64 revision = attention.value("revision").toInteger();
        const auto stale =
            j.run("session.task.resolve_cleanup", {{"task_id", id}, {"revision", revision - 1}});
        QCOMPARE(stale.value("error_code").toString(), QStringLiteral("revision_conflict"));
        QCOMPARE(j.state(id), QStringLiteral("needs_attention"));
        const auto missing = j.run("session.task.resolve_cleanup", {{"task_id", id}});
        QCOMPARE(missing.value("error_code").toString(), QStringLiteral("revision_conflict"));

        const auto resolved =
            j.run("session.task.resolve_cleanup", {{"task_id", id}, {"revision", revision}});
        QVERIFY2(resolved.value("ok").toBool(), QJsonDocument(resolved).toJson());
        QVERIFY(j.until([&] { return j.state(id) == "interrupted"; }));
        QVERIFY(j.until([&] { return j.completions(id) == 1; }));
        QTest::qWait(600);
        QCOMPARE(j.completions(id), 1);
    }

    void noMatchingRunnerFailsTheTaskWithTheRelaysCode() {
        Joined j;
        const QString id = j.approved();
        QVERIFY(!id.isEmpty());
        QVERIFY(
            j.e->h.relay.refuse(QStringLiteral("submit"), QStringLiteral("no_matching_runner")));
        QVERIFY(j.until([&] { return j.state(id) == "failed"; }));
        QCOMPARE(j.task(id).value("reason").toString(), QStringLiteral("no_matching_runner"));
        QVERIFY(j.until([&] { return j.completions(id) == 1; }));
        QCOMPARE(j.e->h.relay.sent(QStringLiteral("submit")), 1);
    }

    // The Relay's 24-hour queue expiry ends a job no runner took.
    void aJobNoRunnerTookExpiresAfterADay() {
        Joined j;
        const QString id = j.queued();
        QVERIFY(!id.isEmpty());
        j.e->h.clock.advance(24 * hour);
        j.e->h.jobs->relayMessage({{"type", "job.changed"}, {"jobId", linkedJob}});
        remote_job_test::RelayJob expired = remote_job_test::relayJob(QStringLiteral("expired"));
        expired.jobId = linkedJob;
        QVERIFY(j.e->h.relay.answer(QStringLiteral("get"), expired));
        QVERIFY(j.until([&] { return j.state(id) == "failed"; }));
        QCOMPARE(j.task(id).value("reason").toString(), QStringLiteral("queue_expired"));
        QVERIFY(j.until([&] { return j.completions(id) == 1; }));
    }

    // F2: the remote jobs service restarts under a running host and ends a job no runner took
    // while the runtime holds no entry. The runtime reads the ended task when it gets the new
    // entry: it publishes it once, lets it go, stops pumping, and the task completes once.
    void aRemoteJobsRestartEndsAnUnsentJobAndTheRuntimeLetsItGo() {
        Joined j;
        const QString id = j.queued();
        QVERIFY(!id.isEmpty());
        bool heldLive = false;
        QString ended;
        j.restartRemoteJobs([&] {
            heldLive = j.runtime()->isLive(id) && j.runtime()->isPumping();
            ended = j.state(id);
        });
        QCOMPARE(ended, QStringLiteral("interrupted")); // before the runtime heard of it
        QVERIFY(heldLive);
        QVERIFY(j.until([&] { return j.state(id) == "interrupted"; }));
        QCOMPARE(j.task(id).value("reason").toString(), QStringLiteral("daemon_restarted"));
        QVERIFY(j.until([&] { return !j.runtime()->isPumping(); }));
        QVERIFY(!j.runtime()->isLive(id));
        QVERIFY(j.until([&] { return j.completions(id) == 1; }));
        QTest::qWait(600);
        QCOMPARE(j.changes(id, QStringLiteral("interrupted"), QStringLiteral("pending")), 1);
        QCOMPARE(j.changes(id, QStringLiteral("interrupted"), QStringLiteral("delivered")), 1);
        QCOMPARE(j.changes(id), 2); // the end, then its completion delivered
        QCOMPARE(j.deliveries(id), 1);
        QCOMPARE(j.completions(id), 1);
        QCOMPARE(j.e->h.relay.pending(QStringLiteral("cancel")), 1);
    }

    // N1: the deadline marks the job; the runner's result decides how it ends.
    void theDeadlineWaitsForTheRunnersResult() {
        Joined j;
        std::unique_ptr<RunnerEnd> end;
        const QString id = j.running(end, minute);
        QVERIFY(!id.isEmpty());
        j.e->h.clock.advance(minute);
        QVERIFY(j.until([&] { return j.state(id) == "stopping"; }));
        QCOMPARE(j.e->h.jobs->record(id)->stopReason, QStringLiteral("deadline"));
        end->send(resultOf(QStringLiteral("timed_out")));
        QVERIFY(j.e->play(*end, [&] { return j.e->resultAcked; }));
        QVERIFY(j.until([&] { return j.state(id) == "timed_out"; }));
        QVERIFY(j.until([&] { return j.completions(id) == 1; }));
    }

    // N2: the deadline after the runner was lost still counts the job as outstanding, and the
    // runner's absence ends in needs_attention, never in a clean end.
    void theDeadlineWithoutTheRunnerNeedsAttention() {
        Joined j;
        std::unique_ptr<RunnerEnd> end;
        const QString id = j.running(end, minute);
        QVERIFY(!id.isEmpty());
        end->lose();
        j.e->h.clock.advance(minute);
        QVERIFY(j.until([&] { return j.state(id) == "stopping"; }));
        QCOMPARE(j.e->h.jobs->record(id)->stopReason, QStringLiteral("deadline"));
        QCOMPARE(j.e->h.jobs->outstanding(), 1);
        j.e->h.clock.advance(20 * minute);
        QVERIFY(j.until([&] { return j.state(id) == "needs_attention"; }));
        QCOMPARE(j.task(id).value("cleanup_status").toString(), QStringLiteral("unverified"));
        QCOMPARE(j.e->h.jobs->outstanding(), 1);
        QCOMPARE(j.completions(id), 0);
    }

    // N3 (1): a refusal or a network error of the Relay after the record exists goes through the
    // record, never the reservation's failure.
    void aSubmitThatFailsAfterTheRecordEndsThroughTheRecord() {
        Joined j;
        const QString id = j.approved();
        QVERIFY(!id.isEmpty());
        QVERIFY(j.e->h.jobs->record(id));
        QVERIFY(j.e->h.relay.refuse(QStringLiteral("submit"), QStringLiteral("network")));
        QTest::qWait(300);
        QCOMPARE(j.state(id), QStringLiteral("queued"));
        QCOMPARE(j.completions(id), 0);
        j.e->h.clock.advance(1000);
        QCOMPARE(j.e->h.relay.sent(QStringLiteral("submit")), 2);
        QVERIFY(j.e->h.relay.refuse(QStringLiteral("submit"), QStringLiteral("queue_full")));
        QVERIFY(j.until([&] { return j.state(id) == "failed"; }));
        const QJsonObject failed = j.task(id);
        QCOMPARE(failed.value("reason").toString(), QStringLiteral("queue_full"));
        QVERIFY(failed.value("error").toString() != QStringLiteral("spawn_failed"));
        for (const auto& call : j.e->h.relay.log)
            QCOMPARE(call.id, id);
        QVERIFY(j.until([&] { return j.completions(id) == 1; }));
    }

    // N3 (2): the daemon stops after the record and before the Relay's answer. The restart
    // submits nothing new and runs nothing: it ends the task and cancels the job it learns
    // with the same submit key.
    void aRestartBeforeTheRelaysAnswerCancelsWithTheSameKey() {
        Joined j;
        const QString id = j.approved();
        QVERIFY(!id.isEmpty());
        j.restart();
        QVERIFY(j.until([&] { return j.state(id) == "interrupted"; }));
        QVERIFY(j.until([&] { return j.e->h.relay.pending(QStringLiteral("submit")) == 2; }));
        // The stopped daemon's submit is never answered: its connection went with it.
        QCOMPARE(j.e->h.relay.calls.first().operation, QStringLiteral("submit"));
        j.e->h.relay.calls.removeFirst();
        remote_job_test::RelayJob queued = remote_job_test::relayJob(QStringLiteral("queued"));
        queued.jobId = linkedJob;
        QVERIFY(j.e->h.relay.answer(QStringLiteral("submit"), queued));
        QVERIFY(j.until([&] { return j.e->h.relay.pending(QStringLiteral("cancel")) == 1; }));
        QStringList submitKeys;
        for (const auto& call : j.e->h.relay.log)
            if (call.operation == QStringLiteral("submit"))
                submitKeys.append(call.id);
            else if (call.operation == QStringLiteral("cancel"))
                QCOMPARE(call.id, linkedJob);
        QCOMPARE(submitKeys, QStringList({id, id}));
        QCOMPARE(j.rows("managed_jobs"), 1);
        QVERIFY(j.e->parts.transports.wires.isEmpty());
        QVERIFY(!j.runtime()->isLive(id));
        QVERIFY(j.until([&] { return j.completions(id) == 1; }));
    }

    // N3 (3): the same start again while the submit waits returns the same task and submits
    // nothing more.
    void aRepeatedStartWhileSubmittingReturnsTheSameTask() {
        Joined j;
        const QString id = j.approved();
        QVERIFY(!id.isEmpty());
        const auto again = j.run("session.task.start", j.remote());
        QCOMPARE(again.value("task").toObject().value("task_id").toString(), id);
        QTest::qWait(300);
        j.e->h.run();
        QCOMPARE(j.e->h.relay.sent(QStringLiteral("submit")), 1);
        QCOMPARE(j.rows("managed_jobs"), 1);
        QVERIFY(j.approvals().isEmpty());
    }

    void aReadOnlyOwnerCannotStartARemoteJob() {
        Joined j(QStringLiteral("read-only"));
        const auto reply = j.run("session.task.start", j.remote());
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("permission_denied"));
        QCOMPARE(j.rows("managed_jobs"), 0);
        QCOMPARE(j.e->h.relay.sent(QStringLiteral("submit")), 0);
    }

    void aStaleGenerationCannotStartARemoteJob() {
        Joined j;
        const auto reply = j.run("session.task.start", j.remote(), QStringLiteral("owner"), 2);
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("stale_generation"));
        QCOMPARE(j.rows("managed_jobs"), 0);
        QCOMPARE(j.e->h.relay.sent(QStringLiteral("submit")), 0);
    }

    // An agent whose turn holds no mutation origin cannot start or stop a remote job.
    void anExpiredOriginCannotStartOrStopARemoteJob() {
        Joined j;
        const QString token = j.host->agentToken(QStringLiteral("owner"));
        QCOMPARE(j.agent(token, "session.task.start", j.remote()).value("error_code").toString(),
                 QStringLiteral("stale_origin"));
        QCOMPARE(j.rows("managed_jobs"), 0);
        const QString id = j.queued();
        QVERIFY(!id.isEmpty());
        QCOMPARE(
            j.agent(token, "session.task.stop", {{"task_id", id}}).value("error_code").toString(),
            QStringLiteral("stale_origin"));
        QCOMPARE(j.state(id), QStringLiteral("queued"));
        QCOMPARE(j.e->h.relay.pending(QStringLiteral("cancel")), 0);
    }

    // The agent credential of a deleted session starts nothing.
    void aRevokedSessionCannotStartARemoteJob() {
        Joined j;
        const QString token = j.host->agentToken(QStringLiteral("other"));
        QVERIFY(!token.isEmpty());
        QVERIFY(j.run("session.delete", {}, QStringLiteral("other")).value("ok").toBool());
        const auto reply = j.agent(token, "session.task.start", j.remote());
        QVERIFY2(!reply.value("ok").toBool(), QJsonDocument(reply).toJson());
        QCOMPARE(j.rows("managed_jobs"), 0);
        QCOMPARE(j.e->h.relay.sent(QStringLiteral("submit")), 0);
    }

    // Another session's remote job is not found by get, output or stop.
    void anotherSessionDoesNotFindARemoteJob() {
        Joined j;
        const QString id = j.queued();
        QVERIFY(!id.isEmpty());
        for (const char* type : {"session.task.get", "session.task.output", "session.task.stop"})
            QCOMPARE(j.run(type, {{"task_id", id}}, QStringLiteral("other"), 1,
                           QStringLiteral("cloud-control-v2"))
                         .value("error_code")
                         .toString(),
                     QStringLiteral("task_not_found"));
        QCOMPARE(j.state(id), QStringLiteral("queued"));
        QCOMPARE(j.e->h.relay.pending(QStringLiteral("cancel")), 0);
    }

    // A queued remote job takes one of the registry's 100 queued places, as a local task does,
    // and gives it back when it ends.
    void aRemoteJobCountsInTheRegistrysCapacity() {
        Joined j;
        const QString id = j.queued();
        QVERIFY(!id.isEmpty());
        for (int n = 1; n < 100; ++n) {
            const auto started = j.run("session.task.start", j.process(QString::number(n)));
            QVERIFY2(started.value("ok").toBool(), QJsonDocument(started).toJson());
        }
        QCOMPARE(j.run("session.task.start", j.process(QStringLiteral("full")))
                     .value("error_code")
                     .toString(),
                 QStringLiteral("resource_exhausted"));
        QVERIFY(j.run("session.task.stop", {{"task_id", id}}).value("ok").toBool());
        remote_job_test::RelayJob cancelled =
            remote_job_test::relayJob(QStringLiteral("cancelled"));
        cancelled.jobId = linkedJob;
        QVERIFY(j.e->h.relay.answer(QStringLiteral("cancel"), cancelled));
        QVERIFY(j.until([&] { return j.state(id) == "cancelled"; }));
        const auto after = j.run("session.task.start", j.process(QStringLiteral("full")));
        QVERIFY2(after.value("ok").toBool(), QJsonDocument(after).toJson());
    }

    // A stop the runner crosses with its result: the record is stopping, and the runner's
    // verified result decides the end, once, with one completion.
    void aStopRacingTheResultCompletesOnce() {
        Joined j;
        std::unique_ptr<RunnerEnd> end;
        const QString id = j.running(end, hour);
        QVERIFY(!id.isEmpty());
        QVERIFY(j.run("session.task.stop", {{"task_id", id}}).value("ok").toBool());
        QCOMPARE(j.e->h.jobs->record(id)->state, QStringLiteral("stopping"));
        end->send(resultOf(QStringLiteral("exited"), 0));
        QVERIFY(j.e->play(*end, [&] { return j.e->resultAcked; }));
        QVERIFY(j.until([&] { return j.completions(id) == 1; }));
        QTest::qWait(600);
        j.e->h.run();
        const QJsonObject ended = j.task(id);
        QCOMPARE(ended.value("state").toString(), QStringLiteral("completed"));
        QCOMPARE(ended.value("exit_code").toInt(-1), 0);
        QCOMPARE(j.completions(id), 1);
        QVERIFY(!j.runtime()->isLive(id));
    }

    // A caller that is not the user cannot acknowledge a remote job's cleanup, with the reply a
    // local task gives; the record stays as it was.
    void aCleanupNotFromTheUserIsRefusedAsOnALocalTask() {
        Joined j;
        std::unique_ptr<RunnerEnd> end;
        const QString id = j.running(end, 2 * hour);
        QVERIFY(!id.isEmpty());
        end->lose();
        j.e->h.clock.advance(20 * minute);
        QVERIFY(j.until([&] { return j.state(id) == "needs_attention"; }));
        const qint64 revision = j.task(id).value("revision").toInteger();
        const auto started = j.run("session.task.start", j.process(QStringLiteral("local")));
        QVERIFY2(started.value("ok").toBool(), QJsonDocument(started).toJson());
        const QString local = started.value("task").toObject().value("task_id").toString();
        const qint64 localRevision = j.task(local).value("revision").toInteger();

        for (const QString client : {QString(), QStringLiteral("cloud-control-v2")}) {
            const auto remote =
                j.run("session.task.resolve_cleanup", {{"task_id", id}, {"revision", revision}},
                      QStringLiteral("owner"), 1, client);
            const auto expected = j.run("session.task.resolve_cleanup",
                                        {{"task_id", local}, {"revision", localRevision}},
                                        QStringLiteral("owner"), 1, client);
            QVERIFY2(!expected.value("ok").toBool(true), QJsonDocument(expected).toJson());
            QCOMPARE(remote.keys(), expected.keys());
            QCOMPARE(remote.value("ok"), expected.value("ok"));
            QCOMPARE(remote.value("error_code").toString(),
                     expected.value("error_code").toString());
        }
        QCOMPARE(j.run("session.task.resolve_cleanup", {{"task_id", id}, {"revision", revision}},
                       QStringLiteral("owner"), 1, QString())
                     .value("error_code")
                     .toString(),
                 QStringLiteral("user_action_required"));
        const QJsonObject after = j.task(id);
        QCOMPARE(after.value("state").toString(), QStringLiteral("needs_attention"));
        QCOMPARE(after.value("revision").toInteger(), revision);
        QCOMPARE(j.e->h.jobs->record(id)->state, QStringLiteral("needs_attention"));
        QCOMPARE(j.completions(id), 0);
    }

    // When the placement of a started remote task cannot be read, its output is unavailable
    // rather than an empty local page.
    void anUnreadablePlacementLeavesNoOutput() {
        Joined j;
        std::unique_ptr<RunnerEnd> end;
        const QString id = j.running(end, hour);
        QVERIFY(!id.isEmpty());
        QSqlQuery query(j.registry->database());
        QVERIFY2(query.exec("ALTER TABLE remote_jobs RENAME TO remote_jobs_away"),
                 qPrintable(query.lastError().text()));
        const auto unreadable = j.run("session.task.output", {{"task_id", id}});
        QVERIFY(query.exec("ALTER TABLE remote_jobs_away RENAME TO remote_jobs"));
        QCOMPARE(unreadable.value("ok").toBool(true), false);
        QCOMPARE(unreadable.value("error_code").toString(), QStringLiteral("output_unavailable"));
        QVERIFY(!unreadable.contains("output"));
        QVERIFY(j.run("session.task.output", {{"task_id", id}}).value("ok").toBool());

        // Unavailable output answers in the shape a local task's does, whatever made it so.
        const auto started = j.run("session.task.start", j.process(QStringLiteral("local")));
        QVERIFY2(started.value("ok").toBool(), QJsonDocument(started).toJson());
        const QString local = started.value("task").toObject().value("task_id").toString();
        const QJsonObject badCursor{{"task_id", local}, {"cursor", "not a cursor"}};
        const auto expected = j.run("session.task.output", badCursor);
        QCOMPARE(expected.value("error_code").toString(), QStringLiteral("output_unavailable"));
        const auto remoteCursor =
            j.run("session.task.output", {{"task_id", id}, {"cursor", "not a cursor"}});
        for (const QJsonObject& remote : {unreadable, remoteCursor}) {
            QCOMPARE(remote.value("error_code").toString(), QStringLiteral("output_unavailable"));
            QCOMPARE(remote.keys(), expected.keys());
            for (const QString& key : expected.keys())
                QCOMPARE(remote.value(key).type(), expected.value(key).type());
        }
    }

    // A stop the runner answers with cancelled: the task ends cancelled, once, and its one
    // completion is suppressed as a local task's stop is.
    void aStopTheRunnerCancelsEndsCancelledOnce() {
        Joined j;
        std::unique_ptr<RunnerEnd> end;
        const QString id = j.running(end, hour);
        QVERIFY(!id.isEmpty());
        QVERIFY(j.run("session.task.stop", {{"task_id", id}}).value("ok").toBool());
        QCOMPARE(j.e->h.jobs->record(id)->state, QStringLiteral("stopping"));
        end->send(resultOf(QStringLiteral("cancelled")));
        QVERIFY(j.e->play(*end, [&] { return j.e->resultAcked; }));
        QVERIFY(j.until([&] { return j.completions(id) == 1; }));
        QTest::qWait(600); // the bridge polls the outbox every 500 ms
        j.e->h.run();
        QCOMPARE(j.state(id), QStringLiteral("cancelled"));
        QCOMPARE(j.completions(id), 1);
        QCOMPARE(j.count("SELECT count(*) FROM managed_job_outbox WHERE task_id='" + id +
                         "' AND status='suppressed' AND reason='cancelled'"),
                 1);
        QCOMPARE(j.deliveries(id), 0);
        QVERIFY(!j.runtime()->isLive(id));
    }

    // F7: a start without runs_on is the same with the remote jobs attached as without them.
    void aLocalStartIsUnchangedByTheRemoteJobs() {
        Joined j;
        const auto with = j.run("session.task.start", j.process(QStringLiteral("with")));
        QVERIFY2(with.value("ok").toBool(), QJsonDocument(with).toJson());
        j.host->setRemoteJobs(nullptr);
        const auto without = j.run("session.task.start", j.process(QStringLiteral("without")));
        QVERIFY2(without.value("ok").toBool(), QJsonDocument(without).toJson());

        QCOMPARE(with.keys(), without.keys()); // sorted
        QCOMPARE(comparable(with.value("task").toObject()),
                 comparable(without.value("task").toObject()));
        const QString a = with.value("task").toObject().value("task_id").toString();
        const QString b = without.value("task").toObject().value("task_id").toString();
        QCOMPARE(comparable(j.task(a)), comparable(j.task(b)));
        QVERIFY(!j.task(a).contains("placement"));
        QVERIFY(!j.task(a).contains("runner"));
        QCOMPARE(j.task(a).value("containment_mode"), j.task(b).value("containment_mode"));
        QCOMPARE(j.rows("remote_jobs"), 0);
        QCOMPARE(j.e->h.relay.sent(QStringLiteral("submit")), 0);
    }

    // F8: remote jobs that could not open their registry accept no remote task.
    void remoteJobsThatAreNotOpenRefuseBeforeAnyTask() {
        Joined j;
        j.host->setRemoteJobs(nullptr);
        j.entry.reset();
        j.e->h.registry = RemoteHarness::Registry{QSqlDatabase(), &*j.registry};
        j.e->h.build();
        j.attach();
        const auto reply = j.run("session.task.start", j.remote());
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("runner_unavailable"));
        QCOMPARE(j.rows("managed_jobs"), 0);
        QVERIFY(j.approvals().isEmpty());
    }

    // F8: a machine the Relay does not know accepts no remote task.
    void anUnregisteredMachineRefusesBeforeAnyTask() {
        Joined j;
        j.e->h.relay.offered = false;
        const auto reply = j.run("session.task.start", j.remote());
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("runner_unavailable"));
        QCOMPARE(j.rows("managed_jobs"), 0);
        QCOMPARE(j.rows("remote_jobs"), 0);
        QVERIFY(j.approvals().isEmpty());
    }
};

QTEST_GUILESS_MAIN(ManagedTaskRemoteJobsTest)
#include "ManagedTaskRemoteJobsTest.moc"
