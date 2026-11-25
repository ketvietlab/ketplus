// Remote jobs in the background task registry: a task is remote from its creation, never runs
// here, keeps its runs_on and timeout, and the registry opens an existing file in place.
#include "RemoteJobLedgerFixture.h"
#include "runner/jobs/RemoteJobSpecs.h"

#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QTest>

using namespace remote_job_ledger_test;
namespace protocol = ketplus::daemon::runner::peer::protocol;

namespace {

RemoteJobRecord firstRecord(const QString& taskId, const QStringList& runsOn = linuxLabels,
                            const qint64 timeoutMs = 60'000) {
    RemoteJobRecord record;
    record.taskId = taskId;
    record.runsOn = runsOn;
    record.timeoutMs = timeoutMs;
    return record;
}

QStringList taskIds(const QJsonObject& reply, const char* name) {
    QStringList ids;
    for (const QJsonValue& task : reply.value(QLatin1StringView(name)).toArray())
        ids.append(task.toObject().value("task_id").toString());
    return ids;
}

} // namespace

class RemoteJobLedgerTest final : public QObject {
    Q_OBJECT

  private slots:
    void aTaskWithRunsOnIsReservedWhenItIsCreated() {
        LedgerFixture f;
        const auto local = f.ledger->create(intentFor("local", std::nullopt));
        const auto remote = f.ledger->create(intentFor("remote"));
        QVERIFY(remote.value("ok").toBool());
        QCOMPARE(f.count("SELECT count(*) FROM remote_jobs"), 1);
        QCOMPARE(f.count("SELECT count(*) FROM remote_jobs WHERE revision=0 AND record IS NULL "
                         "AND runs_on='[\"os:linux\"]' AND timeout_ms=60000 AND task_id='" +
                         taskIdOf(remote) + "'"),
                 1);
        QCOMPARE(*f.store->reserved(), QStringList{taskIdOf(remote)});
        // A local task's fingerprint is the request's; a remote one also covers runs_on.
        QCOMPARE(f.count("SELECT count(*) FROM managed_jobs WHERE spec_fingerprint="
                         "CAST('fingerprint' AS BLOB) AND task_id='" +
                         taskIdOf(local) + "'"),
                 1);
        QCOMPARE(f.count("SELECT count(*) FROM managed_jobs WHERE spec_fingerprint="
                         "CAST('fingerprint' AS BLOB) AND task_id='" +
                         taskIdOf(remote) + "'"),
                 0);
        // A local task never becomes remote.
        QVERIFY(f.approve(taskIdOf(local)));
        QVERIFY(!f.store->put(firstRecord(taskIdOf(local)), 0));
        QCOMPARE(f.count("SELECT count(*) FROM remote_jobs"), 1);
    }

    void creatingAgainAnswersTheSameTaskOrRefusesAnotherSpec() {
        LedgerFixture f;
        const QString id = taskIdOf(f.ledger->create(intentFor("remote")));
        const auto again = f.ledger->create(intentFor("remote"));
        QCOMPARE(taskIdOf(again), id);
        QVERIFY(again.value("reused").toBool());
        QCOMPARE(f.ledger->create(intentFor("remote", QStringList{"arch:arm64"}))
                     .value("error_code")
                     .toString(),
                 QStringLiteral("idempotency_conflict"));
        QCOMPARE(
            f.ledger->create(intentFor("remote", linuxLabels, 1000)).value("error_code").toString(),
            QStringLiteral("idempotency_conflict"));
        QCOMPARE(f.ledger->create(intentFor("remote", std::nullopt)).value("error_code").toString(),
                 QStringLiteral("idempotency_conflict"));
        QCOMPARE(f.ledger->create(intentFor("bad", QStringList{"Not A Label"}))
                     .value("error_code")
                     .toString(),
                 QStringLiteral("invalid_labels"));
        QCOMPARE(f.count("SELECT count(*) FROM managed_jobs"), 1);
    }

    void aFailedReservationCreatesNoTask() {
        LedgerFixture f;
        QVERIFY(f.exec("CREATE TEMP TRIGGER fail_reserve BEFORE INSERT ON remote_jobs "
                       "BEGIN SELECT RAISE(ABORT,'injected'); END"));
        QCOMPARE(f.ledger->create(intentFor("remote")).value("error_code").toString(),
                 QStringLiteral("storage_error"));
        QCOMPARE(f.count("SELECT count(*) FROM managed_jobs"), 0);
        QCOMPARE(f.count("SELECT count(*) FROM remote_jobs"), 0);
    }

    void aRemoteTaskIsNeverClaimedOrGivenAnExecutor() {
        LedgerFixture f;
        const QString reserved = f.remoteTask();
        const QString started = f.remoteTask();
        QVERIFY(f.store->put(firstRecord(started), 0));
        QVERIFY(f.exec("UPDATE managed_jobs SET state='queued'"));
        QCOMPARE(f.ledger->claimNext().value("task"), QJsonValue(QJsonValue::Null));
        for (const QString& id : {reserved, started}) {
            const qint64 revision = f.revision(id);
            QCOMPARE(
                f.ledger
                    ->registerExecutor(
                        id, revision, {{"executor_lease_identity", "lease"}, {"directory", "/tmp"}})
                    .value("error_code")
                    .toString(),
                QStringLiteral("invalid_state"));
            QCOMPARE(f.ledger->park(id, revision, true).value("error_code").toString(),
                     QStringLiteral("invalid_state"));
            QCOMPARE(f.ledger->readyToResume(id, revision).value("error_code").toString(),
                     QStringLiteral("invalid_state"));
            QCOMPARE(f.ledger->reserveChild(id, revision).value("error_code").toString(),
                     QStringLiteral("invalid_state"));
        }
        // A local task next to them is still claimed.
        const QString local = taskIdOf(f.ledger->create(intentFor("local", std::nullopt)));
        QVERIFY(f.approve(local));
        QCOMPARE(f.ledger->claimNext().value("task").toObject().value("task_id").toString(), local);
    }

    void aReservedTaskChangesLikeAQueuedJobButNeverStarts() {
        LedgerFixture f;
        const QString id = taskIdOf(f.ledger->create(intentFor("remote")));
        QVERIFY(f.approve(id)); // permission
        BackgroundTaskChange start;
        start.state = "starting";
        start.handshakeCommitted = true;
        QCOMPARE(f.ledger->change(id, f.revision(id), start).value("error_code").toString(),
                 QStringLiteral("invalid_state"));
        start.state = "running";
        QCOMPARE(f.ledger->change(id, f.revision(id), start).value("error_code").toString(),
                 QStringLiteral("invalid_state"));
        BackgroundTaskChange stop;
        stop.state = "stopping";
        QVERIFY(f.ledger->change(id, f.revision(id), stop).value("ok").toBool());
        BackgroundTaskChange cancelled;
        cancelled.state = "cancelled";
        cancelled.cleanupConfirmed = true;
        cancelled.neverLaunched = true;
        QVERIFY(f.ledger->change(id, f.revision(id), cancelled).value("ok").toBool());
        QVERIFY(!f.store->put(firstRecord(id), 0)); // no longer queued

        const QString denied = taskIdOf(f.ledger->create(intentFor("denied")));
        BackgroundTaskChange refusal;
        refusal.state = "failed";
        refusal.reason = "permission_denied";
        refusal.neverLaunched = true;
        QVERIFY(f.ledger->change(denied, f.revision(denied), refusal).value("ok").toBool());
    }

    void aStartedTaskChangesOnlyThroughItsRecord() {
        LedgerFixture f;
        const QString id = f.remoteTask();
        const QString reserved = f.remoteTask();
        QCOMPARE(taskIds(f.ledger->nonterminal(), "tasks"), (QStringList{id, reserved}));
        QVERIFY(f.store->put(firstRecord(id), 0));
        QCOMPARE(taskIds(f.ledger->nonterminal(), "tasks"), QStringList{reserved});
        BackgroundTaskChange stop;
        stop.state = "stopping";
        QCOMPARE(f.ledger->change(id, f.revision(id), stop).value("error_code").toString(),
                 QStringLiteral("invalid_state"));
        QCOMPARE(f.state(id), QStringLiteral("queued"));
    }

    void theFirstRecordNeedsAnApprovedQueuedReservationWithItsSpec() {
        LedgerFixture f;
        const QString unapproved = taskIdOf(f.ledger->create(intentFor("unapproved")));
        QVERIFY(!f.store->put(firstRecord(unapproved), 0));
        const QString id = f.remoteTask();
        QVERIFY(!f.store->put(firstRecord(id, {"arch:arm64"}), 0));
        QVERIFY(!f.store->put(firstRecord(id, linuxLabels, 1000), 0));
        QVERIFY(!f.store->put(firstRecord(id), 1));
        QVERIFY(!f.store->put(firstRecord("missing"), 0));
        QVERIFY(f.store->put(firstRecord(id), 0));
        RemoteJobRecord changed = firstRecord(id, {"arch:arm64"});
        QVERIFY(!f.store->put(changed, 1)); // runs_on never changes
        QCOMPARE(f.store->load().size(), 1);
        QCOMPARE(*f.store->reserved(), QStringList{unapproved});
    }

    void getAndListShowPlacementAndRunnerOnlyForRemoteJobs() {
        LedgerFixture f;
        const QString local = taskIdOf(f.ledger->create(intentFor("local", std::nullopt)));
        const QJsonObject before = f.task(local);
        const QString id = f.remoteTask();
        RemoteJobRecord record = firstRecord(id);
        QVERIFY(f.store->put(record, 0));
        record.state = state::starting;
        record.placement = placement::assigned;
        record.runner = RemoteRunner{"runner-a", "build-a", 2};
        record.jobId = "job-1";
        QVERIFY(f.store->put(record, 1));
        const QJsonObject shown = f.ledger->get(id, owner).value("task").toObject();
        QCOMPARE(shown.value("state").toString(), QStringLiteral("starting"));
        QCOMPARE(shown.value("placement").toString(), QStringLiteral("assigned"));
        QCOMPARE(shown.value("runner").toObject(),
                 (QJsonObject{{"runnerId", "runner-a"}, {"name", "build-a"}, {"attempt", 2}}));
        const QJsonArray listed = f.ledger->list(owner).value("tasks").toArray();
        QCOMPARE(listed.size(), 2);
        QCOMPARE(f.task(local), before);
        QVERIFY(!before.contains("placement") && !before.contains("runner"));
    }

    void anUnreadableRecordIsSkippedAndNamed() {
        LedgerFixture f;
        const QString good = f.remoteTask();
        const QString bad = f.remoteTask();
        QVERIFY(f.store->put(firstRecord(good), 0));
        QVERIFY(f.store->put(firstRecord(bad), 0));
        QVERIFY(f.exec("UPDATE remote_jobs SET record='{not json' WHERE task_id='" + bad + "'"));
        QTest::ignoreMessage(QtWarningMsg, qPrintable("remote job record unreadable task=" + bad));
        const auto loaded = f.reopen()->load();
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().taskId, good);
        QCOMPARE(f.store->unreadable(), QStringList{bad});
    }

    void pruningATaskRemovesItsRemoteRow() {
        LedgerFixture f;
        const QString id = f.remoteTask();
        RemoteJobRecord record = firstRecord(id);
        QVERIFY(f.store->put(record, 0));
        record.state = state::failed;
        record.reason = "queue_expired";
        QVERIFY(f.store->put(record, 1));
        QVERIFY(f.ledger
                    ->suppressCompletion(f.task(id).value("completion_event_id").toString(),
                                         "delivered")
                    .value("ok")
                    .toBool());
        const qint64 ended = f.task(id).value("ended_at").toInteger();
        QCOMPARE(f.ledger->prune(ended + 8LL * 24 * 60 * 60 * 1000).value("pruned").toInt(), 1);
        QCOMPARE(f.count("SELECT count(*) FROM remote_jobs"), 0);
        QVERIFY(f.store->load().isEmpty());
    }

    void anExistingRegistryIsUpgradedInPlaceOnce() {
        LedgerFixture f;
        const QString local = taskIdOf(f.ledger->create(intentFor("local", std::nullopt)));
        const QJsonObject before = f.task(local);
        // The registry as the previous release left it: no remote table or trigger.
        QVERIFY(f.exec("DROP TRIGGER remote_jobs_follow_task"));
        QVERIFY(f.exec("DROP TABLE remote_jobs"));
        const int version = f.count("PRAGMA user_version");
        const int objects = f.count("SELECT count(*) FROM sqlite_master");
        QVERIFY(f.reopen());
        const int upgraded = f.count("SELECT count(*) FROM sqlite_master");
        QVERIFY(upgraded > objects);
        QCOMPARE(f.count("SELECT count(*) FROM sqlite_master WHERE name IN "
                         "('remote_jobs','remote_jobs_follow_task')"),
                 2);
        QCOMPARE(f.task(local), before);
        QCOMPARE(f.count("PRAGMA user_version"), version);
        QVERIFY(f.reopen());
        QCOMPARE(f.count("SELECT count(*) FROM sqlite_master"), upgraded);
        const QString id = f.remoteTask();
        QVERIFY(f.store->put(firstRecord(id), 0));
    }

    void noSpecValueReachesTheDisk() {
        LedgerFixture f;
        const QString marker = QStringLiteral("spec-marker-7f3a9c");
        RemoteJobSpecs specs;
        protocol::JobSpec spec;
        spec.invocation.argv = QStringList{"make", marker + "-argv"};
        spec.env = {{"TOKEN", marker + "-env"}};
        spec.cwd = marker + "-cwd";
        const QString id = f.remoteTask();
        QVERIFY(specs.store(id, spec));
        RemoteJobRecord record = firstRecord(id);
        record.jobId = "job-1";
        QVERIFY(f.store->put(record, 0));
        specs.recorded(record);
        QVERIFY(specs.spec("job-1", 1));
        f.close();
        QDirIterator files(f.directory.path(), QDir::Files | QDir::Hidden);
        int read = 0;
        while (files.hasNext()) {
            QFile file(files.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(!file.readAll().contains(marker.toUtf8()));
            ++read;
        }
        QVERIFY(read > 0);
    }
};

QTEST_GUILESS_MAIN(RemoteJobLedgerTest)
#include "RemoteJobLedgerTest.moc"
