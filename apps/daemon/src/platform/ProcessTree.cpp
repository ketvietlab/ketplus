#include "platform/ProcessTree.h"

#include <QObject>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace ketplus::daemon {
#if defined(Q_OS_WIN)
namespace {
HANDLE createKillOnCloseJob() {
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr)
        return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                   sizeof(limits))) {
        ::CloseHandle(job);
        return nullptr;
    }
    return job;
}

// Child of the QProcess, so the job handle closes (killing leftovers) with it.
class ProcessJob final : public QObject {
  public:
    explicit ProcessJob(QProcess* process) : QObject(process), process_(process) {
        job_ = createKillOnCloseJob();
        connect(process, &QProcess::started, this, [this] { assign(); });
        connect(process, &QProcess::finished, this, [this] { terminate(); });
    }
    ~ProcessJob() override {
        if (job_ != nullptr)
            ::CloseHandle(job_);
    }
    void terminate() {
        if (job_ != nullptr)
            ::TerminateJobObject(job_, 1);
    }

  private:
    void assign() {
        const qint64 pid = process_->processId();
        if (job_ == nullptr || pid <= 0)
            return;
        HANDLE handle =
            ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (handle == nullptr)
            return;
        // Fails when the process is already in a job that forbids nesting (pre-Windows 8);
        // the daemon-wide job still covers it then.
        ::AssignProcessToJobObject(job_, handle);
        ::CloseHandle(handle);
    }
    QProcess* process_;
    HANDLE job_{nullptr};
};
} // namespace

void attachProcessTree(QProcess* process) {
    if (process != nullptr &&
        process->findChild<ProcessJob*>(QString(), Qt::FindDirectChildrenOnly) == nullptr)
        new ProcessJob(process);
}

void killProcessTree(QProcess* process) {
    if (process == nullptr)
        return;
    if (auto* job = process->findChild<ProcessJob*>(QString(), Qt::FindDirectChildrenOnly))
        job->terminate();
    process->kill();
}

bool confineDaemonProcessTree(QString* error) {
    // Intentionally never closed: the handle's lifetime is the daemon's.
    static HANDLE job = createKillOnCloseJob();
    if (job == nullptr || !::AssignProcessToJobObject(job, ::GetCurrentProcess())) {
        if (error)
            *error = QStringLiteral("Unable to place the daemon in a kill-on-close job object.");
        return false;
    }
    return true;
}
#else
void attachProcessTree(QProcess*) {}

void killProcessTree(QProcess* process) {
    if (process != nullptr)
        process->kill();
}

bool confineDaemonProcessTree(QString*) { return true; }
#endif
} // namespace ketplus::daemon
