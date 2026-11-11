#include "providers/ClaudeAdapter.h"
#include "BrowserAgentInstructions.h"
#include "BrowserToolLaunch.h"
#include "ComputerAgentInstructions.h"
#include "ComputerToolLaunch.h"
#include "providers/ClaudeCredentials.h"
#include "providers/ProviderFrameValidation.h"
#include "providers/ProviderResponseEnvelope.h"
#include "providers/ProviderResponseId.h"
#include "runtime/ResponseGuidance.h"
#include <QRegularExpression>

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

QJsonArray effortList() {
    return {QJsonObject{{QStringLiteral("id"), QStringLiteral("low")},
                        {QStringLiteral("label"), QStringLiteral("low")}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("medium")},
                        {QStringLiteral("label"), QStringLiteral("medium")}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("high")},
                        {QStringLiteral("label"), QStringLiteral("high")}}};
}

QJsonArray reasoningEfforts() {
    return {QJsonObject{{QStringLiteral("reasoningEffort"), QStringLiteral("low")}},
            QJsonObject{{QStringLiteral("reasoningEffort"), QStringLiteral("medium")}},
            QJsonObject{{QStringLiteral("reasoningEffort"), QStringLiteral("high")}}};
}

QJsonObject decorateClaudeModel(QJsonObject source) {
    const QString id = source.value(QStringLiteral("model"))
                           .toString(source.value(QStringLiteral("id")).toString());
    if (id.isEmpty())
        return {};
    const QString label = source.value(QStringLiteral("displayName"))
                              .toString(source.value(QStringLiteral("label")).toString(id));
    source.insert(QStringLiteral("id"), id);
    source.insert(QStringLiteral("model"), id);
    source.insert(QStringLiteral("label"), label);
    source.insert(QStringLiteral("displayName"), label);
    source.insert(QStringLiteral("hidden"), source.value(QStringLiteral("hidden")).toBool());
    source.insert(QStringLiteral("provider"), QStringLiteral("claude"));
    source.insert(QStringLiteral("permissions"), ClaudeAdapter::permissionProfiles());
    source.insert(QStringLiteral("defaultPermission"), QStringLiteral("workspace-write"));
    QJsonArray efforts = source.value(QStringLiteral("efforts")).toArray();
    if (efforts.isEmpty()) {
        for (const QJsonValue& value :
             source.value(QStringLiteral("supportedReasoningEfforts")).toArray()) {
            const QString effortId =
                value.toObject().value(QStringLiteral("reasoningEffort")).toString();
            if (!effortId.isEmpty())
                efforts.append(QJsonObject{{QStringLiteral("id"), effortId},
                                           {QStringLiteral("label"), effortId}});
        }
    }
    if (efforts.isEmpty())
        efforts = effortList();
    source.insert(QStringLiteral("efforts"), efforts);
    if (source.value(QStringLiteral("supportedReasoningEfforts")).toArray().isEmpty()) {
        QJsonArray reasoning;
        for (const QJsonValue& value : efforts) {
            const QString effortId = value.toObject().value(QStringLiteral("id")).toString();
            if (!effortId.isEmpty())
                reasoning.append(QJsonObject{{QStringLiteral("reasoningEffort"), effortId}});
        }
        source.insert(QStringLiteral("supportedReasoningEfforts"), reasoning);
    }
    if (!source.contains(QStringLiteral("defaultReasoningEffort")) &&
        !source.contains(QStringLiteral("default_effort"))) {
        source.insert(
            QStringLiteral("defaultReasoningEffort"),
            source.value(QStringLiteral("default_effort")).toString(QStringLiteral("medium")));
    }
    if (!source.contains(QStringLiteral("default_effort")))
        source.insert(QStringLiteral("default_effort"),
                      source.value(QStringLiteral("defaultReasoningEffort")).toString());
    return source;
}

} // namespace

ClaudeAdapter::ClaudeAdapter(const QString& executable, QObject* parent)
    : ProviderAdapter(parent), executable_(executable.isEmpty() ? findExecutable() : executable),
      process_(new QProcess(this)), restartTimer_(new QTimer(this)) {
    restartTimer_->setSingleShot(true);
    connect(restartTimer_, &QTimer::timeout, this, [this] {
        if (!queued_.isEmpty())
            ensureStarted();
    });
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    environment.insert(QStringLiteral("NODE_OPTIONS"), QString());
    process_->setProcessEnvironment(environment);

    connect(process_, &QProcess::started, this, &ClaudeAdapter::initialize);
    connect(process_, &QProcess::readyReadStandardOutput, this, &ClaudeAdapter::consumeStdout);
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        stderrBuffer_.append(
            QString::fromUtf8(process_->readAllStandardError())
                .replace(QRegularExpression(QStringLiteral("sk-ant-[A-Za-z0-9_-]+")),
                         QStringLiteral("[redacted]"))
                .toUtf8());
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
        noteUnexpectedStop();
        const QString reason =
            QStringLiteral("Unable to start the Claude sidecar: %1").arg(process_->errorString());
        failPending(reason);
        emit stopped(reason);
    });
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](const int, const QProcess::ExitStatus) {
                consumeStdout();
                starting_ = false;
                setReady(false);
                if (stopping_) {
                    stopping_ = false;
                    failPending(QStringLiteral("The Claude sidecar was stopped."));
                    return;
                }
                noteUnexpectedStop();
                if (malformedOutput_) {
                    const QString reason =
                        QStringLiteral("The Claude sidecar emitted a malformed protocol message.");
                    failPending(reason);
                    // Initialization already publishes its own failure; do not emit it twice.
                    if (!stopping_)
                        emit stopped(reason);
                    return;
                }
                const QString reason = QStringLiteral("The Claude sidecar exited unexpectedly.");
                failPending(reason);
                emit stopped(reason);
            });
}

ClaudeAdapter::~ClaudeAdapter() {
    // Owners destroy adapters at shutdown, after the SessionHost whose callbacks are still
    // pending; calling them here would use the freed host. Drop them instead of failing them.
    pending_.clear();
    queued_.clear();
    stop();
}

bool ClaudeAdapter::isAvailable() const { return !executable_.isEmpty(); }

QString ClaudeAdapter::missingSidecarMessage() {
    return QStringLiteral("Claude Agent SDK sidecar was not found.");
}

QString ClaudeAdapter::missingMessage() const { return missingSidecarMessage(); }

void ClaudeAdapter::runtimeStatus(std::function<void(const compat::Runtime&)> callback) {
    callback(compat::bundledClaude(executable_));
}

bool ClaudeAdapter::supports(const QString& capability) const {
    return compat::policy(QStringLiteral("claude")).baselineSupports(capability);
}

bool ClaudeAdapter::isReady() const { return ready_; }

QString ClaudeAdapter::executable() const { return executable_; }

QJsonArray ClaudeAdapter::modelCatalog() {
    const auto model = [](const QString& id, const QString& label, const QString& effort) {
        return decorateClaudeModel(
            QJsonObject{{QStringLiteral("id"), id},
                        {QStringLiteral("label"), label},
                        {QStringLiteral("efforts"), effortList()},
                        {QStringLiteral("default_effort"), effort},
                        {QStringLiteral("model"), id},
                        {QStringLiteral("displayName"), label},
                        {QStringLiteral("hidden"), false},
                        {QStringLiteral("provider"), QStringLiteral("claude")},
                        {QStringLiteral("defaultReasoningEffort"), effort},
                        {QStringLiteral("supportedReasoningEfforts"), reasoningEfforts()}});
    };
    return {
        model(QStringLiteral("opus"), QStringLiteral("Claude Opus"), QStringLiteral("high")),
        model(QStringLiteral("sonnet"), QStringLiteral("Claude Sonnet"), QStringLiteral("medium")),
        model(QStringLiteral("haiku"), QStringLiteral("Claude Haiku"), QStringLiteral("low"))};
}

QJsonArray ClaudeAdapter::permissionProfiles() {
    return {QJsonObject{{QStringLiteral("id"), QStringLiteral("read-only")},
                        {QStringLiteral("label"), QStringLiteral("Read only")},
                        {QStringLiteral("description"),
                         QStringLiteral("Inspect files; Claude stays in plan mode")},
                        {QStringLiteral("elevated"), false}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("workspace-write")},
                        {QStringLiteral("label"), QStringLiteral("Workspace")},
                        {QStringLiteral("description"),
                         QStringLiteral("Edit project files; Claude asks before commands")},
                        {QStringLiteral("elevated"), false}},
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("danger-full-access")},
                {QStringLiteral("label"), QStringLiteral("Full access")},
                {QStringLiteral("description"), QStringLiteral("Skip Claude permission prompts")},
                {QStringLiteral("elevated"), true}}};
}

QString ClaudeAdapter::permissionMode(const QString& profile) {
    if (profile == QStringLiteral("read-only"))
        return QStringLiteral("plan");
    if (profile == QStringLiteral("danger-full-access"))
        return QStringLiteral("bypassPermissions");
    return QStringLiteral("acceptEdits");
}

void ClaudeAdapter::call(const QString& method, const QJsonObject& params, Callback callback) {
    if (!isAvailable()) {
        callback({}, QStringLiteral("Claude Agent SDK sidecar was not found."));
        return;
    }
    Callback delivered = std::move(callback);
    if (method == QStringLiteral("model/list")) {
        delivered = [delivered](const QJsonObject& result, const QString& providerError) {
            QJsonArray data;
            if (providerError.isEmpty()) {
                for (const QJsonValue& value : result.value(QStringLiteral("data")).toArray()) {
                    const QJsonObject model = decorateClaudeModel(value.toObject());
                    if (!model.isEmpty())
                        data.append(model);
                }
            }
            if (data.isEmpty())
                data = ClaudeAdapter::modelCatalog();
            delivered(QJsonObject{{QStringLiteral("data"), data}}, {});
        };
    }
    if (!ready_) {
        queued_.append({method, params, std::move(delivered)});
        ensureStarted();
        return;
    }
    sendCall(method, params, std::move(delivered));
}

void ClaudeAdapter::configurationCatalog(Callback callback) {
    call(QStringLiteral("model/list"), {},
         [callback = std::move(callback)](const QJsonObject& result, const QString&) {
             QJsonArray catalog;
             for (const QJsonValue& value : result.value(QStringLiteral("data")).toArray()) {
                 const QJsonObject source = value.toObject();
                 catalog.append(QJsonObject{
                     {QStringLiteral("id"), source.value(QStringLiteral("id"))},
                     {QStringLiteral("label"), source.value(QStringLiteral("label"))},
                     {QStringLiteral("efforts"), source.value(QStringLiteral("efforts"))},
                     {QStringLiteral("default_effort"),
                      source.value(QStringLiteral("default_effort"))}});
             }
             callback(QJsonObject{{QStringLiteral("catalog"), catalog}}, {});
         });
}

void ClaudeAdapter::respond(const QJsonValue& requestId, const QJsonObject& result) {
    sendObject({{QStringLiteral("id"), requestId}, {QStringLiteral("result"), result}});
}

void ClaudeAdapter::respondError(const QJsonValue& requestId, const QString& code,
                                 const QString& message) {
    sendObject({{QStringLiteral("id"), requestId},
                {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code},
                                                      {QStringLiteral("message"), message}}}});
}

void ClaudeAdapter::stop() {
    stopping_ = true;
    restartTimer_->stop();
    failPending(QStringLiteral("The Claude sidecar was stopped."));
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

void ClaudeAdapter::ensureStarted() {
    if (starting_ || process_->state() != QProcess::NotRunning) {
        return;
    }
    if (!restartNotBefore_.hasExpired()) {
        if (!restartTimer_->isActive())
            restartTimer_->start(std::max<qint64>(1, restartNotBefore_.remainingTime()));
        return;
    }
    malformedOutput_ = false;
    stopping_ = false;
    starting_ = true;
    stdoutBuffer_.clear();
    stderrBuffer_.clear();
    configureProcess();
    process_->start();
}

void ClaudeAdapter::configureProcess() {
    const QFileInfo info(executable_);
    const QString suffix = info.suffix().toLower();
    if (suffix == QStringLiteral("mjs") || suffix == QStringLiteral("js")) {
        const QString node = findNode();
        process_->setProgram(node.isEmpty() ? QStringLiteral("node") : node);
        process_->setArguments({info.absoluteFilePath()});
        return;
    }
    process_->setProgram(executable_);
    process_->setArguments({});
}

void ClaudeAdapter::initialize() {
    sendCall(QStringLiteral("initialize"), {}, [this](const QJsonObject&, const QString& error) {
        starting_ = false;
        if (!error.isEmpty()) {
            const QString reason =
                QStringLiteral("Claude sidecar initialization failed: %1").arg(error);
            noteUnexpectedStop();
            failPending(reason);
            emit stopped(reason);
            stopping_ = true;
            process_->terminate();
            return;
        }
        setReady(true);
        const QList<QueuedCall> calls = std::exchange(queued_, {});
        for (const QueuedCall& call : calls) {
            sendCall(call.method, call.params, call.callback);
        }
    });
}

void ClaudeAdapter::sendCall(const QString& method, const QJsonObject& params, Callback callback) {
    const quint64 id = nextRequestId_++;
    QJsonObject providerParams = params;
    if (credentials_ && method == QStringLiteral("turn/start")) {
        QString error;
        const auto configuration = credentials_->launchConfiguration(&error);
        if (!error.isEmpty()) {
            callback({}, error);
            return;
        }
        providerParams.insert(QStringLiteral("authentication"), configuration);
    }
    pending_.insert(id, std::move(callback));
    if (method == QStringLiteral("turn/start")) {
        const auto launch =
            browser_tools::browserLaunchFromUserSettings(QCoreApplication::applicationDirPath());
        // ADR 0030: the computer front is per conversation; the agent app enforces access.
        const auto computer = computer_tools::launchFromUserSettings(
            params.value(QStringLiteral("sessionId")).toString(),
            QCoreApplication::applicationDirPath());
        QJsonObject servers =
            computer_tools::withClaudeMcpServer(browser_tools::claudeMcpServers(launch), computer);
        QString instructions = browser_tools::hostAgentInstructions(launch.has_value()) +
                               QStringLiteral("\n\n") +
                               computer_tools::agentInstructions(computer.has_value());
        // ADR 0027: the conversation's ketplus_agents server, supplied by SessionHost.
        const QJsonObject agentServer =
            providerParams.take(QStringLiteral("agentMcpServer")).toObject();
        const QString agentInstructions =
            providerParams.take(QStringLiteral("agentInstructions")).toString();
        if (!agentServer.isEmpty()) {
            servers.insert(QStringLiteral("ketplus_agents"), agentServer);
            if (!agentInstructions.isEmpty())
                instructions += QStringLiteral("\n\n") + agentInstructions;
        }
        providerParams.insert(QStringLiteral("mcpServers"), servers);
        providerParams.insert(QStringLiteral("browserInstructions"),
                              withResponseGuidance(instructions, !agentServer.isEmpty()));
    }
    sendObject({{QStringLiteral("id"), static_cast<qint64>(id)},
                {QStringLiteral("method"), method},
                {QStringLiteral("params"), providerParams}});
}

void ClaudeAdapter::sendObject(const QJsonObject& object) {
    if (process_->state() == QProcess::NotRunning) {
        return;
    }
    QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    payload.append('\n');
    process_->write(payload);
}

void ClaudeAdapter::consumeStdout() {
    if (malformedOutput_) {
        process_->setReadChannel(QProcess::StandardOutput);
        while (process_->bytesAvailable() > 0)
            if (process_->read(64 * 1024).isEmpty())
                break;
        return;
    }
    constexpr qint64 readChunkBytes = 64 * 1024;
    process_->setReadChannel(QProcess::StandardOutput);
    while (process_->bytesAvailable() > 0) {
        const QByteArray chunk = process_->read(readChunkBytes);
        if (chunk.isEmpty())
            return;
        stdoutBuffer_.append(chunk);
        while (true) {
            const qsizetype newline = stdoutBuffer_.indexOf('\n');
            const qsizetype lineBytes = newline < 0 ? stdoutBuffer_.size() : newline;
            if (lineBytes > maximumProviderMessageBytes) {
                const QString reason =
                    QStringLiteral("The Claude sidecar emitted an oversized message.");
                stdoutBuffer_.clear();
                // Quarantine this process generation, including already-buffered output.
                // Closing a read channel alone leaves its received bytes available.
                process_->closeReadChannel(QProcess::StandardOutput);
                while (process_->bytesAvailable() > 0)
                    if (process_->read(readChunkBytes).isEmpty())
                        break;
                failPending(reason);
                // A pending initialize has published this stop itself and marked it. Otherwise
                // publish it here, once: the exit that follows must not publish another or
                // count as a second crash.
                if (!stopping_) {
                    noteUnexpectedStop();
                    emit stopped(reason);
                    stopping_ = true;
                }
                process_->kill();
                return;
            }
            if (newline < 0)
                break;
            const QByteArray line = stdoutBuffer_.left(newline).trimmed();
            stdoutBuffer_.remove(0, newline + 1);
            if (!line.isEmpty())
                consumeLine(line);
            if (malformedOutput_)
                return;
        }
    }
}

void ClaudeAdapter::consumeLine(const QByteArray& line) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (!providerFrameHasValidControlBytes(line) || parseError.error != QJsonParseError::NoError ||
        !document.isObject()) {
        // No reliable call identity survives a broken frame. Quarantine the whole
        // generation; finished settles pending calls through the existing lifecycle.
        malformedOutput_ = true;
        setReady(false);
        stdoutBuffer_.clear();
        process_->closeReadChannel(QProcess::StandardOutput);
        process_->kill();
        return;
    }
    const QJsonObject object = document.object();
    const QString method = object.value(QStringLiteral("method")).toString();
    if (method == QStringLiteral("event")) {
        const QJsonObject params = object.value(QStringLiteral("params")).toObject();
        if (params.value(QStringLiteral("type")).toString() ==
            QStringLiteral("provider.authentication")) {
            const QJsonObject payload = params.value(QStringLiteral("payload")).toObject();
            if (credentials_)
                credentials_->observe(payload);
            if (payload.contains(QStringLiteral("source")))
                emit authenticationObserved(payload.value(QStringLiteral("source")).toString());
            return;
        }
        emit canonicalEvent(params.value(QStringLiteral("sessionId")).toString(),
                            params.value(QStringLiteral("type")).toString(),
                            params.value(QStringLiteral("payload")).toObject());
        return;
    }
    if (!method.isEmpty() && object.contains(QStringLiteral("id"))) {
        emit requestReceived(object.value(QStringLiteral("id")), method,
                             object.value(QStringLiteral("params")).toObject());
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
    if (!callback) {
        return;
    }
    if (!validProviderResponseEnvelope(object)) {
        callback({}, QStringLiteral("Malformed provider response envelope."));
    } else if (object.contains(QStringLiteral("error"))) {
        callback({}, errorText(object.value(QStringLiteral("error")).toObject()));
    } else {
        callback(object.value(QStringLiteral("result")).toObject(), {});
    }
}

void ClaudeAdapter::failPending(const QString& reason) {
    const auto pending = std::exchange(pending_, {});
    for (const Callback& callback : pending) {
        callback({}, reason);
    }
    const auto queued = std::exchange(queued_, {});
    for (const QueuedCall& call : queued) {
        call.callback({}, reason);
    }
}

void ClaudeAdapter::setReady(const bool ready) {
    if (ready && !ready_)
        readySince_.start();
    ready_ = ready;
}

void ClaudeAdapter::noteUnexpectedStop() {
    restartDelayMs_ = restartDelayMs_ <= 0 ? 1000 : std::min(restartDelayMs_ * 2, 60 * 1000);
    restartNotBefore_.setRemainingTime(restartDelayMs_);
}

bool ClaudeAdapter::sidecarLooksRunnable(const QString& indexPath) {
    const QFileInfo info(indexPath);
    if (!info.isFile()) {
        return false;
    }
    const QDir directory(info.absolutePath());
    return QFileInfo::exists(directory.filePath(QStringLiteral("runtime.json"))) ||
           QFileInfo::exists(
               directory.filePath(QStringLiteral("node_modules/@anthropic-ai/claude-agent-sdk")));
}

QString ClaudeAdapter::findNode() {
    const QString fromEnv = qEnvironmentVariable("KETPLUS_CLAUDE_NODE");
    if (!fromEnv.isEmpty()) {
        const QFileInfo info(fromEnv);
        if (info.isFile() && info.isExecutable()) {
            return info.absoluteFilePath();
        }
    }
    const QDir executableDirectory(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        executableDirectory.filePath(QStringLiteral("../Resources/ketplus-browser/bin/node")),
        executableDirectory.filePath(QStringLiteral("../Helpers/ketplus-browser/bin/node")),
        executableDirectory.filePath(QStringLiteral("ketplus-browser/bin/node")),
    };
    for (const QString& candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable()) {
            return info.canonicalFilePath();
        }
    }
    return QStandardPaths::findExecutable(QStringLiteral("node"));
}

QString ClaudeAdapter::findExecutable() {
    const QString fromEnv = qEnvironmentVariable("KETPLUS_CLAUDE_SIDECAR");
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv)) {
        return QFileInfo(fromEnv).absoluteFilePath();
    }
    const QDir executableDirectory(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        executableDirectory.filePath(QStringLiteral("claude-sidecar/index.mjs")),
        executableDirectory.filePath(QStringLiteral("../lib/ketplus/claude-sidecar/index.mjs")),
        executableDirectory.filePath(QStringLiteral("../Resources/claude-sidecar/index.mjs")),
        QDir(executableDirectory.filePath(QStringLiteral("../Resources")))
            .filePath(QStringLiteral("claude-sidecar/index.mjs")),
    };
    for (const QString& candidate : candidates) {
        if (sidecarLooksRunnable(candidate)) {
            return QFileInfo(candidate).canonicalFilePath();
        }
    }
    return {};
}

} // namespace ketplus::daemon
