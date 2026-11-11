#include "agent/ActivityGroupLoader.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentSessionState.h"
#include "agent/DaemonTranscriptProjection.h"

namespace ketplus {
void AgentSessionManager::setActivityGroupOpen(const QString& path, const QString& scope,
                                               const QString& key, bool expanded) {
    const auto session = sessions_.value(normalizedPath(path));
    if (!session || scope != session->path + ":" + session->id)
        return;
    for (const auto& row : session->messages) {
        if (row.value("messageKey").toString() != key || row.value("subtype") != "daemonGroup")
            continue;
        activityGroups_->setOpen(session->threadId, row.value("daemonGroupId").toString(),
                                 row.value("groupCursor").toLongLong(), expanded,
                                 row.value("groupMembership").toLongLong());
        return;
    }
}

void AgentSessionManager::publishActivityGroupDetails(const QString& thread, const QString& group) {
    const auto session = sessionForThread(thread);
    if (!session)
        return;
    for (int index = 0; index < session->messages.size(); ++index) {
        auto row = session->messages.at(index);
        if (row.value("subtype") != "daemonGroup" || row.value("daemonGroupId").toString() != group)
            continue;
        row.insert("details", activityGroups_->details(thread, group));
        updateMessage(session, index, row);
        return;
    }
}

void AgentSessionManager::settleActivityGroups(const SessionPtr& session) {
    const auto turn = agent_message::daemonTurnId(session->daemonTurnSequence);
    const auto cursor = session->historyTurnCursors.value(turn).second;
    for (int index = 0; index < session->messages.size(); ++index) {
        auto row = session->messages.at(index);
        if (row.value("subtype") != "daemonGroup" || row.value("turnId") != turn)
            continue;
        if (row.value("groupRunning").toBool()) {
            row.insert("groupRunning", false);
            updateMessage(session, index, row);
        }
        activityGroups_->updateSummary(session->threadId, row.value("daemonGroupId").toString(),
                                       cursor);
    }
}

void AgentSessionManager::acceptActivityGroup(const SessionPtr& session,
                                              const QJsonObject& params) {
    const auto summary = params.value("summary").toObject();
    auto row = agent_message::daemonActivityGroupMessage(summary,
                                                         params.value("turnSequence").toInteger());
    const auto group = summary.value("group_id").toString();
    row.insert("details", activityGroups_->details(session->threadId, group));
    const auto key = row.value("messageKey").toString();
    int existing = -1;
    for (int index = 0; index < session->messages.size(); ++index)
        if (session->messages.at(index).value("messageKey").toString() == key) {
            existing = index;
            break;
        }
    if (existing < 0)
        appendMessage(session, row);
    else
        updateMessage(session, existing, row);
    activityGroups_->updateSummary(session->threadId, group,
                                   summary.value("last_change_cursor").toInteger(),
                                   summary.value("membership_cursor").toInteger());
}
} // namespace ketplus
