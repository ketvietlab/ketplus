#include "SnapshotSyncFakes.h"

#include "runner/sync/SnapshotPreparation.h"
#include "runner/sync/SnapshotWorker.h"
#include "runner/sync/StoreRetention.h"

#include "snapshot/SnapshotStore.h"
#include "snapshot/WorkspaceApply.h"
#include "snapshot/WorktreeSnapshot.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <memory>

using namespace snapshot_sync_test;
namespace snapshot = ketplus::runner::snapshot;

namespace {

const QString key = QStringLiteral("WTqX-UCm9Azol6_YM8-FRbm_J_wZgnOJ");

void write(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(content);
}

QByteArray readAll(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

// Runs git in `directory` with a fixed identity and no user configuration.
QByteArray gitIn(const QString& home, const QString& directory, const QStringList& arguments) {
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
    if (!process.waitForFinished(60'000) || process.exitCode() != 0)
        qWarning() << "git failed" << arguments.value(0);
    return process.readAllStandardOutput().trimmed();
}

int looseObjects(const QString& store) {
    int count = 0;
    QDirIterator it(store + QStringLiteral("/objects"), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (QFileInfo(path).dir().dirName().size() == 2)
            ++count;
    }
    return count;
}

// Counts the packs a sender asks for.
class CountingWork final : public SyncWork {
  public:
    explicit CountingWork(SyncWork& inner) : inner_(inner) {}
    void buildPack(const QString& snapshotId, const QString& baseId,
                   PackCallback callback) override {
        ++builds;
        inner_.buildPack(snapshotId, baseId, std::move(callback));
    }
    void readChunk(const PackFile& pack, qint64 offset, qint64 length,
                   ChunkCallback callback) override {
        inner_.readChunk(pack, offset, length, std::move(callback));
    }
    void hashPack(const PackFile& pack, HashCallback callback) override {
        ++hashes;
        inner_.hashPack(pack, std::move(callback));
    }
    int builds = 0;
    int hashes = 0;

  private:
    SyncWork& inner_;
};

} // namespace

// A real snapshot goes through the sender, the worker and the store retention, and the bytes the
// runner would receive apply into a workspace equal to the source worktree.
class SnapshotSyncIntegrationTest : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        dir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(dir_->isValid());
        root_ = QFileInfo(dir_->path()).canonicalFilePath();
        home_ = root_ + QStringLiteral("/home");
        repo_ = root_ + QStringLiteral("/repo");
        QVERIFY(QDir().mkpath(home_));
        QVERIFY(QDir().mkpath(repo_));
        QProcess git;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("HOME"), home_);
        environment.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral("/dev/null"));
        environment.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
        git.setProcessEnvironment(environment);
        git.setWorkingDirectory(repo_);
        git.start(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("-q")});
        QVERIFY(git.waitForFinished(60'000));
        QCOMPARE(git.exitCode(), 0);
        write(repo_ + QStringLiteral("/src/main.c"), "int main() { return 0; }\n");
        write(repo_ + QStringLiteral("/README"), QByteArray(300'000, 'r'));
    }

    void cleanup() { dir_.reset(); }

    void aSnapshotReachesTheRunnerAndIsReleased() {
        const QString jobId = QStringLiteral("job-int");
        PrepareRequest request;
        request.cwd = repo_ + QStringLiteral("/src");
        request.storesRoot = root_ + QStringLiteral("/stores");
        request.inFlightRef = StoreRetention::inFlightRef(jobId);
        request.userGitSources = snapshot::UserGitSources{home_, home_ + QStringLiteral("/.c"), {}};
        const auto prepared = prepareSnapshot(request);
        QVERIFY2(prepared.prepared, qPrintable(prepared.code));
        const QString store = prepared.prepared->storePath;
        const QString snap = prepared.prepared->snapshotId;

        PackCache packs(root_ + QStringLiteral("/packs"));
        StoreRetention retention(store, packs);
        QVERIFY(retention.load());
        FakeLink link;
        FakeEvents events;
        QByteArray received;
        {
            SnapshotWorker worker(store, packs);
            JobSyncWork work(worker, jobId);
            SnapshotSender sender(SyncJob{jobId, runnerId, key, snap, {}}, link, work, retention,
                                  events);
            sender.start({});
            QTRY_COMPARE(link.of<protocol::SnapshotOffer>().size(), 1);
            const auto offer = link.of<protocol::SnapshotOffer>().first();
            QVERIFY(!offer.baseId.has_value());
            QVERIFY(packs.inFlight(jobId));
            sender.receive(protocol::SnapshotWant{snap, QStringLiteral("need"), 0});
            QTRY_VERIFY(sender.sent() == offer.packBytes);
            for (const auto& [id, bytes] : link.bulk)
                received.append(bytes);
            QCOMPARE(received.size(), offer.packBytes);
            QCOMPARE(sha256(received), offer.packSha256);
            sender.receive(protocol::SnapshotAck{snap, offer.packBytes});
            sender.receive(protocol::SnapshotReady{snap});
            QCOMPARE(events.ready, QStringList{jobId});
            QVERIFY(events.failed.isEmpty());
        }
        QCOMPARE(retention.confirmed(runnerId, key), snap);
        QVERIFY(!retention.pack(jobId));
        const auto refs = snapshot::listRefs(store);
        QVERIFY(refs);
        QVERIFY(!refs.value().contains(StoreRetention::inFlightRef(jobId)));
        QCOMPARE(refs.value().size(), 1);

        const QString packPath = root_ + QStringLiteral("/received.pack");
        write(packPath, received);
        const auto applied = snapshot::applySnapshot({root_ + QStringLiteral("/mirror.git"),
                                                      root_ + QStringLiteral("/workspace"),
                                                      packPath,
                                                      sha256(received).toLatin1(),
                                                      snap,
                                                      {}});
        QVERIFY2(applied, qPrintable(applied ? QString{} : applied.error().message));
        QCOMPARE(readAll(root_ + QStringLiteral("/workspace/src/main.c")),
                 readAll(repo_ + QStringLiteral("/src/main.c")));
        QCOMPARE(readAll(root_ + QStringLiteral("/workspace/README")),
                 readAll(repo_ + QStringLiteral("/README")));
    }

    void theNextJobOnTheWorkspaceSendsAnIncrementalPack() {
        PackCache packs(root_ + QStringLiteral("/packs"));
        const auto first = prepareJob(QStringLiteral("job-1"));
        QVERIFY(first);
        StoreRetention retention(first->storePath, packs);
        QVERIFY(retention.load());
        SnapshotWorker worker(first->storePath, packs);
        FakeLink link;
        FakeEvents events;
        qint64 fullBytes = 0;
        QByteArray firstPack;
        {
            JobSyncWork work(worker, QStringLiteral("job-1"));
            SnapshotSender sender(
                SyncJob{QStringLiteral("job-1"), runnerId, key, first->snapshotId, {}}, link, work,
                retention, events);
            sender.start({});
            QTRY_COMPARE(link.of<protocol::SnapshotOffer>().size(), 1);
            fullBytes = link.of<protocol::SnapshotOffer>().last().packBytes;
            firstPack = sendAll(sender, link, first->snapshotId);
            QCOMPARE(firstPack.size(), fullBytes);
            sender.receive(protocol::SnapshotReady{first->snapshotId});
        }
        write(repo_ + QStringLiteral("/src/main.c"), "int main() { return 1; }\n");
        const auto second = prepareJob(QStringLiteral("job-2"));
        QVERIFY(second);
        QVERIFY(second->snapshotId != first->snapshotId);
        QByteArray secondPack;
        {
            JobSyncWork work(worker, QStringLiteral("job-2"));
            SnapshotSender sender(
                SyncJob{QStringLiteral("job-2"), runnerId, key, second->snapshotId, {}}, link, work,
                retention, events);
            sender.start({{key, first->snapshotId}});
            QTRY_COMPARE(link.of<protocol::SnapshotOffer>().size(), 2);
            const auto offer = link.of<protocol::SnapshotOffer>().last();
            QCOMPARE(offer.baseId, std::optional<QString>(first->snapshotId));
            QVERIFY(offer.packBytes < fullBytes);
            secondPack = sendAll(sender, link, second->snapshotId);
            sender.receive(protocol::SnapshotReady{second->snapshotId});
        }
        QCOMPARE(events.ready, (QStringList{QStringLiteral("job-1"), QStringLiteral("job-2")}));
        QCOMPARE(retention.confirmed(runnerId, key), second->snapshotId);

        const QString mirror = root_ + QStringLiteral("/mirror.git");
        const QString workspace = root_ + QStringLiteral("/workspace");
        for (const auto& [bytes, snap] :
             {std::pair{firstPack, first->snapshotId}, std::pair{secondPack, second->snapshotId}}) {
            const QString path = root_ + QStringLiteral("/in.pack");
            write(path, bytes);
            const auto applied = snapshot::applySnapshot(
                {mirror, workspace, path, sha256(bytes).toLatin1(), snap, {}});
            QVERIFY2(applied, qPrintable(applied ? QString{} : applied.error().message));
        }
        QCOMPARE(readAll(workspace + QStringLiteral("/src/main.c")),
                 QByteArray("int main() { return 1; }\n"));
        QVERIFY(packFiles().isEmpty());
    }

    void cancellingWhilePackingLeavesNoRefOrFile() {
        const auto prepared = prepareJob(QStringLiteral("job-c"));
        QVERIFY(prepared);
        PackCache packs(root_ + QStringLiteral("/packs"));
        StoreRetention retention(prepared->storePath, packs);
        QVERIFY(retention.load());
        SnapshotWorker worker(prepared->storePath, packs);
        JobSyncWork work(worker, QStringLiteral("job-c"));
        FakeLink link;
        FakeEvents events;
        SnapshotSender sender(
            SyncJob{QStringLiteral("job-c"), runnerId, key, prepared->snapshotId, {}}, link, work,
            retention, events);
        sender.start({});
        sender.cancel();
        drain(worker);
        QVERIFY(link.frames.isEmpty());
        QVERIFY(events.ready.isEmpty() && events.failed.isEmpty());
        QVERIFY(packFiles().isEmpty());
        QVERIFY(snapshot::listRefs(prepared->storePath).value().isEmpty());
    }

    void cancellingWhileSendingOrAwaitingReadyLeavesNoRefOrFile() {
        for (const bool allSent : {false, true}) {
            const QString jobId = allSent ? QStringLiteral("job-r") : QStringLiteral("job-s");
            const auto prepared = prepareJob(jobId);
            QVERIFY(prepared);
            PackCache packs(root_ + QStringLiteral("/packs"));
            StoreRetention retention(prepared->storePath, packs);
            QVERIFY(retention.load());
            SnapshotWorker worker(prepared->storePath, packs);
            JobSyncWork work(worker, jobId);
            FakeLink link;
            FakeEvents events;
            SnapshotSender sender(SyncJob{jobId, runnerId, key, prepared->snapshotId, {}}, link,
                                  work, retention, events);
            sender.start({});
            QTRY_COMPARE(link.of<protocol::SnapshotOffer>().size(), 1);
            if (allSent) {
                QVERIFY(!sendAll(sender, link, prepared->snapshotId).isEmpty());
            } else {
                sender.receive(
                    protocol::SnapshotWant{prepared->snapshotId, QStringLiteral("need"), 0});
            }
            sender.cancel();
            drain(worker);
            sender.receive(protocol::SnapshotReady{prepared->snapshotId});
            QVERIFY(events.ready.isEmpty() && events.failed.isEmpty());
            QVERIFY(!retention.confirmed(runnerId, key));
            QVERIFY(packFiles().isEmpty());
            QVERIFY(snapshot::listRefs(prepared->storePath).value().isEmpty());
        }
    }

    void cancellingWhilePreparingDropsTheSnapshot() {
        PackCache packs(root_ + QStringLiteral("/packs"));
        const QString store =
            storePathFor(root_ + QStringLiteral("/stores"),
                         QFileInfo(repo_ + QStringLiteral("/.git")).canonicalFilePath());
        SnapshotWorker worker(store, packs);
        auto prepared = std::make_shared<std::optional<Prepared>>();
        worker.runOnStore([this, prepared] { *prepared = prepareJob(QStringLiteral("job-p")); });
        // The cancel arrives while the snapshot is being built; the drop runs after it.
        worker.runOnStore(
            [store, &packs] { dropJobSnapshot(store, packs, QStringLiteral("job-p")); });
        drain(worker);
        QVERIFY(prepared->has_value());
        QCOMPARE((*prepared)->storePath, store);
        QVERIFY(snapshot::listRefs(store).value().isEmpty());
        QVERIFY(packFiles().isEmpty());
    }

    void destroyingTheWorkerDropsLateAnswers() {
        const QString jobId = QStringLiteral("job-late");
        PrepareRequest request;
        request.cwd = repo_;
        request.storesRoot = root_ + QStringLiteral("/stores");
        request.inFlightRef = StoreRetention::inFlightRef(jobId);
        request.userGitSources = snapshot::UserGitSources{home_, home_ + QStringLiteral("/.c"), {}};
        const auto prepared = prepareSnapshot(request);
        QVERIFY2(prepared.prepared, qPrintable(prepared.code));
        PackCache packs(root_ + QStringLiteral("/packs"));
        bool answered = false;
        {
            SnapshotWorker worker(prepared.prepared->storePath, packs);
            worker.buildPack(jobId, prepared.prepared->snapshotId, {},
                             [&answered](const PackOutcome&) { answered = true; });
        }
        QCoreApplication::processEvents();
        QVERIFY(!answered);
    }

    void aMissingBaseAsksForAFullPack() {
        const QString jobId = QStringLiteral("job-base");
        PrepareRequest request;
        request.cwd = repo_;
        request.storesRoot = root_ + QStringLiteral("/stores");
        request.inFlightRef = StoreRetention::inFlightRef(jobId);
        request.userGitSources = snapshot::UserGitSources{home_, home_ + QStringLiteral("/.c"), {}};
        const auto prepared = prepareSnapshot(request);
        QVERIFY2(prepared.prepared, qPrintable(prepared.code));
        PackCache packs(root_ + QStringLiteral("/packs"));
        SnapshotWorker worker(prepared.prepared->storePath, packs);
        std::optional<PackOutcome> outcome;
        worker.buildPack(jobId, prepared.prepared->snapshotId, QString(40, QLatin1Char('e')),
                         [&outcome](const PackOutcome& answer) { outcome = answer; });
        QTRY_VERIFY(outcome.has_value());
        QVERIFY(outcome->baseMissing);
        QVERIFY(!outcome->pack);
    }

    void aRestartedSenderOffersTheKeptPack() {
        const QString jobId = QStringLiteral("job-restart");
        const auto prepared = prepareJob(jobId);
        QVERIFY(prepared);
        const QString packsDir = root_ + QStringLiteral("/packs");
        const protocol::SnapshotOffer first = offerAndStop(jobId, *prepared);
        QVERIFY(!first.packSha256.isEmpty());
        PackCache packs(packsDir);
        // A sweep before the job is known again keeps its pack, even a day later.
        packs.sweep(QDateTime::currentDateTimeUtc().addDays(2));
        QVERIFY(packs.find(jobId));
        StoreRetention retention(prepared->storePath, packs);
        QVERIFY(retention.load());
        SnapshotWorker worker(prepared->storePath, packs);
        JobSyncWork jobWork(worker, jobId);
        CountingWork work(jobWork);
        FakeLink link;
        FakeEvents events;
        SnapshotSender sender(SyncJob{jobId, runnerId, key, prepared->snapshotId, {}}, link, work,
                              retention, events);
        sender.start({});
        QCOMPARE(sender.phase(), SnapshotSender::Phase::Verifying);
        QTRY_COMPARE(link.of<protocol::SnapshotOffer>().size(), 1);
        QCOMPARE(work.hashes, 1);
        QCOMPARE(work.builds, 0);
        const auto again = link.of<protocol::SnapshotOffer>().first();
        QCOMPARE(again.packSha256, first.packSha256);
        QCOMPARE(again.packBytes, first.packBytes);
        QVERIFY(packs.inFlight(jobId));
        packs.setLiveJobs({});
        packs.sweep(QDateTime::currentDateTimeUtc().addDays(2));
        QVERIFY(packs.find(jobId)); // in flight again
        const QByteArray bytes = sendAll(sender, link, prepared->snapshotId);
        QCOMPARE(sha256(bytes), first.packSha256);
        sender.receive(protocol::SnapshotReady{prepared->snapshotId});
        QCOMPARE(events.ready, QStringList{jobId});
        QVERIFY(!packs.find(jobId));
    }

    // A kept pack whose bytes changed with its size is never offered with its recorded SHA-256.
    void aKeptPackWithOtherBytesIsBuiltAgain() {
        const QString jobId = QStringLiteral("job-changed");
        const auto prepared = prepareJob(jobId);
        QVERIFY(prepared);
        QVERIFY(!offerAndStop(jobId, *prepared).packSha256.isEmpty());
        PackCache packs(root_ + QStringLiteral("/packs"));
        const auto kept = packs.find(jobId);
        QVERIFY(kept);
        write(kept->path, QByteArray(kept->size, 'x'));
        StoreRetention retention(prepared->storePath, packs);
        QVERIFY(retention.load());
        SnapshotWorker worker(prepared->storePath, packs);
        JobSyncWork jobWork(worker, jobId);
        CountingWork work(jobWork);
        FakeLink link;
        FakeEvents events;
        SnapshotSender sender(SyncJob{jobId, runnerId, key, prepared->snapshotId, {}}, link, work,
                              retention, events);
        sender.start({});
        QTRY_COMPARE(link.of<protocol::SnapshotOffer>().size(), 1);
        QCOMPARE(work.hashes, 1);
        QCOMPARE(work.builds, 1);
        const auto offer = link.of<protocol::SnapshotOffer>().first();
        QVERIFY(offer.packSha256 != sha256(QByteArray(kept->size, 'x')));
        const QByteArray bytes = sendAll(sender, link, prepared->snapshotId);
        QCOMPARE(sha256(bytes), offer.packSha256);
        QCOMPARE(packs.find(jobId)->sha256, offer.packSha256);
    }

    void cancellingWhileHashingLeavesNoRefOrFile() {
        const QString jobId = QStringLiteral("job-hash");
        const auto prepared = prepareJob(jobId);
        QVERIFY(prepared);
        QVERIFY(!offerAndStop(jobId, *prepared).packSha256.isEmpty());
        PackCache packs(root_ + QStringLiteral("/packs"));
        StoreRetention retention(prepared->storePath, packs);
        QVERIFY(retention.load());
        SnapshotWorker worker(prepared->storePath, packs);
        JobSyncWork jobWork(worker, jobId);
        CountingWork work(jobWork);
        FakeLink link;
        FakeEvents events;
        SnapshotSender sender(SyncJob{jobId, runnerId, key, prepared->snapshotId, {}}, link, work,
                              retention, events);
        sender.start({});
        QCOMPARE(work.hashes, 1);
        sender.cancel();
        drain(worker);
        QVERIFY(link.frames.isEmpty());
        QCOMPARE(work.builds, 0);
        QVERIFY(events.ready.isEmpty() && events.failed.isEmpty());
        QVERIFY(packFiles().isEmpty());
        QVERIFY(snapshot::listRefs(prepared->storePath).value().isEmpty());
    }

    void aKeptPackWhoseBaseIsGoneIsBuiltAgain() {
        const QString jobId = QStringLiteral("job-stale");
        const auto prepared = prepareJob(jobId);
        QVERIFY(prepared);
        PackCache packs(root_ + QStringLiteral("/packs"));
        const QString base = QString(40, QLatin1Char('e'));
        const QString path = packs.pathFor(jobId, prepared->snapshotId, base);
        write(path, "stale");
        QVERIFY(packs.record(
            jobId, PackFile{path, 5, QString(64, QLatin1Char('a')), prepared->snapshotId, base}));
        StoreRetention retention(prepared->storePath, packs);
        QVERIFY(retention.load());
        SnapshotWorker worker(prepared->storePath, packs);
        JobSyncWork jobWork(worker, jobId);
        CountingWork work(jobWork);
        FakeLink link;
        FakeEvents events;
        SnapshotSender sender(SyncJob{jobId, runnerId, key, prepared->snapshotId, {}}, link, work,
                              retention, events);
        sender.start({}); // the runner no longer lists that base
        QCOMPARE(work.builds, 1);
        QTRY_COMPARE(link.of<protocol::SnapshotOffer>().size(), 1);
        QVERIFY(!link.of<protocol::SnapshotOffer>().first().baseId);
    }

    void aPrunedStoreNeverBreaksAConfirmedBase() {
        gitIn(home_, repo_, {QStringLiteral("add"), QStringLiteral("-A")});
        gitIn(home_, repo_,
              {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("one")});
        gitIn(home_, repo_, {QStringLiteral("gc"), QStringLiteral("-q")});
        const auto base = prepareJob(QStringLiteral("job-base"));
        QVERIFY(base);
        const QString store = base->storePath;
        PackCache packs(root_ + QStringLiteral("/packs"));
        StoreRetention retention(store, packs);
        QVERIFY(retention.load());
        QVERIFY(retention.confirm(runnerId, key, base->snapshotId));
        retention.release(QStringLiteral("job-base"));

        const int before = looseObjects(store);
        QVERIFY(snapshot::pruneStore(store, QDateTime::currentDateTimeUtc().addDays(1)));
        const int after = looseObjects(store);
        qInfo() << "store loose objects before prune" << before << "after" << after;

        // (a) An incremental pack on the base still builds.
        write(repo_ + QStringLiteral("/src/main.c"), "int main() { return 1; }\n");
        const auto next = prepareJob(QStringLiteral("job-next"));
        QVERIFY(next);
        const auto incremental = snapshot::writePack(
            {store, next->snapshotId, base->snapshotId, root_ + QStringLiteral("/next.pack")});
        QVERIFY2(incremental || incremental.error().code == snapshot::ErrorCode::BaseMissing,
                 qPrintable(incremental ? QString{} : incremental.error().message));
        qInfo() << "incremental pack after prune:" << (incremental ? "built" : "base missing");

        // (b) The user's repository loses the base's objects.
        gitIn(home_, repo_,
              {QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("--orphan"),
               QStringLiteral("rewritten")});
        write(repo_ + QStringLiteral("/README"), "rewritten\n");
        gitIn(home_, repo_, {QStringLiteral("add"), QStringLiteral("-A")});
        gitIn(home_, repo_,
              {QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("two")});
        const QString old = QString::fromLatin1(
            gitIn(home_, repo_,
                  {QStringLiteral("for-each-ref"), QStringLiteral("--format=%(refname)"),
                   QStringLiteral("refs/heads/")}));
        for (const QString& ref : old.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            if (ref != QStringLiteral("refs/heads/rewritten"))
                gitIn(home_, repo_, {QStringLiteral("update-ref"), QStringLiteral("-d"), ref});
        gitIn(home_, repo_,
              {QStringLiteral("reflog"), QStringLiteral("expire"), QStringLiteral("--expire=now"),
               QStringLiteral("--all")});
        gitIn(home_, repo_,
              {QStringLiteral("gc"), QStringLiteral("-q"), QStringLiteral("--prune=now")});
        const auto later = prepareJob(QStringLiteral("job-later"));
        QVERIFY(later);
        const auto rebuilt = snapshot::writePack(
            {store, later->snapshotId, base->snapshotId, root_ + QStringLiteral("/later.pack")});
        qInfo() << "pack on the base after the user's gc:"
                << (rebuilt
                        ? "built"
                        : (rebuilt.error().code == snapshot::ErrorCode::BaseMissing ? "base missing"
                                                                                    : "failed"));

        // The sender either uses the base or falls back to a full pack; it never fails.
        SnapshotWorker worker(store, packs);
        JobSyncWork work(worker, QStringLiteral("job-later"));
        FakeLink link;
        FakeEvents events;
        SnapshotSender sender(
            SyncJob{QStringLiteral("job-later"), runnerId, key, later->snapshotId, {}}, link, work,
            retention, events);
        sender.start({{key, base->snapshotId}});
        QTRY_COMPARE_WITH_TIMEOUT(link.of<protocol::SnapshotOffer>().size() + events.failed.size(),
                                  1, 30'000);
        QVERIFY2(events.failed.isEmpty(),
                 qPrintable(events.failed.isEmpty() ? QString{} : events.failed.first().second));
        const QByteArray bytes = sendAll(sender, link, later->snapshotId);
        const auto offer = link.of<protocol::SnapshotOffer>().last();
        const QString packPath = root_ + QStringLiteral("/received.pack");
        write(packPath, bytes);
        qInfo() << "offered" << (offer.baseId ? "incremental" : "full") << "pack";
        if (!offer.baseId) {
            const auto applied = snapshot::applySnapshot({root_ + QStringLiteral("/mirror.git"),
                                                          root_ + QStringLiteral("/workspace"),
                                                          packPath,
                                                          sha256(bytes).toLatin1(),
                                                          later->snapshotId,
                                                          {}});
            QVERIFY2(applied, qPrintable(applied ? QString{} : applied.error().message));
            QCOMPARE(readAll(root_ + QStringLiteral("/workspace/README")),
                     QByteArray("rewritten\n"));
        }

        // A snapshot that lost objects cannot be packed at all: a clear code and no pack file.
        QCOMPARE(snapshot::snapshotComplete(store, base->snapshotId).value(), false);
        QCOMPARE(snapshot::snapshotComplete(store, later->snapshotId).value(), true);
        std::optional<PackOutcome> broken;
        JobSyncWork brokenWork(worker, QStringLiteral("job-broken"));
        brokenWork.buildPack(base->snapshotId, {},
                             [&broken](const PackOutcome& outcome) { broken = outcome; });
        QTRY_VERIFY_WITH_TIMEOUT(broken.has_value(), 30'000);
        QVERIFY(!broken->pack);
        QVERIFY(!broken->baseMissing);
        QCOMPARE(broken->code, code::snapshotIncomplete);
        QVERIFY(!packs.find(QStringLiteral("job-broken")));
    }

    void readsChunksOnlyOfTheRecordedSize() {
        const QString path = root_ + QStringLiteral("/p.pack");
        write(path, "0123456789");
        const PackFile pack{path, 10, QString(64, QLatin1Char('a')), snapshotA, {}};
        QCOMPARE(readPackChunk(pack, 2, 3).bytes, QByteArray("234"));
        QCOMPARE(readPackChunk(pack, 2, 3).sha256, sha256("234"));
        QVERIFY(readPackChunk(pack, 8, 3).bytes.isEmpty());
        QVERIFY(readPackChunk(pack, -1, 3).bytes.isEmpty());
        write(path, "01234");
        QVERIFY(readPackChunk(pack, 0, 3).bytes.isEmpty());
    }

  private:
    // Offers the job's pack, sends part of it and stops, as a daemon that quits mid-transfer:
    // the pack and its side file stay in the cache.
    protocol::SnapshotOffer offerAndStop(const QString& jobId, const Prepared& prepared) {
        PackCache packs(root_ + QStringLiteral("/packs"));
        StoreRetention retention(prepared.storePath, packs);
        if (!retention.load())
            return {};
        SnapshotWorker worker(prepared.storePath, packs);
        JobSyncWork work(worker, jobId);
        FakeLink link;
        FakeEvents events;
        SnapshotSender sender(SyncJob{jobId, runnerId, key, prepared.snapshotId, {}}, link, work,
                              retention, events);
        sender.start({});
        if (!QTest::qWaitFor([&link] { return !link.of<protocol::SnapshotOffer>().isEmpty(); }))
            return {};
        sender.receive(protocol::SnapshotWant{prepared.snapshotId, QStringLiteral("need"), 0});
        if (!QTest::qWaitFor([&sender] { return sender.sent() > 0; }))
            return {};
        drain(worker);
        return link.of<protocol::SnapshotOffer>().first();
    }

    std::optional<Prepared> prepareJob(const QString& jobId) {
        PrepareRequest request;
        request.cwd = repo_;
        request.storesRoot = root_ + QStringLiteral("/stores");
        request.inFlightRef = StoreRetention::inFlightRef(jobId);
        request.userGitSources = snapshot::UserGitSources{home_, home_ + QStringLiteral("/.c"), {}};
        const auto prepared = prepareSnapshot(request);
        if (!prepared.prepared)
            qWarning() << "prepare failed" << prepared.code;
        return prepared.prepared;
    }

    // Waits until the store queue ran everything queued so far and its answers arrived.
    static void drain(SnapshotWorker& worker) {
        auto done = std::make_shared<std::atomic_bool>(false);
        worker.runOnStore([done] { done->store(true); });
        QTRY_VERIFY(done->load());
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
    }

    // Sends the whole pack of the sender's offer and returns its bytes.
    static QByteArray sendAll(SnapshotSender& sender, FakeLink& link, const QString& snap) {
        const auto offer = link.of<protocol::SnapshotOffer>().last();
        link.bulk.clear();
        sender.receive(protocol::SnapshotWant{snap, QStringLiteral("need"), 0});
        QByteArray bytes;
        if (!QTest::qWaitFor([&] { return sender.sent() == offer.packBytes; }, 10'000))
            return bytes;
        for (const auto& transfer : link.bulk)
            bytes.append(transfer.second);
        sender.receive(protocol::SnapshotAck{snap, offer.packBytes});
        return bytes;
    }

    QStringList packFiles() const {
        return QDir(root_ + QStringLiteral("/packs")).entryList(QDir::Files | QDir::Hidden);
    }

    std::unique_ptr<QTemporaryDir> dir_;
    QString root_;
    QString home_;
    QString repo_;
};

QTEST_GUILESS_MAIN(SnapshotSyncIntegrationTest)
#include "SnapshotSyncIntegrationTest.moc"
