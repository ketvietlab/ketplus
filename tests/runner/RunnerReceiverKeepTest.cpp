#include "RunnerReceiverTest.h"

#include "snapshot/ReceiverStore.h"

#include <QDir>
#include <QFileInfo>
#include <QTest>
#include <QTimeZone>

namespace runner_receiver_test {

namespace {
constexpr qint64 day = qint64(24) * 60 * 60 * 1000;
constexpr qint64 minute = 60 * 1000;
} // namespace

void ReceiverTest::partialPacksExpireAfterADay() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QByteArray(3000, 'a')));
    const Packed packed = pack(snapshot());
    const qint64 half = packed.bytes.size() / 2;
    begin(packed.commit);
    offer(packed);
    QVERIFY(nextWant());
    chunk(packed, 0, half);
    QTRY_COMPARE(lastAck(), std::optional<qint64>(half));
    m_receiver->cancel();

    m_nowMs += day; // exactly a day after the last chunk: kept
    m_receiver->idle();
    begin(packed.commit);
    offer(packed);
    auto want = nextWant();
    QVERIFY(want);
    QCOMPARE(want->fromOffset, half);
    m_receiver->cancel();

    m_nowMs += 1; // a day and a millisecond: gone, and the repository with it
    m_receiver->idle();
    const QString repository =
        m_state->path() + QStringLiteral("/repos/") + key(QStringLiteral("repo"));
    QTRY_VERIFY(!QFileInfo::exists(repository));
    QVERIFY(!QFileInfo::exists(partialPack()));
    begin(packed.commit);
    offer(packed);
    want = nextWant();
    QVERIFY(want);
    QCOMPARE(want->fromOffset, 0);
}

void ReceiverTest::idleEvictsTheLeastRecentlyUsed() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QByteArray(3000, 'a')));
    const Packed packed = pack(snapshot());
    QCOMPARE(deliver(packed, std::nullopt, QStringLiteral("one")), QStringLiteral("ready"));
    m_nowMs += minute;
    QCOMPARE(deliver(packed, std::nullopt, QStringLiteral("two")), QStringLiteral("ready"));
    m_receiver->idle();
    QTRY_VERIFY(m_receiver->usedBytes() > 0);
    const qint64 used = m_receiver->usedBytes();

    makeReceiver(used - 1);
    QCOMPARE(m_receiver->usedBytes(), used);
    m_receiver->idle();
    QTRY_VERIFY(!QFileInfo::exists(workspace(QStringLiteral("one"))));
    QVERIFY(QFileInfo::exists(workspace(QStringLiteral("two"))));
    const QMap<QString, QString> held{{key(QStringLiteral("two")), packed.commit}};
    QCOMPARE(m_receiver->heldSnapshots(QStringLiteral("machine-a")), held);

    makeReceiver(1); // nothing fits: every workspace goes, then the repository
    m_receiver->idle();
    QTRY_VERIFY(!QFileInfo::exists(m_state->path() + QStringLiteral("/repos/") +
                                   key(QStringLiteral("repo"))));
    QVERIFY(!QFileInfo::exists(m_work + QLatin1Char('/') + key(QStringLiteral("repo"))));
    QVERIFY(m_receiver->heldSnapshots(QStringLiteral("machine-a")).isEmpty());
}

void ReceiverTest::offerEvictsOtherWorkspacesOnly() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QByteArray(3000, 'a')));
    const Packed first = pack(snapshot());
    QCOMPARE(deliver(first, std::nullopt, QStringLiteral("one")), QStringLiteral("ready"));
    m_nowMs += minute;
    QCOMPARE(deliver(first, std::nullopt, QStringLiteral("two")), QStringLiteral("ready"));
    m_receiver->idle();
    QTRY_VERIFY(m_receiver->usedBytes() > 0);
    const qint64 used = m_receiver->usedBytes();

    QVERIFY(writeFile(m_repo + QStringLiteral("/b.txt"), QByteArray(5000, 'b')));
    const Packed second = pack(snapshot());
    // One byte short: a workspace must go, and only "two" may, though "one" is older.
    makeReceiver(used + second.bytes.size() - 1);
    QCOMPARE(deliver(second, std::nullopt, QStringLiteral("one")), QStringLiteral("ready"));
    QVERIFY(!QFileInfo::exists(workspace(QStringLiteral("two"))));
    const QMap<QString, QString> held{{key(QStringLiteral("one")), second.commit}};
    QCOMPARE(m_receiver->heldSnapshots(QStringLiteral("machine-a")), held);

    QVERIFY(writeFile(m_repo + QStringLiteral("/c.txt"), "c"));
    const Packed third = pack(snapshot());
    makeReceiver(1); // only the current workspace is left, and it is never evicted
    QCOMPARE(deliver(third, std::nullopt, QStringLiteral("one")), QStringLiteral("disk_full"));
    QCOMPARE(readFile(workspace(QStringLiteral("one")) + QStringLiteral("/b.txt")),
             QByteArray(5000, 'b'));
}

void ReceiverTest::heldListsTheMostRecent256() {
    m_receiver.reset();
    const receiver::ReceiverStore store(*m_state, m_work);
    QString error;
    QVERIFY(store.write(
        receiver::RepositoryRecord{key(QStringLiteral("repo")), QStringLiteral("machine-a"), 0},
        &error));
    const QDateTime start = QDateTime::fromMSecsSinceEpoch(m_nowMs, QTimeZone::UTC);
    const auto add = [&](const QString& name, const QString& owner, const bool complete,
                         const int minutes, const bool directory) {
        receiver::WorkspaceRecord record;
        record.repositoryKey = key(QStringLiteral("repo"));
        record.workspaceKey = key(name);
        record.owner = owner;
        record.snapshotId = QString(40, QLatin1Char('a'));
        record.complete = complete;
        record.lastUsed = start.addSecs(60 * minutes);
        QVERIFY(store.write(record, &error));
        if (directory)
            QVERIFY(QDir().mkpath(workspace(name)));
    };
    for (int i = 0; i < 257; ++i)
        add(QStringLiteral("w%1").arg(i, 3, 10, QLatin1Char('0')), QStringLiteral("machine-a"),
            true, i, true);
    add(QStringLiteral("unfinished"), QStringLiteral("machine-a"), false, 1000, true);
    add(QStringLiteral("nodirectory"), QStringLiteral("machine-a"), true, 1000, false);
    add(QStringLiteral("theirs"), QStringLiteral("machine-b"), true, 1000, true);

    makeReceiver();
    const QMap<QString, QString> held = m_receiver->heldSnapshots(QStringLiteral("machine-a"));
    QCOMPARE(held.size(), 256);
    QVERIFY(!held.contains(key(QStringLiteral("w000"))));
    QVERIFY(held.contains(key(QStringLiteral("w001"))));
    QVERIFY(held.contains(key(QStringLiteral("w256"))));
    QVERIFY(!held.contains(key(QStringLiteral("unfinished"))));
    QVERIFY(!held.contains(key(QStringLiteral("nodirectory"))));
    QVERIFY(!held.contains(key(QStringLiteral("theirs"))));
    QCOMPARE(m_receiver->heldSnapshots(QStringLiteral("machine-b")).size(), 1);
}

} // namespace runner_receiver_test
