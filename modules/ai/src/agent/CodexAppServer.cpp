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

CodexAppServer::CodexAppServer(QObject* parent) : QObject(parent) { setupDaemonTransport(); }

CodexAppServer::CodexAppServer(const QString& executable, QObject* parent)
    : QObject(parent), executable_(executable), process_(new QProcess(this)) {
    setupProcessTransport();
}

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
    if (method.startsWith(QStringLiteral("daemon/storageRetention/"))) {
        callback({}, QStringLiteral("Storage settings require an updated local KetPlus daemon."));
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
