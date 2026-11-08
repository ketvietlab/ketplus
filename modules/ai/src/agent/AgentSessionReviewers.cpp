#include "agent/AgentSessionManager.h"
#include "agent/AgentSessionState.h"
#include "agent/CodexAppServer.h"

namespace ketplus {

// ADR 0067: the conversation that reviews each finished turn of the active one.
void AgentSessionManager::readReviewer(const QString& worktreePath) {
    callReviewer(worktreePath, QStringLiteral("reviewer/get"), {});
}

void AgentSessionManager::setReviewer(const QString& worktreePath, const QString& reviewerNumber,
                                      const QString& newProvider) {
    QJsonObject reviewer;
    if (!newProvider.isEmpty()) {
        reviewer.insert("new_provider", newProvider);
    } else if (!reviewerNumber.isEmpty()) {
        // The daemon names conversations by session id; the picker shows numbers.
        for (const auto& conversations : std::as_const(conversationsByPath_))
            for (const SessionPtr& candidate : conversations)
                if (candidate->number == reviewerNumber && !candidate->threadId.isEmpty())
                    reviewer.insert("reviewer_session_id", candidate->threadId);
        if (reviewer.isEmpty()) {
            const QString path = normalizedPath(worktreePath);
            const SessionPtr session = sessions_.value(path);
            emit reviewerResult(
                path, session ? session->threadId : QString{},
                {{"error", "That conversation is no longer available."}, {"change", true}});
            return;
        }
    }
    callReviewer(worktreePath, QStringLiteral("reviewer/set"), reviewer);
}

void AgentSessionManager::callReviewer(const QString& worktreePath, const QString& method,
                                       const QJsonObject& reviewer) {
    const QString path = normalizedPath(worktreePath);
    const SessionPtr session = sessions_.value(path);
    if (!session || session->threadId.isEmpty() || !server_->usesDaemon()) {
        // Nothing to review yet: the picker stays hidden until the first turn starts.
        emit reviewerResult(path, session ? session->threadId : QString{}, {{"available", false}});
        return;
    }
    const QString conversation = session->threadId;
    server_->call(method, {{"threadId", conversation}, {"reviewer", reviewer}},
                  [this, path, conversation, method](const QJsonObject& result,
                                                     const QString& transportError) {
                      // A delayed reply cannot update a different conversation selected in this
                      // workspace.
                      const auto current = sessions_.value(path);
                      if (!current || current->threadId != conversation)
                          return;
                      const QString error = !transportError.isEmpty() ? transportError
                                            : result.value("ok").toBool()
                                                ? QString{}
                                                : result.value("error_message").toString();
                      if (!error.isEmpty()) {
                          emit reviewerResult(
                              path, conversation,
                              {{"error", error}, {"change", method.endsWith("/set")}});
                          return;
                      }
                      QVariantMap state{{"available", true}};
                      if (const auto assigned = result.value("reviewer"); assigned.isObject())
                          state.insert("reviewer", assigned.toObject().toVariantMap());
                      emit reviewerResult(path, conversation, state);
                  });
}

} // namespace ketplus
