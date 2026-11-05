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

namespace ketplus {
namespace {

constexpr qsizetype maximumBufferedMessageBytes = 16 * 1024 * 1024;
constexpr qsizetype maximumStderrBytes = 1024 * 1024;
constexpr int legacyRestoreReplayBatchSize = 16;
constexpr int restoreReplayBatchSize = 128;

QString normalizedLocalPath(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

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

CodexAppServer::CodexAppServer(QObject* parent) : QObject(parent) { setupDaemonTransport(); }

CodexAppServer::CodexAppServer(const QString& executable, QObject* parent)
    : QObject(parent), executable_(executable), process_(new QProcess(this)) {
    setupProcessTransport();
}

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

void CodexAppServer::setupDaemonTransport() {
    daemon_ = new remote::DaemonClient(this);
    daemon_->setActivityGroupsEnabled(true);
    executable_ = daemon_->isAvailable() ? QStringLiteral("ketplus_daemon") : QString{};
    connect(daemon_, &remote::DaemonClient::readyChanged, this, [this](const bool ready) {
        setReady(ready);
        if (ready) {
            refreshDaemonSessions();
        } else {
            daemonSessionListInFlight_ = false;
            ++daemonDiscoveryEpoch_;
            daemonDiscoveryInFlight_ = false;
            daemonDiscoveryCursors_.clear();
            restoringDaemonSessions_.clear();
            daemonRestoreQueue_.clear();
            activeDaemonRestoreSession_.clear();
            setRestoring(false);
        }
    });
    connect(daemon_, &remote::DaemonClient::eventReceived, this,
            &CodexAppServer::consumeDaemonEvent);
    connect(daemon_, &remote::DaemonClient::disconnected, this, [](const QString&) {});
}

CodexAppServer::~CodexAppServer() { stop(); }

bool CodexAppServer::isAvailable() const noexcept {
    return daemon_ ? (daemon_->isReady() || daemon_->isAvailable()) : !executable_.isEmpty();
}

bool CodexAppServer::isReady() const noexcept { return ready_; }

bool CodexAppServer::hasCompactWindowSupport() const noexcept {
    return daemon_ && daemon_->hasFeature(QStringLiteral("session.compact_window/1"));
}

QString CodexAppServer::executable() const { return executable_; }

void CodexAppServer::call(const QString& method, const QJsonObject& params,
                          ResponseCallback callback) {
    if (daemon_ != nullptr) {
        daemonCall(method, params, std::move(callback));
        return;
    }
    if (!isAvailable()) {
        callback({}, QStringLiteral("Codex CLI was not found. Install it or add it to PATH."));
        return;
    }
    if (method.startsWith(QStringLiteral("provider/claudeCredentials/"))) {
        callback({}, QStringLiteral("Claude API key settings require a local KetPlus daemon."));
        return;
    }
    // displayText and clientMessageId only tell the daemon what to journal; Codex rejects them.
    QJsonObject providerParams = params;
    providerParams.remove(QStringLiteral("displayText"));
    providerParams.remove(QStringLiteral("clientMessageId"));
    if (!ready_) {
        queued_.append({method, providerParams, std::move(callback)});
        ensureStarted();
        return;
    }
    sendCall(method, providerParams, std::move(callback));
}

void CodexAppServer::notify(const QString& method, const QJsonObject& params) {
    if (daemon_ != nullptr) {
        Q_UNUSED(method)
        Q_UNUSED(params)
        return;
    }
    if (!ready_) {
        return;
    }
    sendObject({{QStringLiteral("method"), method}, {QStringLiteral("params"), params}});
}

void CodexAppServer::respondToUserInput(const QJsonValue& requestId, const QJsonObject& answers,
                                        const QJsonArray& questions) {
    if (requestId.isUndefined() || requestId.isNull()) {
        return;
    }
    if (daemon_ != nullptr) {
        const QString approvalId = requestId.toString();
        const PendingDaemonApproval approval = daemonApprovals_.value(approvalId);
        auto session = daemonSessions_.find(approval.sessionId);
        if (approval.sessionId.isEmpty() || session == daemonSessions_.end()) {
            return;
        }
        QJsonObject resolution;
        if (approval.kind == QStringLiteral("input")) {
            QJsonArray normalizedAnswers;
            for (const QJsonValue& questionValue : approval.questions) {
                const QJsonObject question = questionValue.toObject();
                const QString questionId = question.value(QStringLiteral("id")).toString();
                const QJsonObject submitted = answers.value(questionId).toObject();
                const QJsonArray optionIds =
                    submitted.value(QStringLiteral("option_ids")).toArray();
                const QString customAnswer =
                    submitted.value(QStringLiteral("custom_answer")).toString();
                QJsonObject normalized{{QStringLiteral("question_id"), questionId},
                                       {QStringLiteral("question_revision"),
                                        question.value(QStringLiteral("revision")).toInt(1)},
                                       {QStringLiteral("option_ids"), optionIds}};
                if (!customAnswer.isEmpty())
                    normalized.insert(QStringLiteral("custom_answer"), customAnswer);
                normalizedAnswers.append(normalized);
            }
            resolution = {{QStringLiteral("request_id"), approvalId},
                          {QStringLiteral("revision"), approval.revision},
                          {QStringLiteral("action"), QStringLiteral("answer")},
                          {QStringLiteral("answers"), normalizedAnswers}};
        } else {
            const QJsonObject submitted = answers.value(QStringLiteral("decision")).toObject();
            const QJsonArray selectedIds = submitted.value(QStringLiteral("option_ids")).toArray();
            const QString selectedId =
                selectedIds.isEmpty() ? QString{} : selectedIds.first().toString();
            QString decision;
            for (const QJsonValue& questionValue : questions) {
                const QJsonObject question = questionValue.toObject();
                if (question.value(QStringLiteral("id")) != QStringLiteral("decision"))
                    continue;
                for (const QJsonValue& optionValue :
                     question.value(QStringLiteral("options")).toArray()) {
                    const QJsonObject option = optionValue.toObject();
                    if (option.value(QStringLiteral("id")).toString() == selectedId) {
                        decision = option.value(QStringLiteral("label")).toString();
                        break;
                    }
                }
            }
            resolution.insert(QStringLiteral("decision"),
                              decision.compare(QStringLiteral("Accept"), Qt::CaseInsensitive) == 0
                                  ? QStringLiteral("accept")
                                  : QStringLiteral("decline"));
        }
        daemon_->call(approval.kind == QStringLiteral("input") ? QStringLiteral("question.answer")
                                                               : QStringLiteral("approval.resolve"),
                      {{QStringLiteral("approval_id"), approvalId},
                       {QStringLiteral("resolution"), resolution}},
                      approval.sessionId, session->generation,
                      [this, sessionId = approval.sessionId, approvalId](const QJsonObject&,
                                                                         const QString& error) {
                          // The daemon may have expired the request already; do not leave the
                          // card claiming the answer was delivered.
                          if (error.isEmpty())
                              return;
                          emit notificationReceived(QStringLiteral("userInput/resolved"),
                                                    {{QStringLiteral("threadId"), sessionId},
                                                     {QStringLiteral("requestId"), approvalId},
                                                     {QStringLiteral("expired"), true}});
                      });
        daemonApprovals_.remove(approvalId);
        return;
    }
    QJsonObject providerAnswers;
    for (const QJsonValue& questionValue : questions) {
        const QJsonObject question = questionValue.toObject();
        const QString questionId = question.value(QStringLiteral("id")).toString();
        const QJsonObject submitted = answers.value(questionId).toObject();
        QHash<QString, QString> labels;
        for (const QJsonValue& optionValue : question.value(QStringLiteral("options")).toArray()) {
            const QJsonObject option = optionValue.toObject();
            labels.insert(option.value(QStringLiteral("id")).toString(),
                          option.value(QStringLiteral("label")).toString());
        }
        QJsonArray values;
        for (const QJsonValue& optionIdValue :
             submitted.value(QStringLiteral("option_ids")).toArray()) {
            const QString optionId = optionIdValue.toString();
            if (labels.contains(optionId))
                values.append(labels.value(optionId));
        }
        const QString customAnswer = submitted.value(QStringLiteral("custom_answer")).toString();
        if (!customAnswer.isEmpty())
            values.append(customAnswer);
        providerAnswers.insert(questionId, QJsonObject{{QStringLiteral("answers"), values}});
    }
    sendObject(
        {{QStringLiteral("id"), requestId},
         {QStringLiteral("result"), QJsonObject{{QStringLiteral("answers"), providerAnswers}}}});
}

void CodexAppServer::rejectUserInput(const QJsonValue& requestId, const QString& code,
                                     const QString& message) {
    if (requestId.isUndefined() || requestId.isNull() || daemon_ != nullptr)
        return;
    sendObject(
        {{QStringLiteral("id"), requestId},
         {QStringLiteral("error"),
          QJsonObject{{QStringLiteral("code"), -32602},
                      {QStringLiteral("message"), message},
                      {QStringLiteral("data"), QJsonObject{{QStringLiteral("code"), code}}}}}});
}

void CodexAppServer::stop() {
    if (daemon_ != nullptr) {
        setReady(false);
        return;
    }
    stopping_ = true;
    queued_.clear();
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

void CodexAppServer::setReady(const bool ready) {
    if (ready_ == ready) {
        return;
    }
    ready_ = ready;
    emit readyChanged(ready_);
}

void CodexAppServer::setRestoring(const bool restoring) {
    if (restoring_ == restoring) {
        return;
    }
    restoring_ = restoring;
    emit restoreStateChanged(restoring_);
}

void CodexAppServer::prioritizeDaemonRestore(const QString& worktreePath,
                                             const QString& sessionId) {
    daemonRestorePriorityPath_ = normalizedLocalPath(worktreePath);
    daemonRestorePrioritySession_ = sessionId;
    reorderDaemonRestoreQueue();
    restoreNextDaemonSession();
}

bool CodexAppServer::isDaemonSessionRestoring(const QString& sessionId) const {
    return daemon_ != nullptr && !sessionId.isEmpty() &&
           (daemonSessionListInFlight_ || restoringDaemonSessions_.contains(sessionId));
}

void CodexAppServer::executionWorktreeCommand(const QString& id, const QString& type,
                                              const QJsonObject& payload,
                                              ResponseCallback callback) {
    const auto found = daemonSessions_.constFind(id);
    if (!daemon_ || found == daemonSessions_.cend()) {
        callback({}, "Conversation is not attached to the daemon.");
        return;
    }
    daemon_->call(
        type, payload, id, found->generation,
        [this, id, callback = std::move(callback)](const QJsonObject& result,
                                                   const QString& error) {
            auto it = daemonSessions_.find(id);
            if (error.isEmpty() && it != daemonSessions_.end() &&
                result.contains("context_revision")) {
                if (result.value("context_revision").toInteger() < it->contextRevision) {
                    callback({}, "The conversation context changed. Refresh the picker.");
                    return;
                }
                it->contextRevision = result.value("context_revision").toInteger();
                it->cwd = result.value("worktree_path").toString(it->cwd);
                emit executionContextChanged(
                    id, result.value("current_branch").toString(QFileInfo(it->cwd).fileName()),
                    it->cwd, it->contextRevision);
            }
            callback(result, error);
        });
}

void CodexAppServer::daemonCall(const QString& method, const QJsonObject& params,
                                ResponseCallback callback) {
    if (!isAvailable()) {
        callback({}, QStringLiteral("KetPlus Daemon executable was not found."));
        return;
    }
    if (method == QStringLiteral("model/list")) {
        daemon_->call(QStringLiteral("provider.models"),
                      {{QStringLiteral("cursor"), params.value(QStringLiteral("cursor"))}}, {}, -1,
                      std::move(callback));
        return;
    }
    if (method == QStringLiteral("account/rateLimits/read")) {
        daemon_->call(QStringLiteral("provider.rate_limits"), {}, {}, -1, std::move(callback));
        return;
    }
    if (method.startsWith(QStringLiteral("provider/claudeCredentials/"))) {
        if (!daemon_->isReady() ||
            !daemon_->hasFeature(QStringLiteral("provider.claude.credentials/1"))) {
            callback(
                {},
                QStringLiteral(
                    "Connect to an updated local KetPlus daemon to configure Claude API keys."));
            return;
        }
        const QString operation = method.mid(QStringLiteral("provider/claudeCredentials/").size());
        daemon_->call(QStringLiteral("provider.claude.credentials.") + operation, params, {}, -1,
                      std::move(callback));
        return;
    }
    if (method == QStringLiteral("provider/runtimeStatus")) {
        // Daemons before protocol minor 1 do not report provider runtimes.
        if (daemon_->isReady() && !daemon_->hasFeature(QStringLiteral("provider.runtime_status"))) {
            callback({}, QStringLiteral("KetPlus Daemon does not support %1 yet.").arg(method));
            return;
        }
        daemon_->call(QStringLiteral("provider.runtime_status"), {}, {}, -1, std::move(callback));
        return;
    }
    if (method == QStringLiteral("thread/start")) {
        const QString sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        DaemonSession session;
        session.cwd = params.value(QStringLiteral("cwd")).toString();
        session.options = params;
        session.windowReady = true;
        daemonSessions_.insert(sessionId, session);
        daemonRestorePrioritySession_ = sessionId;
        callback({{QStringLiteral("thread"), QJsonObject{{QStringLiteral("id"), sessionId},
                                                         {QStringLiteral("turns"), QJsonArray{}}}}},
                 {});
        return;
    }
    if (method == QStringLiteral("thread/resume")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto& session = daemonSessions_[sessionId];
        if (session.cwd.isEmpty())
            session.cwd = params.value(QStringLiteral("cwd")).toString();
        session.options = params;
        callback({{QStringLiteral("thread"), QJsonObject{{QStringLiteral("id"), sessionId},
                                                         {QStringLiteral("turns"), QJsonArray{}}}}},
                 {});
        return;
    }
    if (method == QStringLiteral("thread/turns/list")) {
        callback({{QStringLiteral("data"), QJsonArray{}}}, {});
        return;
    }
    if (method == QStringLiteral("thread/read")) {
        callback({{QStringLiteral("thread"),
                   QJsonObject{{QStringLiteral("id"), params.value(QStringLiteral("threadId"))},
                               {QStringLiteral("turns"), QJsonArray{}}}}},
                 {});
        return;
    }
    if (method == QStringLiteral("thread/name/set")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto found = daemonSessions_.find(sessionId);
        if (found == daemonSessions_.end()) {
            callback({}, QStringLiteral("Unknown daemon session."));
            return;
        }
        daemon_->call(QStringLiteral("session.rename"),
                      {{QStringLiteral("title"), params.value(QStringLiteral("name"))}}, sessionId,
                      found->generation, std::move(callback));
        return;
    }
    if (method == QStringLiteral("thread/delete")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto found = daemonSessions_.find(sessionId);
        if (found == daemonSessions_.end()) {
            callback({}, QStringLiteral("Unknown daemon session."));
            return;
        }
        const qint64 generation = found->generation;
        daemon_->call(QStringLiteral("session.delete"), {}, sessionId, generation,
                      [this, sessionId, callback = std::move(callback)](
                          const QJsonObject& result, const QString& error) mutable {
                          if (error.isEmpty())
                              daemonSessions_.remove(sessionId);
                          callback(result, error);
                      });
        return;
    }
    if (method == QStringLiteral("thread/fork")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto found = daemonSessions_.find(sessionId);
        if (found == daemonSessions_.end()) {
            callback({}, QStringLiteral("Unknown daemon session."));
            return;
        }
        if (daemon_->isReady() && !daemon_->hasFeature(QStringLiteral("session.fork"))) {
            callback({}, QStringLiteral("KetPlus Daemon does not support %1 yet.").arg(method));
            return;
        }
        // The client names the fork so a retried request cannot fork twice.
        const QString forkId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        daemon_->call(
            QStringLiteral("session.fork"), {{QStringLiteral("fork_session_id"), forkId}},
            sessionId, found->generation,
            [this, sessionId, callback = std::move(callback)](const QJsonObject& result,
                                                              const QString& error) {
                if (!error.isEmpty()) {
                    callback({}, error);
                    return;
                }
                const QString forkId = result.value(QStringLiteral("session_id")).toString();
                DaemonSession fork;
                if (const auto source = daemonSessions_.constFind(sessionId);
                    source != daemonSessions_.cend())
                    fork.options = source->options;
                fork.options.insert(QStringLiteral("provider"),
                                    result.value(QStringLiteral("adapter")));
                fork.cwd =
                    normalizedLocalPath(result.value(QStringLiteral("worktree_path")).toString());
                fork.generation = result.value(QStringLiteral("session_generation")).toInteger(1);
                // The copied transcript is read through the transcript window when it is shown.
                fork.cursor = result.value(QStringLiteral("last_event_cursor")).toInteger();
                fork.windowReady = !daemonHasTranscriptWindow_;
                daemonSessions_.insert(forkId, fork);
                emit daemonSessionDiscovered(result);
                callback({{QStringLiteral("thread"),
                           QJsonObject{{QStringLiteral("id"), forkId},
                                       {QStringLiteral("forkedFromId"), sessionId}}}},
                         {});
            });
        return;
    }
    if (method == QStringLiteral("turn/start")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto found = daemonSessions_.find(sessionId);
        if (found == daemonSessions_.end() || found->cwd.isEmpty()) {
            callback({}, QStringLiteral("The daemon session has no worktree."));
            return;
        }
        QString prompt;
        for (const QJsonValue& value : params.value(QStringLiteral("input")).toArray()) {
            const QJsonObject input = value.toObject();
            if (input.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
                prompt = input.value(QStringLiteral("text")).toString();
                break;
            }
        }
        QJsonObject options;
        for (const QString& key : {QStringLiteral("model"), QStringLiteral("effort")}) {
            QString value = params.value(key).toString();
            if (value.isEmpty()) {
                value = found->options.value(key).toString();
            }
            if (!value.isEmpty()) {
                options.insert(key, value);
            }
        }
        const QString sandboxType = params.value(QStringLiteral("sandboxPolicy"))
                                        .toObject()
                                        .value(QStringLiteral("type"))
                                        .toString();
        options.insert(QStringLiteral("permission"),
                       sandboxType == QStringLiteral("readOnly") ? QStringLiteral("read-only")
                       : sandboxType == QStringLiteral("dangerFullAccess")
                           ? QStringLiteral("danger-full-access")
                           : QStringLiteral("workspace-write"));
        const QString provider = params.value(QStringLiteral("provider"))
                                     .toString(found->options.value(QStringLiteral("provider"))
                                                   .toString(QStringLiteral("codex")));
        if (!provider.isEmpty())
            options.insert(QStringLiteral("provider"), provider);
        found->turnStartInFlight = true;
        daemon_->call(
            QStringLiteral("session.start_turn"),
            {{QStringLiteral("worktree_path"), found->cwd},
             {QStringLiteral("expected_context_revision"), found->contextRevision},
             {QStringLiteral("prompt"), prompt},
             {QStringLiteral("display_text"), params.value(QStringLiteral("displayText"))},
             {QStringLiteral("client_message_id"), params.value(QStringLiteral("clientMessageId"))},
             {QStringLiteral("input"), params.value(QStringLiteral("input"))},
             {QStringLiteral("options"), options}},
            sessionId, found->generation,
            [this, sessionId, callback = std::move(callback)](const QJsonObject& result,
                                                              const QString& error) {
                auto found = daemonSessions_.find(sessionId);
                if (found != daemonSessions_.end()) {
                    found->turnStartInFlight = false;
                    if (!error.isEmpty())
                        found->interruptRequested = false;
                    else if (std::exchange(found->interruptRequested, false))
                        daemon_->call(QStringLiteral("session.interrupt"), {}, sessionId,
                                      found->generation, [](const QJsonObject&, const QString&) {});
                }
                if (!error.isEmpty()) {
                    callback({}, error);
                    return;
                }
                if (found == daemonSessions_.end()) {
                    callback({}, QStringLiteral("The daemon session disappeared."));
                    return;
                }
                found->generation =
                    result.value(QStringLiteral("session_generation")).toInteger(found->generation);
                found->cursor = qMax(found->cursor,
                                     result.value(QStringLiteral("last_event_cursor")).toInteger());
                if (const QString number = result.value(QStringLiteral("number")).toString();
                    !number.isEmpty())
                    emit notificationReceived(QStringLiteral("daemon/conversationNumber"),
                                              {{QStringLiteral("threadId"), sessionId},
                                               {QStringLiteral("number"), number}});
                // The daemon has only queued the turn. Report the real id (possibly empty) so a
                // Stop pressed now stays pending until turn.started instead of interrupting a
                // fabricated turn that the daemon rejects.
                callback(
                    {{QStringLiteral("turn"), QJsonObject{{QStringLiteral("id"), found->turnId}}}},
                    {});
            });
        return;
    }
    if (method == QStringLiteral("turn/steer")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto found = daemonSessions_.find(sessionId);
        if (found == daemonSessions_.end()) {
            callback({}, QStringLiteral("Unknown daemon session."));
            return;
        }
        QString prompt;
        const QJsonArray input = params.value(QStringLiteral("input")).toArray();
        if (!input.isEmpty()) {
            prompt = input.first().toObject().value(QStringLiteral("text")).toString();
        }
        // The whole input goes along: the daemon hands its images to the provider.
        daemon_->call(
            QStringLiteral("session.steer"),
            {{QStringLiteral("prompt"), prompt},
             {QStringLiteral("display_text"), params.value(QStringLiteral("displayText"))},
             {QStringLiteral("input"), input},
             {"expected_context_revision", found->contextRevision}},
            sessionId, found->generation, std::move(callback));
        return;
    }
    if (method == QStringLiteral("turn/interrupt")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto found = daemonSessions_.find(sessionId);
        if (found == daemonSessions_.end()) {
            callback({}, QStringLiteral("Unknown daemon session."));
            return;
        }
        if (found->turnStartInFlight) {
            found->interruptRequested = true;
            callback({{QStringLiteral("accepted"), true}}, {});
            return;
        }
        daemon_->call(QStringLiteral("session.interrupt"), {}, sessionId, found->generation,
                      std::move(callback));
        return;
    }
    if (method == QStringLiteral("task/stop") || method == QStringLiteral("task/output")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto found = daemonSessions_.find(sessionId);
        if (found == daemonSessions_.end()) {
            callback({}, QStringLiteral("Unknown daemon session."));
            return;
        }
        daemon_->call(method == QStringLiteral("task/stop") ? QStringLiteral("session.task.stop")
                                                            : QStringLiteral("session.task.output"),
                      {{QStringLiteral("task_id"), params.value(QStringLiteral("taskId"))}},
                      sessionId, found->generation, std::move(callback));
        return;
    }
    callback({}, QStringLiteral("KetPlus Daemon does not support %1 yet.").arg(method));
}

qint64 CodexAppServer::daemonSessionCursor(const QString& sessionId, qint64 generation) const {
    const auto found = daemonSessions_.constFind(sessionId);
    return found != daemonSessions_.cend() && found->generation == generation ? found->cursor : 0;
}

void CodexAppServer::markDaemonSessionSeen(const QString& sessionId, const qint64 cursor,
                                           ResponseCallback callback) {
    if (!daemon_ || sessionId.isEmpty()) {
        callback({}, QStringLiteral("Daemon session marking is unavailable."));
        return;
    }
    auto found = daemonSessions_.find(sessionId);
    if (found == daemonSessions_.end()) {
        callback({}, QStringLiteral("Unknown daemon session."));
        return;
    }
    QJsonObject payload;
    if (cursor >= 0)
        payload.insert(QStringLiteral("cursor"), cursor);
    daemon_->call(QStringLiteral("session.mark_seen"), payload, sessionId, found->generation,
                  std::move(callback));
}

void CodexAppServer::refreshDaemonSessions() {
    daemonSessionListInFlight_ = true;
    restoringDaemonSessions_.clear();
    daemonRestoreQueue_.clear();
    activeDaemonRestoreSession_.clear();
    setRestoring(true);
    daemon_->call(
        QStringLiteral("session.list"), {}, {}, -1,
        [this](const QJsonObject& result, const QString& error) {
            daemonSessionListInFlight_ = false;
            QTimer::singleShot(0, this, &CodexAppServer::discoverDaemonSessions);
            if (!error.isEmpty()) {
                setRestoring(false);
                return;
            }
            daemonHasByteBoundedReplay_ =
                result.value(QStringLiteral("replay_page_max_bytes")).toInteger() > 0;
            daemonHasTranscriptWindow_ = result.value("transcript_window_version").toInt() == 1;
            daemonHasTranscriptAfterCursor_ =
                result.value("transcript_window_after_cursor").toBool(false);
            daemonTranscriptWindowMaxTurns_ =
                std::clamp(result.value("transcript_window_max_turns").toInt(3), 1, 20);
            daemonHasLargeEvents_ = result.value("transcript_large_event_version").toInt() == 1;
            QStringList sessionIds;
            for (const auto& value : result.value("deleted_sessions").toArray()) {
                const auto deleted = value.toObject();
                const auto id = deleted.value("session_id").toString();
                const auto generation = deleted.value("session_generation").toInteger();
                const auto found = daemonSessions_.constFind(id);
                if (found != daemonSessions_.cend() && found->generation == generation)
                    daemonSessions_.remove(id);
                emit daemonSessionDeleted(id, generation);
            }
            for (const QJsonValue& value : result.value(QStringLiteral("sessions")).toArray()) {
                const QJsonObject stored = value.toObject();
                const QString sessionId = stored.value(QStringLiteral("session_id")).toString();
                if (sessionId.isEmpty()) {
                    continue;
                }
                sessionIds.append(sessionId);
                if (!daemonHasTranscriptWindow_)
                    restoringDaemonSessions_.insert(sessionId);
                auto& session = daemonSessions_[sessionId];
                const auto nextGeneration = stored.value("session_generation").toInteger(1);
                if (session.generation != nextGeneration) {
                    session.windowCursor = 0;
                    session.deferredCursor = 0;
                    for (auto it = daemonApprovals_.begin(); it != daemonApprovals_.end();) {
                        if (it->sessionId == sessionId)
                            it = daemonApprovals_.erase(it);
                        else
                            ++it;
                    }
                }
                session.cwd =
                    normalizedLocalPath(stored.value(QStringLiteral("worktree_path")).toString());
                session.contextRevision = stored.value("context_revision").toInteger(1);
                const auto effective = stored.value("effective").toObject();
                if (!effective.isEmpty()) {
                    session.options.insert("model", effective.value("model"));
                    session.options.insert("effort", effective.value("effort"));
                }
                session.generation =
                    stored.value(QStringLiteral("session_generation")).toInteger(1);
                if (daemonHasTranscriptWindow_) {
                    session.windowReady = false;
                    session.cursor = stored.value("last_event_cursor").toInteger();
                }
                emit daemonSessionDiscovered(stored);
                emit notificationReceived(
                    QStringLiteral("thread/backgroundTasks/updated"),
                    {{QStringLiteral("threadId"), sessionId},
                     {QStringLiteral("tasks"), stored.value(QStringLiteral("background_tasks"))}});
                if (!daemonHasTranscriptWindow_)
                    emit daemonSessionRestoreStarted(sessionId);
            }
            if (daemonHasTranscriptWindow_) {
                setRestoring(false);
                return;
            }
            daemonRestoreQueue_ = sessionIds;
            reorderDaemonRestoreQueue();
            restoreNextDaemonSession();
            if (restoringDaemonSessions_.isEmpty()) {
                setRestoring(false);
            }
        });
}

void CodexAppServer::reorderDaemonRestoreQueue() {
    if (daemonRestoreQueue_.size() < 2) {
        return;
    }
    QStringList exact;
    QStringList sameWorktree;
    QStringList background;
    for (const QString& sessionId : std::exchange(daemonRestoreQueue_, {})) {
        if (!daemonRestorePrioritySession_.isEmpty() &&
            sessionId == daemonRestorePrioritySession_) {
            exact.append(sessionId);
        } else if (!daemonRestorePriorityPath_.isEmpty() &&
                   daemonSessions_.value(sessionId).cwd == daemonRestorePriorityPath_) {
            sameWorktree.append(sessionId);
        } else {
            background.append(sessionId);
        }
    }
    daemonRestoreQueue_ = exact + sameWorktree + background;
}

void CodexAppServer::restoreNextDaemonSession() {
    if (daemonSessionListInFlight_ || !activeDaemonRestoreSession_.isEmpty()) {
        return;
    }
    while (!daemonRestoreQueue_.isEmpty()) {
        const QString sessionId = daemonRestoreQueue_.takeFirst();
        if (!restoringDaemonSessions_.contains(sessionId) || !daemonSessions_.contains(sessionId)) {
            restoringDaemonSessions_.remove(sessionId);
            continue;
        }
        activeDaemonRestoreSession_ = sessionId;
        attachDaemonSession(sessionId);
        return;
    }
    if (restoringDaemonSessions_.isEmpty()) {
        setRestoring(false);
    }
}

void CodexAppServer::attachDaemonSession(const QString& sessionId) {
    if (restoringDaemonSessions_.contains(sessionId) && activeDaemonRestoreSession_ != sessionId) {
        return;
    }
    auto found = daemonSessions_.find(sessionId);
    if (found == daemonSessions_.end() || found->replayRequestInFlight) {
        return;
    }
    if (!found->replaying) {
        found->replaying = true;
        found->replayCursor = found->cursor;
    }
    found->replayRequestInFlight = true;
    const qint64 after = found->replayCursor;
    daemon_->call(
        QStringLiteral("session.attach"),
        {{QStringLiteral("after_cursor"), after},
         {QStringLiteral("compact_completed"), true},
         {QStringLiteral("limit"),
          daemonHasByteBoundedReplay_ ? restoreReplayBatchSize : legacyRestoreReplayBatchSize}},
        sessionId, found->generation,
        [this, sessionId](const QJsonObject& result, const QString& error) {
            auto found = daemonSessions_.find(sessionId);
            if (found == daemonSessions_.end()) {
                finishDaemonReplay(sessionId);
                return;
            }
            found->replayRequestInFlight = false;
            if (!error.isEmpty()) {
                const bool pruned = result.value(QStringLiteral("error_code")).toString() ==
                                        QStringLiteral("cursor_expired") ||
                                    result.value(QStringLiteral("events_pruned")).toBool();
                if (pruned) {
                    // Retention removed events after our cursor. Replaying from it would leave a
                    // silent hole, so drop the stale cursor and any events held behind it; the
                    // session manager reloads the transcript from scratch.
                    const qint64 earliest =
                        result.value(QStringLiteral("earliest_available_cursor")).toInteger(1);
                    found->cursor = qMax<qint64>(0, earliest - 1);
                    found->replayCursor = found->cursor;
                    found->deferredEvents.clear();
                    found->deferredBytes = 0;
                    found->windowReady = false;
                }
                finishDaemonReplay(sessionId);
                emit daemonReplayFailed(sessionId, error, pruned);
                return;
            }
            const QJsonArray events = result.value(QStringLiteral("events")).toArray();
            qint64 pageCursor = found->replayCursor;
            for (const QJsonValue& value : events) {
                const QJsonObject event = value.toObject();
                pageCursor =
                    qMax(pageCursor, event.value(QStringLiteral("cursor")).toInteger(pageCursor));
                dispatchDaemonEvent(
                    {{QStringLiteral("session_id"), event.value(QStringLiteral("session_id"))},
                     {QStringLiteral("session_generation"),
                      event.value(QStringLiteral("session_generation"))},
                     {QStringLiteral("cursor"), event.value(QStringLiteral("cursor"))},
                     {QStringLiteral("payload"),
                      QJsonObject{
                          {QStringLiteral("event_type"), event.value(QStringLiteral("event_type"))},
                          {QStringLiteral("data"), event.value(QStringLiteral("payload"))}}}});
            }
            found = daemonSessions_.find(sessionId);
            if (found == daemonSessions_.end()) {
                finishDaemonReplay(sessionId);
                return;
            }
            // An active daemon from an older product build does not return the
            // compact-replay cursor. Fall back to the final event in its page
            // so upgrade deferral cannot leave Desktop in an attach loop.
            found->replayCursor =
                qMax(found->replayCursor,
                     result.value(QStringLiteral("next_event_cursor")).toInteger(pageCursor));
            found->cursor = qMax(found->cursor, found->replayCursor);
            if (found->replayCursor <
                result.value(QStringLiteral("last_event_cursor")).toInteger()) {
                QTimer::singleShot(0, this, [this, sessionId] { attachDaemonSession(sessionId); });
            } else {
                finishDaemonReplay(sessionId);
            }
        });
}

void CodexAppServer::consumeDaemonEvent(const QJsonObject& envelope) {
    const QString sessionId = envelope.value(QStringLiteral("session_id")).toString();
    auto found = daemonSessions_.find(sessionId);
    if (found == daemonSessions_.end()) {
        queueDaemonDiscovery(envelope);
        return;
    }
    if (envelope.value("session_generation").toInteger(found->generation) != found->generation)
        return;
    if (envelope.value("payload").toObject().value("event_type") == "session.deleted") {
        dispatchDaemonEvent(envelope);
        return;
    }
    if (found->replaying) {
        const qint64 cursor = envelope.value(QStringLiteral("cursor")).toInteger();
        found->deferredCursor = std::max(found->deferredCursor, cursor);
        if (daemonHasTranscriptWindow_) {
            if (found->windowOverflow || found->deferredEvents.contains(cursor))
                return;
            const auto bytes = QJsonDocument(envelope).toJson(QJsonDocument::Compact).size();
            if (found->deferredEvents.size() >= 128 || found->deferredBytes + bytes > 512 * 1024) {
                found->windowOverflow = true;
                found->deferredEvents.clear();
                found->deferredBytes = 0;
                return;
            }
            found->deferredBytes += bytes;
        }
        found->deferredEvents.insert(cursor, envelope);
        return;
    }
    dispatchDaemonEvent(envelope);
}

void CodexAppServer::finishDaemonReplay(const QString& sessionId) {
    restoringDaemonSessions_.remove(sessionId);
    const bool scheduledRestore = activeDaemonRestoreSession_ == sessionId;
    if (scheduledRestore) {
        activeDaemonRestoreSession_.clear();
    }
    auto found = daemonSessions_.find(sessionId);
    if (found == daemonSessions_.end()) {
        emit daemonSessionRestoreFinished(sessionId);
        if (scheduledRestore) {
            QTimer::singleShot(0, this, &CodexAppServer::restoreNextDaemonSession);
        } else if (!daemonSessionListInFlight_ && restoringDaemonSessions_.isEmpty()) {
            setRestoring(false);
        }
        return;
    }
    const auto deferred = std::exchange(found->deferredEvents, {});
    found->replaying = false;
    found->replayRequestInFlight = false;
    for (const QJsonObject& event : deferred) {
        dispatchDaemonEvent(event);
    }
    emit daemonSessionRestoreFinished(sessionId);
    if (scheduledRestore) {
        QTimer::singleShot(0, this, &CodexAppServer::restoreNextDaemonSession);
    } else if (!daemonSessionListInFlight_ && restoringDaemonSessions_.isEmpty()) {
        setRestoring(false);
    }
}

void CodexAppServer::dispatchDaemonEvent(const QJsonObject& envelope) {
    const QString sessionId = envelope.value(QStringLiteral("session_id")).toString();
    auto found = daemonSessions_.find(sessionId);
    if (found == daemonSessions_.end()) {
        return;
    }
    const qint64 generation =
        envelope.value(QStringLiteral("session_generation")).toInteger(found->generation);
    const qint64 cursor = envelope.value(QStringLiteral("cursor")).toInteger();
    if (generation != found->generation || cursor <= found->cursor) {
        return;
    }
    found->cursor = cursor;
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    const QString type = payload.value(QStringLiteral("event_type")).toString();
    const QJsonObject data = payload.value(QStringLiteral("data")).toObject();
    if (type == "session.deleted") {
        daemonSessions_.remove(sessionId);
        emit daemonSessionDeleted(sessionId, generation);
        return;
    }
    if (type == "session.configuration_changed") {
        const auto effective = data.value("effective").toObject();
        found->options.insert("model", effective.value("model"));
        found->options.insert("effort", effective.value("effort"));
        QJsonObject settings = data;
        settings.insert("threadId", sessionId);
        emit notificationReceived("daemon/configurationChanged", settings);
        return;
    }
    if (type == "conversation.context_changed") {
        found->contextRevision = data.value("context_revision").toInteger(found->contextRevision);
        executionWorktreeCommand(sessionId, "session.worktrees", {{"limit", 1}},
                                 [](const QJsonObject&, const QString&) {});
        return;
    }
    if (type == QStringLiteral("user.message")) {
        // Every client lists conversations by the daemon's turn start, including background
        // ones whose transcript events are skipped below.
        emit notificationReceived(QStringLiteral("daemon/turnStartedAt"),
                                  {{QStringLiteral("threadId"), sessionId},
                                   {QStringLiteral("startedAt"), envelope.value("sent_at")}});
        // The sidebar names the conversation whose message started this turn, if any.
        QJsonObject sender;
        if (data.value(QStringLiteral("origin")).toString() == QStringLiteral("agent")) {
            const QJsonObject first =
                data.value(QStringLiteral("agent_messages")).toArray().first().toObject();
            sender = {{QStringLiteral("number"), first.value(QStringLiteral("from_number"))},
                      {QStringLiteral("title"), first.value(QStringLiteral("from_title"))},
                      {QStringLiteral("kind"), first.value(QStringLiteral("kind"))}};
        }
        emit notificationReceived(
            QStringLiteral("daemon/agentTurn"),
            {{QStringLiteral("threadId"), sessionId}, {QStringLiteral("sender"), sender}});
    }
    if (type == QStringLiteral("session.renamed")) {
        emit notificationReceived(
            QStringLiteral("thread/name/updated"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("threadName"), data.value(QStringLiteral("title"))},
             {QStringLiteral("customTitle"), data.value(QStringLiteral("custom_title")).toBool()}});
        return;
    }
    if (type == QStringLiteral("usage.context")) {
        emit notificationReceived(
            QStringLiteral("thread/tokenUsage/updated"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("tokenUsage"), data.value(QStringLiteral("tokenUsage"))}});
        return;
    }
    if (type == QStringLiteral("tasks.changed")) {
        emit notificationReceived(QStringLiteral("thread/backgroundTasks/updated"),
                                  {{QStringLiteral("threadId"), sessionId},
                                   {QStringLiteral("tasks"), data.value(QStringLiteral("tasks"))}});
        return;
    }
    if (type == QStringLiteral("usage.session")) {
        emit notificationReceived(QStringLiteral("thread/sessionUsage/updated"),
                                  {{"threadId", sessionId}, {"usage", data}});
        return;
    }
    if (type == QStringLiteral("usage.limits")) {
        emit notificationReceived(
            QStringLiteral("thread/planLimits/updated"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("limits"), data.value(QStringLiteral("limits"))}});
        return;
    }
    if (daemonHasTranscriptWindow_ && envelope.contains("turn_sequence")) {
        emit notificationReceived("daemon/eventScope",
                                  {{"threadId", sessionId},
                                   {"turnSequence", envelope.value("turn_sequence")},
                                   {"cursor", cursor}});
    }
    // Sidebar attention is metadata: retain it even when the transcript window is inactive.
    if (type == "approval.requested" || type == "approval.resolved" || type == "approval.expired" ||
        type == "question.answered" || type == "question.expired") {
        auto id = data.value("approval_id").toString();
        if (id.isEmpty())
            id = data.value("request_id").toString();
        emit notificationReceived("daemon/inputPending",
                                  {{"threadId", sessionId},
                                   {"requestId", id},
                                   {"pending", type == "approval.requested"}});
    }
    // Inactive conversations retain metadata, not a continuously growing copy of
    // their transcript. The next selection catches up the retained journal window.
    if (daemonHasTranscriptWindow_ &&
        (!found->windowReady || sessionId != daemonRestorePrioritySession_) &&
        (type == "assistant.delta" || type == "item.started" || type == "item.completed" ||
         type == "user.message" || type == "command.output_delta" || type == "approval.requested" ||
         type == "activity.group.summary")) {
        if (found->windowReady) {
            found->windowReady = false;
            emit daemonTranscriptInvalidated(sessionId);
        }
        return;
    }
    if (found->windowReady && sessionId == daemonRestorePrioritySession_)
        found->windowCursor = cursor;
    const QJsonObject largeEvent = data.value(QStringLiteral("large_event")).toObject();
    if (!largeEvent.isEmpty()) {
        const quint64 epoch = found->windowEpoch;
        QJsonObject event{
            {QStringLiteral("session_id"), sessionId},
            {QStringLiteral("session_generation"), generation},
            {QStringLiteral("cursor"), cursor},
            {QStringLiteral("turn_sequence"), envelope.value(QStringLiteral("turn_sequence"))},
            {QStringLiteral("event_type"), type},
            {QStringLiteral("payload"), data}};
        resolveDaemonLargeEvents(
            sessionId, generation, epoch,
            {{QStringLiteral("events"), QJsonArray{event}},
             {QStringLiteral("partial_items"), QJsonArray{}}},
            [this, sessionId, type](const QJsonObject& page, const QString& failure) {
                if (!failure.isEmpty()) {
                    emit notificationReceived(QStringLiteral("error"),
                                              {{QStringLiteral("threadId"), sessionId},
                                               {QStringLiteral("error"), failure}});
                    return;
                }
                const QJsonObject resolved = page.value(QStringLiteral("events"))
                                                 .toArray()
                                                 .first()
                                                 .toObject()
                                                 .value(QStringLiteral("payload"))
                                                 .toObject();
                const QJsonObject item = resolved.value(QStringLiteral("item")).toObject();
                if (item.isEmpty()) {
                    emit notificationReceived(
                        QStringLiteral("error"),
                        {{QStringLiteral("threadId"), sessionId},
                         {QStringLiteral("error"),
                          QStringLiteral("Reassembled transcript item is missing its item.")}});
                    return;
                }
                emit notificationReceived(
                    type == QStringLiteral("item.started") ? QStringLiteral("item/started")
                                                           : QStringLiteral("item/completed"),
                    {{QStringLiteral("threadId"), sessionId}, {QStringLiteral("item"), item}});
            });
        return;
    }
    if (type == QStringLiteral("user.message")) {
        // The session manager pairs the echo with the prompt it already shows by client message
        // id. A per-session "locally submitted" flag was cleared by unrelated events, and the
        // echo was then appended as a second copy of the same prompt.
        emit notificationReceived(
            QStringLiteral("daemon/userMessage"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), agent_message::daemonUserItemId(cursor)},
             {QStringLiteral("clientMessageId"), data.value(QStringLiteral("client_message_id"))},
             {QStringLiteral("text"), data.value(QStringLiteral("text"))},
             {QStringLiteral("images"), data.value(QStringLiteral("images"))},
             {QStringLiteral("agentMessages"), data.value(QStringLiteral("agent_messages"))}});
        return;
    }
    if (type == QStringLiteral("turn.steered") &&
        data.value(QStringLiteral("origin")).toString() == QStringLiteral("agent")) {
        // ADR 0027: an urgent agent message steered the running turn; show it as it arrives.
        emit notificationReceived(
            QStringLiteral("daemon/userMessage"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), QStringLiteral("steer:%1").arg(cursor)},
             {QStringLiteral("text"), data.value(QStringLiteral("text"))},
             {QStringLiteral("agentMessages"), data.value(QStringLiteral("agent_messages"))}});
        return;
    }
    if (type == QStringLiteral("agent_message.state")) {
        emit notificationReceived(
            QStringLiteral("daemon/providerNotice"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), agent_message::daemonNoticeItemId(cursor)},
             {QStringLiteral("title"), agent_message::agentMessageNoticeTitle()},
             {QStringLiteral("text"), agent_message::agentMessageStateText(data)}});
        return;
    }
    if (type == QStringLiteral("turn.started")) {
        found->turnId = data.value(QStringLiteral("turn_id")).toString();
        emit notificationReceived(
            QStringLiteral("turn/started"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("turn"), QJsonObject{{QStringLiteral("id"), found->turnId}}}});
        return;
    }
    if (type == "activity.group.summary") {
        emit notificationReceived("daemon/activityGroup",
                                  {{"threadId", sessionId},
                                   {"turnSequence", envelope.value("turn_sequence")},
                                   {"summary", data}});
        return;
    }
    if (type == QStringLiteral("assistant.delta")) {
        emit notificationReceived(
            QStringLiteral("item/agentMessage/delta"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), data.value(QStringLiteral("item_id"))},
             {QStringLiteral("delta"), data.value(QStringLiteral("delta"))}});
        return;
    }
    if (type == QStringLiteral("command.output_delta")) {
        emit notificationReceived(
            QStringLiteral("item/commandExecution/outputDelta"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), data.value(QStringLiteral("item_id"))},
             {QStringLiteral("delta"), data.value(QStringLiteral("delta"))}});
        return;
    }
    if (type == QStringLiteral("item.started") || type == QStringLiteral("item.completed")) {
        QJsonObject item = data.value(QStringLiteral("item")).toObject();
        if (item.isEmpty()) {
            item = {{QStringLiteral("id"), data.value(QStringLiteral("item_id"))},
                    {QStringLiteral("type"), data.value(QStringLiteral("item_type"))},
                    {QStringLiteral("text"), data.value(QStringLiteral("text"))},
                    {QStringLiteral("command"), data.value(QStringLiteral("command"))},
                    {QStringLiteral("status"), data.value(QStringLiteral("status"))}};
        }
        emit notificationReceived(
            type == QStringLiteral("item.started") ? QStringLiteral("item/started")
                                                   : QStringLiteral("item/completed"),
            {{QStringLiteral("threadId"), sessionId}, {QStringLiteral("item"), item}});
        return;
    }
    if (type == QStringLiteral("turn.completed")) {
        emit notificationReceived(
            QStringLiteral("turn/completed"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("turn"),
              QJsonObject{{QStringLiteral("id"), found->turnId},
                          {QStringLiteral("status"), data.value(QStringLiteral("status"))}}}});
        found->turnId.clear();
        return;
    }
    if (type == QStringLiteral("turn.steer_failed") ||
        type == QStringLiteral("turn.interrupt_failed")) {
        // The turn is still running; record the failure without ending it.
        emit notificationReceived(
            QStringLiteral("error"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("nonFatal"), true},
             {QStringLiteral("replayed"), found->replaying},
             {QStringLiteral("error"),
              QJsonObject{{QStringLiteral("message"), data.value(QStringLiteral("message"))}}}});
        return;
    }
    if (type == QStringLiteral("runtime.reloaded"))
        return; // a planned provider reload is not a failure
    if (type == QStringLiteral("provider.request_declined")) {
        // Informational: the turn keeps running, so this must not become an "error".
        emit notificationReceived(
            QStringLiteral("daemon/providerNotice"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), agent_message::daemonNoticeItemId(cursor)},
             {QStringLiteral("text"), agent_message::providerRequestDeclinedText(data)}});
        return;
    }
    if (type == QStringLiteral("provider.protocol_error")) {
        // ADR 0015 §5: a malformed message was skipped; the turn keeps running.
        emit notificationReceived(
            QStringLiteral("daemon/providerNotice"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), agent_message::daemonNoticeItemId(cursor)},
             {QStringLiteral("title"), agent_message::providerProtocolErrorTitle()},
             {QStringLiteral("text"), agent_message::providerProtocolErrorText(data)}});
        return;
    }
    if (type == QStringLiteral("provider.runtime")) {
        // ADR 0015: the CLI runs, but its version is untested or could not be read.
        emit notificationReceived(
            QStringLiteral("daemon/providerNotice"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("itemId"), agent_message::daemonNoticeItemId(cursor)},
             {QStringLiteral("title"), agent_message::providerRuntimeNoticeTitle()},
             {QStringLiteral("text"), data.value(QStringLiteral("message"))}});
        return;
    }
    if (type == QStringLiteral("runtime.error") || type == QStringLiteral("runtime.stopped")) {
        emit notificationReceived(
            QStringLiteral("error"),
            {{QStringLiteral("threadId"), sessionId},
             // Replayed history must not raise old errors again as new failures.
             {QStringLiteral("replayed"), found->replaying},
             {QStringLiteral("error"),
              QJsonObject{{QStringLiteral("code"), data.value(QStringLiteral("code"))},
                          {QStringLiteral("message"), data.value(QStringLiteral("message"))}}}});
        return;
    }
    if (type == QStringLiteral("approval.requested")) {
        dispatchDaemonApproval(sessionId, data);
    }
    if (type == "approval.resolved" || type == "approval.expired" || type == "question.answered" ||
        type == "question.expired") {
        QString approvalId = data.value("approval_id").toString();
        if (approvalId.isEmpty())
            approvalId = data.value("request_id").toString();
        if (found->approvalSyncInFlight)
            found->settledApprovals.insert(approvalId);
        daemonApprovals_.remove(approvalId);
        emit notificationReceived(
            QStringLiteral("userInput/resolved"),
            {{QStringLiteral("threadId"), sessionId},
             {QStringLiteral("requestId"), approvalId},
             {QStringLiteral("expired"), type.endsWith(QStringLiteral("expired"))}});
        emit notificationReceived("daemon/approvalEnded",
                                  {{"threadId", sessionId}, {"approvalId", approvalId}});
    }
}

void CodexAppServer::dispatchDaemonApproval(const QString& sessionId, const QJsonObject& data) {
    const auto found = daemonSessions_.constFind(sessionId);
    if (found == daemonSessions_.cend())
        return;
    if (daemonHasTranscriptWindow_ &&
        (!found->windowReady || sessionId != daemonRestorePrioritySession_))
        return;
    const QString approvalId = data.value(QStringLiteral("approval_id")).toString();
    if (found->settledApprovals.contains(approvalId))
        return;
    if (approvalId.isEmpty() || daemonApprovals_.contains(approvalId))
        return;
    const QString kind = data.value(QStringLiteral("kind")).toString();
    QJsonArray questions = data.value(QStringLiteral("questions")).toArray();
    if (kind != QStringLiteral("input")) {
        QString question = data.value(QStringLiteral("reason")).toString();
        const QString command = data.value(QStringLiteral("command")).toString();
        if (!command.isEmpty()) {
            question =
                question.isEmpty() ? command : QStringLiteral("%1\n\n%2").arg(question, command);
        }
        questions = QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("decision")},
            {QStringLiteral("header"), QStringLiteral("Approval")},
            {QStringLiteral("question"),
             question.isEmpty() ? QStringLiteral("Allow this Codex action?") : question},
            {QStringLiteral("options"),
             QJsonArray{
                 QJsonObject{{QStringLiteral("label"), QStringLiteral("Accept")},
                             {QStringLiteral("description"), QStringLiteral("Allow this action")}},
                 QJsonObject{
                     {QStringLiteral("label"), QStringLiteral("Decline")},
                     {QStringLiteral("description"), QStringLiteral("Block this action")}}}}}};
    }
    daemonApprovals_.insert(
        approvalId, {sessionId, kind, data.value(QStringLiteral("revision")).toInt(1), questions});
    emit userInputRequested(
        approvalId,
        {{QStringLiteral("threadId"), sessionId},
         {QStringLiteral("turnId"), found->turnId},
         {QStringLiteral("itemId"), QStringLiteral("approval:%1").arg(approvalId)},
         {QStringLiteral("approvalKind"),
          kind == QStringLiteral("input") ? QString{} : QStringLiteral("approval")},
         {QStringLiteral("approvalReason"), data.value(QStringLiteral("reason"))},
         {QStringLiteral("approvalCommand"), data.value(QStringLiteral("command"))},
         {QStringLiteral("isBlocking"), data.value(QStringLiteral("is_blocking")).toBool(true)},
         {QStringLiteral("questions"), questions}});
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

QVariantList CodexAppServer::permissionProfiles() {
    return permissionProfilesFor(QStringLiteral("codex"));
}

QVariantList CodexAppServer::permissionProfilesFor(const QString& provider) {
    if (provider == QStringLiteral("claude")) {
        return {QVariantMap{{"id", "read-only"},
                            {"label", "Read only"},
                            {"description", "Inspect files; Claude stays in plan mode"},
                            {"elevated", false}},
                QVariantMap{{"id", "workspace-write"},
                            {"label", "Workspace"},
                            {"description", "Edit project files; Claude asks before commands"},
                            {"elevated", false}},
                QVariantMap{{"id", "danger-full-access"},
                            {"label", "Full access"},
                            {"description", "Skip Claude permission prompts"},
                            {"elevated", true}}};
    }
    if (provider == QStringLiteral("cursor")) {
        return {QVariantMap{{"id", "read-only"},
                            {"label", "Read only"},
                            {"description", "Ask mode: inspect files without edits"},
                            {"elevated", false}},
                QVariantMap{{"id", "workspace-write"},
                            {"label", "Workspace"},
                            {"description", "Agent mode; Cursor asks before tools"},
                            {"elevated", false}},
                QVariantMap{{"id", "danger-full-access"},
                            {"label", "Full access"},
                            {"description", "Skip Cursor permission prompts"},
                            {"elevated", true}}};
    }
    return {QVariantMap{{"id", "read-only"},
                        {"label", "Read only"},
                        {"description", "Inspect files; edits are blocked"},
                        {"elevated", false}},
            QVariantMap{{"id", "workspace-write"},
                        {"label", "Workspace"},
                        {"description", "Edit project files; network is restricted"},
                        {"elevated", false}},
            QVariantMap{{"id", "danger-full-access"},
                        {"label", "Full access"},
                        {"description", "Access files and network without sandbox restrictions"},
                        {"elevated", true}}};
}

QJsonObject CodexAppServer::permissionParams(const QString& profile, bool turn) {
    const QString mode = profile == "read-only" || profile == "danger-full-access"
                             ? profile
                             : QStringLiteral("workspace-write");
    QJsonObject result{{"approvalPolicy", "never"}};
    if (turn) {
        const QString type = mode == "read-only"            ? "readOnly"
                             : mode == "danger-full-access" ? "dangerFullAccess"
                                                            : "workspaceWrite";
        result.insert("sandboxPolicy", QJsonObject{{"type", type}});
    } else
        result.insert("sandbox", mode);
    return result;
}

} // namespace ketplus
