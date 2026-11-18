#include "runner/sync/PackCache.h"
#include "runner/sync/SnapshotPreparation.h"
#include "runner/sync/StoreRetention.h"

#include "snapshot/SnapshotStore.h"
#include "snapshot/WorktreeSnapshot.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace ketplus::daemon::runner::sync;
namespace snapshot = ketplus::runner::snapshot;

namespace {

const QString workspaceKey = QStringLiteral("WTqX-UCm9Azol6_YM8-FRbm_J_wZgnOJ");
const QString digest(64, QLatin1Char('a'));

void runGit(const QString& home, const QString& directory, const QStringList& arguments) {
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("PATH"), qEnvironmentVariable("PATH"));
    environment.insert(QStringLiteral("HOME"), home);
    environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
    environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral("/dev/null"));
    for (const auto* name : {"GIT_AUTHOR_NAME", "GIT_COMMITTER_NAME"})
        environment.insert(QString::fromLatin1(name), QStringLiteral("Fixture"));
    for (const auto* name : {"GIT_AUTHOR_EMAIL", "GIT_COMMITTER_EMAIL"})
        environment.insert(QString::fromLatin1(name), QStringLiteral("fixture@example.invalid"));
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory);
    process.start(QStringLiteral("git"), arguments);
    QVERIFY(process.waitForFinished(60'000));
    QCOMPARE(process.exitCode(), 0);
}

void write(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(content);
}

void age(const QString& path, const QDateTime& when) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.setFileTime(when, QFileDevice::FileModificationTime));
    QVERIFY(file.setFileTime(when, QFileDevice::FileAccessTime));
}

PackFile fakePack(const PackCache& cache, const QString& jobId, const qint64 size,
                  const QString& base = {}) {
    PackFile pack;
    pack.snapshotId = QString(40, QLatin1Char('1'));
    pack.baseId = base;
    pack.path = cache.pathFor(jobId, pack.snapshotId, base);
    pack.size = size;
    pack.sha256 = digest;
    write(pack.path, QByteArray(size, 'p'));
    return pack;
}

// Sets the time of every cache file of the job.
void ageJob(const QString& directory, const PackFile& pack, const QDateTime& when) {
    const QString stem = QFileInfo(pack.path).fileName().left(32);
    for (const QString& name : QDir(directory).entryList({stem + QStringLiteral("*")}, QDir::Files))
        age(QDir(directory).filePath(name), when);
}

QByteArray readSide(const PackCache& cache, const QString& jobId) {
    const QString stem =
        QFileInfo(cache.pathFor(jobId, QString(40, QLatin1Char('1')), {})).fileName().left(32);
    QFile file(QFileInfo(cache.pathFor(jobId, {}, {})).path() + QLatin1Char('/') + stem +
               QStringLiteral(".json"));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QStringList filesIn(const QString& directory) {
    return QDir(directory).entryList(QDir::Files | QDir::Hidden, QDir::Name);
}

} // namespace

class SnapshotRetentionTest : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        root_ = QFileInfo(dir_->path()).canonicalFilePath();
        packs_ = root_ + QStringLiteral("/packs");
    }

    void cleanup() { dir_.reset(); }

    void aPackSurvivesARestart() {
        PackCache cache(packs_);
        const PackFile pack = fakePack(cache, QStringLiteral("job-1"), 10);
        QVERIFY(cache.record(QStringLiteral("job-1"), pack));
        PackCache reopened(packs_);
        const auto found = reopened.find(QStringLiteral("job-1"));
        QVERIFY(found);
        QCOMPARE(found->path, pack.path);
        QCOMPARE(found->size, 10);
        QCOMPARE(found->sha256, digest);
        QCOMPARE(found->snapshotId, pack.snapshotId);
        QVERIFY(!reopened.find(QStringLiteral("job-2")));
        QCOMPARE(cache.pathFor(QStringLiteral("job-1"), pack.snapshotId, {}), pack.path);
        QVERIFY(cache.pathFor(QStringLiteral("job-1"), pack.snapshotId, pack.snapshotId) !=
                pack.path);
    }

    void aPackWhoseFileChangedIsNotFound() {
        PackCache cache(packs_);
        const PackFile pack = fakePack(cache, QStringLiteral("job-1"), 10);
        QVERIFY(cache.record(QStringLiteral("job-1"), pack));
        write(pack.path, "short");
        QVERIFY(!cache.find(QStringLiteral("job-1")));
        QFile::remove(pack.path);
        QVERIFY(!cache.find(QStringLiteral("job-1")));
    }

    void refusesAPackOutsideItsPath() {
        PackCache cache(packs_);
        PackFile pack = fakePack(cache, QStringLiteral("job-1"), 10);
        pack.path = root_ + QStringLiteral("/elsewhere.pack");
        QVERIFY(!cache.record(QStringLiteral("job-1"), pack));
        pack = fakePack(cache, QStringLiteral("job-1"), 10);
        pack.sha256 = QStringLiteral("ABC");
        QVERIFY(!cache.record(QStringLiteral("job-1"), pack));
    }

    void aNewPackReplacesTheJobsOldOne() {
        PackCache cache(packs_);
        const PackFile delta =
            fakePack(cache, QStringLiteral("job-1"), 10, QString(40, QLatin1Char('2')));
        QVERIFY(cache.record(QStringLiteral("job-1"), delta));
        const PackFile full = fakePack(cache, QStringLiteral("job-1"), 20);
        QVERIFY(cache.record(QStringLiteral("job-1"), full));
        QVERIFY(!QFile::exists(delta.path));
        QCOMPARE(cache.find(QStringLiteral("job-1"))->path, full.path);
        cache.remove(QStringLiteral("job-1"));
        QVERIFY(filesIn(packs_).isEmpty());
    }

    void sweepRemovesUnownedPacksAfterADay() {
        PackCache cache(packs_);
        const QDateTime now = QDateTime::currentDateTimeUtc();
        for (const char* job : {"old", "fresh", "held"}) {
            const QString id = QString::fromLatin1(job);
            const PackFile pack = fakePack(cache, id, 10);
            QVERIFY(cache.record(id, pack));
            const qint64 offset = id == QStringLiteral("fresh") ? 10 : -10;
            ageJob(packs_, pack, now.addSecs(-24 * 3600 + offset));
        }
        cache.setLiveJobs({QStringLiteral("held")});
        cache.sweep(now);
        QVERIFY(!cache.find(QStringLiteral("old")));
        QVERIFY(cache.find(QStringLiteral("fresh")));
        QVERIFY(cache.find(QStringLiteral("held")));
    }

    void sweepEvictsTheOldestPackNotInFlight() {
        PackCache cache(packs_, PackCacheLimits{2500, 24 * 3600});
        const QDateTime now = QDateTime::currentDateTimeUtc();
        int minutes = 40;
        for (const char* job : {"oldest-held", "older", "newer", "newest"}) {
            const QString id = QString::fromLatin1(job);
            const PackFile pack = fakePack(cache, id, 1000);
            QVERIFY(cache.record(id, pack));
            ageJob(packs_, pack, now.addSecs(-60 * minutes));
            minutes -= 10;
        }
        cache.setLiveJobs({QStringLiteral("oldest-held")});
        QVERIFY(cache.totalBytes() > 4000);
        cache.sweep(now);
        QVERIFY(cache.find(QStringLiteral("oldest-held")));
        QVERIFY(!cache.find(QStringLiteral("older")));
        QVERIFY(!cache.find(QStringLiteral("newer")));
        QVERIFY(cache.find(QStringLiteral("newest")));
        QVERIFY(cache.totalBytes() <= 2500);
    }

    void inFlightPacksAreNeverEvicted() {
        PackCache cache(packs_, PackCacheLimits{100, 24 * 3600});
        QVERIFY(cache.record(QStringLiteral("a"), fakePack(cache, QStringLiteral("a"), 1000)));
        cache.setLiveJobs({QStringLiteral("a")});
        cache.sweep(QDateTime::currentDateTimeUtc());
        QVERIFY(cache.find(QStringLiteral("a")));
        cache.setInFlight(QStringLiteral("a"), false);
        cache.sweep(QDateTime::currentDateTimeUtc());
        QVERIFY(!cache.find(QStringLiteral("a")));
    }

    void packsWithASideFileWaitForTheLiveJobs() {
        PackCache cache(packs_);
        const QDateTime now = QDateTime::currentDateTimeUtc();
        for (const char* job : {"alive", "gone"}) {
            const QString id = QString::fromLatin1(job);
            const PackFile pack = fakePack(cache, id, 10);
            QVERIFY(cache.record(id, pack));
            ageJob(packs_, pack, now.addDays(-3));
        }
        write(packs_ + QStringLiteral("/stray.tmp"), "x");
        age(packs_ + QStringLiteral("/stray.tmp"), now.addDays(-3));
        PackCache restarted(packs_, PackCacheLimits{1, 24 * 3600});
        restarted.sweep(now);
        QVERIFY(restarted.find(QStringLiteral("alive")));
        QVERIFY(restarted.find(QStringLiteral("gone")));
        QVERIFY(!QFile::exists(packs_ + QStringLiteral("/stray.tmp")));
        restarted.setLiveJobs({QStringLiteral("alive")});
        restarted.sweep(now);
        QVERIFY(restarted.find(QStringLiteral("alive")));
        QVERIFY(!restarted.find(QStringLiteral("gone")));
    }

    void releasingARunnerDropsItsPacksNotInFlight() {
        PackCache cache(packs_);
        const QString runner = QStringLiteral("run-1");
        QVERIFY(cache.record(QStringLiteral("idle"), fakePack(cache, QStringLiteral("idle"), 10),
                             runner));
        QVERIFY(cache.record(QStringLiteral("busy"), fakePack(cache, QStringLiteral("busy"), 10),
                             runner));
        QVERIFY(cache.record(QStringLiteral("other"), fakePack(cache, QStringLiteral("other"), 10),
                             QStringLiteral("run-2")));
        QVERIFY(
            cache.record(QStringLiteral("legacy"), fakePack(cache, QStringLiteral("legacy"), 10)));
        cache.setInFlight(QStringLiteral("busy"), true);
        for (const QString& name : filesIn(packs_))
            QVERIFY2(!name.contains(runner), qPrintable(name));
        QVERIFY(!readSide(cache, QStringLiteral("idle")).contains(runner.toUtf8()));
        cache.removeRunner(runner);
        QVERIFY(!cache.find(QStringLiteral("idle")));
        QVERIFY(cache.find(QStringLiteral("busy")));
        QVERIFY(cache.find(QStringLiteral("other")));
        QVERIFY(cache.find(QStringLiteral("legacy")));
        QCOMPARE(filesIn(packs_).size(), 6);
    }

    void aRefCommandThatHangsFails() {
#if defined(Q_OS_WIN)
        QSKIP("The stand-in git is a sh script, which Windows cannot start directly.");
#endif
        const QString fake = root_ + QStringLiteral("/hanging-git");
        write(fake, "#!/bin/sh\nexec sleep 60\n");
        QVERIFY(
            QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(QDir().mkpath(packs_));
        PackCache cache(packs_);
        snapshot::GitTool tool;
        tool.program = fake;
        const QString store = root_ + QStringLiteral("/store");
        runGit(root_, root_,
               {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("--bare"), store});
        StoreRetention retention(store, cache, tool);
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!retention.confirm(QStringLiteral("run-1"), workspaceKey, QString(40, 'a')));
        QVERIFY(timer.elapsed() >= refCommandTimeoutMs - 500); // Git ran and was stopped
        QVERIFY(timer.elapsed() < refCommandTimeoutMs + 3'000);
        QVERIFY(!retention.confirmed(QStringLiteral("run-1"), workspaceKey));
    }

    void storeRefsFollowTheJobAndTheRunner() {
        const QString home = root_ + QStringLiteral("/home");
        const QString repo = root_ + QStringLiteral("/repo");
        QVERIFY(QDir().mkpath(home));
        QVERIFY(QDir().mkpath(repo));
        runGit(home, repo, {QStringLiteral("init"), QStringLiteral("-q")});
        write(repo + QStringLiteral("/a.txt"), "a\n");

        PrepareRequest request;
        request.cwd = repo;
        request.storesRoot = root_ + QStringLiteral("/stores");
        request.inFlightRef = StoreRetention::inFlightRef(QStringLiteral("job-1"));
        request.userGitSources = snapshot::UserGitSources{home, home + QStringLiteral("/.c"), {}};
        const auto prepared = prepareSnapshot(request);
        QVERIFY2(prepared.prepared, qPrintable(prepared.code));
        const QString store = prepared.prepared->storePath;
        const QString snap = prepared.prepared->snapshotId;

        QVERIFY(QDir().mkpath(packs_));
        PackCache cache(packs_);
        StoreRetention retention(store, cache);
        QVERIFY(retention.load());
        QVERIFY(!retention.confirmed(QStringLiteral("run-1"), workspaceKey));

        const auto written = snapshot::writePack(
            {store, snap, {}, cache.pathFor(QStringLiteral("job-1"), snap, {})});
        QVERIFY(written);
        PackFile pack{written.value().path,
                      written.value().size,
                      QString::fromLatin1(written.value().sha256),
                      snap,
                      {}};
        retention.keepPack(QStringLiteral("job-1"), QStringLiteral("run-1"), pack);
        QVERIFY(cache.inFlight(QStringLiteral("job-1")));
        QCOMPARE(retention.pack(QStringLiteral("job-1"))->sha256, pack.sha256);

        QVERIFY(retention.confirm(QStringLiteral("run-1"), workspaceKey, snap));
        QVERIFY(!retention.confirm(QStringLiteral("run-1"), QStringLiteral("bad/key"), snap));
        retention.release(QStringLiteral("job-1"));
        QVERIFY(!retention.pack(QStringLiteral("job-1")));
        QVERIFY(!cache.inFlight(QStringLiteral("job-1")));

        StoreRetention reloaded(store, cache);
        QVERIFY(reloaded.load());
        QCOMPARE(reloaded.confirmed(QStringLiteral("run-1"), workspaceKey), snap);
        QVERIFY(!reloaded.confirmed(QStringLiteral("run-2"), workspaceKey));
        auto refs = snapshot::listRefs(store);
        QVERIFY(refs);
        QCOMPARE(refs.value().size(), 1); // the in-flight ref is gone

        const PackFile left = fakePack(cache, QStringLiteral("job-2"), 10);
        reloaded.keepPack(QStringLiteral("job-2"), QStringLiteral("run-1"), left);
        cache.setInFlight(QStringLiteral("job-2"), false); // its job was cancelled meanwhile
        reloaded.releaseRunner(QStringLiteral("run-1"));
        QVERIFY(!reloaded.confirmed(QStringLiteral("run-1"), workspaceKey));
        QVERIFY(!cache.find(QStringLiteral("job-2")));
        refs = snapshot::listRefs(store);
        QVERIFY(refs.value().isEmpty());

        QVERIFY(reloaded.confirm(QStringLiteral("run-1"), workspaceKey, snap));
        reloaded.forget(QStringLiteral("run-1"), workspaceKey);
        QVERIFY(!reloaded.confirmed(QStringLiteral("run-1"), workspaceKey));
        QVERIFY(snapshot::listRefs(store).value().isEmpty());
    }

    void aJobThatNeverRanDropsItsSnapshotAndPack() {
        const QString home = root_ + QStringLiteral("/home");
        const QString repo = root_ + QStringLiteral("/repo");
        QVERIFY(QDir().mkpath(home));
        QVERIFY(QDir().mkpath(repo));
        runGit(home, repo, {QStringLiteral("init"), QStringLiteral("-q")});
        write(repo + QStringLiteral("/a.txt"), "a\n");
        PrepareRequest request;
        request.cwd = repo;
        request.storesRoot = root_ + QStringLiteral("/stores");
        request.inFlightRef = StoreRetention::inFlightRef(QStringLiteral("queued-job"));
        request.userGitSources = snapshot::UserGitSources{home, home + QStringLiteral("/.c"), {}};
        const auto prepared = prepareSnapshot(request);
        QVERIFY2(prepared.prepared, qPrintable(prepared.code));
        const QString store = prepared.prepared->storePath;
        QCOMPARE(snapshot::listRefs(store).value().size(), 1);

        PackCache cache(packs_);
        QVERIFY(cache.record(QStringLiteral("queued-job"),
                             fakePack(cache, QStringLiteral("queued-job"), 10)));
        QVERIFY(dropJobSnapshot(store, cache, QStringLiteral("queued-job")));
        QVERIFY(snapshot::listRefs(store).value().isEmpty());
        QVERIFY(!cache.find(QStringLiteral("queued-job")));
        QVERIFY(filesIn(packs_).isEmpty());
        // Dropping again is harmless.
        QVERIFY(dropJobSnapshot(store, cache, QStringLiteral("queued-job")));
    }

  private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString root_;
    QString packs_;
};

QTEST_GUILESS_MAIN(SnapshotRetentionTest)
#include "SnapshotRetentionTest.moc"
