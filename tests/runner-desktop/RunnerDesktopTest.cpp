#include "RunnerDesktopRig.h"

#include "exec/LinuxPolicy.h"
#include "exec/LinuxSystemHost.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTest>

using namespace runner_desktop_test;
namespace state = ketplus::daemon::runner::jobs::state;
namespace placement = ketplus::daemon::runner::jobs::placement;
namespace cleanup = ketplus::daemon::runner::jobs::cleanup;

// The real ketplus-runner and the desktop daemon's real remote job parts, over a fake Relay on
// this machine and real WebRTC without ICE servers. Linux with job containment only, and only
// with KETPLUS_RUN_RUNNER_DESKTOP_E2E=1. Each scenario states its limit; the whole suite stays
// under five minutes.
class RunnerDesktopTest final : public QObject {
    Q_OBJECT

  private:
    QStringList m_unitsBefore;

    // The end of every scenario: nothing of the job is left on this machine.
    static void verifyClean(Rig& rig, const QList<QByteArray>& markers, const QStringList& units,
                            const QStringList& jobData) {
        const QString problem = rig.problems(jobData);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        for (const QByteArray& marker : markers)
            QVERIFY(QTest::qWaitFor([&] { return processesWith(marker) == 0; }, 10'000));
        QVERIFY(QTest::qWaitFor([&] { return jobUnits() == units; }, 10'000));
    }

    static QByteArray readTrace(const Rig& rig, const QString& name) {
        QFile file(QDir(rig.trace).filePath(name));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

  private slots:
    void initTestCase() {
        // The desktop says its version in `welcome`, as the daemon's main sets it.
        QCoreApplication::setApplicationVersion(QStringLiteral(KETPLUS_PRODUCT_VERSION));
#if !defined(Q_OS_LINUX)
        QSKIP("the runner contains jobs on Linux only");
#elif !defined(KETPLUS_REMOTE_DEMO_AVAILABLE)
        QSKIP("the runner and desktop read KETPLUS_CONTROL_URL only in a remote demo build");
#else
        if (qEnvironmentVariable("KETPLUS_RUN_RUNNER_DESKTOP_E2E") != QStringLiteral("1"))
            QSKIP("set KETPLUS_RUN_RUNNER_DESKTOP_E2E=1 in a prepared Linux user session");
        QVERIFY(QFileInfo(QStringLiteral(KETPLUS_RUNNER_PATH)).isExecutable());
        QVERIFY(QFileInfo(QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH)).isExecutable());
        // The runner's own containment checks, as `run` makes them before taking a job.
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
        ketplus::runner::exec::LinuxSystemHost host(runtime);
        ketplus::runner::exec::LinuxPolicy policy(host, QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH));
        std::optional<std::pair<bool, QString>> probed;
        policy.probe([&](const bool ok, const QString& code) { probed = {ok, code}; });
        QVERIFY(QTest::qWaitFor([&] { return probed.has_value(); }, 60'000));
        if (!probed->first)
            QSKIP(qPrintable(QStringLiteral("job containment is not available here: ") +
                             probed->second));
        m_unitsBefore = jobUnits();
#endif
    }

    // Scenario 1, 120 s: committed, modified and untracked files reach the job; its standard
    // output and error come back as the exact bytes, in the order they were written.
    void runsTheWorktreeAsItIs() {
        const Limit limit(120'000);
        Rig rig;
        QVERIFY(rig.ready());
        QVERIFY(!rig.pairAndRun().isEmpty());
        const QString repo =
            rig.repository({{"committed.txt", "committed\n"}, {"modified.txt", "before\n"}},
                           {{"modified.txt", "after\n"}, {"untracked.txt", "untracked\n"}});
        QVERIFY(!repo.isEmpty());
        QString code;
        const QString task = rig.submit("cat committed.txt modified.txt untracked.txt; sleep 0.3; "
                                        "printf 'to-stderr\\n' >&2; sleep 0.3; printf 'last\\n'",
                                        repo, 60'000, Rig::defaultRunsOn(), &code);
        QVERIFY(!task.isEmpty());
        QCOMPARE(code, QString());
        const auto record = rig.waitTerminal(task, 90'000);
        QVERIFY(record);
        QCOMPARE(record->state, state::completed);
        QCOMPARE(record->exitCode, std::optional<qint64>(0));
        QCOMPARE(record->cleanupStatus, cleanup::verified);
        QCOMPARE(rig.desktop->output(task),
                 QByteArray("committed\nafter\nuntracked\nto-stderr\nlast\n"));
        verifyClean(rig, {}, m_unitsBefore, {"to-stderr", "untracked\n", rig.trace});
        QVERIFY2(limit.within(), qPrintable(QString::number(limit.elapsed())));
    }

    // Scenario 2, 90 s: a non-zero exit fails the task with its code and output.
    void failsOnANonZeroExit() {
        const Limit limit(90'000);
        Rig rig;
        QVERIFY(rig.ready());
        QVERIFY(!rig.pairAndRun().isEmpty());
        const QString repo = rig.repository({{"a.txt", "a\n"}});
        const QString task = rig.submit("printf 'bad\\n'; exit 7", repo);
        const auto record = rig.waitTerminal(task, 60'000);
        QVERIFY(record);
        QCOMPARE(record->state, state::failed);
        QCOMPARE(record->reason, QStringLiteral("exit_code"));
        QCOMPARE(record->exitCode, std::optional<qint64>(7));
        QCOMPARE(rig.desktop->output(task), QByteArray("bad\n"));
        verifyClean(rig, {}, m_unitsBefore, {"bad\n"});
        QVERIFY(limit.within());
    }

    // Scenario 3, 90 s: a stop while the job runs ends the job's processes on the runner.
    void stopsARunningJob() {
        const Limit limit(90'000);
        Rig rig;
        QVERIFY(rig.ready());
        QVERIFY(!rig.pairAndRun().isEmpty());
        const QString repo = rig.repository({{"a.txt", "a\n"}});
        const QByteArray marker = sleepMarker();
        const QString task =
            rig.submit("printf 'begin\\n'; exec sleep " + QString::fromLatin1(marker), repo);
        QVERIFY(rig.waitOutput(task, "begin\n", 60'000));
        QCOMPARE(processesWith(marker), 1);
        QCOMPARE(rig.desktop->stop(task), QString());
        const auto record = rig.waitTerminal(task, 30'000);
        QVERIFY(record);
        QCOMPARE(record->state, state::cancelled);
        QCOMPARE(record->reason, QStringLiteral("user"));
        QCOMPARE(processesWith(marker), 0);
        QVERIFY(rig.runner->running());
        verifyClean(rig, {marker}, m_unitsBefore, {QString::fromLatin1(marker)});
        QVERIFY(limit.within());
    }

    // Scenario 4, 90 s: the job's time limit ends it at the runner.
    void endsAtTheDeadline() {
        const Limit limit(90'000);
        Rig rig;
        QVERIFY(rig.ready());
        QVERIFY(!rig.pairAndRun().isEmpty());
        const QString repo = rig.repository({{"a.txt", "a\n"}});
        const QByteArray marker = sleepMarker();
        const QString task =
            rig.submit("printf 'begin\\n'; exec sleep " + QString::fromLatin1(marker), repo, 3'000);
        const auto record = rig.waitTerminal(task, 60'000);
        QVERIFY(record);
        QCOMPARE(record->state, state::timedOut);
        QCOMPARE(record->reason, QStringLiteral("deadline"));
        QCOMPARE(rig.desktop->output(task), QByteArray("begin\n"));
        verifyClean(rig, {marker}, m_unitsBefore, {QString::fromLatin1(marker)});
        QVERIFY(limit.within());
    }

    // Scenario 5, 120 s: a second job on the same repository sends only what changed against
    // the snapshot the runner confirmed, and sees the new content.
    void reusesTheConfirmedSnapshot() {
        const Limit limit(120'000);
        Rig rig;
        QVERIFY(rig.ready());
        QVERIFY(!rig.pairAndRun().isEmpty());
        // 1 MiB of random bytes, which a pack cannot compress: the first pack carries it, the
        // second, on the confirmed base, does not.
        QByteArray big(1 << 20, Qt::Uninitialized);
        QRandomGenerator::global()->fillRange(reinterpret_cast<quint32*>(big.data()),
                                              big.size() / qsizetype(sizeof(quint32)));
        const QString repo = rig.repository({{"a.txt", "one\n"}, {"big.bin", big}});
        const QString first = rig.submit("cat a.txt; sleep 1", repo);
        const auto one = rig.waitTerminal(first, 60'000);
        QVERIFY(one && one->snapshot);
        QCOMPARE(one->state, state::completed);
        QCOMPARE(rig.desktop->output(first), QByteArray("one\n"));
        QFile changed(QDir(repo).filePath("a.txt"));
        QVERIFY(changed.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(changed.write("two\n"), 4);
        changed.close();
        const QString second = rig.submit("cat a.txt; sleep 1", repo);
        const auto two = rig.waitTerminal(second, 60'000);
        QVERIFY(two && two->snapshot);
        QCOMPARE(two->state, state::completed);
        QCOMPARE(rig.desktop->output(second), QByteArray("two\n"));
        QVERIFY(two->snapshot->snapshotId != one->snapshot->snapshotId);
        const QJsonObject firstPack = rig.desktop->packsSeen().value(first);
        const QJsonObject secondPack = rig.desktop->packsSeen().value(second);
        QCOMPARE(firstPack.value("baseId").toString(), QString());
        QCOMPARE(secondPack.value("baseId").toString(), one->snapshot->snapshotId);
        QVERIFY(secondPack.value("size").toInteger() * 8 < firstPack.value("size").toInteger());
        verifyClean(rig, {}, m_unitsBefore, {"one\n", "two\n"});
        QVERIFY(limit.within());
    }

    // Scenario 6, 180 s: the runner killed during a job and started again ends it at the Relay
    // once, as interrupted (runner_restarted), and never runs the command again. The desktop's
    // record, dispatched, does not follow the Relay: with no peer contact for 20 minutes (on the
    // desktop's moved clock) it needs attention with an unverified clean-up, and the user's
    // resolve_cleanup ends it interrupted (cleanup_acknowledged).
    void endsTheJobOfARestartedRunner() {
        const Limit limit(180'000);
        Rig rig;
        QVERIFY(rig.ready());
        QVERIFY(!rig.pairAndRun().isEmpty());
        const QString repo = rig.repository({{"a.txt", "a\n"}});
        const QByteArray marker = sleepMarker();
        const QString task = rig.submit("printf 'run\\n' >> \"$TRACE/runs\"; printf 'begin\\n'; "
                                        "exec sleep " +
                                            QString::fromLatin1(marker),
                                        repo);
        QVERIFY(rig.waitOutput(task, "begin\n", 60'000));
        rig.runner->kill();
        QVERIFY(rig.runner->startRun());
        const auto finished = [&] {
            for (const auto& lease : std::as_const(rig.relay.leases))
                if (!lease.recordedOutcome.isEmpty())
                    return true;
            return false;
        };
        QVERIFY(QTest::qWaitFor(finished, 90'000));
        const auto lease = rig.relay.leases.first();
        QCOMPARE(lease.recordedOutcome, QStringLiteral("interrupted"));
        QCOMPARE(lease.recordedReason, QStringLiteral("runner_restarted"));
        QCOMPARE(rig.relay.count("POST", "/finish"), 1);

        // The Relay's end alone leaves the dispatched record where it was.
        QTest::qWait(3'000);
        auto record = rig.desktop->record(task);
        QVERIFY(record);
        QCOMPARE(record->state, state::running);
        rig.desktop->skipJobTime(20 * 60 * 1000);
        QVERIFY(QTest::qWaitFor(
            [&] {
                record = rig.desktop->record(task);
                return record && record->state == state::needsAttention;
            },
            60'000)); // also covers the desktop noticing the lost peer
        QCOMPARE(record->reason, QStringLiteral("cleanup_unverified"));
        QCOMPARE(record->cleanupStatus, cleanup::unverified);
        QCOMPARE(rig.desktop->resolveCleanup(task), QString());
        record = rig.desktop->record(task);
        QVERIFY(record);
        QCOMPARE(record->state, state::interrupted);
        QCOMPARE(record->reason, QStringLiteral("cleanup_acknowledged"));

        QCOMPARE(readTrace(rig, "runs"), QByteArray("run\n"));
        QCOMPARE(rig.relay.count("POST", "/finish"), 1);
        verifyClean(rig, {marker}, m_unitsBefore, {QString::fromLatin1(marker)});
        QVERIFY(limit.within());
    }

    // Scenario 7, 120 s: the desktop loses the peer channel during a job, the link comes back,
    // and the output arrives whole, nothing lost and nothing twice.
    void keepsTheOutputAcrossALostChannel() {
        const Limit limit(120'000);
        Rig rig;
        QVERIFY(rig.ready());
        const QString runnerId = rig.pairAndRun();
        QVERIFY(!runnerId.isEmpty());
        const QString repo = rig.repository({{"a.txt", "a\n"}});
        const QString task = rig.submit(
            "i=1; while [ $i -le 40 ]; do printf 'line-%d\\n' $i; i=$((i+1)); sleep 0.1; done",
            repo);
        QVERIFY(rig.waitOutput(task, "line-5\n", 60'000));
        const int startsBefore = rig.signalCount(runnerId, QStringLiteral("signal.start"));
        QVERIFY(rig.desktop->transports().loseOpen() >= 1);
        const auto record = rig.waitTerminal(task, 90'000);
        QVERIFY(record);
        QCOMPARE(record->state, state::completed);
        QByteArray expected;
        for (int line = 1; line <= 40; ++line)
            expected += "line-" + QByteArray::number(line) + '\n';
        QCOMPARE(rig.desktop->output(task), expected);
        QVERIFY(rig.signalCount(runnerId, QStringLiteral("signal.start")) > startsBefore);
        verifyClean(rig, {}, m_unitsBefore, {"line-1\n"});
        QVERIFY(limit.within());
    }

    // Scenario 8, 90 s: no paired runner carries the labels; the task fails without reaching
    // any runner.
    void failsWithoutAMatchingRunner() {
        const Limit limit(90'000);
        Rig rig;
        QVERIFY(rig.ready());
        QVERIFY(!rig.pairAndRun().isEmpty());
        const QString repo = rig.repository({{"a.txt", "a\n"}});
        const QString task = rig.submit("printf 'never\\n'", repo, 60'000,
                                        {QStringLiteral("gpu"), QStringLiteral("os:linux")});
        QVERIFY(!task.isEmpty());
        const auto record = rig.waitTerminal(task, 30'000);
        QVERIFY(record);
        QCOMPARE(record->state, state::failed);
        QCOMPARE(record->reason, QStringLiteral("no_matching_runner"));
        QVERIFY(rig.relay.jobs.isEmpty());
        QCOMPARE(rig.relay.count("POST", "/acquire"), 0);
        QCOMPARE(rig.desktop->output(task), QByteArray());
        verifyClean(rig, {}, m_unitsBefore, {"never"});
        QVERIFY(limit.within());
    }

    // Scenario 9, 90 s: the desktop and `register -` show the same code; approving with it pins
    // the runner on the desktop, and a job runs with that pin.
    void pairsWithTheSameCode() {
        const Limit limit(90'000);
        Rig rig;
        QVERIFY(rig.ready());
        const Rig::Pairing pairing = rig.pair();
        QVERIFY(!pairing.runnerCode.isEmpty());
        QVERIFY2(!pairing.desktopCode.isEmpty(),
                 qPrintable(pairing.desktopState + QStringLiteral(" | ") + rig.trail()));
        QCOMPARE(pairing.desktopCode, pairing.runnerCode);
        QCOMPARE(pairing.approveError, QString());
        QCOMPARE(pairing.registerExit, 0);
        QVERIFY(rig.desktop->pinned(pairing.runnerId));
        QVERIFY(rig.runner->startRun());
        QVERIFY(QTest::qWaitFor(
            [&] { return rig.relay.runnerOnline(pairing.runnerId) || !rig.runner->running(); },
            60'000));
        QVERIFY(rig.runner->running());
        const QString task = rig.submit("printf 'paired\\n'", rig.repository({{"a.txt", "a\n"}}));
        const auto record = rig.waitTerminal(task, 60'000);
        QVERIFY(record);
        QCOMPARE(record->state, state::completed);
        QCOMPARE(record->runner ? record->runner->runnerId : QString(), pairing.runnerId);
        QCOMPARE(rig.desktop->output(task), QByteArray("paired\n"));
        verifyClean(rig, {}, m_unitsBefore, {"paired\n"});
        QVERIFY(limit.within());
    }

    // Scenario 9, wrong code, 90 s: nothing is approved at the Relay, the desktop pins nothing,
    // and no job reaches the runner.
    void refusesAWrongCode() {
        const Limit limit(90'000);
        Rig rig;
        QVERIFY(rig.ready());
        const Rig::Pairing pairing = rig.pair([](const QString& shown) {
            QString wrong = shown;
            const QChar last = wrong.back();
            wrong.back() = last == QLatin1Char('2') ? QLatin1Char('3') : QLatin1Char('2');
            return wrong;
        });
        QVERIFY(!pairing.desktopCode.isEmpty());
        QCOMPARE(pairing.desktopCode, pairing.runnerCode);
        QCOMPARE(pairing.approveError, QStringLiteral("code_mismatch"));
        QCOMPARE(rig.relay.count("POST", "/approve"), 0);
        QVERIFY(!rig.desktop->pinned(pairing.runnerId));
        QVERIFY(rig.runner->stop(10'000));
        QVERIFY(rig.runner->startRun());
        QTest::qWait(3'000);
        const QString task = rig.submit("printf 'never\\n'", rig.repository({{"a.txt", "a\n"}}));
        const auto record = rig.waitTerminal(task, 30'000);
        QVERIFY(record);
        QCOMPARE(record->state, state::failed);
        QCOMPARE(record->reason, QStringLiteral("no_matching_runner"));
        QCOMPARE(rig.relay.count("POST", "/acquire"), 0);
        verifyClean(rig, {}, m_unitsBefore, {"never"});
        QVERIFY(limit.within());
    }

    // Coupling check, 60 s: a job still queued at the Relay (its runner is stopped) expires
    // there; the Relay's job.changed must end the desktop's record failed (queue_expired), as
    // the record is not dispatched.
    void endsAJobThatExpiredInTheQueue() {
        const Limit limit(60'000);
        Rig rig;
        QVERIFY(rig.ready());
        QVERIFY(!rig.pairAndRun().isEmpty());
        QVERIFY(rig.runner->stop(10'000));
        const QString task = rig.submit("printf 'never\\n'", rig.repository({{"a.txt", "a\n"}}));
        std::optional<RemoteJobRecord> record;
        QVERIFY(QTest::qWaitFor(
            [&] {
                record = rig.desktop->record(task);
                return record && record->placement == placement::relayQueued &&
                       !record->jobId.isEmpty();
            },
            20'000));
        QVERIFY(rig.desktop->socketOpen());
        QVERIFY(rig.relay.expire(record->jobId));
        record = rig.waitTerminal(task, 10'000);
        QVERIFY2(record, "the desktop did not take the Relay's job.changed");
        QCOMPARE(record->state, state::failed);
        QCOMPARE(record->reason, QStringLiteral("queue_expired"));
        QCOMPARE(rig.relay.count("POST", "/acquire"), 0);
        verifyClean(rig, {}, m_unitsBefore, {"never"});
        QVERIFY(limit.within());
    }
};

QTEST_GUILESS_MAIN(RunnerDesktopTest)
#include "RunnerDesktopTest.moc"
