#include "AgentSessionManager.h"
#include "AgentSessionState.h"
#include "CodexAppServer.h"
#include <QJsonObject>
#include <QSettings>

namespace ketplus {
namespace {
void rememberLastConversationConfiguration(const QString& model, const QString& effort,
                                           const QString& permission, const QString& provider) {
    if (model.trimmed().isEmpty())
        return;
    QSettings().setValue(
        QStringLiteral("agent/lastConversationConfiguration"),
        QVariantMap{{QStringLiteral("model"), model.trimmed()},
                    {QStringLiteral("effort"), effort.trimmed()},
                    {QStringLiteral("permission"), permission.trimmed().isEmpty()
                                                       ? QStringLiteral("workspace-write")
                                                       : permission.trimmed()},
                    {QStringLiteral("provider"), provider.trimmed().toLower()}});
}

} // namespace

void AgentSessionManager::setConversationConfiguration(const QString& worktreePath,
                                                       const QString& model, const QString& effort,
                                                       const QString& permission,
                                                       const QString& provider,
                                                       const QVariantMap& changes) {
    const SessionPtr session = ensureSession(worktreePath);
    if (!session) {
        return;
    }
    const auto finish = [this, session](const QString& error = QString{}) {
        emit conversationConfigurationApplyFinished(session->path, session->id, error);
    };
    const bool hasCompactWindow = changes.contains("compactWindow");
    const QJsonValue compactWindow = QJsonObject::fromVariantMap(changes).value("compactWindow");
    const QString targetProvider = provider.trimmed().isEmpty()
                                       ? session->options.value("provider", "codex").toString()
                                       : provider.trimmed().toLower();
    if (hasCompactWindow) {
        if (!server_->hasCompactWindowSupport() || !server_->isReady()) {
            finish(tr("Reconnect to a desktop with compact-window support before applying."));
            return;
        }
        if (!compactWindow.isNull() &&
            (!compactWindow.isDouble() || compactWindow.toDouble() != compactWindow.toInteger() ||
             compactWindow.toInteger() < 100000 || compactWindow.toInteger() > 1000000)) {
            finish(tr("Enter a whole token count from 100,000 to 1,000,000."));
            return;
        }
        if (!compactWindow.isNull() && targetProvider != "codex" && targetProvider != "claude") {
            finish(tr("This provider does not support a custom compact window."));
            return;
        }
    }
    if (session->configurationApplying) {
        finish(tr("Wait for the active operation to finish before applying."));
        return;
    }
    const QString normalizedPermission =
        permission.trimmed().isEmpty() ? session->permission : permission.trimmed();
    const QString normalizedModel = model.trimmed();
    const QString normalizedEffort = effort.trimmed();
    const QString normalizedProvider = provider.trimmed().toLower();
    if (session->model == normalizedModel && session->effort == normalizedEffort &&
        session->permission == normalizedPermission &&
        (normalizedProvider.isEmpty() ||
         session->options.value("provider").toString().trimmed().toLower() == normalizedProvider) &&
        (!hasCompactWindow ||
         compactWindow == QJsonObject::fromVariantMap(session->options).value("compactWindow") ||
         (compactWindow.isNull() && !session->options.contains("compactWindow")))) {
        finish();
        return;
    }
    if (server_->usesDaemon() && !session->threadId.isEmpty() &&
        (normalizedProvider.isEmpty() ||
         normalizedProvider == session->options.value("provider").toString().trimmed().toLower())) {
        QJsonObject payload{
            {"desktop_selection", true},
            {"expected_revision",
             changes.value("expected_revision", session->configurationRevision).toLongLong()},
            {"model", normalizedModel},
            {"effort", normalizedEffort}};
        if (hasCompactWindow)
            payload.insert("compactWindow", compactWindow);
        session->configurationApplying = true;
        // A model change never writes permission. Only an explicit permission change does.
        if (normalizedPermission != session->permission) {
            payload.insert("execution_policy", normalizedPermission == "danger-full-access" ? "full"
                                               : normalizedPermission == "read-only" ? "read-only"
                                                                                     : "ask");
            payload.insert("risk_acknowledged", normalizedPermission == "danger-full-access");
        }
        server_->executionWorktreeCommand(
            session->threadId, "session.configuration.apply", payload,
            [this, session](const QJsonObject& result, const QString& error) {
                if (!error.isEmpty() || !result.value("ok").toBool()) {
                    const QString reason =
                        error.isEmpty() ? result.value("error_message").toString() : error;
                    emit errorOccurred(reason);
                    const auto message =
                        reason.isEmpty() ? tr("Settings were not applied. Refresh and try again.")
                                         : reason;
                    server_->executionWorktreeCommand(
                        session->threadId, "session.configuration.read", {},
                        [this, session, message](const QJsonObject& fresh,
                                                 const QString& readError) {
                            if (readError.isEmpty())
                                acceptDaemonConfiguration(session, fresh);
                            session->configurationApplying = false;
                            emit conversationConfigurationApplyFinished(session->path, session->id,
                                                                        message);
                        });
                    return;
                }
                acceptDaemonConfiguration(session, result);
                session->configurationApplying = false;
                emit conversationConfigurationApplyFinished(session->path, session->id, {});
            });
        return;
    }
    if (hasCompactWindow)
        session->options.insert("compactWindow", compactWindow.toVariant());
    session->model = normalizedModel;
    session->effort = normalizedEffort;
    session->permission = normalizedPermission;
    session->options.insert(QStringLiteral("permission"), session->permission);
    if (!normalizedProvider.isEmpty())
        session->options.insert(QStringLiteral("provider"), normalizedProvider);
    if (session->model.isEmpty()) {
        session->options.remove(QStringLiteral("model"));
    } else {
        session->options.insert(QStringLiteral("model"), session->model);
    }
    if (session->effort.isEmpty()) {
        session->options.remove(QStringLiteral("effort"));
    } else {
        session->options.insert(QStringLiteral("effort"), session->effort);
    }
    persistWorkspace(session->path);
    rememberLastConversationConfiguration(
        session->model, session->effort, session->permission,
        session->options.value("provider").toString().trimmed().toLower());
    emit conversationsChanged(session->path, conversations(session->path));
    emitConfiguration(session);
    emitUsageSnapshot(session);
    finish();
}

} // namespace ketplus
