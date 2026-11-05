#include "agent/AgentSessionManager.h"

#include "agent/CodexAppServer.h"
#include "agent/CodexRateLimits.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSettings>
#include <QUrl>
#include <QUuid>

#include "agent/AgentMessageFormatter.h"
#include "agent/AgentSessionState.h"

namespace ketplus {
using namespace agent_message;

void AgentSessionManager::ensureThread(const SessionPtr& session) {
    if (session->cancelRequested) {
        updateState(session, QStringLiteral("Canceled"), false);
        return;
    }
    if (session->readyInProcess) {
        startTurn(session);
        return;
    }

    if (session->threadId.isEmpty()) {
        QJsonObject params{{QStringLiteral("cwd"), session->path},
                           {QStringLiteral("sandbox"), QStringLiteral("workspace-write")},
                           {QStringLiteral("approvalPolicy"), QStringLiteral("never")},
                           {QStringLiteral("approvalsReviewer"), QStringLiteral("user")},
                           {QStringLiteral("ephemeral"), false},
                           {QStringLiteral("experimentalRawEvents"), false}};
        const auto permissions = CodexAppServer::permissionParams(
            session->options.value("permission").toString(), false);
        for (auto it = permissions.begin(); it != permissions.end(); ++it)
            params.insert(it.key(), it.value());
        if (!session->model.isEmpty()) {
            params.insert(QStringLiteral("model"), session->model);
        } else {
            applyModelOverride(params, false);
        }
        const QPointer<AgentSessionManager> guard(this);
        server_->call(QStringLiteral("thread/start"), params,
                      [guard, this, session](const QJsonObject& result, const QString& error) {
                          if (!guard)
                              return;
                          if (!error.isEmpty()) {
                              failSession(session, error);
                              return;
                          }
                          const QJsonObject thread =
                              result.value(QStringLiteral("thread")).toObject();
                          session->threadId = thread.value(QStringLiteral("id")).toString();
                          if (session->threadId.isEmpty()) {
                              failSession(session, QStringLiteral("Codex returned no thread id."));
                              return;
                          }
                          session->readyInProcess = true;
                          session->historyLoaded = true;
                          emitHistoryState(session);
                          sessionsByThread_.insert(session->threadId, session);
                          persistWorkspace(session->path);
                          startTurn(session);
                      });
        return;
    }

    QJsonObject params{{QStringLiteral("threadId"), session->threadId},
                       {QStringLiteral("cwd"), session->path},
                       {QStringLiteral("sandbox"), QStringLiteral("workspace-write")},
                       {QStringLiteral("approvalPolicy"), QStringLiteral("never")},
                       {QStringLiteral("approvalsReviewer"), QStringLiteral("user")},
                       {QStringLiteral("excludeTurns"), true}};
    const auto permissions =
        CodexAppServer::permissionParams(session->options.value("permission").toString(), false);
    for (auto it = permissions.begin(); it != permissions.end(); ++it)
        params.insert(it.key(), it.value());
    if (!session->model.isEmpty()) {
        params.insert(QStringLiteral("model"), session->model);
    } else {
        applyModelOverride(params, false);
    }
    const QPointer<AgentSessionManager> guard(this);
    server_->call(QStringLiteral("thread/resume"), params,
                  [guard, this, session](const QJsonObject& result, const QString& error) {
                      if (!guard)
                          return;
                      if (!error.isEmpty()) {
                          if (session->pendingCompact) {
                              failSession(session, error);
                              return;
                          }
                          sessionsByThread_.remove(session->threadId);
                          session->threadId.clear();
                          session->readyInProcess = false;
                          persistWorkspace(session->path);
                          ensureThread(session);
                          return;
                      }
                      session->readyInProcess = true;
                      const QJsonObject thread = result.value(QStringLiteral("thread")).toObject();
                      if (!session->historyLoaded) {
                          if (!thread.value("turns").toArray().isEmpty())
                              hydrateHistory(session, thread);
                          else
                              loadHistoryPage(session);
                      }
                      sessionsByThread_.insert(session->threadId, session);
                      startTurn(session);
                  });
}

void AgentSessionManager::startTurn(const SessionPtr& session) {
    if (session->cancelRequested) {
        session->pendingPrompt.clear();
        updateState(session, QStringLiteral("Canceled"), false);
        return;
    }
    if (session->pendingCompact) {
        session->pendingCompact = false;
        const QPointer<AgentSessionManager> guard(this);
        server_->call(QStringLiteral("thread/compact/start"),
                      {{QStringLiteral("threadId"), session->threadId}},
                      [guard, this, session](const QJsonObject&, const QString& error) {
                          if (!guard)
                              return;
                          if (!error.isEmpty())
                              failSession(
                                  session,
                                  QStringLiteral("Could not compact context: %1").arg(error));
                          // The response only acknowledges the request; item/turn events finish it.
                      });
        return;
    }
    const QString prompt = std::exchange(session->pendingPrompt, {});
    if (prompt.isEmpty()) {
        failSession(session, QStringLiteral("The pending prompt was lost."));
        return;
    }
    session->turnId.clear();
    session->messageIndexByItem.clear();
    session->agentMessagePhaseByItem.clear();
    session->deferredAgentTextByItem.clear();
    session->deferredAgentOrder.clear();
    session->explicitFinalInTurn = false;
    updateState(session, QStringLiteral("Thinking…"), true);
    QJsonObject params{{QStringLiteral("threadId"), session->threadId},
                       {QStringLiteral("cwd"), session->path},
                       {QStringLiteral("approvalPolicy"), QStringLiteral("never")},
                       {QStringLiteral("input"), std::exchange(session->pendingInput, {})},
                       // Daemon-only: the transcript shows this, not the wrapped input text.
                       {QStringLiteral("displayText"), prompt},
                       {QStringLiteral("clientMessageId"),
                        std::exchange(session->pendingClientMessageId, {})}};
    const auto permissionParams =
        CodexAppServer::permissionParams(session->options.value("permission").toString(), true);
    for (auto it = permissionParams.begin(); it != permissionParams.end(); ++it)
        params.insert(it.key(), it.value());
    if (session->model.isEmpty()) {
        applyModelOverride(params, true);
    }
    for (const QString& key :
         {QStringLiteral("model"), QStringLiteral("effort"), QStringLiteral("provider")}) {
        const QString value = session->options.value(key).toString();
        if (!value.isEmpty())
            params.insert(key, value);
    }
    const QPointer<AgentSessionManager> guard(this);
    server_->call(QStringLiteral("turn/start"), params,
                  [guard, this, session](const QJsonObject& result, const QString& error) {
                      if (!guard)
                          return;
                      if (!error.isEmpty()) {
                          failSession(session, error);
                          return;
                      }
                      session->turnId = result.value(QStringLiteral("turn"))
                                            .toObject()
                                            .value(QStringLiteral("id"))
                                            .toString();
                      if (session->customTitle)
                          syncConversationTitle(session);
                      if (session->cancelRequested) {
                          cancelSession(session);
                      }
                  });
}

} // namespace ketplus
