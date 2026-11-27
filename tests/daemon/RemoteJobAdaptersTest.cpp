// The coordinator's clock on the event loop and its runner trust from the desktop's pins.
#include "cloud-privacy/MemoryCredentials.h"
#include "cloud/CredentialStore.h"
#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingIdentity.h"
#include "pairing/PairingPins.h"
#include "runner/RunnerPinStore.h"
#include "runner/jobs/PinRunnerTrust.h"
#include "runner/jobs/QtJobClock.h"

#include <QStandardPaths>
#include <QTest>
#include <QTimeZone>

using namespace ketplus::daemon;
using namespace ketplus::daemon::runner::jobs;
namespace pairing = ketplus::runner::pairing;

namespace {

using Fire = QPair<QString, JobTimer>;

pairing::DesktopRunnerPin pinFor(const QString& runnerId, const QString& name) {
    const auto key = ketplus::control_crypto::generateEd25519();
    return {runnerId,      1,
            key.publicKey, pairing::runnerFingerprint(key.publicKey),
            name,          QDateTime(QDate(2026, 11, 18), QTime(9, 0), QTimeZone::UTC)};
}

} // namespace

class RemoteJobAdaptersTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void init() {
        r03::resetCredentials();
        fired_.clear();
    }

    void aTimerFiresOnceAndIsGone() {
        QtJobClock clock([this](const QString& taskId, const JobTimer timer) {
            fired_.append({taskId, timer});
        });
        clock.arm("task-a", JobTimer::RelayRetry, 10);
        QVERIFY(clock.armed("task-a", JobTimer::RelayRetry));
        QTRY_COMPARE(fired_, QList<Fire>{Fire("task-a", JobTimer::RelayRetry)});
        QVERIFY(!clock.armed("task-a", JobTimer::RelayRetry));
        QCOMPARE(clock.size(), 0);
        QTest::qWait(30);
        QCOMPARE(fired_.size(), 1);
    }

    void armingAgainReplacesTheTime() {
        QtJobClock clock([this](const QString& taskId, const JobTimer timer) {
            fired_.append({taskId, timer});
        });
        clock.arm("task-a", JobTimer::Deadline, 10);
        clock.arm("task-a", JobTimer::Deadline, 60'000);
        QCOMPARE(clock.size(), 1);
        QTest::qWait(50);
        QVERIFY(fired_.isEmpty());
        clock.arm("task-a", JobTimer::Deadline, 0);
        QTRY_COMPARE(fired_, QList<Fire>{Fire("task-a", JobTimer::Deadline)});
    }

    void aDisarmedTimerNeverFires() {
        QtJobClock clock([this](const QString& taskId, const JobTimer timer) {
            fired_.append({taskId, timer});
        });
        clock.arm("task-a", JobTimer::PeerAbsent, 10);
        clock.disarm("task-a", JobTimer::PeerAbsent);
        clock.disarm("task-b", JobTimer::PeerAbsent); // nothing armed: nothing happens
        QCOMPARE(clock.size(), 0);
        QTest::qWait(50);
        QVERIFY(fired_.isEmpty());
    }

    void eachTaskAndKindHasItsOwnTimer() {
        QtJobClock clock;
        clock.setFired([this](const QString& taskId, const JobTimer timer) {
            fired_.append({taskId, timer});
        });
        clock.arm("task-a", JobTimer::RelayRetry, 10);
        clock.arm("task-a", JobTimer::Deadline, 20);
        clock.arm("task-b", JobTimer::RelayRetry, 30);
        QCOMPARE(clock.size(), 3);
        clock.disarm("task-a", JobTimer::Deadline);
        QTRY_COMPARE(fired_.size(), 2);
        QCOMPARE(fired_, (QList<Fire>{Fire("task-a", JobTimer::RelayRetry),
                                      Fire("task-b", JobTimer::RelayRetry)}));
    }

    void aTimerCanBeArmedFromItsOwnExpiry() {
        QtJobClock clock;
        clock.setFired([this, &clock](const QString& taskId, const JobTimer timer) {
            fired_.append({taskId, timer});
            if (fired_.size() == 1)
                clock.arm(taskId, timer, 10);
        });
        clock.arm("task-a", JobTimer::RelayRetry, 10);
        QTRY_COMPARE(fired_.size(), 2);
        QCOMPARE(clock.size(), 0);
    }

    void aDestroyedClockFiresNothing() {
        {
            QtJobClock clock([this](const QString& taskId, const JobTimer timer) {
                fired_.append({taskId, timer});
            });
            clock.arm("task-a", JobTimer::RelayRetry, 10);
        }
        QTest::qWait(50);
        QVERIFY(fired_.isEmpty());
    }

    void nowIsUtc() {
        const QtJobClock clock;
        const QDateTime before = QDateTime::currentDateTimeUtc();
        const QDateTime now = clock.now();
        QCOMPARE(now.timeSpec(), Qt::UTC);
        QVERIFY(now >= before && now <= QDateTime::currentDateTimeUtc());
    }

    void onlyAPinnedRunnerIsTrusted() {
        CredentialStore store;
        runner::RunnerPinStore pins(store);
        const PinRunnerTrust trust(pins);
        QCOMPARE(trust.pinnedName("runner-a"), std::nullopt);
        QVERIFY(pins.store(pinFor("runner-a", "build-a")));
        QCOMPARE(trust.pinnedName("runner-a"), std::optional<QString>("build-a"));
        QCOMPARE(trust.pinnedName("runner-b"), std::nullopt);
        QCOMPARE(trust.pinnedName(QString()), std::nullopt);
        QVERIFY(pins.remove("runner-a"));
        QCOMPARE(trust.pinnedName("runner-a"), std::nullopt);
    }

    void aPinRecordThatCannotBeReadTrustsNothing() {
        CredentialStore store;
        runner::RunnerPinStore pins(store);
        const PinRunnerTrust trust(pins);
        QVERIFY(pins.store(pinFor("runner-a", "build-a")));
        QVERIFY(store.write("runner-pins", "{not json"));
        QCOMPARE(trust.pinnedName("runner-a"), std::nullopt);
        QVERIFY(pins.store(pinFor("runner-a", "build-a")));
        r03::failReadsOf = "runner-pins";
        QCOMPARE(trust.pinnedName("runner-a"), std::nullopt);
    }

  private:
    QList<Fire> fired_;
};

QTEST_GUILESS_MAIN(RemoteJobAdaptersTest)
#include "RemoteJobAdaptersTest.moc"
