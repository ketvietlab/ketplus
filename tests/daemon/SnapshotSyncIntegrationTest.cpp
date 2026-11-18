#include "SnapshotSyncFakes.h"

#include "runner/sync/SnapshotPreparation.h"
#include "runner/sync/SnapshotWorker.h"
#include "runner/sync/StoreRetention.h"

#include "snapshot/SnapshotStore.h"
#include "snapshot/WorkspaceApply.h"

#include <QDir>
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
