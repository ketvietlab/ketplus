#pragma once

#include "agents/AgentBroker.h"
#include "storage/AgentMessageRecord.h"

#include <QJsonArray>
#include <QList>
#include <QString>

namespace ketplus::daemon::agent_messaging {

// "4821" from "4821", "#4821" or "@agent/4821", trimmed; other text unchanged.
[[nodiscard]] QString bareNumber(const QString& reference);
// Host policy for the injected ketplus_agents tools, naming the conversation's own number.
[[nodiscard]] QString toolInstructions(const QString& ownNumber);
// ADR 0114: what a member of a team may do, by role. `lane` and `paths` are a developer's.
struct TeamRoleContext final {
    QString team;
    QString goal;
    QString role;
    QString lane;
    QStringList paths;
    QStringList sharedPaths;
    QString mergePolicy;
    QString integrationBranch;
    bool leaderWritesRequirements{true};
};
[[nodiscard]] QString teamInstructions(const TeamRoleContext& context);
// Provider input for delivered messages: each is framed as a peer's message, not user text.
[[nodiscard]] QString deliveryText(const QList<AgentMessageRecord>& messages);
// True for text deliveryText framed, or a title or preview cut from it: the envelope's marker
// is delivery metadata, never a conversation's subject (T-26).
[[nodiscard]] bool isDeliveryText(const QString& text);
// Replaces the user-facing response guidance in turns a message started. `messages` are the
// delivered `agent_messages`; whether one expects a reply decides if the final message is sent.
[[nodiscard]] QString automaticTurnGuidance(const QJsonArray& messages);
// Conversations the user referenced with `#NNNN` or `@agent/NNNN`.
[[nodiscard]] QString referenceText(const QList<AgentConversation>& conversations);

} // namespace ketplus::daemon::agent_messaging
