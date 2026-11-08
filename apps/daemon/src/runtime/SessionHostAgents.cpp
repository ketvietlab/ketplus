#include "runtime/SessionHost.h"

#include "agents/AgentInstructions.h"
#include "protocol/DaemonProtocol.h"
#include "providers/compat/ProviderCapabilities.h"

#include <QJsonArray>
#include <QUuid>

namespace ketplus::daemon {
namespace {

QJsonObject internalRequest(const QString& type, const QString& sessionId, const qint64 generation,
                            const QJsonObject& payload) {
    return {{QStringLiteral("protocol_version"), protocol::currentVersion},
            {QStringLiteral("message_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("message_type"), type},
            {QStringLiteral("sent_at"), protocol::timestamp()},
            {QStringLiteral("session_id"), sessionId},
            {QStringLiteral("session_generation"), generation},
            {QStringLiteral("payload"), payload}};
}

// What clients show for a delivered message: its text, so older clients still read it.
QString displayText(const QJsonObject& origin) {
    QStringList bodies;
    for (const QJsonValue& value : origin.value(QStringLiteral("agent_messages")).toArray())
        bodies.append(value.toObject().value(QStringLiteral("body")).toString());
    return bodies.join(QStringLiteral("\n\n"));
}

} // namespace

void SessionHost::setAgentEndpoint(const QString& socketName, const QString& executable) {
    agentSocket_ = socketName;
    agentExecutable_ = executable;
}

QHash<QString, QJsonObject> SessionHost::agentTurnSenders() const {
    return agents_->automaticTurnSenders();
}

bool SessionHost::isAgentToken(const QString& token) const {
    return agents_->sessionForToken(token).has_value();
}

QJsonObject SessionHost::handleAgentCommand(const QString& token, const QString& type,
                                            const QJsonObject& payload) {
    return agents_->handle(token, type, payload);
}

QString SessionHost::agentToken(const QString& sessionId) {
    const auto it = sessions_.constFind(sessionId);
    return it == sessions_.cend() ? QString{} : agents_->tokenFor(sessionId, it->stored.generation);
}

AgentConversation SessionHost::agentConversationOf(const Session& session) const {
    const StoredSession& stored = session.stored;
    AgentConversation conversation;
    conversation.sessionId = stored.id;
    conversation.generation = stored.generation;
    conversation.number = journal_->conversationNumber(stored.id, stored.generation);
    conversation.title = stored.title;
    conversation.provider = stored.adapter;
    conversation.model = stored.model;
    conversation.worktree = stored.worktreePath;
    conversation.state = stored.state;
    conversation.lastActivity = stored.lastTurnStartedAt;
    const QString& state = stored.state;
    const bool turnOpen = session.startingTurn || state == QStringLiteral("starting") ||
                          state == QStringLiteral("running") ||
                          state == QStringLiteral("waiting_for_input") ||
                          state == QStringLiteral("stopping");
    conversation.idle = !turnOpen && !hasLiveBackgroundTasks(session);
    const bool claude = stored.adapter == QStringLiteral("claude");
    conversation.steerable = state == QStringLiteral("running") && !stored.turnId.isEmpty() &&
                             (claude || !stored.threadId.isEmpty()) &&
                             providerSupports(stored.adapter, compat::capability::turnSteer);
    return conversation;
}

std::optional<AgentConversation> SessionHost::agentConversation(const QString& sessionId) const {
    const auto it = sessions_.constFind(sessionId);
    if (it == sessions_.cend())
        return std::nullopt;
    return agentConversationOf(*it);
}

QList<AgentConversation> SessionHost::agentConversations() const {
    QList<AgentConversation> conversations;
    conversations.reserve(sessions_.size());
    for (const Session& session : sessions_)
        conversations.append(agentConversationOf(session));
    return conversations;
}

QString SessionHost::startAgentTurn(const QString& sessionId, const qint64 generation,
                                    const QString& providerText, const QJsonObject& origin) {
    const auto it = sessions_.constFind(sessionId);
    if (it == sessions_.cend() || it->stored.generation != generation)
        return QStringLiteral("The conversation no longer exists.");
    const QJsonObject result =
        startTurn(internalRequest(
                      QStringLiteral("session.start_turn"), sessionId, generation,
                      {{QStringLiteral("prompt"), providerText},
                       {QStringLiteral("display_text"), displayText(origin)},
                       {QStringLiteral("expected_context_revision"), it->stored.contextRevision}}),
                  true, origin);
    return result.value(QStringLiteral("ok")).toBool()
               ? QString{}
               : result.value(QStringLiteral("error_message")).toString();
}

QString SessionHost::steerAgentTurn(const QString& sessionId, const qint64 generation,
                                    const QString& providerText, const QJsonObject& origin) {
    const auto it = sessions_.constFind(sessionId);
    if (it == sessions_.cend() || it->stored.generation != generation)
        return QStringLiteral("The conversation no longer exists.");
    const QJsonObject result =
        steer(internalRequest(
                  QStringLiteral("session.steer"), sessionId, generation,
                  {{QStringLiteral("prompt"), providerText},
                   {QStringLiteral("display_text"), displayText(origin)},
                   {QStringLiteral("expected_context_revision"), it->stored.contextRevision}}),
              {}, true, origin);
    return result.value(QStringLiteral("ok")).toBool()
               ? QString{}
               : result.value(QStringLiteral("error_message")).toString();
}

qint64 SessionHost::agentTurnSequence(const QString& sessionId) const {
    const auto it = sessions_.constFind(sessionId);
    return it == sessions_.cend() ? 0 : it->stored.currentTurnSequence;
}

void SessionHost::appendAgentNotice(const QString& sessionId, const qint64 generation,
                                    const QJsonObject& payload) {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end() || it->stored.generation != generation)
        return;
    (void)appendEvent(*it, QStringLiteral("agent_message.state"), payload);
}

std::optional<agent_tools::Launch> SessionHost::agentLaunch(const QString& sessionId,
                                                            const qint64 generation) {
    if (agentSocket_.isEmpty() || agentExecutable_.isEmpty())
        return std::nullopt;
    return agent_tools::Launch{agentExecutable_, agentSocket_,
                               agents_->tokenFor(sessionId, generation)};
}

QString SessionHost::agentInstructions(const Session& session) const {
    if (agentSocket_.isEmpty() || agentExecutable_.isEmpty())
        return {};
    const QString number =
        journal_->conversationNumber(session.stored.id, session.stored.generation);
    return number.isEmpty() ? QString{} : agent_messaging::toolInstructions(number);
}

void SessionHost::addAgentContext(Session& session, const QString& userText, QJsonArray& input,
                                  const bool userTurn) {
    if (userTurn) {
        if (const QString pending =
                agents_->takeForUserTurn(session.stored.id, session.stored.generation);
            !pending.isEmpty())
            input.prepend(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                      {QStringLiteral("text"), pending}});
    }
    if (const QString references = agents_->referenceBlock(session.stored.id, userText);
        !references.isEmpty())
        input.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                 {QStringLiteral("text"), references}});
}

QJsonObject SessionHost::heldAgentMessages(const QJsonObject& request, bool release) {
    Session* session = nullptr;
    const auto failure = checkedSession(request, &session);
    if (!failure.isEmpty())
        return failure;
    const auto payload = request.value("payload").toObject();
    const auto values = payload.value("message_ids").toArray();
    QStringList ids;
    for (const auto& value : values) {
        if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 128)
            return {{"ok", false},
                    {"error_code", "invalid_held_batch"},
                    {"error_message", "Invalid message ID."}};
        ids.append(value.toString());
    }
    return release ? agents_->releaseHeldMessages(session->stored.id, session->stored.generation,
                                                  ids, request.value("message_id").toString())
                   : agents_->heldMessages(session->stored.id, session->stored.generation, ids);
}

} // namespace ketplus::daemon
