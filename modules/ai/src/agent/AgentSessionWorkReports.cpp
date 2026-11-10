#include "agent/AgentSessionManager.h"
#include "agent/AgentSessionState.h"
#include "agent/CodexAppServer.h"

namespace ketplus {

// ADR 0106: the Report tab shows the daemon's ledger for the active conversation.
void AgentSessionManager::readWorkReport(const QString& worktreePath) {
    const QString path = normalizedPath(worktreePath);
    const SessionPtr session = sessions_.value(path);
    if (!session || session->threadId.isEmpty() || !server_->usesDaemon()) {
        // A conversation that has not started has no report yet.
        emit workReportChanged(path, session ? session->id : QString{}, {}, {});
        return;
    }
    const QString thread = session->threadId;
    const QString conversation = session->id;
    server_->call(QStringLiteral("workReport/read"), {{QStringLiteral("threadId"), thread}},
                  [this, path, thread, conversation](const QJsonObject& result,
                                                     const QString& transportError) {
                      // A late reply must not show another conversation's report.
                      const auto current = sessions_.value(path);
                      if (!current || current->threadId != thread)
                          return;
                      const QString error =
                          !transportError.isEmpty() ? transportError
                          : result.value(QStringLiteral("ok")).toBool()
                              ? QString{}
                              : result.value(QStringLiteral("error_message")).toString();
                      if (!error.isEmpty())
                          workReportReadsToRetry_.insert(path);
                      emit workReportChanged(
                          path, conversation,
                          result.value(QStringLiteral("report")).toObject().toVariantMap(), error);
                  });
}

} // namespace ketplus
