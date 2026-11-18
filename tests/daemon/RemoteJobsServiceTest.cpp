// RemoteJobsService on the daemon's real parts: the registry of a journal, the Control client's
// runner port and Relay socket, Git on the thread pool and the hourly sweep.
#include "runner/remote/RemoteJobsService.h"
#include "RunnerTestFixture.h"
#include "runner/remote/RemoteJobs.h"
#include "runner/sync/SnapshotPreparation.h"
#include "transport/Signaling.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

using namespace runner_test;
using ketplus::daemon::runner::remote::RemoteJobsService;
using ketplus::daemon::runner::remote::SubmitRequest;
namespace prep = ::ketplus::daemon::runner::sync;

namespace {

QJsonObject revoked(const QString& device) {
    return {{"type", "device.revoked"}, {"deviceId", device}};
}

} // namespace

class RemoteJobsServiceTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void init() { r03::resetCredentials(); }

    // Available while the Relay offers runners; runner messages are taken, device ones are not.
    void theServiceTakesTheRunnerMessagesOfTheSocket() {
        Fixture f;
        QTemporaryDir state;
        RemoteJobsService service(f.journal.backgroundTasks(), *f.client, state.path());
        service.start();
        QCOMPARE(service.jobs().available(), prep::checkGit().isEmpty());
        f.offerRunners(false);
        QVERIFY(!service.jobs().available());

        QStringList seen;
        QObject::connect(&Access::signaling(*f.client), &transport::Signaling::deviceRevoked,
                         [&](const QString& device) { seen.append(device); });
        emit Access::wakeups(*f.client).message({{"type", "job.changed"}, {"jobId", "job-1"}});
        emit Access::wakeups(*f.client).message(revoked("device-1"));
        QCOMPARE(seen, QStringList{"device-1"});
    }

    // The worktree is frozen on the thread pool and the answer comes back on the event loop;
    // a submit that fails there leaves no record.
    void aSubmitIsPreparedOffTheEventLoop() {
        if (!prep::checkGit().isEmpty())
            QSKIP("no usable system git");
        Fixture f;
        QTemporaryDir state;
        QTemporaryDir notARepository;
        RemoteJobsService service(f.journal.backgroundTasks(), *f.client, state.path());
        service.start();
        QVERIFY(service.jobs().available());
        SubmitRequest request;
        request.spec.cwd = notARepository.path();
        request.runsOn = {QStringLiteral("linux")};
        request.timeoutMs = 60'000;
        QString code;
        service.jobs().submit(QStringLiteral("task-1"), request,
                              [&](const QString& answer) { code = answer; });
        QVERIFY(code.isEmpty()); // not answered inside the call
        QTRY_COMPARE(code, prep::prepare::notAWorktree);
        QVERIFY(!service.jobs().record(QStringLiteral("task-1")));
    }

    // The sweep runs once at start and then every hour on its timer.
    void theHourlySweepClearsOldPacks() {
        Fixture f;
        QTemporaryDir state;
        RemoteJobsService service(f.journal.backgroundTasks(), *f.client, state.path());
        service.start();
        auto* sweep = service.findChild<QTimer*>(QStringLiteral("remote-jobs-sweep"));
        QVERIFY(sweep);
        QVERIFY(sweep->isActive());
        QCOMPARE(sweep->interval(), 60 * 60 * 1000);

        const QString packs = QDir(state.path()).filePath("runner-snapshots/packs");
        QVERIFY(QDir().mkpath(packs));
        const QString stale = QDir(packs).filePath(QStringLiteral("stale.pack"));
        QFile file(stale);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("pack") == 4);
        QVERIFY(file.flush()); // before the time, which a later write would move
        QVERIFY(file.setFileTime(QDateTime::currentDateTimeUtc().addDays(-30),
                                 QFileDevice::FileModificationTime));
        file.close();
        QTest::qWait(50);
        QVERIFY(QFile::exists(stale)); // written after the sweep at start
        sweep->setInterval(1);
        QTRY_VERIFY(!QFile::exists(stale));
    }

    // A service that stops gives the socket back: every message goes where it always went.
    void aStoppedServiceLeavesTheSocketAsItWas() {
        Fixture f;
        QTemporaryDir state;
        auto service = std::make_unique<RemoteJobsService>(f.journal.backgroundTasks(), *f.client,
                                                           state.path());
        service->start();
        service.reset();
        QStringList seen;
        QObject::connect(&Access::signaling(*f.client), &transport::Signaling::deviceRevoked,
                         [&](const QString& device) { seen.append(device); });
        emit Access::wakeups(*f.client).message({{"type", "job.changed"}, {"jobId", "job-1"}});
        emit Access::wakeups(*f.client).openChanged(true);
        emit Access::wakeups(*f.client).message(revoked("device-1"));
        QCOMPARE(seen, QStringList{"device-1"});
    }
};

QTEST_GUILESS_MAIN(RemoteJobsServiceTest)
#include "RemoteJobsServiceTest.moc"
