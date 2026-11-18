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
    std::unique_ptr<QTemporaryDir> dir_;
    QString root_;
    QString home_;
    QString repo_;
};

QTEST_GUILESS_MAIN(SnapshotSyncIntegrationTest)
#include "SnapshotSyncIntegrationTest.moc"
