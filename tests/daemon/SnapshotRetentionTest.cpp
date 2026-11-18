#include "runner/sync/PackCache.h"
#include "runner/sync/SnapshotPreparation.h"
#include "runner/sync/StoreRetention.h"

#include "snapshot/SnapshotStore.h"
#include "snapshot/WorktreeSnapshot.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <sys/time.h>

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
    const time_t seconds = static_cast<time_t>(when.toSecsSinceEpoch());
    const timeval times[2] = {{seconds, 0}, {seconds, 0}};
    QCOMPARE(utimes(QFile::encodeName(path).constData(), times), 0);
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
        cache.setInFlight(QStringLiteral("held"), true);
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
        cache.setInFlight(QStringLiteral("oldest-held"), true);
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
        cache.setInFlight(QStringLiteral("a"), true);
        cache.sweep(QDateTime::currentDateTimeUtc());
        QVERIFY(cache.find(QStringLiteral("a")));
        cache.setInFlight(QStringLiteral("a"), false);
        cache.sweep(QDateTime::currentDateTimeUtc());
        QVERIFY(!cache.find(QStringLiteral("a")));
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
        retention.keepPack(QStringLiteral("job-1"), pack);
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

        reloaded.releaseRunner(QStringLiteral("run-1"));
        QVERIFY(!reloaded.confirmed(QStringLiteral("run-1"), workspaceKey));
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
