#pragma once

#include "agents/AgentBroker.h"
#include "storage/AgentMessageRecord.h"

#include <QList>
#include <QString>

namespace ketplus::daemon::agent_messaging {

// Host policy for the injected ketplus_agents tools, naming the conversation's own number.
[[nodiscard]] QString toolInstructions(const QString& ownNumber);
// Provider input for delivered messages: each is framed as a peer's message, not user text.
[[nodiscard]] QString deliveryText(const QList<AgentMessageRecord>& messages);
// Replaces the user-facing response guidance in turns a message started.
[[nodiscard]] QString automaticTurnGuidance();
// Conversations the user referenced with `#NNNN`.
[[nodiscard]] QString referenceText(const QList<AgentConversation>& conversations);

} // namespace ketplus::daemon::agent_messaging
