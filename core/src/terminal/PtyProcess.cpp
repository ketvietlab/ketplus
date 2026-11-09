#include "terminal/PtyProcess.h"

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(Q_OS_MACOS)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

namespace ketplus {
namespace {

QString findShell() {
    const QString configured = qEnvironmentVariable("SHELL");
    if (!configured.isEmpty() && QFileInfo(configured).isExecutable()) {
        return configured;
    }
    for (const QString& candidate : {QStringLiteral("zsh"), QStringLiteral("bash"),
                                     QStringLiteral("sh")}) {
        const QString executable = QStandardPaths::findExecutable(candidate);
        if (!executable.isEmpty()) {
            return executable;
        }
    }
    return {};
}

#if defined(Q_OS_UNIX)
// Shells that have not exited by the time their terminal closes are reaped off the UI thread,
// and killed if they ignore the hangup for 2 s, so a closed terminal leaves no zombie or stray
// shell behind. The reaper is joined when the application exits, after its shells are gone.
class ShellReaper final {
  public:
    static ShellReaper& instance() {
        static ShellReaper reaper;
        return reaper;
    }

    void add(const pid_t child) {
        const std::lock_guard lock(mutex_);
        pending_.push_back({child, Clock::now() + std::chrono::seconds(2)});
        if (!thread_.joinable())
            thread_ = std::thread([this] { run(); });
        wake_.notify_one();
    }

    ~ShellReaper() {
        {
            const std::lock_guard lock(mutex_);
            exiting_ = true;
        }
        wake_.notify_one();
        if (thread_.joinable())
            thread_.join();
    }

  private:
    using Clock = std::chrono::steady_clock;
    struct Pending final {
        pid_t child;
        Clock::time_point deadline;
    };

    void run() {
        std::unique_lock lock(mutex_);
        while (true) {
            for (auto it = pending_.begin(); it != pending_.end();) {
                if (::waitpid(it->child, nullptr, WNOHANG) != 0) {
                    it = pending_.erase(it);
                } else if (Clock::now() >= it->deadline) {
                    ::kill(it->child, SIGKILL);
                    while (::waitpid(it->child, nullptr, 0) < 0 && errno == EINTR) {
                    }
                    it = pending_.erase(it);
                } else {
                    ++it;
                }
            }
            if (pending_.empty()) {
                if (exiting_)
                    return;
                wake_.wait(lock, [this] { return exiting_ || !pending_.empty(); });
            } else {
                wake_.wait_for(lock, std::chrono::milliseconds(50));
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<Pending> pending_;
    std::thread thread_;
    bool exiting_{false};
};
#endif

QString systemError(const char* operation) {
    return QStringLiteral("%1: %2")
        .arg(QString::fromLatin1(operation), QString::fromLocal8Bit(std::strerror(errno)));
}

} // namespace

PtyProcess::PtyProcess(QObject* parent) : QObject(parent), childTimer_(new QTimer(this)) {
    childTimer_->setInterval(250);
    connect(childTimer_, &QTimer::timeout, this, &PtyProcess::checkChild);
}

PtyProcess::~PtyProcess() { stop(); }

bool PtyProcess::isRunning() const noexcept { return childProcessId_ > 0; }

QString PtyProcess::shellPath() const { return shellPath_; }

bool PtyProcess::start(const QString& workingDirectory, const int rows, const int columns) {
    if (isRunning()) {
        return true;
    }

#if defined(Q_OS_UNIX)
    shellPath_ = findShell();
    if (shellPath_.isEmpty()) {
        emit errorOccurred(QStringLiteral("No interactive shell was found."));
        return false;
    }

    const QString cleanWorkingDirectory =
        QDir::cleanPath(workingDirectory.isEmpty() ? QDir::homePath() : workingDirectory);
    if (!QFileInfo(cleanWorkingDirectory).isDir()) {
        emit errorOccurred(QStringLiteral("Terminal folder does not exist: %1")
                               .arg(cleanWorkingDirectory));
        return false;
    }

    const QByteArray shellBytes = QFile::encodeName(shellPath_);
    const QByteArray shellName = QFileInfo(shellPath_).fileName().toLocal8Bit();
    const QByteArray loginName = QByteArray("-") + shellName;
    const QByteArray directoryBytes = QFile::encodeName(cleanWorkingDirectory);
    QByteArray terminalProgram = QCoreApplication::applicationName().toUtf8();
    terminalProgram.replace(" ", "");
    if (terminalProgram.isEmpty()) {
        terminalProgram = QByteArrayLiteral("KetPlusCM");
    }
    QByteArray terminalProgramVersion = QCoreApplication::applicationVersion().toUtf8();
    if (terminalProgramVersion.isEmpty()) {
        terminalProgramVersion = QByteArrayLiteral("0.1.0");
    }
    struct winsize size {};
    size.ws_row = static_cast<unsigned short>(std::clamp(rows, 1, 65535));
    size.ws_col = static_cast<unsigned short>(std::clamp(columns, 1, 65535));

    int master = -1;
    const pid_t child = forkpty(&master, nullptr, nullptr, &size);
    if (child < 0) {
        emit errorOccurred(systemError("Unable to create terminal PTY"));
        return false;
    }

    if (child == 0) {
        if (::chdir(directoryBytes.constData()) != 0) {
            _exit(126);
        }
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLORTERM", "truecolor", 1);
        ::setenv("TERM_PROGRAM", terminalProgram.constData(), 1);
        ::setenv("TERM_PROGRAM_VERSION", terminalProgramVersion.constData(), 1);
        ::execl(shellBytes.constData(), loginName.constData(), "-i", nullptr);
        _exit(127);
    }

    // Without this, every later shell and child process holds this terminal's master open.
    ::fcntl(master, F_SETFD, FD_CLOEXEC);
    masterFileDescriptor_ = master;
    childProcessId_ = static_cast<qint64>(child);
    const int flags = ::fcntl(masterFileDescriptor_, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(masterFileDescriptor_, F_SETFL, flags | O_NONBLOCK);
    }

    readNotifier_ = new QSocketNotifier(masterFileDescriptor_, QSocketNotifier::Read, this);
    writeNotifier_ = new QSocketNotifier(masterFileDescriptor_, QSocketNotifier::Write, this);
    writeNotifier_->setEnabled(false);
    connect(readNotifier_, &QSocketNotifier::activated, this,
            [this](QSocketDescriptor, QSocketNotifier::Type) { readAvailable(); });
    connect(writeNotifier_, &QSocketNotifier::activated, this,
            [this](QSocketDescriptor, QSocketNotifier::Type) { flushPendingWrite(); });
    childTimer_->start();
    emit started();
    return true;
#else
    Q_UNUSED(workingDirectory)
    Q_UNUSED(rows)
    Q_UNUSED(columns)
    emit errorOccurred(QStringLiteral("Embedded terminals are not available on this platform yet."));
    return false;
#endif
}

void PtyProcess::send(const QByteArray& bytes) {
    if (!isRunning() || bytes.isEmpty()) {
        return;
    }
    pendingWrite_.append(bytes);
    flushPendingWrite();
}

void PtyProcess::resizeTerminal(const int rows, const int columns) {
#if defined(Q_OS_UNIX)
    if (masterFileDescriptor_ < 0) {
        return;
    }
    struct winsize size {};
    size.ws_row = static_cast<unsigned short>(std::clamp(rows, 1, 65535));
    size.ws_col = static_cast<unsigned short>(std::clamp(columns, 1, 65535));
    if (::ioctl(masterFileDescriptor_, TIOCSWINSZ, &size) == 0 && childProcessId_ > 0) {
        ::kill(static_cast<pid_t>(childProcessId_), SIGWINCH);
    }
#else
    Q_UNUSED(rows)
    Q_UNUSED(columns)
#endif
}

void PtyProcess::stop() {
#if defined(Q_OS_UNIX)
    if (childProcessId_ > 0) {
        const auto child = static_cast<pid_t>(childProcessId_);
        ::kill(child, SIGHUP);
        if (::waitpid(child, nullptr, WNOHANG) == 0)
            ShellReaper::instance().add(child);
    }
#endif
    childTimer_->stop();
    childProcessId_ = -1;
    pendingWrite_.clear();
    closeMaster();
}

void PtyProcess::readAvailable() {
#if defined(Q_OS_UNIX)
    QByteArray output;
    char buffer[16 * 1024];
    while (masterFileDescriptor_ >= 0) {
        const ssize_t count = ::read(masterFileDescriptor_, buffer, sizeof(buffer));
        if (count > 0) {
            output.append(buffer, static_cast<qsizetype>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        break;
    }
    if (!output.isEmpty()) {
        emit outputReceived(output);
    }
#endif
}

void PtyProcess::flushPendingWrite() {
#if defined(Q_OS_UNIX)
    while (masterFileDescriptor_ >= 0 && !pendingWrite_.isEmpty()) {
        const ssize_t count =
            ::write(masterFileDescriptor_, pendingWrite_.constData(),
                    static_cast<size_t>(pendingWrite_.size()));
        if (count > 0) {
            pendingWrite_.remove(0, static_cast<qsizetype>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    if (writeNotifier_ != nullptr) {
        writeNotifier_->setEnabled(!pendingWrite_.isEmpty());
    }
#endif
}

void PtyProcess::checkChild() {
#if defined(Q_OS_UNIX)
    if (childProcessId_ <= 0) {
        return;
    }
    int status = 0;
    const pid_t result = ::waitpid(static_cast<pid_t>(childProcessId_), &status, WNOHANG);
    if (result <= 0) {
        return;
    }
    readAvailable();
    childTimer_->stop();
    childProcessId_ = -1;
    closeMaster();
    const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    emit exited(exitCode);
#endif
}

void PtyProcess::closeMaster() {
    if (readNotifier_ != nullptr) {
        readNotifier_->setEnabled(false);
        readNotifier_->deleteLater();
        readNotifier_ = nullptr;
    }
    if (writeNotifier_ != nullptr) {
        writeNotifier_->setEnabled(false);
        writeNotifier_->deleteLater();
        writeNotifier_ = nullptr;
    }
#if defined(Q_OS_UNIX)
    if (masterFileDescriptor_ >= 0) {
        ::close(masterFileDescriptor_);
    }
#endif
    masterFileDescriptor_ = -1;
}

} // namespace ketplus
