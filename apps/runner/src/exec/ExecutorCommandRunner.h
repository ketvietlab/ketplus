#pragma once

#include "exec/ExecPorts.h"
#include "exec/ExecutionPlan.h"
#include "exec/ExecutorDirectories.h"
#include "exec/JobTmpDirectories.h"
#include "exec/Readiness.h"
#include "session/SessionPorts.h"

#include <QElapsedTimer>
#include <QList>
#include <QMap>
#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

// The session's CommandRunner over the job executor: one command at a time, its output read from
// the executor's store, its end reported once; and whether a command may run at all.
namespace ketplus::runner::exec {

class ExecutorCommandRunner final : public QObject, public session::CommandRunner {
  public:
    // The directories a job's mounts hide. The job's own /tmp is made under `work`.
    struct Mounts {
        QString state;
        QString work;
        QString runtime; // the user's runtime directory, or empty
    };
    struct Options {
        QString directory; // the executor's job directories, under the runner's state directory
        // Where the executor applies them: every command runs with them, or not at all.
        std::optional<Mounts> mounts;
        // The runner's own environment, read at each launch.
        std::function<QMap<QString, QString>()> environment;
        qint64 backstopMs = 60'000;          // added to the job's time limit for the executor's
        qint64 killWaitMs = 60'000;          // for the executor's result after kill()
        qint64 pollMs = 100;                 // between reads of the output store
        int pagesPerPoll = 32;               // of at most 8 KiB each
        qint64 unavailableProbeMs = 600'000; // between probes while the platform is unavailable
        qint64 cleanupRetryMs = 300'000;     // between cleanups of directories still left
        qint64 cleanupWaitMs = 60'000;       // for a left job's cleanup to be confirmed
    };
    // The session's answers, and a change of readiness.
    struct Events {
        std::function<void(const QByteArray&)> output;
        std::function<void(qint64)> outputLost;
        std::function<void(const session::CommandExit&)> finished;
        std::function<void(const Readiness&)> readinessChanged;
    };

    ExecutorCommandRunner(Options options, Executor& executor, PlatformPolicy& policy,
                          Events events, QObject* parent = nullptr);
    ~ExecutorCommandRunner() override;

    // Probes the platform; readiness() is Unknown until it answers.
    void probe();
    // At start, before any job: ends what an earlier process left in the job directories, never
    // running a command again. `done` gets whether nothing is left, or the code of what is.
    // Directories still left are retried every `cleanupRetryMs`.
    void cleanUpLeftovers(std::function<void(bool clean, const QString& code)> done);
    [[nodiscard]] Readiness readiness() const { return m_readiness; }

    [[nodiscard]] bool admit() override;
    void launch(const session::LaunchRequest& request) override;
    void kill() override;

  private:
    struct Running {
        ExecutorDirectories::Entry entry;
        std::unique_ptr<ExecutorJob> job;
        std::unique_ptr<OutputSpool> output;
        QElapsedTimer clock;
        qint64 outputBytes = 0;
        int readFailures = 0;
        std::optional<QJsonObject> result; // the executor's final result
        bool killed = false;
        bool reported = false; // commandFinished was called
    };
    struct Leftover {
        ExecutorDirectories::Entry entry;
        std::unique_ptr<ExecutorJob> job;
        std::optional<bool> clean;
    };

    void fail(const session::LaunchRequest& request, const QString& reason);
    [[nodiscard]] PreparedLaunch prepareMounted(const session::LaunchRequest& request,
                                                const QString& taskId);
    void removeTmp(const QString& taskId);
    void sweepTmp(); // job tmp directories of no job left
    void onEvent(const Running* running, const QJsonObject& event);
    void poll();
    bool readOutput(int pages);
    void report(const session::CommandExit& exit);
    void killExpired();
    void settleLater(); // the unsettled command's cleanup was confirmed

    void probed(bool available, const QString& code);
    void scanLeftovers();
    void leftoverEvent(const QString& taskId, const QJsonObject& event);
    void leftoversExpired();
    void finishScanSoon();
    void finishScan();
    void updateReadiness();

    Options m_options;
    Executor& m_executor;
    PlatformPolicy& m_policy;
    Events m_events;
    ExecutorDirectories m_directories;
    std::optional<JobTmpDirectories> m_tmp; // with mounts
    std::unique_ptr<Running> m_running;
    std::unique_ptr<Running> m_unsettled; // reported unverified, waiting for its cleanup
    QTimer m_poll;
    QTimer m_killWait;

    Readiness m_readiness;
    std::optional<bool> m_platformAvailable;
    QString m_platformCode;
    bool m_probing = false;
    QTimer m_probeTimer;

    std::vector<Leftover> m_leftovers;
    QStringList m_unidentified;
    bool m_scanning = false;
    QString m_leftoverCode;     // what the last cleanup left: empty, or the code reported
    QString m_reportedLeftover; // the code reported once for what is left
    std::function<void(bool, const QString&)> m_cleanupDone;
    QTimer m_cleanupWait;
    QTimer m_cleanupRetry;
};

} // namespace ketplus::runner::exec
