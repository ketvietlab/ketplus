#pragma once

#include "Environment.h"
#include "FakeRelay.h"

#include <QObject>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

namespace runner_app_test {

// A fake clock that sleeping advances, captured output, and a Relay on loopback.
class TestHost {
  public:
    TestHost();

    [[nodiscard]] ketplus::runner::app::Environment environment();
    // Runs `ketplus-runner ARGS --state-dir <state>`; the output of earlier runs is kept.
    int run(QStringList arguments);
    [[nodiscard]] QString output() const { return (out + err).join(QLatin1Char('\n')); }
    [[nodiscard]] QString stateDir() const { return directory.path() + QStringLiteral("/state"); }
    // Every byte of every file under the state directory.
    [[nodiscard]] QByteArray stateBytes() const;
    [[nodiscard]] QStringList stateFiles() const;
    // A file holding `content`, outside the state directory.
    [[nodiscard]] QString writeFile(const QString& name, const QByteArray& content) const;
    // A runner registered and paired with "desktop-1"; returns its id.
    QString registerRunner(const QStringList& extra = {});

    QTemporaryDir directory;
    QDateTime clock;
    QList<qint64> sleeps;
    QStringList out;
    QStringList err;
    QByteArray stdinData;
    ketplus::runner::app::Platform platform{QStringLiteral("linux"), QStringLiteral("x86_64")};
    QString hostName = QStringLiteral("build-node-1");
    double randomValue = 0.0;
    bool stop = false;
    int stopAfterSleeps = 200; // a loop that never ends fails instead of hanging
    std::function<void(qint64)> onSleep;
    std::unique_ptr<FakeRelay> relay;
};

class RunnerAppStateTest : public QObject {
    Q_OBJECT
  private slots:
    void createsAPrivateDirectory();
    void refusesADirectoryOthersCanReach();
    void refusesADirectoryOfAnotherUser();
    void refusesASecondProcess();
    void statusWhileAnotherProcessHoldsTheLock();
    void writesAtomically();
    void reportsCorruptFilesWithoutOverwriting_data();
    void reportsCorruptFilesWithoutOverwriting();
    void refusesAFileOthersCanRead();
    void resolvesTheDirectory();
    void checksNamesAndLabels();
    void backoffDoublesWithJitterUpToTheCap();
};

class RunnerAppPairingTest : public QObject {
    Q_OBJECT
  private slots:
    void registerPairsWithTheDesktop();
    void tokenFromStdinAndFile();
    void refusesLocallyBeforeTheRelay_data();
    void refusesLocallyBeforeTheRelay();
    void pairAddsASecondDesktop();
    void pairRefusesATokenForAnotherRunner();
    void desktopKeyMismatchOnRegister_data();
    void desktopKeyMismatchOnRegister();
    void desktopKeyMismatchOnPair();
    void approvalThatDoesNotVerify_data();
    void approvalThatDoesNotVerify();
    void pairingThatEnds_data();
    void pairingThatEnds();
    void lostRegisterAnswer();
    void lostClaimAnswer();
    void serverErrorIsALostAnswer();
    void unreadableClientErrorIsARefusal();
    void unsupportedPlatform();
};

class RunnerAppRunTest : public QObject {
    Q_OBJECT
  private slots:
    void heartbeatEveryThirtySeconds();
    void tokenSignsTheChallengeAndRotatesTheCredential();
    void lostTokenAnswerKeepsTheRunnerWorking();
    void stopBetweenRotationAndStore();
    void tokenRenewsBeforeItExpires();
    void fatalRefusalsStopTheRun_data();
    void fatalRefusalsStopTheRun();
    void retriedRefusalsBackOff_data();
    void retriedRefusalsBackOff();
    void heartbeatInvalidAccessRenewsOnce();
    void disabledRunnerKeepsRunning();
    void runFinishesPendingPairings();
    void unpairRemovesThePinFirst();
    void deregisterRemovesEverything();
    void deregisterLocalOnly();
    void statusShowsNoSecret();
    void secretsNeverLeave();
    void helpListsEveryExitCode();
};

class RunnerAppDeliveryFilesTest : public QObject {
    Q_OBJECT
  private slots:
    void acceptedJobsSurviveARestart();
    void acceptedJobsExpireAfterSevenDays();
    void corruptAcceptedJobsAreReported_data();
    void corruptAcceptedJobsAreReported();
    void failedAcceptedWriteKeepsNothing();
    void journalFileHoldsTheRecordOnly();
    void corruptJournalIsReported();
    void activePinsAreTrusted();
};

class RunnerAppJobCallsTest : public QObject {
    Q_OBJECT
  private slots:
    void acquireReturnsTheLease();
    void acquireRefusals_data();
    void acquireRefusals();
    void leaseCalls();
    void releaseBeforeStart();
    void relayEndedTheJob();
    void lostAnswers_data();
    void lostAnswers();
    void invalidAccessRenewsTheTokenOnce();
    void deliveryCallsMapTheAnswers();
};

} // namespace runner_app_test
