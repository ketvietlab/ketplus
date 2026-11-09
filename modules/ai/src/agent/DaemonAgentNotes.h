#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>

namespace ketplus::agent_message {

// The mock's note for a message this conversation sent to another agent: "Message to
// @agent/8941", then "DS Icon · Claude · Replied" and the start of the text. One note follows
// its message: later states and the reply update it instead of adding rows.
// Empty for anything else (incoming notices, reviews), which keeps its own presentation.
QVariantMap agentNote(const QJsonObject& data, const QString& itemId);
// Folds a later state of the same message into its note, keeping what only the first knew.
QVariantMap mergedAgentNote(const QVariantMap& note, const QVariantMap& update);
// The note marked replied, when an agent's reply names its message.
QVariantMap repliedAgentNote(const QVariantMap& note);
// The messages a delivered agent turn answers (`reply_to` of its replies).
QStringList agentRepliesTo(const QJsonArray& agentMessages);

} // namespace ketplus::agent_message
