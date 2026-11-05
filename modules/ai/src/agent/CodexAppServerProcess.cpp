#include "agent/CodexAppServer.h"
#include "BrowserAgentInstructions.h"

#include "agent/DaemonTranscriptProjection.h"
#include <algorithm>

#include "CodexBrowserTools.h"
#include "CodexLaunchReloader.h"
#include "CodexServerRequests.h"
#include "DaemonClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

#include <utility>

// Direct mode: a codex app-server child process over stdio JSON-RPC. The application always
// talks to the daemon (setupDaemonTransport); tests construct this mode with a fake executable.

namespace ketplus {
namespace {

constexpr qsizetype maximumBufferedMessageBytes = 16 * 1024 * 1024;
constexpr qsizetype maximumStderrBytes = 1024 * 1024;

QString responseError(const QJsonObject& error) {
    QString message = error.value(QStringLiteral("message")).toString();
    if (message.isEmpty()) {
        message = QString::fromUtf8(QJsonDocument(error).toJson(QJsonDocument::Compact));
    }
    // Keep the JSON-RPC "method not found" code visible: history fallbacks match on it.
    const int code = error.value(QStringLiteral("code")).toInt();
    if (code == -32601 && !message.contains(QStringLiteral("-32601")))
        message += QStringLiteral(" (-32601)");
    return message;
}

} // namespace

void CodexAppServer::setupProcessTransport() {
    // The daemon applies browser settings to its own provider; only direct mode reloads here.
    reloader_ = new browser_tools::CodexLaunchReloader(
        [this] { return browserArguments(); }, [this] { return isIdle(); },
        browser_tools::settingsFilePath(), browser_tools::CodexLaunchReloader::defaultDebounceMs,
        this);
    connect(reloader_, &browser_tools::CodexLaunchReloader::restartRequested, this,
            &CodexAppServer::restartForBrowserSettings);
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    process_->setProcessEnvironment(environment);

    connect(process_, &QProcess::started, this, &CodexAppServer::initialize);
    connect(process_, &QProcess::readyReadStandardOutput, this, &CodexAppServer::consumeStdout);
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
        const QString reason =
            QStringLiteral("Unable to start Codex app-server: %1").arg(process_->errorString());
        failPending(reason);
        emit serverStopped(reason);
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
                emit serverStopped(reason);
            });
}

bool CodexAppServer::isIdle() const {
    return activeTurns_.isEmpty() && pending_.isEmpty() && queued_.isEmpty() && !starting_;
}

QStringList CodexAppServer::browserArguments() const {
    return browserArgumentsProvider_ ? browserArgumentsProvider_()
                                     : browser_tools::codexArgumentsFromUserSettings(
                                           QCoreApplication::applicationDirPath());
}

void CodexAppServer::setBrowserArgumentsProvider(std::function<QStringList()> provider) {
    browserArgumentsProvider_ = std::move(provider);
}

void CodexAppServer::restartForBrowserSettings() {
    if (daemon_ != nullptr || process_->state() == QProcess::NotRunning || !isIdle())
        return;
    // serverStopped() marks sessions as not loaded in this process, so their next turn resumes
    // the thread; the new process starts lazily on the next call with the new arguments.
    stop();
    // Not serverStopped(): that fails running sessions and shows an error row.
    emit serverReloaded(QStringLiteral("browser_settings"));
}

void CodexAppServer::ensureStarted() {
    if (starting_ || process_->state() != QProcess::NotRunning) {
        return;
    }
    stopping_ = false;
    starting_ = true;
    stdoutBuffer_.clear();
    stderrBuffer_.clear();
    process_->setProgram(executable_);
    // Browser tools (ADR 0008): read at each start; changes restart an idle process (reloader_).
    launchedBrowserArguments_ = browserArguments();
    const QStringList& browserArguments = launchedBrowserArguments_;
    process_->setArguments(
        QStringList{QStringLiteral("app-server"), QStringLiteral("--stdio"), QStringLiteral("-c"),
                    QStringLiteral("features.default_mode_request_user_input=true")} +
        browserArguments);
    reloader_->launched(browserArguments);
    process_->start();
}

void CodexAppServer::initialize() {
    sendCall(
        QStringLiteral("initialize"),
        {{QStringLiteral("clientInfo"),
          QJsonObject{{QStringLiteral("name"), QStringLiteral("ketplus")},
                      {QStringLiteral("title"), QStringLiteral("KetPlus")},
                      {QStringLiteral("version"), QStringLiteral("0.1.0")}}},
         {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("experimentalApi"), true}}}},
        [this](const QJsonObject&, const QString& error) {
            starting_ = false;
            if (!error.isEmpty()) {
                const QString reason = QStringLiteral("Codex initialization failed: %1").arg(error);
                failPending(reason);
                emit serverStopped(reason);
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

void CodexAppServer::sendCall(const QString& method, const QJsonObject& params,
                              ResponseCallback callback) {
    const quint64 id = nextRequestId_++;
    if (method == QStringLiteral("turn/start")) {
        // Active from the request: a reload before turn/started would kill the new turn.
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
    sendObject(
        {{QStringLiteral("id"), static_cast<qint64>(id)},
         {QStringLiteral("method"), method},
         {QStringLiteral("params"), browser_tools::withCodexBrowserInstructions(
                                        method, params, !launchedBrowserArguments_.isEmpty())}});
}

void CodexAppServer::sendObject(const QJsonObject& object) {
    if (process_->state() == QProcess::NotRunning) {
        return;
    }
    QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    payload.append('\n');
    process_->write(payload);
}

void CodexAppServer::consumeStdout() {
    stdoutBuffer_.append(process_->readAllStandardOutput());
    if (stdoutBuffer_.size() > maximumBufferedMessageBytes && !stdoutBuffer_.contains('\n')) {
        const QString reason = QStringLiteral("Codex app-server emitted an oversized message.");
        stdoutBuffer_.clear();
        failPending(reason);
        emit serverStopped(reason);
        return;
    }
    qsizetype newline = -1;
    while ((newline = stdoutBuffer_.indexOf('\n')) >= 0) {
        const QByteArray line = stdoutBuffer_.left(newline).trimmed();
        stdoutBuffer_.remove(0, newline + 1);
        if (!line.isEmpty()) {
            consumeLine(line);
        }
    }
}

void CodexAppServer::consumeLine(const QByteArray& line) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return;
    }
    const QJsonObject object = document.object();
    if (object.contains(QStringLiteral("method"))) {
        if (object.contains(QStringLiteral("id"))) {
            handleServerRequest(object);
        } else {
            const QString method = object.value(QStringLiteral("method")).toString();
            const QJsonObject params = object.value(QStringLiteral("params")).toObject();
            emit notificationReceived(method, params);
            const QString thread = params.value(QStringLiteral("threadId")).toString();
            if (method == QStringLiteral("turn/started"))
                activeTurns_.insert(thread);
            else if (method == QStringLiteral("turn/completed") ||
                     (method == QStringLiteral("error") &&
                      !params.value(QStringLiteral("willRetry")).toBool()))
                activeTurns_.remove(thread); // a final error ends the turn without turn/completed
            reloader_->idleChanged();
        }
        return;
    }
    if (!object.contains(QStringLiteral("id"))) {
        return;
    }
    const quint64 id = object.value(QStringLiteral("id")).toVariant().toULongLong();
    auto callback = pending_.take(id);
    if (!callback) {
        return;
    }
    if (object.contains(QStringLiteral("error"))) {
        callback({}, responseError(object.value(QStringLiteral("error")).toObject()));
    } else {
        callback(object.value(QStringLiteral("result")).toObject(), {});
    }
    reloader_->idleChanged();
}

void CodexAppServer::handleServerRequest(const QJsonObject& request) {
    const QJsonValue id = request.value(QStringLiteral("id"));
    const QString method = request.value(QStringLiteral("method")).toString();
    if (method == QStringLiteral("item/commandExecution/requestApproval") ||
        method == QStringLiteral("item/fileChange/requestApproval")) {
        sendObject({{QStringLiteral("id"), id},
                    {QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("decision"), QStringLiteral("decline")}}}});
        return;
    }
    if (method == QStringLiteral("item/tool/requestUserInput")) {
        emit userInputRequested(id, request.value(QStringLiteral("params")).toObject());
        return;
    }
    if (method == codex_requests::mcpElicitationMethod) {
        // Direct mode shows no approval cards (commands above are declined too), but browser
        // tools never prompt (ADR 0008), even if Codex ignores the launch approval mode. Only the
        // server injected at launch qualifies; a user-configured `ketplus_browser` is declined.
        const QJsonObject params = request.value(QStringLiteral("params")).toObject();
        const bool browserTool =
            codex_requests::isInjectedBrowserToolApproval(params, launchedBrowserArguments_);
        sendObject(
            {{QStringLiteral("id"), id},
             {QStringLiteral("result"), browserTool ? codex_requests::mcpElicitationResult(true)
                                                    : codex_requests::declinedResult(method)}});
        return;
    }
    if (method == QStringLiteral("item/permissions/requestApproval") ||
        method == QStringLiteral("item/tool/call")) {
        sendObject({{QStringLiteral("id"), id},
                    {QStringLiteral("result"), codex_requests::declinedResult(method)}});
        return;
    }
    sendObject(
        {{QStringLiteral("id"), id},
         {QStringLiteral("error"),
          QJsonObject{{QStringLiteral("code"), -32601},
                      {QStringLiteral("message"),
                       QStringLiteral("KetPlus does not support this server request yet.")}}}});
}

void CodexAppServer::failPending(const QString& reason) {
    const auto callbacks = std::exchange(pending_, {});
    for (const auto& callback : callbacks) {
        callback({}, reason);
    }
    const auto calls = std::exchange(queued_, {});
    for (const QueuedCall& call : calls) {
        call.callback({}, reason);
    }
}

QString CodexAppServer::findCodexExecutable() {
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

} // namespace ketplus
