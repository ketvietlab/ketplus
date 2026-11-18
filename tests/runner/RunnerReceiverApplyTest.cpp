#include "RunnerReceiverTest.h"

#include "snapshot/WorkspaceApply.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTest>
#include <QTimeZone>

namespace runner_receiver_test {

using receiver::WorkStep;

namespace {

struct FileState {
    QByteArray content;
    QDateTime modified;
    bool operator==(const FileState&) const = default;
};

QMap<QString, FileState> stateOf(const QString& root) {
    QMap<QString, FileState> files;
    const QDir dir(root);
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        QFile in(file);
        if (!in.open(QIODevice::ReadOnly))
            continue;
        files.insert(dir.relativeFilePath(file),
                     {in.readAll(), QFileInfo(file).lastModified(QTimeZone::UTC)});
    }
    return files;
}

void age(const QString& file) {
    QFile handle(file);
    if (handle.open(QIODevice::ReadWrite))
        handle.setFileTime(QDateTime(QDate(2020, 1, 1), QTime(0, 0), QTimeZone::UTC),
                           QFileDevice::FileModificationTime);
}

// Git in `directory` with a fixed identity; the trimmed standard output, or nothing.
std::optional<QByteArray> git(const QString& directory, const QStringList& arguments,
                              const QByteArray& input = {}, const QString& outputFile = {}) {
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const char* name : {"GIT_AUTHOR_NAME", "GIT_COMMITTER_NAME"})
        environment.insert(QString::fromLatin1(name), QStringLiteral("Fixture"));
    for (const char* name : {"GIT_AUTHOR_EMAIL", "GIT_COMMITTER_EMAIL"})
        environment.insert(QString::fromLatin1(name), QStringLiteral("fixture@example.invalid"));
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory);
    if (!outputFile.isEmpty())
        process.setStandardOutputFile(outputFile);
    process.start(QStringLiteral("git"), arguments);
    process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(60'000) || process.exitCode() != 0)
        return std::nullopt;
    return process.readAllStandardOutput().trimmed();
}

} // namespace

void ReceiverTest::badPacksLeaveTheWorkspace() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const Packed first = pack(snapshot());
    QCOMPARE(deliver(first), QStringLiteral("ready"));
    QVERIFY(writeFile(workspace() + QStringLiteral("/build/out.o"), "object"));
    age(workspace() + QStringLiteral("/a.txt"));
    const auto before = stateOf(workspace());

    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "second"));
    const Packed second = pack(snapshot());

    Packed wrongDigest = second; // chunks match, the offer's digest does not
    wrongDigest.sha256 = QString(64, QLatin1Char('0'));
    QCOMPARE(deliver(wrongDigest), QStringLiteral("pack_invalid"));
    QCOMPARE(stateOf(workspace()), before);

    Packed broken = second; // whole and as offered, but not a pack
    broken.bytes[broken.bytes.size() / 2] = char(broken.bytes[broken.bytes.size() / 2] ^ 0xff);
    broken.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(broken.bytes, QCryptographicHash::Sha256).toHex());
    QCOMPARE(deliver(broken), QStringLiteral("pack_invalid"));
    QCOMPARE(stateOf(workspace()), before);

    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "third"));
    Packed missingCommit = second; // the pack does not hold the snapshot offered
    missingCommit.commit = snapshot();
    QCOMPARE(deliver(missingCommit), QStringLiteral("pack_invalid"));
    QCOMPARE(stateOf(workspace()), before);

    const QMap<QString, QString> held{{key(QStringLiteral("ws")), first.commit}};
    QCOMPARE(m_receiver->heldSnapshots(QStringLiteral("machine-a")), held);
    QVERIFY(!QFileInfo::exists(partialPack()));
}

void ReceiverTest::checkoutTooLargeLeavesTheWorkspace() {
    snapshot::ApplyLimits limits = receiver::ReceiverOptions{}.limits;
    limits.maxCheckoutBytes = 1000;
    m_limits = limits;
    makeReceiver();
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const Packed first = pack(snapshot());
    QCOMPARE(deliver(first), QStringLiteral("ready"));
    age(workspace() + QStringLiteral("/a.txt"));
    const auto before = stateOf(workspace());

    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QByteArray(2000, 'a')));
    QCOMPARE(deliver(pack(snapshot())), QStringLiteral("snapshot_too_large"));
    QCOMPARE(stateOf(workspace()), before);
    const QMap<QString, QString> held{{key(QStringLiteral("ws")), first.commit}};
    QCOMPARE(m_receiver->heldSnapshots(QStringLiteral("machine-a")), held);
}

// A path the snapshot library refuses though Git accepts it: only a forged pack holds one.
void ReceiverTest::unsafePathLeavesTheWorkspace() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const Packed first = pack(snapshot());
    QCOMPARE(deliver(first), QStringLiteral("ready"));
    age(workspace() + QStringLiteral("/a.txt"));
    const auto before = stateOf(workspace());

    const QString forge = path(QStringLiteral("forge"));
    QVERIFY(git(m_root->path(),
                {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("--bare"), forge}));
    const QStringList dir{QStringLiteral("--git-dir=") + forge};
    const auto blob = git(forge,
                          dir + QStringList{QStringLiteral("hash-object"), QStringLiteral("-w"),
                                            QStringLiteral("--stdin")},
                          "escaped\n");
    QVERIFY(blob);
    const QByteArray entry = "100644 a\\b.txt" + QByteArray(1, '\0') + QByteArray::fromHex(*blob);
    const auto tree = git(forge,
                          dir + QStringList{QStringLiteral("hash-object"), QStringLiteral("-t"),
                                            QStringLiteral("tree"), QStringLiteral("-w"),
                                            QStringLiteral("--stdin")},
                          entry);
    QVERIFY(tree);
    const auto commit =
        git(forge, dir + QStringList{QStringLiteral("commit-tree"), QString::fromLatin1(*tree),
                                     QStringLiteral("-m"), QStringLiteral("f")});
    QVERIFY(commit);
    const QString packFile = path(QStringLiteral("forged.pack"));
    QVERIFY(git(forge,
                dir + QStringList{QStringLiteral("pack-objects"), QStringLiteral("--revs"),
                                  QStringLiteral("--stdout"), QStringLiteral("-q")},
                *commit + '\n', packFile));
    Packed forged{QString::fromLatin1(*commit), readFile(packFile), {}};
    forged.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(forged.bytes, QCryptographicHash::Sha256).toHex());
    QVERIFY(!forged.bytes.isEmpty());
    // The library alone names the reason: the path, not the pack.
    const auto alone = snapshot::applySnapshot(
        {path(QStringLiteral("check/mirror.git")), path(QStringLiteral("check/ws")), packFile,
         forged.sha256.toLatin1(), forged.commit, receiver::ReceiverOptions{}.limits});
    QVERIFY(!alone);
    QCOMPARE(alone.error().code, snapshot::ErrorCode::UnsafePath);

    QCOMPARE(deliver(forged), QStringLiteral("pack_invalid"));
    QCOMPARE(stateOf(workspace()), before);
    QVERIFY(!QFileInfo::exists(workspace() + QStringLiteral("/a")));
    const QMap<QString, QString> held{{key(QStringLiteral("ws")), first.commit}};
    QCOMPARE(m_receiver->heldSnapshots(QStringLiteral("machine-a")), held);
}

void ReceiverTest::incrementalApplyKeepsUntrackedFiles() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    QVERIFY(writeFile(m_repo + QStringLiteral("/b.txt"), "beta"));
    QVERIFY(writeFile(m_repo + QStringLiteral("/keep.txt"), "kept"));
    const Packed first = pack(snapshot());
    QCOMPARE(deliver(first), QStringLiteral("ready"));
    QVERIFY(writeFile(workspace() + QStringLiteral("/build/out.o"), "object"));
    age(workspace() + QStringLiteral("/keep.txt"));
    const QDateTime kept =
        QFileInfo(workspace() + QStringLiteral("/keep.txt")).lastModified(QTimeZone::UTC);

    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "changed"));
    QVERIFY(QFile::remove(m_repo + QStringLiteral("/b.txt")));
    const Packed second = pack(snapshot(), first.commit);
    QCOMPARE(deliver(second, first.commit), QStringLiteral("ready"));
    QCOMPARE(readFile(workspace() + QStringLiteral("/a.txt")), QByteArray("changed"));
    QVERIFY(!QFileInfo::exists(workspace() + QStringLiteral("/b.txt")));
    QCOMPARE(readFile(workspace() + QStringLiteral("/build/out.o")), QByteArray("object"));
    QCOMPARE(QFileInfo(workspace() + QStringLiteral("/keep.txt")).lastModified(QTimeZone::UTC),
             kept);
}

void ReceiverTest::cleanRemovesUntrackedFilesOnly() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const Packed first = pack(snapshot());
    QCOMPARE(deliver(first), QStringLiteral("ready"));
    QVERIFY(writeFile(workspace() + QStringLiteral("/build/out.o"), "object"));
    QVERIFY(writeFile(workspace() + QStringLiteral("/nested/.git/HEAD"), "ref: refs/heads/x"));
    QVERIFY(writeFile(path(QStringLiteral("outside/victim.txt")), "outside"));
    QVERIFY(QFile::link(path(QStringLiteral("outside")), workspace() + QStringLiteral("/link")));

    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "changed"));
    const Packed second = pack(snapshot(), first.commit);
    QCOMPARE(deliver(second, first.commit, QStringLiteral("ws"), true), QStringLiteral("ready"));
    QCOMPARE(readFile(workspace() + QStringLiteral("/a.txt")), QByteArray("changed"));
    QVERIFY(!QFileInfo::exists(workspace() + QStringLiteral("/build")));
    QVERIFY(!QFileInfo::exists(workspace() + QStringLiteral("/nested")));
    QVERIFY(!QFileInfo(workspace() + QStringLiteral("/link")).isSymLink());
    QCOMPARE(readFile(path(QStringLiteral("outside/victim.txt"))), QByteArray("outside"));
}

void ReceiverTest::missingBaseAsksForAFullPack() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const QString base = snapshot();
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "changed"));
    const Packed delta = pack(snapshot(), base);
    QCOMPARE(deliver(delta, base), QStringLiteral("base_missing"));
    QVERIFY(m_answers.isEmpty());
    QVERIFY(!QFileInfo::exists(partialPack()));
    const Packed full = pack(delta.commit);
    offer(full); // the same job, now without a base
    const auto want = nextWant();
    QVERIFY(want);
    QCOMPARE(want->state, QStringLiteral("need"));
    QVERIFY(sendPack(full));
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("ready"));
    QCOMPARE(readFile(workspace() + QStringLiteral("/a.txt")), QByteArray("changed"));
}

void ReceiverTest::unindexableIncrementalPackAsksOnce() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "one"));
    const Packed first = pack(snapshot());
    QCOMPARE(deliver(first), QStringLiteral("ready"));
    QVERIFY(writeFile(m_repo + QStringLiteral("/b.txt"), "only in the second"));
    const QString second = snapshot();
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "three"));
    const Packed third = pack(snapshot(), second); // lacks b.txt, which the mirror lacks too

    QCOMPARE(deliver(third, first.commit), QStringLiteral("base_missing"));
    QVERIFY(m_answers.size() == 1); // only the first ready
    offer(third, first.commit);     // the same pack again
    const auto want = nextWant();
    QVERIFY(want);
    QCOMPARE(want->fromOffset, 0);
    QVERIFY(sendPack(third));
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("pack_invalid"));
    QCOMPARE(errorsSent(), QStringList{QStringLiteral("base_missing")});
    QCOMPARE(readFile(workspace() + QStringLiteral("/a.txt")), QByteArray("one"));
}

void ReceiverTest::foreignOwnerIsRefused() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const Packed packed = pack(snapshot());
    QCOMPARE(deliver(packed), QStringLiteral("ready"));
    const auto before = stateOf(workspace());
    const qsizetype sent = m_sent.size();
    for (const QString& name : {QStringLiteral("ws"), QStringLiteral("other")}) {
        QCOMPARE(deliver(packed, std::nullopt, name, false, QStringLiteral("machine-b")),
                 QStringLiteral("checkout_failed"));
    }
    QCOMPARE(m_sent.size(), sent);
    QCOMPARE(stateOf(workspace()), before);
    QVERIFY(!QFileInfo::exists(workspace(QStringLiteral("other"))));
    QVERIFY(m_receiver->heldSnapshots(QStringLiteral("machine-b")).isEmpty());
    QCOMPARE(m_receiver->heldSnapshots(QStringLiteral("machine-a")).size(), 1);
}

void ReceiverTest::cancelWhileWriting() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const Packed packed = pack(snapshot());
    begin(packed.commit);
    offer(packed);
    QVERIFY(nextWant());
    holdAt(WorkStep::Write);
    chunk(packed, 0, packed.bytes.size());
    QVERIFY(held());
    m_receiver->cancel();
    release();
    QTest::qWait(300);
    QVERIFY(m_answers.isEmpty());
    QVERIFY(!lastAck());
    QVERIFY(!QFileInfo::exists(workspace()));
    QVERIFY(m_receiver->heldSnapshots(QStringLiteral("machine-a")).isEmpty());
    QCOMPARE(deliver(packed), QStringLiteral("ready"));
}

void ReceiverTest::cancelWhileApplying() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const Packed first = pack(snapshot());
    QCOMPARE(deliver(first), QStringLiteral("ready"));
    const auto before = stateOf(workspace());
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "second"));
    const Packed second = pack(snapshot());
    holdAt(WorkStep::Apply);
    begin(second.commit);
    offer(second);
    QVERIFY(nextWant());
    chunk(second, 0, second.bytes.size());
    QVERIFY(held());
    m_receiver->cancel();
    release();
    QTest::qWait(300);
    QCOMPARE(m_answers.size(), 1);
    QCOMPARE(stateOf(workspace()), before);
    const QMap<QString, QString> held{{key(QStringLiteral("ws")), first.commit}};
    QCOMPARE(m_receiver->heldSnapshots(QStringLiteral("machine-a")), held);
    makeReceiver();
    QCOMPARE(m_receiver->heldSnapshots(QStringLiteral("machine-a")), held);
}

void ReceiverTest::cancelAfterApplying() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const Packed packed = pack(snapshot());
    holdAt(WorkStep::Applied);
    begin(packed.commit);
    offer(packed);
    QVERIFY(nextWant());
    chunk(packed, 0, packed.bytes.size());
    QVERIFY(held());
    m_receiver->cancel();
    release();
    QTest::qWait(300);
    QVERIFY(m_answers.isEmpty());
    QVERIFY(QFileInfo::exists(workspace() + QStringLiteral("/a.txt"))); // applied, not held
    QVERIFY(m_receiver->heldSnapshots(QStringLiteral("machine-a")).isEmpty());
    makeReceiver();
    QVERIFY(m_receiver->heldSnapshots(QStringLiteral("machine-a")).isEmpty());
    QVERIFY(writeFile(workspace() + QStringLiteral("/stray"), "from the cancelled job"));
    QCOMPARE(deliver(packed), QStringLiteral("ready")); // from the mirror, after removal
    QCOMPARE(sentOf<protocol::SnapshotWant>().constLast().state, QStringLiteral("have"));
    QVERIFY(!QFileInfo::exists(workspace() + QStringLiteral("/stray")));
    QCOMPARE(readFile(workspace() + QStringLiteral("/a.txt")), QByteArray("alpha"));
}

} // namespace runner_receiver_test
