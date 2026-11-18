#pragma once

#include "exec/ExecPorts.h"
#include "exec/ExecutorCommandRunner.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

// The command runner over a fake executor and platform policy.
namespace runner_exec_test {

using namespace ketplus::runner;
using namespace ketplus::runner::exec;

// What one fake executor job was asked, and its event callback.
struct JobState {
    QString directory;
    QString taskId;
    QString lease;
    QJsonObject execution;
    qint64 timeoutMs = -1;
    ExecutorJob::Event event;
    QStringList stops;
    int acknowledged = 0;
    bool reconnected = false;
    bool startOk = true;
    bool reconnectOk = true;
    bool alive = true;

    void done(const QJsonObject& result) {
        QJsonObject event = result;
        event.insert(QStringLiteral("event"), QStringLiteral("done"));
        this->event(event);
    }
};

struct SpoolState {
    QList<OutputSpool::Page> pages; // read in order; then empty pages
    QList<qint64> readAt;           // ms since the spool was made, of every read
    QElapsedTimer clock;
};

class FakeExecutor final : public Executor {
  public:
    std::unique_ptr<ExecutorJob> job(const QString& directory, const QString& taskId,
                                     const QString& lease) override;
    std::unique_ptr<OutputSpool> output(const QString& directory, const QString& taskId) override;

    QList<std::shared_ptr<JobState>> jobs;
    QList<std::shared_ptr<SpoolState>> spools;
    bool startOk = true;                // for the next jobs made
    bool reconnectOk = true;            // likewise
    QList<OutputSpool::Page> nextPages; // the next spool's pages
};

class ManualPolicy final : public PlatformPolicy {
  public:
    void probe(std::function<void(bool, const QString&)> done) override;
    void answer(bool available, const QString& code = {});

    int probes = 0;
    std::optional<bool> autoAnswer; // answers at once when set
    QString autoCode;

  private:
    QList<std::function<void(bool, const QString&)>> m_pending;
};

// A runner, its events, and a workspace.
struct Fixture {
    explicit Fixture(const std::function<void(ExecutorCommandRunner::Options&)>& tweak = {});

    session::LaunchRequest request(const QString& jobId = QStringLiteral("job-1")) const;
    // Probes and answers available: the runner is ready.
    void ready();
    std::shared_ptr<JobState> job() const { return executor.jobs.last(); }
    QString executorRoot() const {
        return QFileInfo(temp.path()).canonicalFilePath() + QStringLiteral("/state/executor");
    }

    QTemporaryDir temp;
    QString workspace;
    FakeExecutor executor;
    ManualPolicy policy;
    QStringList events; // "output <n>", "lost <n>", "finished <state> <reason> <cleanup>"
    QByteArray output;
    QList<session::CommandExit> exits;
    QList<Readiness> readiness;
    std::unique_ptr<ExecutorCommandRunner> runner;
};

OutputSpool::Page page(QByteArray bytes, qint64 lost = 0);
QJsonObject result(const char* state, int exitCode, const char* cleanup = "verified",
                   bool started = true);

class RunnerExecRunnerTest : public QObject {
    Q_OBJECT

  private slots:
    void readinessFollowsThePolicy();
    void closedPolicyNeverAdmits();
    void launchFailuresReportOnce();
    void launchWhileBusyIsRefused();
    void launchPassesTheBackstop();
    void outputIsPaced();
    void outputDrainsBeforeTheEnd();
    void lostOutputIsReported();
    void readFailuresAreRetried();
    void oneEndPerCommand();
    void killStopsOnce();
    void killAfterTheEndDoesNothing();
    void killWithoutAnAnswerLeavesItUnverified();
    void unverifiedEndBlocksUntilConfirmed();
    void logsHoldNoCommandData();
};

class RunnerExecCleanupTest : public QObject {
    Q_OBJECT

  private slots:
    void nothingLeftIsCleanAtOnce();
    void leftJobIsStoppedAndRemoved();
    void unverifiedLeftJobIsRetried();
    void leftJobWithoutAnAnswerIsKept();
    void unreachableLeftJobIsKept();
    void unidentifiedDirectoryIsKept();
    void runningCommandIsNotALeftover();
};

class RunnerExecMountsTest : public QObject {
    Q_OBJECT

  private slots:
    void launchCarriesTheMounts();
    void withoutMountsNothingIsAdded();
    void refusedPlanRunsNothing();
    void failedStartRemovesTheTmp();
    void unverifiedEndKeepsTheTmp();
    void leftTmpIsRemovedAtStart();
    void tmpOfAnEarlierAttemptIsEmptied();
};

class RunnerExecLinuxPolicyTest : public QObject {
    Q_OBJECT

  private slots:
    void readyWhenEveryCheckHolds();
    void eachFailedCheckIsUnavailable_data();
    void eachFailedCheckIsUnavailable();
    void recoveryIsReadyAtTheNextProbe();
    void nestedRunnerDirectoriesAreUnavailable_data();
    void nestedRunnerDirectoriesAreUnavailable();
    void separatedRunnerDirectoriesAreReadyAgain();
    void lateAnswerAfterDestruction();
    void closedPolicyStaysClosed();
};

} // namespace runner_exec_test
