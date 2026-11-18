// ConPTY (CreatePseudoConsole, Windows 10 1809+) backend for PtyProcess.
#include "terminal/PtyBackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ketplus {
namespace {

// Honours SHELL when it names a real executable, then prefers PowerShell and falls back to cmd.
QString findShell() {
    const QString configured = qEnvironmentVariable("SHELL");
    if (!configured.isEmpty() && QFileInfo(configured).isFile()) {
        return QDir::toNativeSeparators(configured);
    }
    for (const QString& candidate :
         {QStringLiteral("pwsh.exe"), QStringLiteral("powershell.exe")}) {
        const QString executable = QStandardPaths::findExecutable(candidate);
        if (!executable.isEmpty()) {
            return QDir::toNativeSeparators(executable);
        }
    }
    const QString comSpec = qEnvironmentVariable("COMSPEC");
    if (!comSpec.isEmpty() && QFileInfo(comSpec).isFile()) {
        return QDir::toNativeSeparators(comSpec);
    }
    const QString cmd = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
    return cmd.isEmpty() ? QString() : QDir::toNativeSeparators(cmd);
}

QString windowsError(const char* operation) {
    return QStringLiteral("%1 (Windows error %2)")
        .arg(QString::fromLatin1(operation))
        .arg(static_cast<qulonglong>(::GetLastError()));
}

COORD consoleSize(const int rows, const int columns) {
    return COORD{static_cast<SHORT>(std::clamp(columns, 1, 32767)),
                 static_cast<SHORT>(std::clamp(rows, 1, 32767))};
}

void closeHandle(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(handle);
    }
    handle = nullptr;
}

// Environment block (sorted, double-NUL terminated) for the child: ours plus terminal hints.
std::vector<wchar_t> environmentBlock() {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("TERM_PROGRAM"),
                       QCoreApplication::applicationName().remove(QLatin1Char(' ')).isEmpty()
                           ? QStringLiteral("KetPlusCM")
                           : QCoreApplication::applicationName().remove(QLatin1Char(' ')));
    environment.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
    QStringList entries;
    for (const QString& key : environment.keys()) {
        entries.append(key + QLatin1Char('=') + environment.value(key));
    }
    std::sort(entries.begin(), entries.end(), [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    std::vector<wchar_t> block;
    for (const QString& entry : entries) {
        const std::wstring wide = entry.toStdWString();
        block.insert(block.end(), wide.begin(), wide.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

class WindowsPtyBackend final : public PtyBackend {
  public:
    WindowsPtyBackend() : exitTimer_(new QTimer(this)) {
        exitTimer_->setInterval(100);
        connect(exitTimer_, &QTimer::timeout, this, &WindowsPtyBackend::checkChild);
    }

    ~WindowsPtyBackend() override { stop(); }

    bool isRunning() const noexcept override { return running_; }

    bool start(const QString& workingDirectory, const int rows, const int columns,
               QString& error) override {
        shellPath_ = findShell();
        if (shellPath_.isEmpty()) {
            error = QStringLiteral("No interactive shell was found.");
            return false;
        }
        const QString directory =
            QDir::cleanPath(workingDirectory.isEmpty() ? QDir::homePath() : workingDirectory);
        if (!QFileInfo(directory).isDir()) {
            error = QStringLiteral("Terminal folder does not exist: %1").arg(directory);
            return false;
        }

        HANDLE inputRead = nullptr;
        HANDLE outputWrite = nullptr;
        if (!::CreatePipe(&inputRead, &inputWrite_, nullptr, 0) ||
            !::CreatePipe(&outputRead_, &outputWrite, nullptr, 0)) {
            error = windowsError("Unable to create terminal pipes");
            closeHandle(inputRead);
            closeHandle(outputWrite);
            releaseHandles();
            return false;
        }
        const HRESULT created =
            ::CreatePseudoConsole(consoleSize(rows, columns), inputRead, outputWrite, 0, &console_);
        closeHandle(inputRead);
        closeHandle(outputWrite);
        if (FAILED(created)) {
            error = QStringLiteral("Unable to create ConPTY (HRESULT 0x%1). Windows 10 1809 or "
                                   "newer is required.")
                        .arg(static_cast<quint32>(created), 8, 16, QLatin1Char('0'));
            releaseHandles();
            return false;
        }

        SIZE_T attributeBytes = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<char> attributeStorage(attributeBytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) {
            error = windowsError("Unable to prepare terminal process");
            releaseHandles();
            return false;
        }
        bool ok = ::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                              console_, sizeof(console_), nullptr, nullptr);
        PROCESS_INFORMATION process{};
        if (ok) {
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.lpAttributeList = attributes;
            QString commandLine = QLatin1Char('"') + shellPath_ + QLatin1Char('"');
            const QString name = QFileInfo(shellPath_).completeBaseName().toLower();
            if (name == QLatin1String("pwsh") || name == QLatin1String("powershell")) {
                commandLine += QStringLiteral(" -NoLogo");
            }
            std::wstring mutableCommand = commandLine.toStdWString();
            mutableCommand.push_back(L'\0');
            std::vector<wchar_t> environment = environmentBlock();
            const std::wstring directoryWide = QDir::toNativeSeparators(directory).toStdWString();
            ok = ::CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                                  EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                                  environment.data(), directoryWide.c_str(), &startup.StartupInfo,
                                  &process);
        }
        if (!ok) {
            error = windowsError("Unable to start the terminal shell");
            ::DeleteProcThreadAttributeList(attributes);
            releaseHandles();
            return false;
        }
        ::DeleteProcThreadAttributeList(attributes);
        closeHandle(process.hThread);
        process_ = process.hProcess;

        // Killing the job on close ends the shell and everything it started.
        job_ = ::CreateJobObjectW(nullptr, nullptr);
        if (job_ != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits,
                                      sizeof(limits));
            ::AssignProcessToJobObject(job_, process_);
        }

        running_ = true;
        const quint64 generation = ++generation_;
        reader_ = std::thread([this, generation] { readLoop(generation); });
        writer_ = std::thread([this] { writeLoop(); });
        exitTimer_->start();
        return true;
    }

    void write(const QByteArray& bytes) override {
        {
            const std::lock_guard lock(writeMutex_);
            writeQueue_.push_back(bytes);
        }
        writeWake_.notify_one();
    }

    void resize(const int rows, const int columns) override {
        if (console_ != nullptr) {
            ::ResizePseudoConsole(console_, consoleSize(rows, columns));
        }
    }

    void stop() override {
        exitTimer_->stop();
        running_ = false;
        ++generation_; // drops output and exit events still queued for this run
        shutdown();
    }

  private:
    // Reader thread: blocks on the output pipe and hands chunks to the owner's thread.
    void readLoop(const quint64 generation) {
        char buffer[16 * 1024];
        const HANDLE pipe = outputRead_;
        for (;;) {
            DWORD count = 0;
            if (!::ReadFile(pipe, buffer, sizeof(buffer), &count, nullptr) || count == 0) {
                return;
            }
            QByteArray chunk(buffer, static_cast<qsizetype>(count));
            QMetaObject::invokeMethod(
                this,
                [this, generation, chunk = std::move(chunk)] {
                    if (generation == generation_) {
                        emit outputReceived(chunk);
                    }
                },
                Qt::QueuedConnection);
        }
    }

    // Writer thread: keeps a stalled shell from blocking the UI on a full input pipe.
    void writeLoop() {
        const HANDLE pipe = inputWrite_;
        for (;;) {
            QByteArray bytes;
            {
                std::unique_lock lock(writeMutex_);
                writeWake_.wait(lock, [this] { return writerStop_ || !writeQueue_.empty(); });
                if (writerStop_) {
                    return;
                }
                bytes = std::move(writeQueue_.front());
                writeQueue_.pop_front();
            }
            qsizetype offset = 0;
            while (offset < bytes.size()) {
                DWORD written = 0;
                if (!::WriteFile(pipe, bytes.constData() + offset,
                                 static_cast<DWORD>(bytes.size() - offset), &written, nullptr) ||
                    written == 0) {
                    return;
                }
                offset += static_cast<qsizetype>(written);
            }
        }
    }

    void checkChild() {
        if (!running_ || process_ == nullptr ||
            ::WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
            return;
        }
        DWORD code = 0;
        ::GetExitCodeProcess(process_, &code);
        exitTimer_->stop();
        running_ = false;
        // Closing the console flushes what the shell printed last; the reader then ends, so every
        // output chunk is queued ahead of the exit event below.
        shutdown();
        const quint64 generation = generation_;
        const int exitCode = static_cast<int>(code);
        QMetaObject::invokeMethod(
            this,
            [this, generation, exitCode] {
                if (generation == generation_) {
                    emit exited(exitCode);
                }
            },
            Qt::QueuedConnection);
    }

    // Ends the shell tree, closes the pseudo console and joins both I/O threads.
    void shutdown() {
        closeHandle(job_);
        if (process_ != nullptr && ::WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
            ::TerminateProcess(process_, 1);
        }
        {
            const std::lock_guard lock(writeMutex_);
            writerStop_ = true;
            writeQueue_.clear();
        }
        writeWake_.notify_all();
        if (writer_.joinable()) {
            // A write blocked on a full pipe is released once the console closes below.
            closeConsole();
            writer_.join();
        }
        closeConsole();
        if (reader_.joinable()) {
            reader_.join();
        }
        releaseHandles();
        writerStop_ = false;
    }

    void closeConsole() {
        if (console_ != nullptr) {
            ::ClosePseudoConsole(console_);
            console_ = nullptr;
        }
    }

    void releaseHandles() {
        closeConsole();
        closeHandle(inputWrite_);
        closeHandle(outputRead_);
        closeHandle(process_);
    }

    HPCON console_{nullptr};
    HANDLE inputWrite_{nullptr};
    HANDLE outputRead_{nullptr};
    HANDLE process_{nullptr};
    HANDLE job_{nullptr};
    std::atomic<bool> running_{false};
    std::atomic<quint64> generation_{0};
    std::thread reader_;
    std::thread writer_;
    std::mutex writeMutex_;
    std::condition_variable writeWake_;
    std::deque<QByteArray> writeQueue_;
    bool writerStop_{false};
    QTimer* exitTimer_{nullptr};
};

} // namespace

std::unique_ptr<PtyBackend> PtyBackend::create(QObject* parent) {
    auto backend = std::make_unique<WindowsPtyBackend>();
    backend->setParent(parent);
    return backend;
}

} // namespace ketplus
