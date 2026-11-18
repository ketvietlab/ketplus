// The entry of tasks started with runs_on: reading the labels, the field rules of a remote spec,
// the checks before a task exists, the submit after approval, and the records in replies.
#include "runner/entry/RemoteJobEntry.h"
#include "RemoteJobsHarness.h"
#include "runner/entry/RemoteSpec.h"
#include "runner/entry/RunsOnRequest.h"
#include "runner/jobs/RemoteJobLabels.h"

#include <QDir>
#include <QJsonArray>
#include <QTest>

using namespace remote_jobs_test;
using namespace ketplus::daemon::runner::entry;

namespace {

const QString secretArgument = QStringLiteral("--token=entry-secret-argument");
const QString secretValue = QStringLiteral("entry-secret-value");

QJsonObject startPayload(const QJsonValue& runsOn, const QString& kind = "process") {
    QJsonObject payload{{"client_request_id", "request"},
                        {"kind", kind},
                        {"title", "Build"},
                        {"timeout_ms", 60'000},
                        {"execution_spec", QJsonObject{{"argv", QJsonArray{"make"}}}}};
    if (!runsOn.isUndefined())
        payload.insert("runs_on", runsOn);
    return payload;
}

// A process execution as the shared checks leave it.
QJsonObject execution(const QString& cwd) {
    return {{"argv", QJsonArray{"make", secretArgument}},
            {"cwd", cwd},
            {"env_delta", QJsonObject{{"SECRET", secretValue}, {"UNSET", QJsonValue::Null}}}};
}

// A directory inside a Git worktree, as worktreeOf finds one.
struct Worktree {
    QTemporaryDir directory;
    QString root;
    QString inside;
    Worktree() {
        QDir dir(directory.path());
        dir.mkpath(QStringLiteral("repo/.git"));
        dir.mkpath(QStringLiteral("repo/src"));
        root = QDir(dir.filePath("repo")).canonicalPath();
        inside = QDir(dir.filePath("repo/src")).canonicalPath();
    }
};

sync::PreparedOutcome preparedIn(const RemoteHarness& h, const QString& root) {
    sync::Prepared prepared;
    prepared.worktreeRoot = root;
    prepared.cwd = QStringLiteral("src");
    prepared.repositoryId = root + QStringLiteral("/.git");
    prepared.worktreeId = root;
    prepared.storePath = sync::storePathFor(h.stores->storesRoot(), prepared.repositoryId);
    prepared.snapshotId = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    prepared.objectFormat = QStringLiteral("sha1");
    return {prepared, {}};
}

// Ends a task that has not run, as the runtime's Stop does: stopping, then cancelled.
bool cancelQueued(LedgerFixture& f, const QString& id) {
    BackgroundTaskChange stopping;
    stopping.state = QStringLiteral("stopping");
    stopping.terminalTarget = QStringLiteral("cancelled");
    stopping.reason = QStringLiteral("cancelled");
    if (!f.ledger->change(id, f.revision(id), stopping).value("ok").toBool())
        return false;
    BackgroundTaskChange cancelled;
    cancelled.state = QStringLiteral("cancelled");
    cancelled.reason = QStringLiteral("cancelled");
    cancelled.cleanupConfirmed = true;
    cancelled.neverLaunched = true;
    return f.ledger->change(id, f.revision(id), cancelled).value("ok").toBool();
}

QStringList capturedLog;
void captureLog(QtMsgType, const QMessageLogContext&, const QString& message) {
    capturedLog.append(message);
}

struct Entry {
    RemoteHarness h;
    std::unique_ptr<RemoteJobEntry> entry;
    QList<std::pair<QString, QString>> failed;
    QStringList changed;
    Entry() { make(); }
    // A new RemoteJobs on the same registry, as after a daemon restart, and a new entry on it.
    void rebuild() {
        entry.reset();
        h.build();
        make();
    }
    void make() {
        entry = std::make_unique<RemoteJobEntry>(*h.jobs);
        entry->setPorts(
            {[this](const QString& taskId, const QString& code) { failed.append({taskId, code}); },
             [this](const QString& taskId) { changed.append(taskId); }});
    }
};

} // namespace

class RemoteJobEntryTest final : public QObject {
    Q_OBJECT

  private slots:
    void init() { capturedLog.clear(); }

    void aStartWithoutRunsOnIsLocal() {
        const RunsOnRead read = readRunsOn(startPayload(QJsonValue::Undefined));
        QVERIFY(read.ok());
        QVERIFY(!read.runsOn);
        const QJsonObject payload = startPayload(QJsonValue::Undefined);
        QCOMPARE(normalizedRequest(payload), payload);
    }

    void runsOnOnAnAgentIsRefused() {
        const RunsOnRead read = readRunsOn(startPayload(QJsonArray{"os:linux"}, "agent"));
        QCOMPARE(read.errorCode, QStringLiteral("runs_on_unsupported"));
    }

    void invalidLabelsAreRefused_data() {
        QTest::addColumn<QJsonValue>("runsOn");
        QJsonArray seventeen;
        for (int i = 0; i < 17; ++i)
            seventeen.append(QStringLiteral("label-%1").arg(i));
        QTest::newRow("not an array") << QJsonValue("os:linux");
        QTest::newRow("empty") << QJsonValue(QJsonArray{});
        QTest::newRow("seventeen") << QJsonValue(seventeen);
        QTest::newRow("upper case") << QJsonValue(QJsonArray{"Linux"});
        QTest::newRow("unknown os") << QJsonValue(QJsonArray{"os:windows"});
        QTest::newRow("not a string") << QJsonValue(QJsonArray{1});
        QTest::newRow("too long") << QJsonValue(QJsonArray{QString(65, u'a')});
    }
    void invalidLabelsAreRefused() {
        QFETCH(QJsonValue, runsOn);
        const RunsOnRead read = readRunsOn(startPayload(runsOn));
        QCOMPARE(read.errorCode, QStringLiteral("invalid_labels"));
        QVERIFY(!read.error.isEmpty());
    }

    void theSameLabelsInAnyOrderOrRepeatedAreOneRequest() {
        const QJsonObject one = startPayload(QJsonArray{"os:linux", "gpu", "arch:x86_64"});
        const QJsonObject other = startPayload(QJsonArray{"gpu", "arch:x86_64", "os:linux", "gpu"});
        QCOMPARE(normalizedRequest(one), normalizedRequest(other));
        const RunsOnRead read = readRunsOn(other);
        QVERIFY(read.ok());
        QCOMPARE(*read.runsOn, QStringList({"arch:x86_64", "gpu", "os:linux"}));
        QVERIFY(!withoutRunsOn(one).contains("runs_on"));
    }

    void aRetryCarriesTheLabelsAndTimeout() {
        BackgroundTaskIntent intent;
        intent.requestPayload = normalizedRequest(startPayload(QJsonArray{"os:linux", "gpu"}));
        prepareRetry(intent);
        QCOMPARE(intent.runsOn, std::optional<QStringList>(QStringList{"gpu", "os:linux"}));
        QCOMPARE(intent.timeoutMs, qint64(60'000));
        BackgroundTaskIntent local;
        local.requestPayload = startPayload(QJsonValue::Undefined);
        prepareRetry(local);
        QVERIFY(!local.runsOn);
        QCOMPARE(local.timeoutMs, qint64(0));
    }

    void aProcessExecutionBecomesAJobSpec() {
        const auto argvSpec = jobSpecOf(execution("/work/repo/src"), 60'000);
        QVERIFY(argvSpec);
        QCOMPARE(argvSpec->invocation.argv, std::optional<QStringList>({"make", secretArgument}));
        QVERIFY(!argvSpec->invocation.shell);
        QCOMPARE(argvSpec->cwd, QStringLiteral("/work/repo/src"));
        QCOMPARE(argvSpec->env.value("SECRET"), std::optional<QString>(secretValue));
        QVERIFY(argvSpec->env.contains("UNSET") && !argvSpec->env.value("UNSET"));
        QCOMPARE(argvSpec->timeoutMs, qint64(60'000));
        QJsonObject shell = execution("/work/repo");
        shell.remove("argv");
        shell.insert("shell_command", "make all");
        const auto shellSpec = jobSpecOf(shell, 1000);
        QVERIFY(shellSpec);
        QCOMPARE(shellSpec->invocation.shell, std::optional<QString>("make all"));
        // A local-only key is not a remote execution.
        QJsonObject local = execution("/work/repo");
        local.insert("_base_environment", QJsonObject{});
        QVERIFY(!jobSpecOf(local, 1000));
    }

    void aSpecWithinTheFieldRulesPasses() {
        Worktree w;
        QCOMPARE(remoteSpecProblem(execution(w.inside), 60'000), QString());
        QCOMPARE(remoteSpecProblem(execution(w.root), 24LL * 60 * 60 * 1000), QString());
    }

    void aSpecBeyondTheFieldRulesIsRefused_data() {
        QTest::addColumn<QJsonObject>("execution");
        QTest::addColumn<qint64>("timeoutMs");
        Worktree w;
        const QJsonObject base = execution(QStringLiteral("/nowhere"));
        QTest::newRow("timeout under a second") << base << qint64(999);
        QTest::newRow("timeout over a day") << base << qint64(24LL * 60 * 60 * 1000 + 1);
        QJsonObject shell = base;
        shell.remove("argv");
        shell.insert("shell_command", QString(32 * 1024 + 1, u'x'));
        QTest::newRow("shell over 32 KiB") << shell << qint64(60'000);
        QJsonObject names = base;
        QJsonObject env;
        for (int i = 0; i < 65; ++i)
            env.insert(QStringLiteral("NAME_%1").arg(i), "v");
        names.insert("env_delta", env);
        QTest::newRow("65 names") << names << qint64(60'000);
        QJsonObject reserved = base;
        reserved.insert("env_delta", QJsonObject{{"KETPLUS_TOKEN", "v"}});
        QTest::newRow("KETPLUS_ name") << reserved << qint64(60'000);
        QJsonObject large = base;
        large.insert("env_delta", QJsonObject{{"BIG", QString(32 * 1024, u'v')}});
        QTest::newRow("env over 32 KiB") << large << qint64(60'000);
        QJsonObject neither = base;
        neither.remove("argv");
        QTest::newRow("no command") << neither << qint64(60'000);
    }
    void aSpecBeyondTheFieldRulesIsRefused() {
        QFETCH(QJsonObject, execution);
        QFETCH(qint64, timeoutMs);
        QVERIFY(!remoteSpecProblem(execution, timeoutMs).isEmpty());
    }

    void aLocalTaskIsNeverRefused() {
        Entry e;
        e.h.relay.offered = false;
        QVERIFY(e.entry->admit(intentFor("local", std::nullopt), execution("/nowhere")).isEmpty());
    }

    void aRelayWithoutRunnersRefusesBeforeAnyTask() {
        Entry e;
        Worktree w;
        e.h.relay.offered = false;
        const QJsonObject reply = e.entry->admit(intentFor("remote"), execution(w.inside));
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("runner_unavailable"));
        QCOMPARE(e.h.f.count("SELECT count(*) FROM managed_jobs"), 0);
        QCOMPARE(e.h.f.count("SELECT count(*) FROM remote_jobs"), 0);
        QVERIFY(e.h.relay.log.isEmpty());
    }

    // The field rules come before the checks of the remote jobs service.
    void anExecutionNoRunnerTakesIsInvalidBeforeAnyTask() {
        Entry e;
        Worktree w;
        e.h.relay.offered = false;
        QJsonObject spec = execution(w.inside);
        spec.insert("env_delta", QJsonObject{{"KETPLUS_TOKEN", "x"}});
        const QJsonObject reply = e.entry->admit(intentFor("remote"), spec);
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("invalid_request"));
        QVERIFY(!reply.value("error_message").toString().contains("KETPLUS_TOKEN"));
        QVERIFY(e.entry->admit(intentFor("remote", linuxLabels, 0), execution(w.inside))
                    .value("error_code") == QStringLiteral("invalid_request"));
        QCOMPARE(e.h.f.count("SELECT count(*) FROM managed_jobs"), 0);
        QCOMPARE(e.h.f.count("SELECT count(*) FROM remote_jobs"), 0);
        QVERIFY(e.h.relay.log.isEmpty());
    }

    void anOldGitRefusesBeforeAnyTask() {
        Entry e;
        Worktree w;
        e.h.adjust = [](RemoteJobsPorts& ports) {
            ports.gitProblem = sync::prepare::gitUnsupported;
        };
        e.rebuild();
        const QJsonObject reply = e.entry->admit(intentFor("remote"), execution(w.inside));
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("git_unsupported"));
        QCOMPARE(e.h.f.count("SELECT count(*) FROM managed_jobs"), 0);
    }

    void aCwdOutsideAWorktreeRefusesBeforeAnyTask() {
        Entry e;
        QTemporaryDir plain;
        const QJsonObject reply = e.entry->admit(intentFor("remote"), execution(plain.path()));
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("not_a_worktree"));
        QCOMPARE(e.h.f.count("SELECT count(*) FROM managed_jobs"), 0);
    }

    // Reserved tasks count with the records: the 65th unfinished remote job is refused, and an
    // ended reservation frees its place.
    void the65thUnfinishedRemoteJobIsRefused() {
        Entry e;
        Worktree w;
        QStringList ids;
        for (int i = 0; i < ketplus::daemon::runner::jobs::maximumOutstanding; ++i) {
            const QString id = taskIdOf(e.h.f.ledger->create(intentFor(QString::number(i))));
            QVERIFY(!id.isEmpty());
            ids.append(id);
        }
        const QJsonObject full = e.entry->admit(intentFor("one-more"), execution(w.inside));
        QCOMPARE(full.value("error_code").toString(), QStringLiteral("resource_exhausted"));
        // One of them recorded: still 64 unfinished.
        QVERIFY(e.h.f.approve(ids.first()));
        e.h.prepare = [&](const sync::PrepareRequest&) { return preparedIn(e.h, w.root); };
        e.entry->submit(ids.first(), execution(w.inside), 60'000, linuxLabels);
        QVERIFY(e.h.jobs->record(ids.first()));
        QCOMPARE(e.h.jobs->outstanding(), 1);
        QCOMPARE(e.entry->admit(intentFor("one-more"), execution(w.inside))
                     .value("error_code")
                     .toString(),
                 QStringLiteral("resource_exhausted"));
        // An ended reservation is not counted.
        QVERIFY(cancelQueued(e.h.f, ids.last()));
        QVERIFY(e.entry->admit(intentFor("one-more"), execution(w.inside)).isEmpty());
    }

    void anUncountableRegistryIsAStorageError() {
        Entry e;
        Worktree w;
        QVERIFY(e.h.f.exec("ALTER TABLE remote_jobs RENAME TO remote_jobs_away"));
        const QJsonObject reply = e.entry->admit(intentFor("remote"), execution(w.inside));
        QCOMPARE(reply.value("error_code").toString(), QStringLiteral("storage_error"));
        QVERIFY(e.h.f.exec("ALTER TABLE remote_jobs_away RENAME TO remote_jobs"));
    }

    void anApprovedTaskIsSubmittedOnce() {
        Entry e;
        Worktree w;
        const QString id = e.h.f.remoteTask();
        e.entry->submit(id, execution(w.inside), 60'000, linuxLabels);
        e.entry->submit(id, execution(w.inside), 60'000, linuxLabels);
        QCOMPARE(e.h.prepared.size(), 1);
        QCOMPARE(e.h.prepared.first().cwd, w.inside);
        e.h.preparing.takeFirst()(preparedIn(e.h, w.root));
        QVERIFY(e.h.jobs->record(id));
        QCOMPARE(e.h.relay.pending("submit"), 1);
        QVERIFY(e.failed.isEmpty());
        QVERIFY(e.changed.contains(id));
    }

    void aSubmitThatFailsAfterApprovalEndsTheReservation_data() {
        QTest::addColumn<QString>("code");
        QTest::newRow("repository") << sync::prepare::repositoryUnsupported;
        QTest::newRow("snapshot") << sync::prepare::snapshotBuildFailed;
        QTest::newRow("relay gone") << QStringLiteral("runner_unavailable");
    }
    void aSubmitThatFailsAfterApprovalEndsTheReservation() {
        QFETCH(QString, code);
        Entry e;
        Worktree w;
        const QString id = e.h.f.remoteTask();
        e.entry->submit(id, execution(w.inside), 60'000, linuxLabels);
        if (code == QStringLiteral("runner_unavailable")) {
            e.h.relay.offered = false;
            e.h.preparing.takeFirst()(preparedIn(e.h, w.root));
        } else {
            e.h.preparing.takeFirst()(sync::PreparedOutcome{std::nullopt, code});
        }
        QCOMPARE(e.failed, (QList<std::pair<QString, QString>>{{id, code}}));
        QVERIFY(!e.h.jobs->record(id));
        QVERIFY(e.h.relay.log.isEmpty());
    }

    // A Stop while the snapshot is prepared ends the task before it has a record: the late
    // answer makes no record, reaches no Relay and keeps no spec.
    void aStopWhileTheSnapshotIsPreparedSendsNothing() {
        Entry e;
        Worktree w;
        const QString id = e.h.f.remoteTask();
        e.entry->submit(id, execution(w.inside), 60'000, linuxLabels);
        QVERIFY(!e.entry->stop(id)); // no record: the runtime cancels the task
        QVERIFY(cancelQueued(e.h.f, id));
        e.h.preparing.takeFirst()(preparedIn(e.h, w.root));
        QVERIFY(!e.h.jobs->record(id));
        QVERIFY(e.h.relay.log.isEmpty());
        QCOMPARE(e.h.f.state(id), QStringLiteral("cancelled"));
        QCOMPARE(e.h.f.count("SELECT count(*) FROM remote_jobs WHERE revision>0"), 0);
    }

    void aStopOfARecordGoesToTheRemoteJob() {
        Entry e;
        Worktree w;
        e.h.prepare = [&](const sync::PrepareRequest&) { return preparedIn(e.h, w.root); };
        const QString id = e.h.f.remoteTask();
        e.entry->submit(id, execution(w.inside), 60'000, linuxLabels);
        const auto first = e.entry->stop(id);
        QVERIFY(first);
        QCOMPARE(*first, QString());
        QVERIFY(e.entry->stop(id));               // again: answered, nothing new
        QCOMPARE(e.h.relay.pending("cancel"), 0); // not on the Relay yet: no job id
    }

    void cleanupOfATaskWithoutARecordIsTheRuntimes() {
        Entry e;
        const QString id = e.h.f.remoteTask();
        QVERIFY(!e.entry->resolveCleanup(id, "user-client"));
        QVERIFY(!e.entry->resolveCleanup("unknown", "user-client"));
    }

    void cleanupOfARecordNotInAttentionIsInvalidState() {
        Entry e;
        Worktree w;
        e.h.prepare = [&](const sync::PrepareRequest&) { return preparedIn(e.h, w.root); };
        const QString id = e.h.f.remoteTask();
        e.entry->submit(id, execution(w.inside), 60'000, linuxLabels);
        QCOMPARE(e.entry->resolveCleanup(id, "user-client"),
                 std::optional<QString>("invalid_state"));
    }

    void repliesNameThePlacementAndRunner() {
        Entry e;
        const QString id = e.h.f.remoteTask();
        QVERIFY(e.h.write(syncingSteps(id, "job-1", "runner-a", std::nullopt)));
        e.rebuild();
        e.h.jobs->restore();
        const QJsonObject reply =
            e.entry->withPlacement({{"ok", true}, {"task", QJsonObject{{"task_id", id}}}});
        const QJsonObject task = reply.value("task").toObject();
        QCOMPARE(task.value("placement").toString(), placement::syncing);
        QCOMPARE(task.value("runner").toObject(),
                 QJsonObject({{"runnerId", "runner-a"}, {"name", "Runner"}, {"attempt", 1}}));
        const QJsonObject local{{"task_id", "local-task"}, {"state", "running"}};
        const QJsonObject list = e.entry->withPlacement(
            {{"ok", true}, {"tasks", QJsonArray{local, QJsonObject{{"task_id", id}}}}});
        QCOMPARE(list.value("tasks").toArray().at(0).toObject(), local);
        QCOMPARE(list.value("tasks").toArray().at(1).toObject().value("placement").toString(),
                 placement::syncing);
    }

    void aWholeEntryLogsNoCommandEnvironmentOrDirectory() {
        const auto previous = qInstallMessageHandler(captureLog);
        {
            Entry e;
            Worktree w;
            (void)e.entry->admit(intentFor("remote"), execution(w.inside));
            const QString id = e.h.f.remoteTask();
            e.entry->submit(id, execution(w.inside), 60'000, linuxLabels);
            e.h.preparing.takeFirst()(
                sync::PreparedOutcome{std::nullopt, sync::prepare::snapshotBuildFailed});
            const QString other = e.h.f.remoteTask();
            e.h.prepare = [&](const sync::PrepareRequest&) { return preparedIn(e.h, w.root); };
            e.entry->submit(other, execution(w.inside), 60'000, linuxLabels);
            (void)e.entry->stop(other);
            for (const QString& line : std::as_const(capturedLog)) {
                QVERIFY2(!line.contains(secretArgument) && !line.contains(secretValue) &&
                             !line.contains(w.root),
                         qPrintable(line));
            }
        }
        qInstallMessageHandler(previous);
    }

    void theGuidanceSpeaksOfTheRunner() { QVERIFY(remoteManualGuidance().contains("runner")); }
};

QTEST_GUILESS_MAIN(RemoteJobEntryTest)
#include "RemoteJobEntryTest.moc"
