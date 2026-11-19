#include "runtime/executor/LinuxJobContainment.h"

#include <QElapsedTimer>
#include <QThread>
#include <QtTest>

#include <algorithm>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

using namespace ketplus::daemon::executor;

namespace {

// A cgroup.events reader that answers `answers` in order and repeats the last one.
struct Events {
    QList<std::optional<QByteArray>> answers;
    int reads = 0;
    std::optional<QByteArray> operator()() {
        const auto answer = answers.at(std::min<qsizetype>(reads, answers.size() - 1));
        ++reads;
        return answer;
    }
};

// The wait status of a real child that ends as `child` makes it.
int statusOf(void (*child)()) {
    const pid_t pid = fork();
    if (pid == 0) {
        child();
        _exit(0);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return status;
}

} // namespace

class LinuxProbeWaitTest final : public QObject {
    Q_OBJECT

  private slots:
    void readsThePopulatedLineOnly() {
        QCOMPARE(linuxPopulatedOf("populated 0\nfrozen 0"), LinuxPopulated::Empty);
        QCOMPARE(linuxPopulatedOf("frozen 1\npopulated 1"), LinuxPopulated::Populated);
        for (const QByteArray& unclear :
             {QByteArray(), QByteArray("frozen 0"), QByteArray("populated 2"),
              QByteArray("populated  0"), QByteArray("populated"), QByteArray("populated 0 "),
              QByteArray("populated 0\npopulated 0"), QByteArray("populated 1\npopulated 0")})
            QCOMPARE(linuxPopulatedOf(unclear), LinuxPopulated::Unknown);
    }

    // (1) A boundary that still reports populated 1 and then 0 is empty.
    void populatedThenEmptyIsEmpty() {
        Events events{{QByteArray("populated 1\nfrozen 0"), QByteArray("populated 0\nfrozen 0")}};
        QVERIFY(linuxAwaitEmpty(std::ref(events), 1000, 5));
        QCOMPARE(events.reads, 2);
    }

    // (2) Still populated at the deadline fails, and the wait ends with the deadline. The poll
    // sleeps at most until the deadline, so the overrun is one more read plus scheduling delay;
    // 100 ms of slack covers a loaded machine.
    void stillPopulatedAtTheDeadlineFails() {
        constexpr int deadline = 200;
        constexpr int slack = 100;
        Events events{{QByteArray("populated 1")}};
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(!linuxAwaitEmpty(std::ref(events), deadline, 5));
        const qint64 took = elapsed.elapsed();
        QVERIFY2(took >= deadline, qPrintable(QString::number(took)));
        QVERIFY2(took <= deadline + slack, qPrintable(QString::number(took)));
        QVERIFY(events.reads > 1);
    }

    // The deadline is fixed when the wait starts: a slow reader does not extend it.
    void slowReadsDoNotExtendTheDeadline() {
        constexpr int deadline = 200;
        Events events{{QByteArray("populated 1")}};
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(!linuxAwaitEmpty(
            [&events] {
                QThread::msleep(60);
                return events();
            },
            deadline, 5));
        // The last sleep ends by the deadline, so only the read after it can cross it.
        QVERIFY2(elapsed.elapsed() <= deadline + 60 + 100,
                 qPrintable(QString::number(elapsed.elapsed())));
    }

    // (3) An unreadable file or an unclear text fails at once, also after populated 1.
    void unreadableOrUnclearFailsAtOnce() {
        Events unreadable{{std::nullopt}};
        QVERIFY(!linuxAwaitEmpty(std::ref(unreadable), 1000, 5));
        QCOMPARE(unreadable.reads, 1);
        Events unclear{{QByteArray("populated x")}};
        QVERIFY(!linuxAwaitEmpty(std::ref(unclear), 1000, 5));
        QCOMPARE(unclear.reads, 1);
        Events lost{{QByteArray("populated 1"), std::nullopt}};
        QVERIFY(!linuxAwaitEmpty(std::ref(lost), 1000, 5));
        QCOMPARE(lost.reads, 2);
    }

    // (4) A launcher that was not placed, exited nonzero or was killed fails without waiting.
    void aFailedLauncherFailsWithoutWaiting() {
        int asked = 0;
        const auto emptied = [&asked] {
            ++asked;
            return true;
        };
        const int clean = statusOf([] { _exit(0); });
        const int nonzero = statusOf([] { _exit(125); });
        const int killed = statusOf([] { raise(SIGKILL); });
        QVERIFY(WIFEXITED(clean) && WEXITSTATUS(clean) == 0);
        QVERIFY(!linuxProbeSucceeded(false, clean, emptied));
        QVERIFY(!linuxProbeSucceeded(true, nonzero, emptied));
        QVERIFY(!linuxProbeSucceeded(true, killed, emptied));
        QCOMPARE(asked, 0);
        QVERIFY(linuxProbeSucceeded(true, clean, emptied));
        QCOMPARE(asked, 1);
        QVERIFY(!linuxProbeSucceeded(true, clean, [] { return false; }));
    }
};

QTEST_GUILESS_MAIN(LinuxProbeWaitTest)
#include "LinuxProbeWaitTest.moc"
