#pragma once

#include "agents/AgentBroker.h"
#include "storage/AgentMessageRecord.h"

#include <QJsonArray>
#include <QList>
#include <QString>

namespace ketplus::daemon::agent_messaging {

// Host policy for the injected ketplus_agents tools, naming the conversation's own number.
[[nodiscard]] QString toolInstructions(const QString& ownNumber);
// Provider input for delivered messages: each is framed as a peer's message, not user text.
[[nodiscard]] QString deliveryText(const QList<AgentMessageRecord>& messages);
// Replaces the user-facing response guidance in turns a message started. `messages` are the
// delivered `agent_messages`; whether one expects a reply decides if the final message is sent.
[[nodiscard]] QString automaticTurnGuidance(const QJsonArray& messages);
// Conversations the user referenced with `#NNNN`.
[[nodiscard]] QString referenceText(const QList<AgentConversation>& conversations);

} // namespace ketplus::daemon::agent_messaging
