#include "agents/AgentBroker.h"
#include "storage/LocalJournal.h"
#include <QJsonArray>

namespace ketplus::daemon {
namespace {
QJsonObject failure(const QString& message) {
    return {{"ok", false}, {"error_code", "invalid_held_batch"}, {"error_message", message}};
}
} // namespace

QJsonObject AgentBroker::heldMessages(const QString& sessionId, qint64 generation,
                                      const QStringList& ids) const {
    const auto receiver = host_->agentConversation(sessionId);
    if (!receiver || receiver->generation != generation)
        return failure(QStringLiteral("The conversation no longer exists."));
    if (ids.isEmpty() || ids.size() > 100)
        return failure(QStringLiteral("Select between 1 and 100 messages."));
    QJsonArray entries;
    for (const auto& id : ids) {
        const auto message = journal_->agentMessage(id);
        if (!message || message->toSession != sessionId || message->toGeneration != generation)
            return failure(
                QStringLiteral("The selected message does not belong to this conversation."));
        const bool expired = !message->expiresAt.isEmpty() &&
                             QDateTime::fromString(message->expiresAt, Qt::ISODateWithMs) <=
                                 QDateTime::currentDateTimeUtc();
        const bool retry =
            message->state == "failed" && message->delivery.startsWith("user_authorized:");
        entries.append(QJsonObject{
            {"message_id", message->id},
            {"state", expired && message->state != "delivered" && message->state != "answered"
                          ? QStringLiteral("expired")
                          : message->state},
            {"reason", message->error},
            {"peer_number", message->fromNumber},
            {"peer_title", message->fromTitle},
            {"can_release", !expired && (message->state == "held" || retry)}});
    }
    return {{"ok", true}, {"messages", entries}};
}

QJsonObject AgentBroker::releaseHeldMessages(const QString& sessionId, qint64 generation,
                                             const QStringList& ids,
                                             const QString& authorizationId) {
    const auto snapshot = heldMessages(sessionId, generation, ids);
    if (!snapshot.value("ok").toBool())
        return snapshot;
    QString error;
    if (!journal_->authorizeAgentMessages(sessionId, generation, ids, authorizationId, &error))
        return failure(error);
    for (const auto& id : ids) {
        if (const auto message = journal_->agentMessage(id))
            notice(*message, QStringLiteral("authorized"), QStringLiteral("user_release"));
    }
    scheduleDelivery(sessionId);
    return heldMessages(sessionId, generation, ids);
}
} // namespace ketplus::daemon
