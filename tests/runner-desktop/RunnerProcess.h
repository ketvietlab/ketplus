#pragma once

#include <QByteArray>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <memory>

namespace runner_desktop_test {

// The real ketplus-runner binary in a state and work directory of the test, talking to the
// in-process Relay. One command at a time; its standard output and error are kept in memory.
class RunnerProcess {
  public:
    RunnerProcess(QString program, const QString& root, QUrl relay);
    ~RunnerProcess();
    RunnerProcess(const RunnerProcess&) = delete;
    RunnerProcess& operator=(const RunnerProcess&) = delete;

    // `register -` with the token on standard input; it waits for the desktop's approval.
    bool startRegister(const QByteArray& token, const QStringList& labels);
    // The code `register` printed, or empty when none came within `timeoutMs`.
    QString waitCode(int timeoutMs);
    // `run`: takes and runs jobs until stopped.
    bool startRun();
    // The command ended within `timeoutMs`; its exit code in `code`.
    bool waitExit(int timeoutMs, int* code = nullptr);
    // SIGTERM and a wait; SIGKILL when it does not end in time.
    bool stop(int timeoutMs);
    // SIGKILL, as a machine that loses the process would.
    void kill();
    [[nodiscard]] bool running() const;
    [[nodiscard]] qint64 pid() const;
    // Everything the commands printed so far.
    [[nodiscard]] QByteArray log() const { return m_log; }

    [[nodiscard]] QString stateDirectory() const { return m_state; }
    [[nodiscard]] QString workDirectory() const { return m_work; }

  private:
    bool start(const QStringList& arguments);
    void read();

    QString m_program;
    QString m_state;
    QString m_work;
    QUrl m_relay;
    std::unique_ptr<QProcess> m_process;
    QByteArray m_log;
};

} // namespace runner_desktop_test
