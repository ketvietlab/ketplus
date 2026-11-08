#include "providers/CodexAdapter.h"
#include "BrowserAgentInstructions.h"
#include "providers/ProviderFrameValidation.h"
#include "providers/ProviderResponseEnvelope.h"
#include "providers/ProviderResponseId.h"

#include "CodexBrowserTools.h"
#include "CodexLaunchReloader.h"
#include "providers/compat/ProviderVersionProbe.h"
#include "runtime/ResponseGuidance.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

#include <utility>

namespace ketplus::daemon {
namespace {

constexpr qsizetype maximumProviderMessageBytes = 16 * 1024 * 1024;
constexpr qsizetype maximumStderrBytes = 1024 * 1024;

QString errorText(const QJsonObject& error) {
    const QString message = error.value(QStringLiteral("message")).toString();
    return message.isEmpty()
               ? QString::fromUtf8(QJsonDocument(error).toJson(QJsonDocument::Compact))
               : message;
}

} // namespace

CodexAdapter::CodexAdapter(const QString& executable, QObject* parent)
    : ProviderAdapter(parent), executable_(executable.isEmpty() ? findExecutable() : executable),
      process_(new QProcess(this)),
      stdoutReader_(maximumProviderMessageBytes,
                    {QStringLiteral("id"), QStringLiteral("method"),
                     QStringLiteral("params.threadId"), QStringLiteral("params.turnId"),
                     QStringLiteral("params.turn.id")}),
      restartTimer_(new QTimer(this)),
      versionProbe_(
          new compat::ProviderVersionProbe(compat::policy(QStringLiteral("codex")), {}, this)) {
    restartTimer_->setSingleShot(true);
    reloader_ = new browser_tools::CodexLaunchReloader(
        [this] { return browserArguments(); }, [this] { return isIdle(); },
        browser_tools::settingsFilePath(), browser_tools::CodexLaunchReloader::defaultDebounceMs,
        this);
    connect(reloader_, &browser_tools::CodexLaunchReloader::restartRequested, this,
            &CodexAdapter::restartForBrowserSettings);
    connect(restartTimer_, &QTimer::timeout, this, [this] {
        if (!queued_.isEmpty())
            ensureStarted();
    });
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    process_->setProcessEnvironment(environment);

    connect(process_, &QProcess::started, this, &CodexAdapter::initialize);
    connect(process_, &QProcess::readyReadStandardOutput, this, &CodexAdapter::consumeStdout);
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        stderrBuffer_.append(process_->readAllStandardError());
        if (stderrBuffer_.size() > maximumStderrBytes) {
            stderrBuffer_.remove(0, stderrBuffer_.size() - maximumStderrBytes);
        }
    });
    connect(process_, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) {
            return;
        }
        starting_ = false;
        setReady(false);
        activeTurns_.clear();
        reloader_->stopped();
        noteUnexpectedStop();
        const QString reason =
            QStringLiteral("Unable to start Codex app-server: %1").arg(process_->errorString());
        failPending(reason);
        emit stopped(reason);
    });
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](const int exitCode, const QProcess::ExitStatus status) {
                consumeStdout();
                starting_ = false;
                setReady(false);
                activeTurns_.clear();
                reloader_->stopped();
                if (stopping_) {
                    return;
                }
                noteUnexpectedStop();
                if (malformedOutput_) {
                    const QString reason =
                        QStringLiteral("Codex app-server emitted a malformed protocol message.");
                    failPending(reason);
                    // Initialization already publishes its own failure; do not emit it twice.
                    if (!stopping_)
                        emit stopped(reason);
                    return;
                }
                QString reason =
                    QStringLiteral("Codex app-server stopped (exit %1%2)")
                        .arg(exitCode)
                        .arg(status == QProcess::CrashExit ? QStringLiteral(", crashed")
                                                           : QString());
                const QString detail = QString::fromUtf8(stderrBuffer_).trimmed();
                if (!detail.isEmpty()) {
                    reason.append(QStringLiteral(": %1").arg(detail.right(2000)));
                }
                failPending(reason);
                emit stopped(reason);
            });
}

CodexAdapter::~CodexAdapter() {
    // Owners destroy adapters at shutdown, after the SessionHost whose callbacks are still
    // pending; calling them here would use the freed host. Drop them instead of failing them.
    pending_.clear();
    queued_.clear();
    stop();
}

bool CodexAdapter::isAvailable() const { return !executable_.isEmpty(); }

QString CodexAdapter::missingMessage() const {
    return compat::missing(compat::policy(QStringLiteral("codex"))).message;
}

bool CodexAdapter::isReady() const { return ready_; }

QString CodexAdapter::executable() const { return executable_; }

void CodexAdapter::call(const QString& method, const QJsonObject& params, Callback callback) {
    if (!isAvailable()) {
        callback({}, QStringLiteral("Codex CLI was not found."));
        return;
    }
    if (!ready_) {
        queued_.append({method, params, std::move(callback)});
        ensureStarted();
        return;
    }
    sendCall(method, params, std::move(callback));
}

void CodexAdapter::runtimeStatus(std::function<void(const compat::Runtime&)> callback) {
    versionProbe_->check(executable_,
                         [this, callback = std::move(callback)](const compat::Runtime& runtime) {
                             callback(narrowing_.apply(runtime));
                         });
}

bool CodexAdapter::supports(const QString& capability) const {
    if (narrowing_.removed(capability))
        return false;
    const auto runtime = versionProbe_->cached(executable_);
    return runtime ? runtime->supports(capability)
                   : versionProbe_->policy().baselineSupports(capability);
}

void CodexAdapter::configurationCatalog(Callback callback) {
    call(QStringLiteral("model/list"), {{QStringLiteral("limit"), 100}},
         [callback = std::move(callback)](const QJsonObject& result, const QString& providerError) {
             if (!providerError.isEmpty()) {
                 callback({}, providerError);
                 return;
             }
             QJsonArray catalog;
             for (const QJsonValue& value : result.value(QStringLiteral("data")).toArray()) {
                 const QJsonObject source = value.toObject();
                 const QString id = source.value(QStringLiteral("model")).toString();
                 if (id.isEmpty() || source.value(QStringLiteral("hidden")).toBool())
                     continue;
                 QJsonArray efforts;
                 for (const QJsonValue& effortValue :
                      source.value(QStringLiteral("supportedReasoningEfforts")).toArray()) {
                     const QString effortId =
                         effortValue.toObject().value(QStringLiteral("reasoningEffort")).toString();
                     if (!effortId.isEmpty())
                         efforts.append(QJsonObject{{QStringLiteral("id"), effortId},
                                                    {QStringLiteral("label"), effortId}});
                 }
                 catalog.append(
                     QJsonObject{{QStringLiteral("id"), id},
                                 {QStringLiteral("label"),
                                  source.value(QStringLiteral("displayName")).toString(id)},
                                 {QStringLiteral("efforts"), efforts},
                                 {QStringLiteral("default_effort"),
                                  source.value(QStringLiteral("defaultReasoningEffort"))},
                                 // A conversation that chose no model runs this one; the
                                 // phone opens its model picker on it.
                                 {QStringLiteral("is_default"),
                                  source.value(QStringLiteral("isDefault")).toBool()}});
             }
             callback(QJsonObject{{QStringLiteral("catalog"), catalog}}, {});
         });
}

void CodexAdapter::respond(const QJsonValue& requestId, const QJsonObject& result) {
    sendObject({{QStringLiteral("id"), requestId}, {QStringLiteral("result"), result}});
}

void CodexAdapter::respondError(const QJsonValue& requestId, const QString& code,
                                const QString& message) {
    sendObject({{QStringLiteral("id"), requestId},
                {QStringLiteral("error"),
                 QJsonObject{{QStringLiteral("code"), code},
                             {QStringLiteral("message"), message}}}});
}

int CodexAdapter::restartDelayMs() const { return restartDelayMs_; }

bool CodexAdapter::restartScheduled() const { return restartTimer_->isActive(); }

bool CodexAdapter::isIdle() const {
    return activeTurns_.isEmpty() && pending_.isEmpty() && queued_.isEmpty() && !starting_ &&
           !checkingVersion_;
}

void CodexAdapter::trackTurn(const QString& method, const QJsonObject& params) {
    const QString thread = params.value(QStringLiteral("threadId")).toString();
    if (method == QStringLiteral("turn/started"))
        activeTurns_.insert(thread);
    else if (method == QStringLiteral("turn/completed"))
        activeTurns_.remove(thread);
    // A turn-level error that will not be retried ends the turn even without turn/completed;
    // otherwise a deferred settings reload would wait forever.
    else if (method == QStringLiteral("error") &&
             !params.value(QStringLiteral("willRetry")).toBool())
        activeTurns_.remove(thread);
}

QStringList CodexAdapter::browserArguments() const {
    return browserArgumentsProvider_ ? browserArgumentsProvider_()
                                     : browser_tools::codexArgumentsFromUserSettings(
                                           QCoreApplication::applicationDirPath());
}

void CodexAdapter::setBrowserArgumentsProvider(std::function<QStringList()> provider) {
    browserArgumentsProvider_ = std::move(provider);
}

void CodexAdapter::restartForBrowserSettings() {
    if (process_->state() == QProcess::NotRunning || !isIdle())
        return;
    // A planned stop: no crash backoff and no stopped() (that is a crash to sessions). reloaded()
    // detaches idle sessions silently; their next turn resumes the thread in the new process,
    // which starts lazily on the next call with the new browser arguments.
    stop();
    emit reloaded(QStringLiteral("browser_settings"));
}

void CodexAdapter::noteUnexpectedStop() {
    // A crash after a long healthy run is a fresh failure, not part of a crash loop.
    const bool stable = readySince_.isValid() && readySince_.elapsed() >= stableRunMs;
    readySince_.invalidate();
    restartDelayMs_ = stable || restartDelayMs_ == 0
                          ? initialRestartDelayMs
                          : std::min(restartDelayMs_ * 2, maximumRestartDelayMs);
    restartNotBefore_ = QDeadlineTimer(restartDelayMs_);
}

void CodexAdapter::stop() {
    stopping_ = true;
    restartTimer_->stop();
    failPending(QStringLiteral("Codex app-server is shutting down."));
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

void CodexAdapter::ensureStarted() {
    if (starting_ || process_->state() != QProcess::NotRunning) {
        return;
    }
    if (!restartNotBefore_.hasExpired()) {
        // Calls stay queued and start the provider once the backoff elapses.
        if (!restartTimer_->isActive())
            restartTimer_->start(std::max<qint64>(1, restartNotBefore_.remainingTime()));
        return;
    }
    // Every launch re-checks the CLI version (cached until the executable changes on disk), so an
    // update or downgrade between launches is noticed (ADR 0015).
    if (!versionCleared_) {
        if (checkingVersion_)
            return;
        checkingVersion_ = true;
        versionProbe_->check(executable_, [this](const compat::Runtime& runtime) {
            checkingVersion_ = false;
            if (runtime.blocksLaunch()) {
                failPending(runtime.message);
                return;
            }
            // stop() during the check already failed the queued calls; nothing to launch for.
            if (queued_.isEmpty())
                return;
            versionCleared_ = true;
            ensureStarted();
        });
        return;
    }
    versionCleared_ = false;
    malformedOutput_ = false;
    stopping_ = false;
    starting_ = true;
    stdoutReader_.clear();
    stderrBuffer_.clear();
    process_->setProgram(executable_);
    // Browser tools (ADR 0008) are read from the shared settings file on every start; a later
    // settings change restarts an idle provider through reloader_.
    launchedBrowserArguments_ = browserArguments();
    const QStringList& browserArguments = launchedBrowserArguments_;
    process_->setArguments(
        QStringList{QStringLiteral("app-server"), QStringLiteral("--stdio"), QStringLiteral("-c"),
                    QStringLiteral("features.default_mode_request_user_input=true")} +
        browserArguments);
    reloader_->launched(browserArguments);
    process_->start();
}

void CodexAdapter::initialize() {
    // A new process may be a different CLI build; what the old one lacked no longer applies.
    narrowing_.reset();
    sendCall(
        QStringLiteral("initialize"),
        {{QStringLiteral("clientInfo"),
          QJsonObject{{QStringLiteral("name"), QStringLiteral("ketplus-daemon")},
                      {QStringLiteral("title"), QStringLiteral("KetPlus Daemon")},
                      {QStringLiteral("version"), QStringLiteral("0.1.0")}}},
         {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("experimentalApi"), true}}}},
        [this](const QJsonObject&, const QString& error) {
            starting_ = false;
            if (!error.isEmpty()) {
                const QString reason = QStringLiteral("Codex initialization failed: %1").arg(error);
                noteUnexpectedStop();
                failPending(reason);
                emit stopped(reason);
                stopping_ = true;
                process_->terminate();
                return;
            }
            sendObject({{QStringLiteral("method"), QStringLiteral("initialized")},
                        {QStringLiteral("params"), QJsonObject{}}});
            setReady(true);
            const QList<QueuedCall> calls = std::exchange(queued_, {});
            for (const QueuedCall& call : calls) {
                sendCall(call.method, call.params, call.callback);
            }
        });
}

void CodexAdapter::sendCall(const QString& method, const QJsonObject& params, Callback callback) {
    const quint64 id = nextRequestId_++;
    if (method == QStringLiteral("turn/start")) {
        // The turn is active from the request, not from turn/started: a reload between the
        // response and that notification would kill the new turn.
        const QString thread = params.value(QStringLiteral("threadId")).toString();
        activeTurns_.insert(thread);
        callback = [this, thread, callback = std::move(callback)](const QJsonObject& result,
                                                                  const QString& error) {
            if (!error.isEmpty())
                activeTurns_.remove(thread);
            callback(result, error);
        };
    }
    pending_.insert(id, std::move(callback));
    pendingMethods_.insert(id, method);
    QJsonObject providerParams = browser_tools::withCodexBrowserInstructions(
        method, params, !launchedBrowserArguments_.isEmpty());
    if (method == QStringLiteral("thread/start") || method == QStringLiteral("thread/resume"))
        providerParams.insert(
            QStringLiteral("developerInstructions"),
            withResponseGuidance(
                providerParams.value(QStringLiteral("developerInstructions")).toString()));
    sendObject({{QStringLiteral("id"), static_cast<qint64>(id)},
                {QStringLiteral("method"), method},
                {QStringLiteral("params"), providerParams}});
}

void CodexAdapter::sendObject(const QJsonObject& object) {
    if (process_->state() == QProcess::NotRunning) {
        return;
    }
    QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    payload.append('\n');
    process_->write(payload);
}

void CodexAdapter::consumeStdout() {
    if (malformedOutput_) {
        process_->setReadChannel(QProcess::StandardOutput);
        while (process_->bytesAvailable() > 0)
            if (process_->read(64 * 1024).isEmpty())
                break;
        return;
    }
    stdoutReader_.append(
        process_->readAllStandardOutput(), [this](const QByteArray& line) { consumeLine(line); },
        [this](const OversizedProviderLine& line) { skipOversized(line); });
}

void CodexAdapter::skipOversized(const OversizedProviderLine& line) {
    const QJsonValue id = line.fields.value(QStringLiteral("id"));
    const QString method = line.fields.value(QStringLiteral("method")).toString();
    const QString reason =
        QStringLiteral("Codex app-server sent a %1 MiB message, over the %2 MiB limit.")
            .arg(line.bytes / (1024 * 1024))
            .arg(maximumProviderMessageBytes / (1024 * 1024));
    qWarning("%s Skipped %s.", qUtf8Printable(reason),
             qUtf8Printable(method.isEmpty() ? QStringLiteral("a response") : method));
    if (method.isEmpty()) {
        // A response: only the call waiting for it fails.
        const auto callId = providerResponseId(id);
        if (!callId)
            return;
        pendingMethods_.remove(*callId);
        if (const auto callback = pending_.take(*callId))
            callback({}, reason);
        reloader_->idleChanged();
        return;
    }
    if (line.fields.contains(QStringLiteral("id"))) {
        // A request Codex waits on: answer it so the turn does not hang.
        sendObject({{QStringLiteral("id"), id},
                    {QStringLiteral("error"),
                     QJsonObject{{QStringLiteral("code"), -32603},
                                 {QStringLiteral("message"), reason}}}});
        return;
    }
    const QString thread = line.fields.value(QStringLiteral("params.threadId")).toString();
    emit notificationSkipped(method, thread, line.bytes);
    if (method == QStringLiteral("turn/completed")) {
        // The turn still ends; only its final payload is lost.
        const QString turn = line.fields.value(QStringLiteral("params.turn.id"))
                                 .toString(line.fields.value(QStringLiteral("params.turnId"))
                                               .toString());
        handleNotification(method,
                           {{QStringLiteral("threadId"), thread},
                            {QStringLiteral("turn"),
                             QJsonObject{{QStringLiteral("id"), turn},
                                         {QStringLiteral("status"), QStringLiteral("completed")}}}});
    }
}

void CodexAdapter::handleNotification(const QString& method, const QJsonObject& params) {
    emit notificationReceived(method, params);
    trackTurn(method, params);
    reloader_->idleChanged();
}

void CodexAdapter::consumeLine(const QByteArray& line) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (!providerFrameHasValidControlBytes(line) || parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        // No reliable call identity survives a broken frame. Quarantine the whole
        // generation; finished settles pending calls through the existing lifecycle.
        malformedOutput_ = true;
        setReady(false);
        stdoutReader_.clear();
        process_->closeReadChannel(QProcess::StandardOutput);
        process_->kill();
        return;
    }
    const QJsonObject object = document.object();
    if (object.contains(QStringLiteral("method"))) {
        const QString method = object.value(QStringLiteral("method")).toString();
        const QJsonObject params = object.value(QStringLiteral("params")).toObject();
        if (object.contains(QStringLiteral("id"))) {
            emit requestReceived(object.value(QStringLiteral("id")), method, params);
        } else {
            handleNotification(method, params);
        }
        return;
    }
    if (!object.contains(QStringLiteral("id"))) {
        return;
    }
    const auto responseId = providerResponseId(object.value(QStringLiteral("id")));
    if (!responseId)
        return;
    const quint64 id = *responseId;
    const auto callback = pending_.take(id);
    const QString method = pendingMethods_.take(id);
    if (!callback) {
        return;
    }
    if (!validProviderResponseEnvelope(object)) {
        callback({}, QStringLiteral("Malformed provider response envelope."));
    } else if (object.contains(QStringLiteral("error"))) {
        const QJsonObject error = object.value(QStringLiteral("error")).toObject();
        if (error.value(QStringLiteral("code")).toInt() == -32601)
            (void)narrowing_.methodNotFound(method);
        callback({}, errorText(error));
    } else {
        callback(object.value(QStringLiteral("result")).toObject(), {});
    }
    reloader_->idleChanged();
}

void CodexAdapter::failPending(const QString& reason) {
    pendingMethods_.clear();
    const auto pending = std::exchange(pending_, {});
    for (const Callback& callback : pending) {
        callback({}, reason);
    }
    const auto queued = std::exchange(queued_, {});
    for (const QueuedCall& call : queued) {
        call.callback({}, reason);
    }
}

void CodexAdapter::setReady(const bool ready) {
    if (ready && !ready_)
        readySince_.start();
    ready_ = ready;
}

QString CodexAdapter::findExecutable() {
    const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("codex"));
    if (!fromPath.isEmpty()) {
        return fromPath;
    }
    const QStringList candidates{
        QDir::home().filePath(QStringLiteral(".local/bin/codex")),
        QStringLiteral("/opt/homebrew/bin/codex"),
        QStringLiteral("/usr/local/bin/codex"),
    };
    for (const QString& candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable()) {
            return info.absoluteFilePath();
        }
    }
    return {};
}

} // namespace ketplus::daemon
