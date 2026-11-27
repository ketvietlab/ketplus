#include "providers/CopilotCatalog.h"
#include "providers/CopilotAdapter.h"
#include "providers/CopilotCli.h"
#include "providers/ProviderFrameValidation.h"
#include "providers/ProviderResponseEnvelope.h"
#include "providers/ProviderResponseId.h"
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <QTimer>
#include <utility>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace ketplus::daemon::copilot_catalog {
namespace {
Snapshot bounded(Snapshot snapshot) {
    // UI aliases and approval metadata expand the raw ACP catalog. Reserve room
    // in the one-MiB daemon frame for the envelope and other providers' models.
    if (QJsonDocument(snapshot.models).toJson(QJsonDocument::Compact).size() > maximumCatalogBytes)
        return {};
    return snapshot;
}
bool identifier(const QString& value) {
    if (value.isEmpty() || value.size() > maximumIdLength || value != value.trimmed())
        return false;
    for (const QChar ch : value)
        if (ch.category() == QChar::Other_Control)
            return false;
    return true;
}
QString display(QString text, int limit) {
    text = text.left(limit);
    for (auto& ch : text)
        if (ch.category() == QChar::Other_Control)
            ch = QLatin1Char(' ');
    return text;
}
QJsonObject entry(const QString& id, const QString& label, const QString& description,
                  bool current) {
    // This is a host approval policy, not a claim about a Copilot sandbox or mode.
    const QJsonArray permissions{
        QJsonObject{{"id", "workspace-write"},
                    {"label", "Ask for permission"},
                    {"elevated", false},
                    {"description", "Send Copilot permission requests to KetPlus for approval."}}};
    return {{"id", id},
            {"model", id},
            {"provider", "copilot"},
            {"label", display(label.isEmpty() ? id : label, maximumLabelLength)},
            {"displayName", display(label.isEmpty() ? id : label, maximumLabelLength)},
            {"description", display(description, maximumDescriptionLength)},
            {"hidden", false},
            {"isDefault", current},
            {"efforts", QJsonArray{}},
            {"default_effort", ""},
            {"supportedReasoningEfforts", QJsonArray{}},
            {"defaultReasoningEffort", ""},
            {"permissions", permissions},
            {"defaultPermission", "workspace-write"}};
}
} // namespace

QJsonArray fallback() {
    return {entry("default", "Copilot default",
                  "Model catalog unavailable. Copilot will choose its provider default; "
                  "check CLI sign-in and refresh to discover selectable models.",
                  true)};
}

Snapshot parse(const QJsonObject& session) {
    Snapshot result;
    const QJsonArray configs = session.value("configOptions").toArray();
    if (configs.size() > maximumModels)
        return result;
    for (const auto& value : configs) {
        const QJsonObject config = value.toObject();
        const auto category = config.value("category").toString();
        if (category != "model" && !(category.isEmpty() && config.value("id") == "model"))
            continue;
        // Prefer the first model selector, including an invalid/unsupported one.
        if (config.value("type") != "select" || !identifier(config.value("id").toString()))
            return result;
        result.configId = config.value("id").toString();
        result.current = config.value("currentValue").toString();
        QJsonArray choices = config.value("options").toArray();
        if (choices.size() > maximumModels)
            return {};
        QJsonArray flat;
        for (const auto& choice : choices) {
            const auto object = choice.toObject();
            if (object.contains("group")) {
                const auto nested = object.value("options").toArray();
                if (flat.size() + nested.size() > maximumModels)
                    return {};
                for (const auto& item : nested)
                    flat.append(item);
            } else {
                flat.append(choice);
            }
            if (flat.size() > maximumModels)
                return {};
        }
        QSet<QString> seen;
        for (const auto& choice : flat) {
            const auto object = choice.toObject();
            const QString id = object.value("value").toString();
            if (!identifier(id) || seen.contains(id))
                continue;
            seen.insert(id);
            result.models.append(entry(id, object.value("name").toString(),
                                       object.value("description").toString(),
                                       id == result.current));
        }
        return seen.contains(result.current) ? bounded(std::move(result)) : Snapshot{};
    }
    const auto legacy = session.value("models").toObject();
    const auto choices = legacy.value("availableModels").toArray();
    if (choices.size() > maximumModels)
        return {};
    result.legacy = true;
    result.current = legacy.value("currentModelId").toString();
    QSet<QString> seen;
    for (const auto& choice : choices) {
        const auto object = choice.toObject();
        const QString id = object.value("modelId").toString();
        if (!identifier(id) || seen.contains(id))
            continue;
        seen.insert(id);
        result.models.append(entry(id, object.value("name").toString(),
                                   object.value("description").toString(), id == result.current));
    }
    return seen.contains(result.current) ? bounded(std::move(result)) : Snapshot{};
}

Selection select(const Snapshot& catalog, const QString& sessionId, const QString& requested) {
    if (requested.isEmpty() || (requested == "default" && catalog.models.isEmpty()))
        return {};
    bool found = false;
    for (const auto& value : catalog.models)
        found = found || value.toObject().value("id") == requested;
    if (!found && requested == "default")
        return {}; // The fallback row means leave the session's provider default unchanged.
    if (!found)
        return {{},
                {},
                QStringLiteral("GitHub Copilot did not advertise the selected model. "
                               "Refresh models or choose Copilot default.")};
    if (requested == catalog.current)
        return {};
    return {catalog.legacy ? "session/set_model" : "session/set_config_option",
            catalog.legacy ? QJsonObject{{"sessionId", sessionId}, {"modelId", requested}}
                           : QJsonObject{{"sessionId", sessionId},
                                         {"configId", catalog.configId},
                                         {"value", requested}},
            {}};
}
} // namespace ketplus::daemon::copilot_catalog

namespace ketplus::daemon {
CopilotCatalog::CopilotCatalog(QObject* parent, int timeoutMs, int freshMs)
    : QObject(parent), process_(new QProcess(this)), timer_(new QTimer(this)),
      timeoutMs_(timeoutMs), freshMs_(freshMs) {
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, [this] { finish(false); });
    connect(process_, &QProcess::started, this, [this] {
        send(1, "initialize",
             {{"protocolVersion", 1},
              {"clientCapabilities", QJsonObject{}},
              {"clientInfo", QJsonObject{{"name", "ketplus-catalog"}, {"version", "1"}}}});
    });
    connect(process_, &QProcess::readyReadStandardOutput, this, &CopilotCatalog::drain);
    connect(process_, &QProcess::readyReadStandardError, this, &CopilotCatalog::drain);
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false);
    });
    connect(process_, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        drainingFinished_ = true;
        drain();
        drainingFinished_ = false;
        if (!finishing_) {
            finishing_ = true;
            successful_ = false;
        }
        publish();
    });
}
CopilotCatalog::~CopilotCatalog() {
    waiters_.clear();
    process_->disconnect(this);
    process_->kill();
    if (process_->state() != QProcess::NotRunning)
        process_->waitForFinished(1000);
}
void CopilotCatalog::get(const QString& executable, ProviderAdapter::Callback done) {
    if (executable == executable_ && fetched_.isValid() && fetched_.elapsed() < freshMs_) {
        done({{"data", cached_}}, {});
        return;
    }
    waiters_.append(std::move(done));
    if (waiters_.size() > 1)
        return;
    executable_ = executable;
    buffer_.clear();
    candidate_ = {};
    received_ = 0;
    expected_ = 0;
    finishing_ = false;
    successful_ = false;
    const auto context = copilot_cli::systemDiscoveryContext();
    const auto invocation =
        copilot_cli::processInvocation(executable, context.platform, context.environment);
    if (!invocation.error.isEmpty() || !directory_.isValid()) {
        finish(false);
        return;
    }
    process_->setProgram(invocation.program);
    process_->setArguments(invocation.arguments);
#ifdef Q_OS_WIN
    process_->setNativeArguments(invocation.nativeArguments);
    process_->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    process_->setProcessEnvironment(context.environment);
    process_->setWorkingDirectory(directory_.path());
    timer_->start(timeoutMs_);
    process_->start();
}
void CopilotCatalog::stop() { finish(false); }

void CopilotCatalog::send(int id, const QString& method, const QJsonObject& params) {
    expected_ = id;
    process_->write(
        QJsonDocument(
            QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}})
            .toJson(QJsonDocument::Compact) +
        '\n');
}
void CopilotCatalog::drain() {
    for (const auto channel : {QProcess::StandardError, QProcess::StandardOutput}) {
        process_->setReadChannel(channel);
        while (process_->bytesAvailable() > 0) {
            const QByteArray bytes = process_->read(64 * 1024);
            if (bytes.isEmpty())
                break;
            received_ += bytes.size();
            if (received_ > copilot_catalog::maximumOutputBytes) {
                successful_ = false;
                finish(false);
                return;
            }
            if (channel == QProcess::StandardError || finishing_)
                continue;
            buffer_ += bytes;
            while (true) {
                const auto newline = buffer_.indexOf('\n');
                if (newline < 0)
                    break;
                const auto frame = buffer_.left(newline).trimmed();
                buffer_.remove(0, newline + 1);
                if (!frame.isEmpty())
                    line(frame);
                if (finishing_)
                    break;
            }
        }
    }
}
void CopilotCatalog::line(const QByteArray& bytes) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (!providerFrameHasValidControlBytes(bytes) || error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        finish(false);
        return;
    }
    const auto object = document.object();
    // No server requests are authorized in a catalog probe, including permissions.
    if (object.contains("method")) {
        if (object.contains("id"))
            finish(false);
        return;
    }
    const auto id = providerResponseId(object.value("id"));
    if (object.value("jsonrpc") != "2.0" || !id || *id != static_cast<quint64>(expected_) ||
        !validProviderResponseEnvelope(object) || object.contains("error")) {
        finish(false);
        return;
    }
    const auto result = object.value("result").toObject();
    if (expected_ == 1) {
        if (result.value("protocolVersion") != 1) {
            finish(false);
            return;
        }
        send(2, "session/new", {{"cwd", directory_.path()}, {"mcpServers", QJsonArray{}}});
    } else if (expected_ == 2) {
        candidate_ = copilot_catalog::parse(result).models;
        finish(!result.value("sessionId").toString().isEmpty() && !candidate_.isEmpty());
    }
}
void CopilotCatalog::finish(bool success) {
    if (finishing_)
        return;
    finishing_ = true;
    successful_ = success;
    timer_->stop();
    if (process_->state() == QProcess::NotRunning) {
        if (!drainingFinished_)
            publish();
    } else
        process_->kill();
}
void CopilotCatalog::publish() {
    if (waiters_.isEmpty())
        return;
    timer_->stop();
    cached_ = successful_ ? candidate_ : copilot_catalog::fallback();
    fetched_.restart();
    const auto waiters = std::exchange(waiters_, {});
    const QJsonObject result{{"data", cached_}};
    for (const auto& done : waiters)
        done(result, {});
}
} // namespace ketplus::daemon

namespace ketplus::daemon {
void CopilotAdapter::prepareSessionModel(const QString& sessionId, const QString& model,
                                         const QJsonObject& result, Callback done) {
    // A provider without session/load creates a new ACP id each turn. Drop its
    // superseded catalog instead of retaining one model list per historical turn.
    for (auto it = sessionModels_.begin(); it != sessionModels_.end();) {
        if (!ketplusByAcp_.contains(it.key()))
            it = sessionModels_.erase(it);
        else
            ++it;
    }
    if (result.contains("configOptions") || result.contains("models"))
        sessionModels_.insert(sessionId, copilot_catalog::parse(result));
    const auto snapshot = sessionModels_.value(sessionId);
    const auto selection = copilot_catalog::select(snapshot, sessionId, model);
    if (!selection.error.isEmpty() || selection.method.isEmpty()) {
        done({}, selection.error);
        return;
    }
    sendCall(
        selection.method, selection.params,
        [this, sessionId, model, snapshot, done = std::move(done)](const QJsonObject& result,
                                                                   const QString& error) mutable {
            if (!error.isEmpty()) {
                done({}, QStringLiteral("GitHub Copilot could not select the requested model: %1")
                             .arg(error));
                return;
            }
            auto updated = snapshot;
            if (snapshot.legacy) {
                updated.current = model;
            } else {
                updated = copilot_catalog::parse(result);
                if (updated.configId != snapshot.configId || updated.current != model) {
                    done({}, QStringLiteral("GitHub Copilot did not confirm the selected model. "
                                            "Refresh models and retry; no prompt was sent."));
                    return;
                }
            }
            sessionModels_.insert(sessionId, updated);
            done({}, {});
        });
}

void CopilotAdapter::handleSessionUpdate(const QJsonObject& params) {
    const auto update = params.value("update").toObject();
    const auto id = params.value("sessionId").toString();
    if (ketplusByAcp_.contains(id) && update.value("sessionUpdate") == "config_option_update")
        sessionModels_.insert(id, copilot_catalog::parse(update));
    AcpAdapter::handleSessionUpdate(params);
}
void CopilotAdapter::resetProviderState() { sessionModels_.clear(); }
void CopilotAdapter::stopAuxiliaryProcesses() { catalog_->stop(); }
} // namespace ketplus::daemon
