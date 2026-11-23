#pragma once

#include "agents/AgentBroker.h"
#include "storage/AgentMessageRecord.h"

#include <QJsonArray>
#include <QList>
#include <QString>

namespace ketplus::daemon::agent_messaging {

// "4821" from "4821", "#4821" or "@agent/4821", trimmed; other text unchanged.
[[nodiscard]] QString bareNumber(const QString& reference);
// Host policy for the injected ketplus_agents tools.
[[nodiscard]] QString toolInstructions();
// ADR 0114: conversation-specific team settings. The developer lane is a name.
struct TeamRoleContext final {
    QString team;
    QString goal;
    QString role;
    QString lane;
    QString mergePolicy;
    QString integrationBranch;
    bool leaderWritesRequirements{true};
    // Content fallback (auto/en/vi). Prompt instructions win; application chrome is English.
    QString language;
};
[[nodiscard]] QString teamInstructions(const QString& role);
[[nodiscard]] QString conversationContext(const QString& number, const TeamRoleContext& context);
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
