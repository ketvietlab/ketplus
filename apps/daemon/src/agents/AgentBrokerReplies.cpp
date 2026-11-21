// ADR 0027: a turn the broker started ends; each request that expected a reply gets the turn's
// answer, or fails with why. ADR 0067 and ADR 0114 §2 read a review's answer as its verdict;
// a Codex review's branch is checked first (ketplus#295, AgentBrokerIntegrity.cpp).
#include "agents/AgentBroker.h"
#include "storage/LocalJournal.h"

#include <QDateTime>
#include <QUuid>

namespace ketplus::daemon {
namespace {

QString nowIso() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }

QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QString truncatedUtf8(const QString& text, const qsizetype maximumBytes) {
    if (text.toUtf8().size() <= maximumBytes)
        return text;
    QString result = text.left(static_cast<qsizetype>(maximumBytes));
    while (!result.isEmpty() && result.toUtf8().size() > maximumBytes)
        result.chop(qMax<qsizetype>(1, result.size() / 16));
    return result;
}

} // namespace

void AgentBroker::finishTurn(const QString& sessionId, const qint64 generation,
                             const QString& status) {
    const auto found = turns_.constFind(sessionId);
    if (found == turns_.cend())
        return;
    const TurnOrigin origin = found.value();
    turns_.erase(found);
    if (!origin.review)
        reviewFinishedTurn(sessionId, generation, status, origin);
    if (!origin.automatic || origin.expectReply.isEmpty())
        return;
    if (!origin.scopes.isEmpty()) {
        checkReviewedBranches(sessionId, generation, status, origin);
        return;
    }
    answerRequests(sessionId, generation, status, origin, {});
}

void AgentBroker::answerRequests(const QString& sessionId, const qint64 generation,
                                 const QString& status, const TurnOrigin& origin,
                                 const QSet<QString>& voided) {
    const bool completed = status.isEmpty() || status == QStringLiteral("completed");
    const auto receiver = host_->agentConversation(sessionId);
    const QString text = origin.lastText.trimmed();
    for (const QString& id : origin.expectReply) {
        auto request = journal_->agentMessage(id);
        if (voided.contains(id) || !request || request->state != QStringLiteral("delivered"))
            continue;
        if (!completed || text.isEmpty() || !receiver || receiver->generation != generation) {
            const QString reason =
                completed ? QStringLiteral("no_reply") : QStringLiteral("turn_") + status;
            markState(*request, QStringLiteral("failed"), reason);
            if (request->fromSession.isEmpty()) {
                host_->teamRequestFinished(sessionId, *request, {}, reason);
                continue;
            }
            notice(*request, QStringLiteral("failed"), reason);
            continue;
        }
        markState(*request, QStringLiteral("answered"));
        if (request->fromSession.isEmpty()) {
            // ADR 0114: the team sent it; the team reads the answer.
            host_->teamRequestFinished(sessionId, *request, text, {});
            continue;
        }
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
        if (recordReviewReply(reply, *request)) {
            // Recorded in the working conversation without starting its turn.
        } else if (const QString error = post(reply); !error.isEmpty()) {
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

} // namespace ketplus::daemon
