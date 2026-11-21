#include "runner/peer/OutputIntake.h"

#include <QTest>

using namespace ketplus::daemon::runner::peer;

namespace {

class MemorySpool final : public OutputSpool {
  public:
    [[nodiscard]] qint64 size() const override { return size_; }
    bool append(const QByteArray& bytes) override {
        if (failing)
            return false;
        bytes_.append(bytes);
        size_ += bytes.size();
        ++writes;
        return true;
    }
    bool markGap(const qint64 toOffset) override {
        if (failing)
            return false;
        gaps.append({size_, toOffset});
        size_ = toOffset;
        return true;
    }
    QByteArray bytes_;
    qint64 size_ = 0;
    int writes = 0;
    bool failing = false;
    QList<std::pair<qint64, qint64>> gaps;
};

QByteArray filled(const qint64 count, const char c = 'o') { return QByteArray(count, c); }

} // namespace

class PeerOutputTest : public QObject {
    Q_OBJECT

  private slots:
    void writesTheNextPageAndArmsTheAckTimer() {
        MemorySpool spool;
        OutputIntake intake(spool);
        const OutputStep step = intake.page(0, "hello");
        QCOMPARE(step.kind, OutputStep::Kind::Written);
        QVERIFY(step.armAckTimer);
        QVERIFY(!step.ackAt);
        QCOMPARE(spool.bytes_, QByteArray("hello"));
        QCOMPARE(intake.ackDue(), std::optional<qint64>(5));
        QVERIFY(!intake.ackDue());
    }

    void acknowledgesEvery256KiB() {
        MemorySpool spool;
        OutputIntake intake(spool);
        constexpr qint64 page = 32 * 1024;
        qint64 offset = 0;
        for (int i = 0; i < 7; ++i) {
            const OutputStep step = intake.page(offset, filled(page));
            QVERIFY(!step.ackAt);
            offset += page;
        }
        const OutputStep eighth = intake.page(offset, filled(page));
        QCOMPARE(eighth.ackAt, std::optional<qint64>(8 * page));
        QVERIFY(!eighth.armAckTimer);
        QCOMPARE(intake.acknowledged(), 8 * page);
        QVERIFY(!intake.ackDue());
    }

    void ignoresWhatTheSpoolHolds() {
        MemorySpool spool;
        OutputIntake intake(spool);
        QCOMPARE(intake.page(0, "abcdef").kind, OutputStep::Kind::Written);
        QCOMPARE(intake.page(0, "abcdef").kind, OutputStep::Kind::Duplicate);
        QCOMPARE(intake.page(2, "cd").kind, OutputStep::Kind::Duplicate);
        QCOMPARE(intake.page(4, "efgh").kind, OutputStep::Kind::Written);
        QCOMPARE(spool.bytes_, QByteArray("abcdefgh"));
        QCOMPARE(spool.writes, 2);
    }

    void asksOnceToResumeFromTheSpool() {
        MemorySpool spool;
        OutputIntake intake(spool);
        QVERIFY(intake.page(0, "abc").kind == OutputStep::Kind::Written);
        OutputStep step = intake.page(10, "xyz");
        QCOMPARE(step.kind, OutputStep::Kind::Ahead);
        QCOMPARE(step.resumeFrom, std::optional<qint64>(3));
        step = intake.page(13, "more");
        QCOMPARE(step.kind, OutputStep::Kind::Ahead);
        QVERIFY(!step.resumeFrom);
        QCOMPARE(spool.bytes_, QByteArray("abc"));
        QVERIFY(intake.page(3, "def").kind == OutputStep::Kind::Written);
        QCOMPARE(intake.page(20, "x").resumeFrom, std::optional<qint64>(6));
        intake.reconnected();
        QCOMPARE(intake.page(20, "x").resumeFrom, std::optional<qint64>(6));
    }

    void aGapMovesTheSpoolOn() {
        MemorySpool spool;
        OutputIntake intake(spool);
        QVERIFY(intake.page(0, "abc").kind == OutputStep::Kind::Written);
        QCOMPARE(intake.gap(3, 100).kind, OutputStep::Kind::Written);
        QCOMPARE(spool.gaps.size(), 1);
        QCOMPARE(spool.gaps.first(), (std::pair<qint64, qint64>{3, 100}));
        QCOMPARE(intake.gap(3, 100).kind, OutputStep::Kind::Duplicate);
        QCOMPARE(intake.gap(150, 200).kind, OutputStep::Kind::Ahead);
        QCOMPARE(intake.page(100, "after").kind, OutputStep::Kind::Written);
        QCOMPARE(intake.size(), 105);
    }

    void aGapOverlappingTheSpoolKeepsWhatIsThere() {
        MemorySpool spool;
        OutputIntake intake(spool);
        QVERIFY(intake.page(0, "abcdef").kind == OutputStep::Kind::Written);
        QCOMPARE(intake.gap(2, 10).kind, OutputStep::Kind::Written);
        QCOMPARE(spool.gaps.first(), (std::pair<qint64, qint64>{6, 10}));
        QCOMPARE(spool.bytes_, QByteArray("abcdef"));
    }

    void neverAcknowledgesWhatFailedToWrite() {
        MemorySpool spool;
        OutputIntake intake(spool);
        QVERIFY(intake.page(0, "abc").kind == OutputStep::Kind::Written);
        spool.failing = true;
        const OutputStep step = intake.page(3, filled(300 * 1024));
        QCOMPARE(step.kind, OutputStep::Kind::Failed);
        QVERIFY(!step.ackAt);
        QCOMPARE(intake.gap(3, 10).kind, OutputStep::Kind::Failed);
        QCOMPARE(intake.ackDue(), std::optional<qint64>(3));
        spool.failing = false;
        QCOMPARE(intake.page(3, "d").kind, OutputStep::Kind::Written);
    }

    void aResumedConnectionCountsTheSpoolAsAcknowledged() {
        MemorySpool spool;
        OutputIntake intake(spool);
        QVERIFY(intake.page(0, "abc").kind == OutputStep::Kind::Written);
        intake.reconnected();
        QCOMPARE(intake.acknowledged(), 3);
        QVERIFY(!intake.ackDue());
    }
};

QTEST_GUILESS_MAIN(PeerOutputTest)
#include "PeerOutputTest.moc"
