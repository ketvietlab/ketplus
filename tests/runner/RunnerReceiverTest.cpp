#include "RunnerReceiverTest.h"

#include "protocol/FrameLimits.h"
#include "snapshot/SealedGit.h"
#include "snapshot/WorktreeSnapshot.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTest>
#include <QTimeZone>

namespace runner_receiver_test {

using protocol::SnapshotWant;
using receiver::SnapshotStoreReceiver;

QString key(const QString& name) { return name.leftJustified(32, QLatin1Char('0')); }

void ReceiverTest::initTestCase() {
    const auto version = snapshot::SealedGit().checkVersion();
    if (!version)
        QSKIP(qPrintable(QStringLiteral("system git unusable: ") + version.error().message));
}

void ReceiverTest::init() {
    m_root = std::make_unique<QTemporaryDir>();
    QVERIFY(m_root->isValid());
    // Canonical, so /var vs /private/var on macOS never makes two spellings of one path.
    m_root = std::make_unique<QTemporaryDir>(QFileInfo(m_root->path()).canonicalFilePath() +
                                             QStringLiteral("/t-XXXXXX"));
    QVERIFY(m_root->isValid());
    // The snapshot library reads the caller's git configuration; the developer's must not
    // shape any test.
    QDir().mkpath(path(QStringLiteral("home")));
    qputenv("HOME", path(QStringLiteral("home")).toUtf8());
    qputenv("XDG_CONFIG_HOME", path(QStringLiteral("home/.config")).toUtf8());
    qunsetenv("GIT_CONFIG_GLOBAL");

    m_repo = path(QStringLiteral("repo"));
    m_store = path(QStringLiteral("store"));
    m_work = path(QStringLiteral("work"));
    QDir().mkpath(m_repo);
    QProcess git;
    git.setWorkingDirectory(m_repo);
    git.start(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("-q")});
    QVERIFY(git.waitForFinished(60'000) && git.exitCode() == 0);

    QString error;
    m_state = app::StateDirectory::open(path(QStringLiteral("state")), &error);
    QVERIFY2(m_state.has_value(), qPrintable(error));
    m_nowMs = QDateTime(QDate(2026, 11, 18), QTime(9, 0), QTimeZone::UTC).toMSecsSinceEpoch();
    m_diskFull = false;
    m_holdAt = -1;
    m_reached.tryAcquire(m_reached.available());
    m_released.tryAcquire(m_released.available());
    m_sent.clear();
    m_wantsSeen = 0;
    m_answers.clear();
    makeReceiver();
}

void ReceiverTest::cleanup() {
    m_holdAt = -1;
    m_released.release(8); // never leave the worker waiting
    m_receiver.reset();
    m_state.reset();
    m_root.reset();
}

QString ReceiverTest::path(const QString& name) const {
    return m_root->path() + QLatin1Char('/') + name;
}

bool ReceiverTest::writeFile(const QString& file, const QByteArray& content) const {
    QDir().mkpath(QFileInfo(file).path());
    QFile out(file);
    return out.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           out.write(content) == content.size();
}

QByteArray ReceiverTest::readFile(const QString& file) const {
    QFile in(file);
    return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
}

QString ReceiverTest::snapshot() {
    const auto made = snapshot::createSnapshot({m_repo, m_store});
    if (!made) {
        qWarning("createSnapshot: %s", qPrintable(made.error().message));
        return {};
    }
    return made.value().commit;
}

Packed ReceiverTest::pack(const QString& commit, const QString& base) {
    const QString out = path(QStringLiteral("pack-%1").arg(++m_transfer));
    const auto written = snapshot::writePack({m_store, commit, base, out});
    if (!written) {
        qWarning("writePack: %s", qPrintable(written.error().message));
        return {};
    }
    const QByteArray bytes = readFile(out);
    return {
        commit, bytes,
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())};
}

void ReceiverTest::makeReceiver(const qint64 budget) {
    m_receiver.reset();
    receiver::ReceiverOptions options;
    options.budgetBytes = budget;
    options.now = [this] { return QDateTime::fromMSecsSinceEpoch(m_nowMs, QTimeZone::UTC); };
    options.writer = [this](const QString& file, const qint64 offset, const QByteArray& data) {
        return m_diskFull ? receiver::WriteStatus::DiskFull
                          : receiver::writeAndSync(file, offset, data);
    };
    options.hold = [this](const receiver::WorkStep step) {
        int expected = int(step);
        if (m_holdAt.compare_exchange_strong(expected, -1)) {
            m_reached.release();
            m_released.acquire();
        }
    };
    QString error;
    m_receiver = SnapshotStoreReceiver::create(*m_state, m_work, options, &error);
    QVERIFY2(m_receiver, qPrintable(error));
    m_receiver->answerTo(
        [this](const QString& workspace, const QStringList& warnings) {
            m_readyWorkspace = workspace;
            m_readyWarnings = warnings;
            m_answers.append(QStringLiteral("ready"));
        },
        [this](const QString& code) { m_answers.append(code); });
}

QString ReceiverTest::workspace(const QString& workspaceName) const {
    return m_work + QLatin1Char('/') + key(QStringLiteral("repo")) + QLatin1Char('/') +
           key(workspaceName);
}

QString ReceiverTest::partialPack(const QString& workspaceName) const {
    return m_state->path() + QStringLiteral("/repos/") + key(QStringLiteral("repo")) +
           QStringLiteral("/partial/") + key(workspaceName) + QStringLiteral(".pack");
}

void ReceiverTest::begin(const QString& snapshotId, const QString& workspaceName, const bool clean,
                         const QString& machine) {
    protocol::JobSpec spec;
    spec.jobId = QStringLiteral("job-1");
    spec.repositoryKey = key(QStringLiteral("repo"));
    spec.workspaceKey = key(workspaceName);
    spec.snapshotId = snapshotId;
    spec.clean = clean;
    m_receiver->begin(spec, machine,
                      [this](const protocol::Frame& frame) { m_sent.append(frame); });
}

void ReceiverTest::offer(const Packed& packed, const std::optional<QString>& base,
                         const QStringList& warnings) {
    protocol::SnapshotOffer offer;
    offer.epoch = 1;
    offer.jobId = QStringLiteral("job-1");
    offer.snapshotId = packed.commit;
    offer.baseId = base;
    offer.packBytes = packed.bytes.size();
    offer.packSha256 = packed.sha256;
    offer.chunkBytes = protocol::limits::chunkBytes;
    offer.warnings = warnings;
    m_receiver->receive(offer);
}

void ReceiverTest::chunk(const Packed& packed, const qint64 offset, const qint64 bytes,
                         const bool payloadFirst, std::optional<QByteArray> payload) {
    const QByteArray data = payload.value_or(packed.bytes.mid(offset, bytes));
    protocol::SnapshotChunk frame;
    frame.epoch = 1;
    frame.snapshotId = packed.commit;
    frame.offset = offset;
    frame.bytes = bytes;
    frame.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(packed.bytes.mid(offset, bytes), QCryptographicHash::Sha256)
            .toHex());
    frame.transferId = QStringLiteral("%1").arg(++m_transfer, 16, 16, QLatin1Char('0'));
    if (payloadFirst)
        m_receiver->receivePayload(frame.transferId, data);
    m_receiver->receive(frame);
    if (!payloadFirst)
        m_receiver->receivePayload(frame.transferId, data);
}

bool ReceiverTest::sendPack(const Packed& packed, const qint64 from, qint64 size) {
    const qint64 total = packed.bytes.size();
    if (size <= 0)
        size = total / 3 + 1;
    const qsizetype answers = m_answers.size();
    for (qint64 offset = from; offset < total; offset += size) {
        const qint64 bytes = qMin(size, total - offset);
        chunk(packed, offset, bytes);
        if (!QTest::qWaitFor(
                [&] { return lastAck() == offset + bytes || m_answers.size() > answers; },
                20'000) ||
            lastAck() != offset + bytes)
            return false;
    }
    return true;
}

QString ReceiverTest::deliver(const Packed& packed, const std::optional<QString>& base,
                              const QString& workspaceName, const bool clean,
                              const QString& machine) {
    const qsizetype answers = m_answers.size();
    const qsizetype errors = errorsSent().size();
    begin(packed.commit, workspaceName, clean, machine);
    offer(packed, base);
    (void)QTest::qWaitFor([&] {
        return sentOf<SnapshotWant>().size() > m_wantsSeen || m_answers.size() > answers ||
               errorsSent().size() > errors;
    });
    if (errorsSent().size() > errors)
        return errorsSent().constLast();
    if (m_answers.size() > answers)
        return m_answers.constLast();
    const std::optional<SnapshotWant> want = nextWant();
    if (want && want->state == QStringLiteral("need"))
        sendPack(packed, want->fromOffset);
    if (!QTest::qWaitFor([&] { return m_answers.size() > answers || errorsSent().size() > errors; },
                         60'000))
        return QStringLiteral("no answer");
    return m_answers.size() > answers ? m_answers.constLast() : errorsSent().constLast();
}

std::optional<SnapshotWant> ReceiverTest::nextWant() {
    if (!QTest::qWaitFor([this] { return sentOf<SnapshotWant>().size() > m_wantsSeen; }, 20'000))
        return std::nullopt;
    return sentOf<SnapshotWant>().at(m_wantsSeen++);
}

std::optional<qint64> ReceiverTest::lastAck() const {
    const QList<protocol::SnapshotAck> acks = sentOf<protocol::SnapshotAck>();
    return acks.isEmpty() ? std::nullopt : std::optional(acks.constLast().offset);
}

QStringList ReceiverTest::errorsSent() const {
    QStringList codes;
    for (const protocol::SnapshotError& error : sentOf<protocol::SnapshotError>())
        codes.append(error.code);
    return codes;
}

bool ReceiverTest::waitAnswer() {
    const qsizetype answers = m_answers.size();
    return QTest::qWaitFor([&] { return m_answers.size() > answers; }, 60'000);
}

QString ReceiverTest::lastAnswer() const {
    return m_answers.isEmpty() ? QString() : m_answers.constLast();
}

void ReceiverTest::holdAt(const receiver::WorkStep step) { m_holdAt = int(step); }

bool ReceiverTest::held() {
    return QTest::qWaitFor([this] { return m_reached.tryAcquire(); }, 20'000);
}

void ReceiverTest::release() { m_released.release(); }

// --- layout, keys, want ------------------------------------------------------------------

void ReceiverTest::nestedDirectoriesAreRefused() {
    QString error;
    receiver::ReceiverOptions options;
    QVERIFY(!SnapshotStoreReceiver::create(*m_state, m_state->path() + QStringLiteral("/work"),
                                           options, &error));
    QVERIFY(!SnapshotStoreReceiver::create(*m_state, m_root->path(), options, &error));
    QVERIFY(!SnapshotStoreReceiver::create(*m_state, m_state->path(), options, &error));
    QVERIFY(!SnapshotStoreReceiver::create(*m_state, QStringLiteral("work"), options, &error));
}

void ReceiverTest::unsafeKeysTouchNothing() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const Packed packed = pack(snapshot());
    const QStringList bad{QStringLiteral(".."),
                          QStringLiteral("../") + QString(29, QLatin1Char('a')),
                          QString(16, QLatin1Char('a')) + QLatin1Char('/') +
                              QString(15, QLatin1Char('a')),
                          QString(),
                          QString(33, QLatin1Char('a')),
                          QString(32, QLatin1Char('.'))};
    int failures = 0;
    for (const QString& badKey : bad) {
        for (const bool repository : {true, false}) {
            protocol::JobSpec spec;
            spec.jobId = QStringLiteral("job-1");
            spec.repositoryKey = repository ? badKey : key(QStringLiteral("repo"));
            spec.workspaceKey = repository ? key(QStringLiteral("ws")) : badKey;
            spec.snapshotId = packed.commit;
            m_receiver->begin(spec, QStringLiteral("machine-a"),
                              [this](const protocol::Frame& frame) { m_sent.append(frame); });
            offer(packed);
            ++failures;
            QTRY_COMPARE(m_answers.size(), failures);
            QCOMPARE(m_answers.constLast(), QStringLiteral("checkout_failed"));
        }
    }
    QTest::qWait(100);
    QVERIFY(m_sent.isEmpty());
    QVERIFY(!QFileInfo::exists(m_work));
    QVERIFY(!QFileInfo::exists(m_state->path() + QStringLiteral("/repos")));
    QCOMPARE(QDir(m_root->path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(),
             5); // home, repo, store, state and the pack
}

void ReceiverTest::fullPackIsAppliedAndHeld() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    QVERIFY(writeFile(m_repo + QStringLiteral("/dir/b.txt"), "beta"));
    const Packed packed = pack(snapshot());
    QVERIFY(!packed.bytes.isEmpty());
    begin(packed.commit);
    offer(packed, std::nullopt, {QStringLiteral("submodules_not_sent")});
    const auto want = nextWant();
    QVERIFY(want);
    QCOMPARE(*want, (SnapshotWant{packed.commit, QStringLiteral("need"), 0}));
    QVERIFY(sendPack(packed));
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("ready"));
    QCOMPARE(m_readyWorkspace, workspace());
    QCOMPARE(m_readyWarnings, QStringList{QStringLiteral("submodules_not_sent")});
    QCOMPARE(readFile(workspace() + QStringLiteral("/a.txt")), QByteArray("alpha"));
    QCOMPARE(readFile(workspace() + QStringLiteral("/dir/b.txt")), QByteArray("beta"));
    QDirIterator it(workspace(), QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString name = QFileInfo(it.next()).fileName();
        QVERIFY2(name != QStringLiteral(".git") && name != QStringLiteral("index"),
                 qPrintable(name));
    }
    QVERIFY(!QFileInfo::exists(partialPack()));
    const QMap<QString, QString> held{{key(QStringLiteral("ws")), packed.commit}};
    QCOMPARE(m_receiver->heldSnapshots(QStringLiteral("machine-a")), held);
    QVERIFY(m_receiver->heldSnapshots(QStringLiteral("machine-b")).isEmpty());
    makeReceiver();
    QCOMPARE(m_receiver->heldSnapshots(QStringLiteral("machine-a")), held);
    QDir(workspace()).removeRecursively();
    QVERIFY(m_receiver->heldSnapshots(QStringLiteral("machine-a")).isEmpty());
}

void ReceiverTest::haveRestoresTheSnapshotFromTheMirror() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const Packed packed = pack(snapshot());
    QCOMPARE(deliver(packed), QStringLiteral("ready"));
    QVERIFY(writeFile(workspace() + QStringLiteral("/a.txt"), "changed by the last job"));
    begin(packed.commit);
    offer(packed);
    const auto want = nextWant();
    QVERIFY(want);
    QCOMPARE(want->state, QStringLiteral("have"));
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("ready"));
    QCOMPARE(readFile(workspace() + QStringLiteral("/a.txt")), QByteArray("alpha"));
    QVERIFY(sentOf<protocol::SnapshotAck>().size() > 0); // only from the first delivery
}

void ReceiverTest::needResumesAfterARestart() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QByteArray(4096, 'a')));
    QVERIFY(writeFile(m_repo + QStringLiteral("/b.txt"), "beta"));
    const Packed packed = pack(snapshot());
    const qint64 half = packed.bytes.size() / 2;
    begin(packed.commit);
    offer(packed);
    QVERIFY(nextWant());
    chunk(packed, 0, half);
    QTRY_COMPARE(lastAck(), std::optional<qint64>(half));
    makeReceiver();
    begin(packed.commit);
    offer(packed);
    const auto want = nextWant();
    QVERIFY(want);
    QCOMPARE(*want, (SnapshotWant{packed.commit, QStringLiteral("need"), half}));
    QVERIFY(sendPack(packed, half, packed.bytes.size()));
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("ready"));
    QCOMPARE(readFile(workspace() + QStringLiteral("/b.txt")), QByteArray("beta"));
}

void ReceiverTest::sameOfferResumesAndAnotherPackStartsOver() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const Packed first = pack(snapshot());
    const qint64 half = first.bytes.size() / 2;
    begin(first.commit);
    offer(first);
    QVERIFY(nextWant());
    chunk(first, 0, half);
    QTRY_COMPARE(lastAck(), std::optional<qint64>(half));
    offer(first);
    auto want = nextWant();
    QVERIFY(want);
    QCOMPARE(*want, (SnapshotWant{first.commit, QStringLiteral("need"), half}));
    m_receiver->cancel();

    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "another"));
    const Packed second = pack(snapshot());
    begin(second.commit);
    offer(second);
    want = nextWant();
    QVERIFY(want);
    QCOMPARE(*want, (SnapshotWant{second.commit, QStringLiteral("need"), 0}));
    QVERIFY(sendPack(second));
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("ready"));
    QCOMPARE(readFile(workspace() + QStringLiteral("/a.txt")), QByteArray("another"));
}

} // namespace runner_receiver_test

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    runner_receiver_test::ReceiverTest test;
    return QTest::qExec(&test, argc, argv);
}
