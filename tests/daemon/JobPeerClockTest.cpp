// The peer sessions' clock on Qt timers, with a real event loop.
#include "runner/peer/QtJobPeerClock.h"

#include <QElapsedTimer>
#include <QtTest>

#include <memory>

using ketplus::daemon::runner::peer::PeerTimer;
using ketplus::daemon::runner::peer::QtJobPeerClock;

namespace {

struct Recorder {
    QStringList fired;
    std::function<void(const QString&, PeerTimer)> also;
    QtJobPeerClock::Fired callback() {
        return [this](const QString& jobId, const PeerTimer timer) {
            fired.append(jobId + "/" + QString::number(int(timer)));
            if (also)
                also(jobId, timer);
        };
    }
};

QString key(const QString& jobId, const PeerTimer timer) {
    return jobId + "/" + QString::number(int(timer));
}

} // namespace

class JobPeerClockTest final : public QObject {
    Q_OBJECT

  private slots:
    void nowIsUtc() {
        Recorder recorder;
        QtJobPeerClock clock(recorder.callback());
        const QDateTime now = clock.now();
        QCOMPARE(now.timeSpec(), Qt::UTC);
        QVERIFY(qAbs(now.msecsTo(QDateTime::currentDateTimeUtc())) < 1000);
    }

    void firesOnceAfterTheDelay() {
        Recorder recorder;
        QtJobPeerClock clock(recorder.callback());
        QElapsedTimer elapsed;
        elapsed.start();
        clock.arm("a", PeerTimer::Hello, 50);
        QVERIFY(clock.armed("a", PeerTimer::Hello));
        QTRY_COMPARE_WITH_TIMEOUT(recorder.fired.size(), 1, 2000);
        QVERIFY(elapsed.elapsed() >= 45);
        QCOMPARE(recorder.fired.first(), key("a", PeerTimer::Hello));
        QVERIFY(!clock.armed("a", PeerTimer::Hello));
        QTest::qWait(120);
        QCOMPARE(recorder.fired.size(), 1);
    }

    void neverFiresInsideArm() {
        Recorder recorder;
        QtJobPeerClock clock(recorder.callback());
        clock.arm("a", PeerTimer::Retry, 0);
        clock.arm("b", PeerTimer::Retry, -5);
        QVERIFY(recorder.fired.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(recorder.fired.size(), 2, 1000);
        QVERIFY(recorder.fired.contains(key("a", PeerTimer::Retry)));
        QVERIFY(recorder.fired.contains(key("b", PeerTimer::Retry)));
    }

    void reArmingRestarts() {
        Recorder recorder;
        QtJobPeerClock clock(recorder.callback());
        clock.arm("a", PeerTimer::OutputAck, 300);
        QTest::qWait(200);
        clock.arm("a", PeerTimer::OutputAck, 300);
        QTest::qWait(200); // 400 ms after the first arm, 200 ms after the second
        QVERIFY(recorder.fired.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(recorder.fired.size(), 1, 2000);
        QTest::qWait(100);
        QCOMPARE(recorder.fired.size(), 1);
    }

    void disarmStopsOneTimer() {
        Recorder recorder;
        QtJobPeerClock clock(recorder.callback());
        clock.arm("a", PeerTimer::Hello, 0);
        clock.arm("a", PeerTimer::Retry, 20);
        clock.disarm("a", PeerTimer::Hello);
        clock.disarm("a", PeerTimer::SpoolRetry); // not armed: nothing happens
        clock.disarm("x", PeerTimer::Hello);
        QVERIFY(!clock.armed("a", PeerTimer::Hello));
        QTRY_COMPARE_WITH_TIMEOUT(recorder.fired.size(), 1, 1000);
        QCOMPARE(recorder.fired, QStringList{key("a", PeerTimer::Retry)});
        QTest::qWait(50);
        QCOMPARE(recorder.fired.size(), 1);
    }

    void disarmAllStopsOneJob() {
        Recorder recorder;
        QtJobPeerClock clock(recorder.callback());
        for (const auto timer :
             {PeerTimer::Hello, PeerTimer::OutputAck, PeerTimer::Retry, PeerTimer::SpoolRetry}) {
            clock.arm("a", timer, 10);
            clock.arm("b", timer, 10);
        }
        clock.disarmAll("a");
        clock.disarmAll("none");
        QTRY_COMPARE_WITH_TIMEOUT(recorder.fired.size(), 4, 1000);
        QTest::qWait(50);
        QCOMPARE(recorder.fired.size(), 4);
        for (const QString& fired : recorder.fired)
            QVERIFY(fired.startsWith("b/"));
    }

    void callbacksMayArmAgain() {
        Recorder recorder;
        QtJobPeerClock clock(recorder.callback());
        int rounds = 0;
        recorder.also = [&](const QString& jobId, const PeerTimer timer) {
            QVERIFY(!clock.armed(jobId, timer));
            if (++rounds < 3)
                clock.arm(jobId, timer, 0);
        };
        clock.arm("a", PeerTimer::SpoolRetry, 0);
        QTRY_COMPARE_WITH_TIMEOUT(recorder.fired.size(), 3, 1000);
        QTest::qWait(50);
        QCOMPARE(recorder.fired.size(), 3);
        QVERIFY(!clock.armed("a", PeerTimer::SpoolRetry));
    }

    void aCallbackMayDisarmOtherTimers() {
        Recorder recorder;
        QtJobPeerClock clock(recorder.callback());
        recorder.also = [&](const QString& jobId, PeerTimer) { clock.disarmAll(jobId); };
        clock.arm("a", PeerTimer::Hello, 0);
        clock.arm("a", PeerTimer::Retry, 0);
        QTRY_COMPARE_WITH_TIMEOUT(recorder.fired.size(), 1, 1000);
        QTest::qWait(50);
        QCOMPARE(recorder.fired.size(), 1);
    }

    void nothingFiresAfterDestruction() {
        Recorder recorder;
        auto clock = std::make_unique<QtJobPeerClock>(recorder.callback());
        clock->arm("a", PeerTimer::Hello, 0);
        clock->arm("a", PeerTimer::Retry, 10);
        clock.reset();
        QTest::qWait(60);
        QVERIFY(recorder.fired.isEmpty());
    }
};

QTEST_GUILESS_MAIN(JobPeerClockTest)
#include "JobPeerClockTest.moc"
