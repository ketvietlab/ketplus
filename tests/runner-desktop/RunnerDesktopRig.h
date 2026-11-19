#pragma once

#include "DesktopEnd.h"
#include "FakeRelay.h"
#include "RunnerProcess.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QString>
#include <QTemporaryDir>

#include <memory>
#include <optional>

namespace runner_desktop_test {

using RemoteJobRecord = ketplus::daemon::runner::jobs::RemoteJobRecord;

// Processes, seen from the test, whose command line holds `marker`.
[[nodiscard]] int processesWith(const QByteArray& marker);
// The job units of this build's executor in this user's session, by cgroup directory name.
[[nodiscard]] QStringList jobUnits();
// A fresh marker for a job's `exec sleep 4000.<digits>`: digits only, so sleep accepts it.
[[nodiscard]] QByteArray sleepMarker();
// TCP connections of `pid` to anything but the loopback address, as "local remote".
[[nodiscard]] QStringList nonLoopbackTcp(qint64 pid);

// One scenario's world: the fake Relay, the real desktop parts and the real runner binary, in
// a directory of the build tree that goes with it.
class Rig {
  public:
    Rig();
    ~Rig();
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    [[nodiscard]] bool ready() const { return m_ready; }

    // The desktop's pairing flow with the runner's `register -`, the desktop approving with
    // `code` (the code it shows when empty). The runner's and the desktop's codes in `shown`.
    struct Pairing {
        QString ticketId;
        QString runnerId;
        QString desktopCode;
        QString runnerCode;
        QString approveCode;   // empty when approval was not reached
        QString approveError;  // error_code of the approval, empty when accepted
        int registerExit = -2; // -2: still running
        QString desktopState;  // the last status: state, failure reason and last error
    };
    Pairing pair(const std::function<QString(const QString&)>& codeFor = {});
    // pair() with the shown code, then `run`, online at the Relay. Empty runner id on failure.
    QString pairAndRun();

    // A git repository below the rig: committed files, then the working tree changes.
    QString repository(const QMap<QString, QByteArray>& committed,
                       const QMap<QString, QByteArray>& working = {});
    // Starts and approves `shell` in `cwd` through the session host; the task id once the remote
    // jobs hold its record, or empty (with the reason in the log) when that did not happen.
    QString submit(const QString& shell, const QString& cwd, qint64 timeoutMs = 60'000,
                   const QStringList& runsOn = defaultRunsOn(), QString* code = nullptr);
    // The record once terminal, or nothing after `timeoutMs`.
    std::optional<RemoteJobRecord> waitTerminal(const QString& taskId, int timeoutMs);
    bool waitOutput(const QString& taskId, const QByteArray& part, int timeoutMs);
    [[nodiscard]] static QStringList defaultRunsOn() {
        return {QStringLiteral("e2e"), QStringLiteral("os:linux")};
    }

    // What every scenario checks at its end: a Relay that saw no departure from the protocol,
    // no host other than the loopback, only host candidates, and no job data in the runner's
    // output. Answers the first problem, or empty.
    [[nodiscard]] QString problems(const QStringList& jobData) const;
    // The Relay's requests as "METHOD path" and its violations, for a failure message.
    [[nodiscard]] QString trail() const;
    // Signaling messages the runner or desktop sent with this type.
    [[nodiscard]] int signalCount(const QString& from, const QString& type) const;

    runner_app_test::FakeRelay relay{[] { return QDateTime::currentDateTimeUtc(); }};
    std::unique_ptr<DesktopEnd> desktop;
    std::unique_ptr<RunnerProcess> runner;
    QString root;
    QString trace; // a directory jobs may write to, passed as $TRACE

  private:
    QTemporaryDir m_directory;
    bool m_ready = false;
    int m_repositories = 0;
};

// Fails the scenario when it took longer than its stated limit.
class Limit {
  public:
    explicit Limit(const qint64 ms) : m_ms(ms) { m_timer.start(); }
    [[nodiscard]] bool within() const { return m_timer.elapsed() <= m_ms; }
    [[nodiscard]] qint64 elapsed() const { return m_timer.elapsed(); }

  private:
    qint64 m_ms;
    QElapsedTimer m_timer;
};

} // namespace runner_desktop_test
