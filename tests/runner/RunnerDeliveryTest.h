#pragma once

#include "delivery/DeliveryRecords.h"
#include "delivery/JobDelivery.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <functional>
#include <memory>

// Fakes for every interface of job delivery, and a host whose clock only moves when a test
// moves it.
namespace runner_delivery_test {

using namespace ketplus::runner::delivery;

class FakeRelayCalls final : public RelayCalls {
  public:
    void send(const LeaseCall& call) override {
        calls.append(call);
        journalWritesAtSend.append(journalWrites ? *journalWrites : 0);
    }
    QList<LeaseCall> calls;
    QList<int> journalWritesAtSend; // how many journal writes preceded each call
    const int* journalWrites = nullptr;
};

class FakeSession final : public SessionLink {
  public:
    void open(const HeldLease& lease) override {
        opened.append(lease);
        currentPhase = QStringLiteral("assigned");
    }
    [[nodiscard]] bool withdraw() override {
        log.append(QStringLiteral("withdraw"));
        if (currentPhase != QStringLiteral("assigned") || !withdrawable)
            return false;
        currentPhase.clear();
        return true;
    }
    void leaseStarted(const QString& startedAt) override {
        log.append(QStringLiteral("started %1").arg(startedAt));
    }
    void leaseStartRefused(const QString& code) override {
        log.append(QStringLiteral("start refused %1").arg(code));
        if (onStartRefused)
            onStartRefused(code);
    }
    void leaseLost(const QString& code, const QString& reason) override {
        log.append(QStringLiteral("lost %1 %2").arg(code, reason).trimmed());
        if (onLost)
            onLost(code);
    }
    [[nodiscard]] QString phase() const override { return currentPhase; }

    QList<HeldLease> opened;
    QStringList log;
    QString currentPhase;
    bool withdrawable = true;
    std::function<void(const QString&)> onStartRefused;
    std::function<void(const QString&)> onLost;
};

class FakeJournal final : public DeliveryJournal {
  public:
    [[nodiscard]] bool write(const JournalRecord& record) override {
        if (failSteps.contains(int(record.step)))
            return false;
        ++writes;
        records.append(record);
        current = record;
        return true;
    }
    [[nodiscard]] bool clear() override {
        ++clears;
        current.reset();
        return true;
    }
    [[nodiscard]] QStringList steps() const;

    QList<JournalRecord> records;
    std::optional<JournalRecord> current;
    int writes = 0;
    int clears = 0;
    QSet<int> failSteps; // JournalRecord::Step values whose write fails
};

class FakeTrust final : public RequesterTrust {
  public:
    [[nodiscard]] bool trusted(const QString& machineId) const override {
        return pinned.contains(machineId);
    }
    QSet<QString> pinned{QStringLiteral("desktop-1")};
};

class FakeClock final : public DeliveryClock {
  public:
    void arm(DeliveryTimer timer, qint64 delayMs) override { deadlines[timer] = now + delayMs; }
    void disarm(DeliveryTimer timer) override { deadlines.remove(timer); }
    [[nodiscard]] qint64 remaining(DeliveryTimer timer) const {
        return deadlines.contains(timer) ? deadlines.value(timer) - now : -1;
    }
    qint64 now = 0;
    QMap<DeliveryTimer, qint64> deadlines;
};

struct Host {
    explicit Host(const RestartPolicy* policy = nullptr);

    // Moves the clock, firing every timer that comes due, earliest first.
    void advance(qint64 milliseconds);
    // Answers the last call.
    void answer(const CallResult& result);
    void answerOk(const QJsonObject& body = {}) { answer({CallResult::Kind::Ok, {}, body}); }
    void refuse(const QString& code, const QJsonObject& body = {});
    void lose() { answer({CallResult::Kind::Lost, {}, {}}); }
    // An acquire answered with a lease from desktop-1 for `runsOn`, and nothing else sent yet.
    void hold(const QStringList& runsOn = {QStringLiteral("gpu")});
    // Through `job.start` answered.
    void start();
    [[nodiscard]] const LeaseCall& last() const { return relay.calls.last(); }
    [[nodiscard]] QStringList callKinds() const;
    [[nodiscard]] QString heartbeatState() const;
    [[nodiscard]] QJsonValue currentJob() const;

    FakeRelayCalls relay;
    FakeSession session;
    FakeJournal journal;
    FakeTrust trust;
    NeverRelaunch neverRelaunch;
    FakeClock clock;
    QStringList reports;
    int nextId = 0;
    std::unique_ptr<JobDelivery> delivery;
};

[[nodiscard]] QJsonObject leaseBody(const QString& jobId = QStringLiteral("job-1"),
                                    const QString& leaseId = QStringLiteral("lease-1"),
                                    const QString& machineId = QStringLiteral("desktop-1"),
                                    const QStringList& runsOn = {QStringLiteral("gpu")});
[[nodiscard]] HeldLease heldLease(const QStringList& runsOn = {QStringLiteral("gpu")});
inline const QString startedAt = QStringLiteral("2026-11-18T09:00:00Z");

class RunnerDeliveryAcquireTest : public QObject {
    Q_OBJECT
  private slots:
    void acquiresOnlyWhenIdleAndTakingJobs();
    void jobAvailableAsksForThatJob();
    void acquireIdIsJournaledFirst();
    void lostAnswerAsksAgainWithTheSameId_data();
    void lostAnswerAsksAgainWithTheSameId();
    void refusalsGoIdleWithoutRetrying_data();
    void refusalsGoIdleWithoutRetrying();
    void busyLeaseOfUnknownRequesterIsReleased();
    void busyLeaseThatWasStartedIsFinished();
    void sessionGetsATrustedLease();
    void untrustedRequesterIsReleased();
    void labelsOutsideTheRunnerAreReleased();
    void unreadableLeaseIsAskedAgain();
};

class RunnerDeliveryLeaseTest : public QObject {
    Q_OBJECT
  private slots:
    void renewsEverySixtySeconds();
    void renewalRefusalsReachTheSession_data();
    void renewalRefusalsReachTheSession();
    void renewalFailuresBackOff_data();
    void renewalFailuresBackOff();
    void lostCredentialKeepsTheJobRunning();
    void startAnswersTheSession();
    void startRefusals_data();
    void startRefusals();
    void startWithoutAnswerIsAskedAgain();
    void finishIsSentOnceAndRepeatedUnchanged();
    void closeEndsOnTheseAnswers_data();
    void closeEndsOnTheseAnswers();
    void releaseFromTheSession();
    void nextJobAfterTheClose();
};

class RunnerDeliveryRestartTest : public QObject {
    Q_OBJECT
  private slots:
    void journalStepsComeBeforeTheirEffects();
    void journalHoldsNoJobContent();
    void journalRoundTripsAndRefusesCorruption();
    void journalWriteFailures();
    void restartWhileAcquiring();
    void restartWithALeaseAndNoSpec();
    void restartWithAnUntrustedLease();
    void restartWhileReleasingABusyLease();
    void restartAfterTheSpec_data();
    void restartAfterTheSpec();
    void restartPolicyIsReplaceable();
    void drainReleasesALeaseWithoutSpec();
    void drainLetsAStartedJobFinish();
    void drainWhileAcquiring();
    void heartbeatInEachPhase();
    void storageFailureStopsTakingJobs();
    void effectiveLabelsAddTheSystemLabels();
};

} // namespace runner_delivery_test
