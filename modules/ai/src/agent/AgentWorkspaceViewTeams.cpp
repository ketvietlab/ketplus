// #467, #468: what the Agent Workspace asks of the conversation's team.
#include "agent/AgentWorkspaceView.h"

namespace ketplus {

void AgentWorkspaceView::requestTeamTask(const QString& teamName, const QString& taskId) {
    if (localActionRefused())
        return;
    QString teamId;
    for (const QVariant& value : conversations_) {
        const QVariantMap conversation = value.toMap();
        if (conversation.value(QStringLiteral("active")).toBool())
            teamId = teamRoles_.value(conversation.value(QStringLiteral("sessionId")).toString())
                         .toMap()
                         .value(QStringLiteral("team_id"))
                         .toString();
    }
    for (auto it = teamRoles_.cbegin(); teamId.isEmpty() && it != teamRoles_.cend(); ++it)
        if (it.value().toMap().value(QStringLiteral("team_name")).toString() == teamName)
            teamId = it.value().toMap().value(QStringLiteral("team_id")).toString();
    if (!teamId.isEmpty() && !taskId.isEmpty())
        emit teamTaskRequested(teamId, taskId);
}

} // namespace ketplus
