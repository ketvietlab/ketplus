#include "BrowserToolLaunch.h"
#include "providers/acp/AcpAdapter.h"
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <algorithm>
#include <utility>
#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif
namespace {
constexpr qsizetype maximumStderrBytes = 1024 * 1024;
void configureHiddenProcess(QProcess* process) {
#if defined(Q_OS_WIN)
    process->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#else
    Q_UNUSED(process);
#endif
}
} // namespace

namespace ketplus::daemon {
AcpAdapter::AcpAdapter(QString label, QObject* parent)
    : ProviderAdapter(parent), label_(std::move(label)) {
    process_ = new QProcess(this);
    restartTimer_ = new QTimer(this);
    restartTimer_->setSingleShot(true);
    connect(restartTimer_, &QTimer::timeout, this, [this] {
        if (!queued_.isEmpty())
            ensureStarted();
    });
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    configureHiddenProcess(process_);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    process_->setProcessEnvironment(environment);

    connect(process_, &QProcess::started, this, &AcpAdapter::initialize);
    connect(process_, &QProcess::readyReadStandardOutput, this, &AcpAdapter::consumeStdout);
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        stderrBuffer_.append(process_->readAllStandardError());
        if (stderrBuffer_.size() > maximumStderrBytes)
            stderrBuffer_.remove(0, stderrBuffer_.size() - maximumStderrBytes);
    });
    connect(process_, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        starting_ = false;
        setReady(false);
        noteUnexpectedStop();
        const QString reason =
            processFailure(QStringLiteral("Unable to start %1 ACP: %2%3")
                               .arg(label_, process_->errorString(), stderrTail()),
                           std::nullopt);
        failPending(reason);
        emit stopped(reason);
    });
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](const int exitCode, const QProcess::ExitStatus) {
                consumeStdout();
                starting_ = false;
                setReady(false);
                if (stopping_) {
                    stopping_ = false;
                    failPending(QStringLiteral("The %1 ACP process was stopped.").arg(label_));
                    return;
                }
                noteUnexpectedStop();
                if (malformedOutput_) {
                    const QString reason =
                        QStringLiteral("The %1 ACP process emitted a malformed protocol message.")
                            .arg(label_);
                    failPending(reason);
                    // Initialization already publishes its own failure; do not emit it twice.
                    if (!stopping_)
                        emit stopped(reason);
                    return;
                }
                const QString reason =
                    processFailure(QStringLiteral("The %1 ACP process exited unexpectedly.%2")
                                       .arg(label_, stderrTail()),
                                   exitCode);
                failPending(reason);
                emit stopped(reason);
            });
}

AcpAdapter::~AcpAdapter() {
    // Owners destroy adapters at shutdown, after the SessionHost whose callbacks are still
    // pending; calling them here would use the freed host. Drop them instead of failing them.
    pending_.clear();
    queued_.clear();
    stop();
}

void AcpAdapter::stop() {
    stopping_ = true;
    restartTimer_->stop();
    failPending(QStringLiteral("The %1 ACP process was stopped.").arg(label_));
    stopAuxiliaryProcesses();
    if (process_->state() != QProcess::NotRunning) {
        process_->terminate();
        if (!process_->waitForFinished(500)) {
            process_->kill();
            process_->waitForFinished(500);
        }
    }
    starting_ = false;
    setReady(false);
}

void AcpAdapter::ensureStarted() {
    if (starting_ || process_->state() != QProcess::NotRunning)
        return;
    if (!restartNotBefore_.hasExpired()) {
        if (!restartTimer_->isActive())
            restartTimer_->start(std::max<qint64>(1, restartNotBefore_.remainingTime()));
        return;
    }
    if (!launchCleared_) {
        if (checkingLaunch_)
            return;
        checkingLaunch_ = true;
        checkLaunch([this](const QString& error) {
            checkingLaunch_ = false;
            if (!error.isEmpty()) {
                failPending(error);
                return;
            }
            if (queued_.isEmpty())
                return;
            launchCleared_ = true;
            ensureStarted();
        });
        return;
    }
    launchCleared_ = false;
    malformedOutput_ = false;
    stopping_ = false;
    starting_ = true;
    stdoutBuffer_.clear();
    stderrBuffer_.clear();
    configureProcess();
    process_->start();
}

QJsonObject AcpAdapter::clientCapabilities() const {
    return QJsonObject{
        {QStringLiteral("fs"), QJsonObject{{QStringLiteral("readTextFile"), false},
                                           {QStringLiteral("writeTextFile"), false}}},
        {QStringLiteral("terminal"), false},
        {QStringLiteral("session"),
         QJsonObject{{QStringLiteral("configOptions"),
                      QJsonObject{{QStringLiteral("boolean"), QJsonObject{}}}}}}};
}

void AcpAdapter::initialize() {
    narrowing_.reset();
    sendCall(QStringLiteral("initialize"),
             {{QStringLiteral("protocolVersion"), 1},
              {QStringLiteral("clientCapabilities"), clientCapabilities()},
              {QStringLiteral("clientInfo"),
               QJsonObject{{QStringLiteral("name"), QStringLiteral("ketplus-daemon")},
                           {QStringLiteral("title"), QStringLiteral("KetPlus Daemon")},
                           {QStringLiteral("version"), QStringLiteral("0.1.0")}}}},
             [this](const QJsonObject& result, const QString& error) {
                 starting_ = false;
                 narrowing_.acpInitialized(result);
                 const QString startupError = error.isEmpty() ? initializationError(result) : error;
                 if (!startupError.isEmpty()) {
                     const QString reason =
                         processFailure(QStringLiteral("%1 ACP initialization failed: %2%3")
                                            .arg(label_, startupError, stderrTail()),
                                        std::nullopt);
                     noteUnexpectedStop();
                     failPending(reason);
                     emit stopped(reason);
                     stopping_ = true;
                     process_->terminate();
                     return;
                 }
                 setReady(true);
                 const QList<QueuedCall> calls = std::exchange(queued_, {});
                 for (const QueuedCall& call : calls)
                     call.method == QStringLiteral("turn/start")
                         ? startTurn(call.params, call.callback)
                     : call.method == QStringLiteral("turn/interrupt")
                         ? interruptTurn(call.params, call.callback)
                         : sendCall(call.method, call.params, call.callback);
             });
}

void AcpAdapter::failPending(const QString& reason) {
    for (auto* timer : std::exchange(pendingTimers_, {})) {
        timer->stop();
        timer->deleteLater();
    }
    pendingMethods_.clear();
    const auto pending = std::exchange(pending_, {});
    for (const Callback& callback : pending)
        callback({}, reason);
    const auto queued = std::exchange(queued_, {});
    for (const QueuedCall& call : queued)
        call.callback({}, reason);
    // The next process numbers its requests from the start again; an answer to one of them
    // must not take the shape of an unanswered request this process left behind.
    cancelPermissions();
    permissionOptionsByRequest_.clear();
    // Failure and shutdown must signal workers too, before dropping captured injection state.
    for (auto it = browserSessions_.cbegin(); it != browserSessions_.cend(); ++it)
        if (!it.value().isEmpty())
            browser_tools::signalTurnEnd(it.value());
    startingSessions_.clear();
    browserSessions_.clear();
    agentSessions_.clear();
    computerSessions_.clear();
    acpSessionByKetplus_.clear();
    ketplusByAcp_.clear();
    resetProviderState();
    const auto turns = std::exchange(turns_, {});
    for (const TurnState& turn : turns) {
        emit canonicalEvent(turn.ketplusSessionId, QStringLiteral("runtime.error"),
                            {{QStringLiteral("message"), reason}});
        emit canonicalEvent(turn.ketplusSessionId, QStringLiteral("turn.completed"),
                            {{QStringLiteral("status"), QStringLiteral("failed")},
                             {QStringLiteral("thread_id"), turn.acpSessionId}});
    }
}

void AcpAdapter::setReady(const bool ready) { ready_ = ready; }
void AcpAdapter::noteUnexpectedStop() {
    restartDelayMs_ = restartDelayMs_ <= 0 ? 1000 : std::min(restartDelayMs_ * 2, 60 * 1000);
    restartNotBefore_.setRemainingTime(restartDelayMs_);
}

QString AcpAdapter::stderrTail() const {
    constexpr qsizetype maximumTailBytes = 2000;
    const QString text =
        QString::fromUtf8(stderrBuffer_.right(std::min(stderrBuffer_.size(), maximumTailBytes)))
            .trimmed();
    return text.isEmpty() ? QString() : QStringLiteral("\n\n%1").arg(text);
}

} // namespace ketplus::daemon
