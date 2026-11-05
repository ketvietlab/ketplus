#include "agents/AgentBroker.h"

#include "agents/AgentInstructions.h"
#include "storage/LocalJournal.h"

#include <QDateTime>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>

namespace ketplus::daemon {
namespace {

QString nowIso() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }

QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QString tokenKey(const QString& sessionId, const qint64 generation) {
    return sessionId + QLatin1Char(':') + QString::number(generation);
}

QJsonObject ok(QJsonObject payload = {}) {
    payload.insert(QStringLiteral("ok"), true);
    return payload;
}

QJsonObject failure(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

// "4821", "#4821" or 4821; empty unless it is four or five digits.
QString normalizedNumber(const QJsonValue& value) {
    QString text = value.isDouble() ? QString::number(value.toInteger()) : value.toString();
    text = text.trimmed();
    if (text.startsWith(QLatin1Char('#')))
        text.remove(0, 1);
    static const QRegularExpression digits(QStringLiteral("^[1-9][0-9]{3,4}$"));
    return digits.match(text).hasMatch() ? text : QString{};
}

QString truncatedUtf8(const QString& text, const qsizetype maximumBytes) {
    if (text.toUtf8().size() <= maximumBytes)
        return text;
    QString result = text.left(static_cast<qsizetype>(maximumBytes));
    while (!result.isEmpty() && result.toUtf8().size() > maximumBytes)
        result.chop(qMax<qsizetype>(1, result.size() / 16));
    return result;
}

QJsonObject originJson(const QList<AgentMessageRecord>& messages) {
    QJsonArray entries;
    for (const AgentMessageRecord& message : messages)
        entries.append(QJsonObject{{QStringLiteral("message_id"), message.id},
                                   {QStringLiteral("kind"), message.kind},
                                   {QStringLiteral("chain_id"), message.chainId},
                                   {QStringLiteral("hop"), message.hop},
                                   {QStringLiteral("from_number"), message.fromNumber},
                                   {QStringLiteral("from_title"), message.fromTitle},
                                   {QStringLiteral("from_provider"), message.fromProvider},
                                   {QStringLiteral("to_number"), message.toNumber},
                                   {QStringLiteral("reply_to"), message.replyTo},
                                   {QStringLiteral("expect_reply"), message.expectReply},
                                   {QStringLiteral("urgent"), message.urgent},
                                   {QStringLiteral("body"), message.body}});
    return {{QStringLiteral("origin"), QStringLiteral("agent")},
            {QStringLiteral("agent_messages"), entries}};
}

} // namespace

AgentBroker::AgentBroker(LocalJournal* journal, AgentHostPort* host, QObject* parent)
    : QObject(parent), journal_(journal), host_(host) {
    deliveryTimer_.setSingleShot(true);
    deliveryTimer_.setInterval(0);
    connect(&deliveryTimer_, &QTimer::timeout, this, &AgentBroker::processDeliveries);
    expiryTimer_.setInterval(expiryCheckMs);
    connect(&expiryTimer_, &QTimer::timeout, this, &AgentBroker::expireOverdue);
    expiryTimer_.start();
}

QString AgentBroker::tokenFor(const QString& sessionId, const qint64 generation) {
    const QString key = tokenKey(sessionId, generation);
    if (const auto existing = tokenBySession_.constFind(key); existing != tokenBySession_.cend())
        return existing.value();
    QByteArray bytes(32, Qt::Uninitialized);
    QRandomGenerator::system()->generate(bytes.begin(), bytes.end());
    const QString token = QString::fromLatin1(bytes.toHex());
    tokenBySession_.insert(key, token);
    sessionByToken_.insert(token, {sessionId, generation});
    return token;
}

std::optional<std::pair<QString, qint64>> AgentBroker::sessionForToken(const QString& token) const {
    const auto found = sessionByToken_.constFind(token);
    if (token.isEmpty() || found == sessionByToken_.cend())
        return std::nullopt;
    return found.value();
}

bool AgentBroker::isOperation(const QString& type) {
    return type == QStringLiteral("agents.whoami") || type == QStringLiteral("agents.list") ||
           type == QStringLiteral("agents.send") || type == QStringLiteral("agents.inbox");
}

QJsonObject AgentBroker::handle(const QString& token, const QString& operation,
                                const QJsonObject& payload) {
    const auto owner = sessionForToken(token);
    if (!owner)
        return failure(QStringLiteral("authentication_failed"),
                       QStringLiteral("The agent credential is invalid."));
    const auto caller = host_->agentConversation(owner->first);
    if (!caller || caller->generation != owner->second)
        return failure(QStringLiteral("session_not_found"),
                       QStringLiteral("This conversation no longer exists."));
    if (operation == QStringLiteral("agents.whoami"))
        return whoami(*caller);
    if (operation == QStringLiteral("agents.list"))
        return list(*caller, payload);
    if (operation == QStringLiteral("agents.send"))
        return send(*caller, payload);
    if (operation == QStringLiteral("agents.inbox"))
        return inbox(*caller, payload);
    return failure(QStringLiteral("unsupported_operation"),
                   QStringLiteral("Unknown agent operation."));
}

QJsonObject AgentBroker::conversationJson(const AgentConversation& conversation) {
    return {{QStringLiteral("number"), conversation.number},
            {QStringLiteral("title"), conversation.title},
            {QStringLiteral("provider"), conversation.provider},
            {QStringLiteral("model"), conversation.model},
            {QStringLiteral("state"),
             conversation.idle ? QStringLiteral("idle") : QStringLiteral("working")},
            {QStringLiteral("worktree"), conversation.worktree},
            {QStringLiteral("last_activity"), conversation.lastActivity}};
}

QJsonObject AgentBroker::messageJson(const AgentMessageRecord& message) {
    QJsonObject value{{QStringLiteral("message_id"), message.id},
                      {QStringLiteral("kind"), message.kind},
                      {QStringLiteral("from"), message.fromNumber},
                      {QStringLiteral("from_title"), message.fromTitle},
                      {QStringLiteral("to"), message.toNumber},
                      {QStringLiteral("state"), message.state},
                      {QStringLiteral("expect_reply"), message.expectReply},
                      {QStringLiteral("created_at"), message.createdAt}};
    if (!message.replyTo.isEmpty())
        value.insert(QStringLiteral("reply_to"), message.replyTo);
    if (!message.error.isEmpty())
        value.insert(QStringLiteral("reason"), message.error);
    return value;
}

QJsonObject AgentBroker::whoami(const AgentConversation& caller) const {
    return ok(conversationJson(caller));
}

QJsonObject AgentBroker::list(const AgentConversation& caller, const QJsonObject& payload) const {
    const QString query = payload.value(QStringLiteral("query")).toString().trimmed();
    const int limit = std::clamp(payload.value(QStringLiteral("limit")).toInt(50), 1, 200);
    QList<AgentConversation> conversations = host_->agentConversations();
    std::sort(conversations.begin(), conversations.end(),
              [](const AgentConversation& left, const AgentConversation& right) {
                  return left.lastActivity > right.lastActivity;
              });
    QJsonArray values;
    int total = 0;
    for (const AgentConversation& conversation : std::as_const(conversations)) {
        if (conversation.number.isEmpty())
            continue;
        const QString haystack =
            QStringList{QLatin1Char('#') + conversation.number, conversation.title,
                        conversation.provider, conversation.worktree}
                .join(QLatin1Char('\n'));
        if (!query.isEmpty() && !haystack.contains(query, Qt::CaseInsensitive))
            continue;
        ++total;
        if (values.size() >= limit)
            continue;
        QJsonObject value = conversationJson(conversation);
        if (conversation.sessionId == caller.sessionId)
            value.insert(QStringLiteral("self"), true);
        values.append(value);
    }
    return ok({{QStringLiteral("conversations"), values}, {QStringLiteral("total"), total}});
}

QJsonObject AgentBroker::send(const AgentConversation& caller, const QJsonObject& payload) {
    const QString to = normalizedNumber(payload.value(QStringLiteral("to")));
    if (to.isEmpty())
        return failure(QStringLiteral("invalid_number"),
                       QStringLiteral("`to` must be a conversation number such as 4821."));
    const QString text = payload.value(QStringLiteral("text")).toString().trimmed();
    if (text.isEmpty())
        return failure(QStringLiteral("invalid_message"), QStringLiteral("`text` is empty."));
    if (text.toUtf8().size() > maximumBodyBytes)
        return failure(QStringLiteral("message_too_large"),
                       QStringLiteral("A message holds at most 32 KiB of text."));
    if (to == caller.number)
        return failure(QStringLiteral("self_message"),
                       QStringLiteral("A conversation cannot message itself."));
    const auto target = journal_->sessionForConversationNumber(to);
    const auto receiver = target ? host_->agentConversation(target->first) : std::nullopt;
    if (!receiver || receiver->generation != target->second)
        return failure(QStringLiteral("unknown_number"),
                       QStringLiteral("No conversation on this computer has number #%1.").arg(to));

    AgentMessageRecord message;
    message.id = newId();
    message.replyTo = payload.value(QStringLiteral("reply_to")).toString().trimmed();
    message.expectReply = payload.value(QStringLiteral("expect_reply")).toBool();
    message.urgent = payload.value(QStringLiteral("urgent")).toBool();
    std::optional<AgentMessageRecord> replied;
    if (!message.replyTo.isEmpty()) {
        replied = journal_->agentMessage(message.replyTo);
        if (!replied || replied->toSession != caller.sessionId ||
            replied->fromSession != receiver->sessionId)
            return failure(QStringLiteral("invalid_reply"),
                           QStringLiteral("reply_to must name a message #%1 sent to you.").arg(to));
        message.kind = QStringLiteral("reply");
        message.chainId = replied->chainId;
        message.hop = replied->hop + 1;
    } else {
        message.kind = message.expectReply ? QStringLiteral("request") : QStringLiteral("notice");
        const auto origin = turns_.constFind(caller.sessionId);
        if (origin != turns_.cend() && !origin->chainId.isEmpty()) {
            message.chainId = origin->chainId;
            message.hop = origin->hop + 1;
        } else {
            message.chainId = newId();
            message.hop = 1;
        }
    }
    message.fromSession = caller.sessionId;
    message.fromGeneration = caller.generation;
    message.fromNumber = caller.number;
    message.fromTitle = caller.title;
    message.fromProvider = caller.provider;
    message.toSession = receiver->sessionId;
    message.toGeneration = receiver->generation;
    message.toNumber = receiver->number;
    message.body = text;
    message.state = QStringLiteral("queued");
    message.createdAt = nowIso();
    if (message.expectReply)
        message.expiresAt = QDateTime::currentDateTimeUtc()
                                .addSecs(qint64{requestLifetimeHours} * 3600)
                                .toString(Qt::ISODateWithMs);
    if (const QString error = post(message); !error.isEmpty())
        return failure(QStringLiteral("journal_error"), error);
    if (replied && replied->state != QStringLiteral("answered")) {
        markState(*replied, QStringLiteral("answered"));
        if (auto origin = turns_.find(caller.sessionId); origin != turns_.end())
            origin->expectReply.removeAll(replied->id);
    }
    const QString delivery = receiver->idle                          ? QStringLiteral("starting")
                             : message.urgent && receiver->steerable ? QStringLiteral("steering")
                                                                     : QStringLiteral("queued");
    // The sender's transcript names the receiver in words; its tool row is provider-specific.
    host_->appendAgentNotice(caller.sessionId, caller.generation,
                             {{QStringLiteral("message_id"), message.id},
                              {QStringLiteral("kind"), message.kind},
                              {QStringLiteral("state"), QStringLiteral("sent")},
                              {QStringLiteral("delivery"), delivery},
                              {QStringLiteral("expect_reply"), message.expectReply},
                              {QStringLiteral("direction"), QStringLiteral("outgoing")},
                              {QStringLiteral("peer_number"), receiver->number},
                              {QStringLiteral("peer_title"), receiver->title},
                              {QStringLiteral("peer_provider"), receiver->provider}});
    return ok({{QStringLiteral("message_id"), message.id},
               {QStringLiteral("kind"), message.kind},
               {QStringLiteral("delivery"), delivery},
               {QStringLiteral("to"), conversationJson(*receiver)}});
}

QJsonObject AgentBroker::inbox(const AgentConversation& caller, const QJsonObject& payload) const {
    const QString wanted = payload.value(QStringLiteral("message_id")).toString();
    if (!wanted.isEmpty()) {
        const auto message = journal_->agentMessage(wanted);
        if (!message ||
            (message->toSession != caller.sessionId && message->fromSession != caller.sessionId))
            return failure(QStringLiteral("message_not_found"),
                           QStringLiteral("No such message for this conversation."));
        QJsonObject value = messageJson(*message);
        value.insert(QStringLiteral("body"), message->body);
        return ok({{QStringLiteral("message"), value}});
    }
    const int limit = std::clamp(payload.value(QStringLiteral("limit")).toInt(20), 1, 50);
    QJsonArray values;
    for (const AgentMessageRecord& message :
         journal_->agentMessagesOf(caller.sessionId, caller.generation, limit)) {
        QJsonObject value = messageJson(message);
        value.insert(QStringLiteral("direction"), message.toSession == caller.sessionId
                                                      ? QStringLiteral("incoming")
                                                      : QStringLiteral("outgoing"));
        constexpr qsizetype previewBytes = 2000;
        value.insert(QStringLiteral("body"), truncatedUtf8(message.body, previewBytes));
        if (message.body.toUtf8().size() > previewBytes)
            value.insert(QStringLiteral("truncated"), true);
        values.append(value);
    }
    return ok({{QStringLiteral("messages"), values}});
}

QString AgentBroker::post(AgentMessageRecord& message) {
    QString error;
    if (!journal_->insertAgentMessage(message, &error))
        return error.isEmpty() ? QStringLiteral("The journal rejected the message.") : error;
    scheduleDelivery(message.toSession);
    return {};
}

void AgentBroker::scheduleDelivery(const QString& sessionId) {
    pendingDeliveries_.insert(sessionId);
    deliveryTimer_.start();
}

void AgentBroker::processDeliveries() {
    const QSet<QString> sessions = std::exchange(pendingDeliveries_, {});
    for (const QString& sessionId : sessions)
        deliverTo(sessionId);
}

void AgentBroker::deliverTo(const QString& sessionId) {
    const auto receiver = host_->agentConversation(sessionId);
    if (!receiver)
        return;
    QList<AgentMessageRecord> queued =
        journal_->agentMessagesTo(sessionId, receiver->generation, {QStringLiteral("queued")});
    if (queued.isEmpty())
        return;
    if (!receiver->idle) {
        QList<AgentMessageRecord> urgent;
        for (const AgentMessageRecord& message : std::as_const(queued))
            if (message.urgent)
                urgent.append(message);
        if (urgent.isEmpty() || !receiver->steerable)
            return;
        if (!host_
                 ->steerAgentTurn(sessionId, receiver->generation,
                                  agent_messaging::deliveryText(urgent), originJson(urgent))
                 .isEmpty())
            return; // Not handed over: the messages wait for the turn to end.
        const qint64 turn = host_->agentTurnSequence(sessionId);
        for (AgentMessageRecord& message : urgent) {
            message.state = QStringLiteral("delivered");
            message.delivery = QStringLiteral("steer");
            message.deliveredTurn = turn;
            message.deliveredAt = nowIso();
            (void)journal_->updateAgentMessage(message);
        }
        return;
    }
    const int hourly = journal_->automaticAgentTurnsSince(
        sessionId, receiver->generation, QDateTime::currentDateTimeUtc().addSecs(-3600));
    QList<AgentMessageRecord> deliverable;
    for (AgentMessageRecord& message : queued) {
        const QString reason =
            message.hop > maximumAutomaticHop ? QStringLiteral("hop_limit")
            : journal_->automaticAgentTurnsInChain(message.chainId) >= maximumChainTurns
                ? QStringLiteral("chain_limit")
            : hourly >= maximumHourlyTurns ? QStringLiteral("hourly_limit")
                                           : QString{};
        if (reason.isEmpty()) {
            deliverable.append(message);
            continue;
        }
        markState(message, QStringLiteral("held"), reason);
        notice(message, QStringLiteral("held"), reason);
    }
    if (deliverable.isEmpty())
        return;
    TurnOrigin origin;
    origin.automatic = true;
    origin.sender = {{QStringLiteral("number"), deliverable.first().fromNumber},
                     {QStringLiteral("title"), deliverable.first().fromTitle},
                     {QStringLiteral("kind"), deliverable.first().kind}};
    for (const AgentMessageRecord& message : std::as_const(deliverable)) {
        if (message.hop >= origin.hop) {
            origin.hop = message.hop;
            origin.chainId = message.chainId;
        }
        if (message.expectReply)
            origin.expectReply.append(message.id);
    }
    turns_.insert(sessionId, origin);
    const QString error =
        host_->startAgentTurn(sessionId, receiver->generation,
                              agent_messaging::deliveryText(deliverable), originJson(deliverable));
    if (!error.isEmpty()) {
        turns_.remove(sessionId);
        for (AgentMessageRecord& message : deliverable) {
            markState(message, QStringLiteral("failed"), error);
            notice(message, QStringLiteral("failed"), error);
        }
        return;
    }
    const qint64 turn = host_->agentTurnSequence(sessionId);
    for (AgentMessageRecord& message : deliverable) {
        message.state = QStringLiteral("delivered");
        message.delivery = QStringLiteral("auto");
        message.deliveredTurn = turn;
        message.deliveredAt = nowIso();
        (void)journal_->updateAgentMessage(message);
    }
}

QString AgentBroker::takeForUserTurn(const QString& sessionId, const qint64 generation) {
    TurnOrigin origin;
    origin.chainId = newId();
    turns_.insert(sessionId, origin);
    QList<AgentMessageRecord> pending = journal_->agentMessagesTo(
        sessionId, generation, {QStringLiteral("queued"), QStringLiteral("held")});
    if (pending.isEmpty())
        return {};
    // Called once the turn's user.message is journaled, which opened this turn sequence.
    const qint64 turn = host_->agentTurnSequence(sessionId);
    for (AgentMessageRecord& message : pending) {
        message.state = QStringLiteral("delivered");
        message.delivery = QStringLiteral("user_turn");
        message.deliveredTurn = turn;
        message.deliveredAt = nowIso();
        message.error.clear();
        (void)journal_->updateAgentMessage(message);
    }
    return agent_messaging::deliveryText(pending);
}

QString AgentBroker::referenceBlock(const QString& sessionId, const QString& text) const {
    static const QRegularExpression reference(
        QStringLiteral("(?<![\\w#])#([1-9][0-9]{3,4})(?!\\d)"));
    QStringList seen;
    QList<AgentConversation> conversations;
    for (auto match = reference.globalMatch(text); match.hasNext();) {
        const QString number = match.next().captured(1);
        if (seen.contains(number))
            continue;
        seen.append(number);
        const auto target = journal_->sessionForConversationNumber(number);
        if (!target || target->first == sessionId)
            continue;
        if (const auto conversation = host_->agentConversation(target->first);
            conversation && conversation->generation == target->second)
            conversations.append(*conversation);
    }
    return agent_messaging::referenceText(conversations);
}

void AgentBroker::noteEvent(const QString& sessionId, const qint64 generation, const QString& type,
                            const QJsonObject& payload) {
    if (type == QStringLiteral("assistant.delta") || type == QStringLiteral("item.completed")) {
        auto origin = turns_.find(sessionId);
        if (origin == turns_.end() || !origin->automatic)
            return;
        const QString itemId = payload.value(QStringLiteral("item_id")).toString();
        if (type == QStringLiteral("assistant.delta")) {
            if (itemId != origin->lastItemId) {
                origin->lastItemId = itemId;
                origin->lastText.clear();
            }
            origin->lastText += payload.value(QStringLiteral("delta")).toString();
            return;
        }
        if (payload.value(QStringLiteral("item_type")).toString() != QStringLiteral("agentMessage"))
            return;
        const QJsonValue text =
            payload.contains(QStringLiteral("text"))
                ? payload.value(QStringLiteral("text"))
                : payload.value(QStringLiteral("item")).toObject().value(QStringLiteral("text"));
        origin->lastItemId = itemId;
        origin->lastText = text.toString();
        return;
    }
    if (type == QStringLiteral("turn.completed") || type == QStringLiteral("runtime.stopped")) {
        finishTurn(sessionId, generation,
                   type == QStringLiteral("runtime.stopped")
                       ? QStringLiteral("stopped")
                       : payload.value(QStringLiteral("status")).toString());
        scheduleDelivery(sessionId);
        return;
    }
    if (type == QStringLiteral("tasks.changed") || type == QStringLiteral("runtime.error"))
        scheduleDelivery(sessionId);
}

void AgentBroker::finishTurn(const QString& sessionId, const qint64 generation,
                             const QString& status) {
    const auto found = turns_.constFind(sessionId);
    if (found == turns_.cend())
        return;
    const TurnOrigin origin = found.value();
    turns_.erase(found);
    if (!origin.automatic || origin.expectReply.isEmpty())
        return;
    const bool completed = status.isEmpty() || status == QStringLiteral("completed");
    const auto receiver = host_->agentConversation(sessionId);
    const QString text = origin.lastText.trimmed();
    for (const QString& id : origin.expectReply) {
        auto request = journal_->agentMessage(id);
        if (!request || request->state != QStringLiteral("delivered"))
            continue;
        if (!completed || text.isEmpty() || !receiver || receiver->generation != generation) {
            const QString reason =
                completed ? QStringLiteral("no_reply") : QStringLiteral("turn_") + status;
            markState(*request, QStringLiteral("failed"), reason);
            notice(*request, QStringLiteral("failed"), reason);
            continue;
        }
        markState(*request, QStringLiteral("answered"));
        const auto sender = host_->agentConversation(request->fromSession);
        if (!sender || sender->generation != request->fromGeneration)
            continue;
        AgentMessageRecord reply;
        reply.id = newId();
        reply.chainId = request->chainId;
        reply.hop = qMax(request->hop, origin.hop) + 1;
        reply.fromSession = sessionId;
        reply.fromGeneration = generation;
        reply.fromNumber = receiver->number;
        reply.fromTitle = receiver->title;
        reply.fromProvider = receiver->provider;
        reply.toSession = request->fromSession;
        reply.toGeneration = request->fromGeneration;
        reply.toNumber = request->fromNumber;
        reply.kind = QStringLiteral("reply");
        reply.replyTo = request->id;
        reply.body = truncatedUtf8(text, maximumBodyBytes);
        reply.state = QStringLiteral("queued");
        reply.createdAt = nowIso();
        if (const QString error = post(reply); !error.isEmpty()) {
            qWarning("Unable to queue the reply to agent message %s: %s", qUtf8Printable(id),
                     qUtf8Printable(error));
            continue;
        }
        // The answer left this conversation; say so under it.
        host_->appendAgentNotice(sessionId, generation,
                                 {{QStringLiteral("message_id"), reply.id},
                                  {QStringLiteral("kind"), reply.kind},
                                  {QStringLiteral("state"), QStringLiteral("replied")},
                                  {QStringLiteral("reply_to"), request->id},
                                  {QStringLiteral("direction"), QStringLiteral("outgoing")},
                                  {QStringLiteral("peer_number"), request->fromNumber},
                                  {QStringLiteral("peer_title"), request->fromTitle}});
    }
}

QHash<QString, QJsonObject> AgentBroker::automaticTurnSenders() const {
    QHash<QString, QJsonObject> senders;
    for (auto it = turns_.cbegin(); it != turns_.cend(); ++it)
        if (it->automatic && !it->sender.value(QStringLiteral("number")).toString().isEmpty())
            senders.insert(it.key(), it->sender);
    return senders;
}

void AgentBroker::expireOverdue() {
    for (AgentMessageRecord message :
         journal_->overdueAgentRequests(QDateTime::currentDateTimeUtc())) {
        markState(message, QStringLiteral("expired"), QStringLiteral("no_reply_in_time"));
        notice(message, QStringLiteral("expired"), QStringLiteral("no_reply_in_time"));
    }
}

void AgentBroker::markState(AgentMessageRecord& message, const QString& state,
                            const QString& reason) {
    message.state = state;
    message.error = reason;
    QString error;
    if (!journal_->updateAgentMessage(message, &error))
        qWarning("Unable to record agent message %s as %s: %s", qUtf8Printable(message.id),
                 qUtf8Printable(state), qUtf8Printable(error));
}

void AgentBroker::notice(const AgentMessageRecord& message, const QString& state,
                         const QString& reason) {
    const QJsonObject common{{QStringLiteral("message_id"), message.id},
                             {QStringLiteral("kind"), message.kind},
                             {QStringLiteral("state"), state},
                             {QStringLiteral("reason"), reason}};
    QJsonObject outgoing = common;
    outgoing.insert(QStringLiteral("direction"), QStringLiteral("outgoing"));
    outgoing.insert(QStringLiteral("peer_number"), message.toNumber);
    host_->appendAgentNotice(message.fromSession, message.fromGeneration, outgoing);
    if (state != QStringLiteral("held"))
        return;
    QJsonObject incoming = common;
    incoming.insert(QStringLiteral("direction"), QStringLiteral("incoming"));
    incoming.insert(QStringLiteral("peer_number"), message.fromNumber);
    incoming.insert(QStringLiteral("peer_title"), message.fromTitle);
    host_->appendAgentNotice(message.toSession, message.toGeneration, incoming);
}

void AgentBroker::forget(const QString& sessionId) {
    const QString prefix = sessionId + QLatin1Char(':');
    for (auto it = tokenBySession_.begin(); it != tokenBySession_.end();) {
        if (it.key().startsWith(prefix)) {
            sessionByToken_.remove(it.value());
            it = tokenBySession_.erase(it);
        } else {
            ++it;
        }
    }
    turns_.remove(sessionId);
    pendingDeliveries_.remove(sessionId);
}

void AgentBroker::resume() {
    expireOverdue();
    for (const QString& sessionId : journal_->sessionsWithQueuedAgentMessages())
        scheduleDelivery(sessionId);
}

} // namespace ketplus::daemon
