// ADR 0114: messages a team sends into its members' conversations.
#include "agents/AgentBroker.h"
#include "storage/LocalJournal.h"

#include <QDateTime>
#include <QUuid>

namespace ketplus::daemon {

QString AgentBroker::postTeamMessage(const QString& fromSession, const QString& fromTitle,
                                     const QString& toSession, const QString& kind,
                                     const QString& body, const bool expectReply,
                                     QString* messageId, const QString& id) {
    const auto target = host_->agentConversation(toSession);
    if (!target)
        return QStringLiteral("The member's conversation no longer exists.");
    AgentMessageRecord message;
    // A team that must know the id before the message is sent chooses it.
    message.id = id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id;
    message.chainId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    message.hop = 1;
    if (const auto sender =
            fromSession.isEmpty() ? std::nullopt : host_->agentConversation(fromSession)) {
        message.fromSession = sender->sessionId;
        message.fromGeneration = sender->generation;
        message.fromNumber = sender->number;
        message.fromTitle = sender->title;
        message.fromProvider = sender->provider;
    } else {
        // The team itself: empty, not null, columns.
        message.fromSession = QStringLiteral("");
        message.fromNumber = QStringLiteral("");
        message.fromProvider = QStringLiteral("");
        message.fromTitle = fromTitle;
    }
    message.toSession = target->sessionId;
    message.toGeneration = target->generation;
    message.toNumber = target->number;
    message.kind = kind;
    message.expectReply = expectReply;
    message.body = body.toUtf8().size() > maximumBodyBytes
                       ? QString::fromUtf8(body.toUtf8().left(maximumBodyBytes - 64)) +
                             QStringLiteral("\n(shortened)")
                       : body;
    message.state = QStringLiteral("queued");
    message.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    message.expiresAt = QDateTime::currentDateTimeUtc()
                            .addSecs(qint64{requestLifetimeHours} * 3600)
                            .toString(Qt::ISODateWithMs);
    if (messageId)
        *messageId = message.id;
    return post(message);
}

} // namespace ketplus::daemon
