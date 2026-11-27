#include "RunnerRunTest.h"

#include "protocol/FrameLimits.h"
#include "snapshot/SealedGit.h"
#include "snapshot/SnapshotStoreReceiver.h"
#include "snapshot/WorktreeSnapshot.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTest>

// A whole job through the run loop with the real snapshot receiver: a git snapshot is offered,
// sent in `bulk` transfers and applied before the command starts in the workspace it made.
namespace runner_run_test {

namespace {

bool writeFile(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(content) == content.size();
}

template <typename T> QList<T> sentOf(const Run& run) {
    QList<T> found;
    for (const protocol::Frame& frame : run.sentFrames())
        if (const T* typed = std::get_if<T>(&frame))
            found.append(*typed);
    return found;
}

} // namespace

void RunnerRunJobTest::wholeJobWithARealSnapshot() {
    const auto version = snapshot::SealedGit().checkVersion();
    if (!version)
        QSKIP(qPrintable(QStringLiteral("system git unusable: ") + version.error().message));

    Run run;
    const QString base = QFileInfo(run.root.path()).canonicalFilePath();
    // The snapshot library reads the caller's git configuration; the developer's must not shape
    // this test.
    QDir().mkpath(base + "/home");
    qputenv("HOME", (base + "/home").toUtf8());
    qputenv("XDG_CONFIG_HOME", (base + "/home/.config").toUtf8());
    qunsetenv("GIT_CONFIG_GLOBAL");
    const QString repo = base + "/repo";
    QDir().mkpath(repo);
    QProcess git;
    git.setWorkingDirectory(repo);
    git.start(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("-q")});
    QVERIFY(git.waitForFinished(60'000) && git.exitCode() == 0);
    QVERIFY(writeFile(repo + "/src/main.c", "int main(void) { return 0; }\n"));
    const auto made = snapshot::createSnapshot({repo, base + "/store"});
    QVERIFY2(made, made ? "" : qPrintable(made.error().message));
    const QString commit = made.value().commit;
    QVERIFY(snapshot::writePack({base + "/store", commit, {}, base + "/pack"}));
    QFile packFile(base + "/pack");
    QVERIFY(packFile.open(QIODevice::ReadOnly));
    const QByteArray pack = packFile.readAll();
    QVERIFY(!pack.isEmpty() && pack.size() <= protocol::limits::chunkBytes);

    QString error;
    const std::unique_ptr<receiver::SnapshotStoreReceiver> store =
        receiver::SnapshotStoreReceiver::create(*run.state, base + "/work", {}, &error);
    QVERIFY2(store, qPrintable(error));
    const ReceiverHost host{
        *store,
        [&store](auto ready, auto failed) { store->answerTo(std::move(ready), std::move(failed)); },
        [&store] { store->idle(); }};
    run.make(&host);
    run.start();
    run.socketReady();
    run.acquire();
    run.connectDesktop();
    protocol::JobSpec spec = makeSpec();
    spec.snapshotId = commit;
    spec.sourceHead = commit;
    run.deliver(spec);
    QCOMPARE(run.loop->session().phase(), session::JobSession::Phase::Syncing);

    protocol::SnapshotOffer offer;
    offer.epoch = 7;
    offer.jobId = spec.jobId;
    offer.snapshotId = commit;
    offer.packBytes = pack.size();
    offer.packSha256 =
        QString::fromLatin1(QCryptographicHash::hash(pack, QCryptographicHash::Sha256).toHex());
    offer.chunkBytes = protocol::limits::chunkBytes;
    run.deliver(offer);
    QTRY_COMPARE_WITH_TIMEOUT(sentOf<protocol::SnapshotWant>(run).size(), 1, 20'000);
    QCOMPARE(sentOf<protocol::SnapshotWant>(run).first().state, QStringLiteral("need"));

    constexpr quint64 transferId = 0x1f;
    protocol::SnapshotChunk chunk;
    chunk.epoch = 7;
    chunk.snapshotId = commit;
    chunk.offset = 0;
    chunk.bytes = pack.size();
    chunk.sha256 = offer.packSha256;
    chunk.transferId = QStringLiteral("000000000000001f");
    run.deliver(chunk);
    for (const auto& packet :
         ketplus::daemon::transport::FrameCodec::encodeTransfer(transferId, pack))
        run.transports.made.last()->events->received(peer::Channel::Bulk, packet.bytes, true);
    run.settle();

    QTRY_COMPARE_WITH_TIMEOUT(run.command.launches.size(), 1, 60'000);
    const QString workspace = run.command.launches.first().workspace;
    QVERIFY(workspace.startsWith(base + "/work/"));
    QFile source(workspace + "/src/main.c");
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), QByteArray("int main(void) { return 0; }\n"));
    QCOMPARE(run.loop->session().phase(), session::JobSession::Phase::Running);

    session::CommandExit exit;
    exit.exitCode = 0;
    exit.cleanupStatus = QStringLiteral("verified");
    run.command.events.finished(exit);
    run.settle();
    run.deliver(protocol::JobResultAck{7, spec.jobId});
    QCOMPARE(run.leaseKinds(), (QStringList{"acquire", "start", "finish succeeded exited"}));
    QCOMPARE(run.loop->delivery().state(), delivery::JobDelivery::State::Idle);
    run.loop.reset(); // before the receiver it points at
}

} // namespace runner_run_test
