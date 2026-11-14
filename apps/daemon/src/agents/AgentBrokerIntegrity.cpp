// ketplus#295 (ADR 0067, ADR 0114 §2): a Codex reviewer reviews with its own Full access. The
// broker records each reviewed branch before the review turn starts and again after it ends,
// with any status. A change voids that review: no verdict, no round, nothing reverted, and the
// working conversation (and for a team, its history and leader) is told what changed.
#include "agents/AgentBroker.h"
#include "agents/ReviewerPolicy.h"
#include "storage/LocalJournal.h"

#include <QJsonArray>
#include <QPointer>

#include <algorithm>
#include <memory>

namespace ketplus::daemon {
namespace {

bool reviewRequest(const AgentMessageRecord& message) {
    return message.kind == QStringLiteral("review") && message.expectReply;
}

QStringList idsOf(const QList<AgentMessageRecord>& messages) {
    QStringList ids;
    for (const AgentMessageRecord& message : messages)
        ids.append(message.id);
    return ids;
}

// Records every scope; `done` receives the snapshots by request id once all are in.
void captureAll(QObject* owner, const QHash<QString, review_integrity::Scope>& scopes,
                std::function<void(QHash<QString, review_integrity::Snapshot>)> done) {
    auto snapshots = std::make_shared<QHash<QString, review_integrity::Snapshot>>();
    auto left = std::make_shared<qsizetype>(scopes.size());
    auto finish = std::make_shared<decltype(done)>(std::move(done));
    for (auto it = scopes.cbegin(); it != scopes.cend(); ++it) {
        const QString id = it.key();
        review_integrity::capture(owner, it.value(),
                                  [snapshots, left, finish, id](review_integrity::Snapshot shot) {
                                      snapshots->insert(id, std::move(shot));
                                      if (--*left == 0)
                                          (*finish)(*snapshots);
                                  });
    }
}

} // namespace

std::optional<review_integrity::Scope>
AgentBroker::reviewScope(const AgentConversation& receiver,
                         const AgentMessageRecord& request) const {
    review_integrity::Scope scope;
    if (request.fromSession.isEmpty()) {
        // A team's review round: the task's worktree and branch.
        const auto team = host_->teamReviewScope(receiver.sessionId, request.id);
        if (!team)
            return std::nullopt;
        scope = *team;
    } else {
        // ADR 0067: a conversation's turn, in the working conversation's worktree.
        const auto working = host_->agentConversation(request.fromSession);
        scope.worktree = working ? working->worktree : QString{};
        scope.workingSession = request.fromSession;
    }
    // Without a worktree of its own to review, the reviewer's checkout is what it could change.
    if (scope.worktree.isEmpty())
        scope.worktree = receiver.worktree;
    if (scope.remoteDirectory.isEmpty())
        scope.remoteDirectory = scope.worktree;
    return scope;
}

bool AgentBroker::branchesRecorded(const QString& sessionId, const AgentConversation& receiver,
                                   const QList<AgentMessageRecord>& deliverable) {
    if (!reviewer::checksBranch(receiver.provider) ||
        std::none_of(deliverable.cbegin(), deliverable.cend(), reviewRequest)) {
        recordedBranches_.remove(sessionId);
        return true;
    }
    const QStringList ids = idsOf(deliverable);
    const qint64 turn = host_->agentTurnSequence(sessionId);
    const auto found = recordedBranches_.constFind(sessionId);
    if (found != recordedBranches_.cend() && found->messageIds == ids && found->turn == turn)
        return found->pending == 0;
    RecordedBranches record;
    record.messageIds = ids;
    record.turn = turn;
    record.token = ++recordToken_;
    for (const AgentMessageRecord& message : deliverable)
        if (reviewRequest(message))
            if (const auto scope = reviewScope(receiver, message))
                record.scopes.insert(message.id, *scope);
    record.pending = record.scopes.isEmpty() ? 0 : 1;
    const quint64 token = record.token;
    const auto scopes = record.scopes;
    recordedBranches_.insert(sessionId, record);
    if (scopes.isEmpty())
        return true;
    captureAll(this, scopes,
               [self = QPointer<AgentBroker>(this), sessionId,
                token](const QHash<QString, review_integrity::Snapshot>& before) {
                   if (!self)
                       return;
                   const auto it = self->recordedBranches_.find(sessionId);
                   if (it == self->recordedBranches_.end() || it->token != token)
                       return;
                   it->before = before;
                   it->pending = 0;
                   self->scheduleDelivery(sessionId);
               });
    return false;
}

QString AgentBroker::takeRecordedBranches(const QString& sessionId, TurnOrigin& origin,
                                          const QString& providerText) {
    const auto found = recordedBranches_.constFind(sessionId);
    if (found == recordedBranches_.cend())
        return providerText;
    origin.scopes = found->scopes;
    origin.before = found->before;
    recordedBranches_.erase(found);
    if (origin.scopes.isEmpty())
        return providerText;
    return review_integrity::codexReviewInstructions() + QStringLiteral("\n\n") + providerText;
}

void AgentBroker::checkReviewedBranches(const QString& sessionId, const qint64 generation,
                                        const QString& status, const TurnOrigin& origin) {
    // A team request's head_commit is read again; the worktree and branch stay the recorded
    // ones, so a task moved meanwhile is still compared with what was reviewed.
    QHash<QString, review_integrity::Scope> scopes = origin.scopes;
    for (auto it = scopes.begin(); it != scopes.end(); ++it) {
        const auto request = journal_->agentMessage(it.key());
        if (!request || !request->fromSession.isEmpty())
            continue;
        if (const auto now = host_->teamReviewScope(sessionId, it.key()))
            it->taskHead = now->taskHead;
    }
    captureAll(this, scopes,
               [self = QPointer<AgentBroker>(this), sessionId, generation, status,
                origin](const QHash<QString, review_integrity::Snapshot>& after) {
                   if (!self)
                       return;
                   QSet<QString> voided;
                   for (auto it = origin.scopes.cbegin(); it != origin.scopes.cend(); ++it) {
                       const QString& id = it.key();
                       const review_integrity::Snapshot before = origin.before.value(id);
                       const review_integrity::Snapshot now = after.value(id);
                       if (!review_integrity::remoteCompared(it.value(), before, now))
                           qWarning("Review %s: the remote branch %s could not be read; it is "
                                    "not compared.",
                                    qUtf8Printable(id), qUtf8Printable(it->branch));
                       const QJsonArray changed = review_integrity::changes(before, now);
                       if (changed.isEmpty())
                           continue;
                       auto request = self->journal_->agentMessage(id);
                       if (!request || request->state != QStringLiteral("delivered"))
                           continue;
                       voided.insert(id);
                       self->reviewTampered(sessionId, *request, it.value(), changed);
                   }
                   self->answerRequests(sessionId, generation, status, origin, voided);
               });
}

void AgentBroker::reviewTampered(const QString& sessionId, AgentMessageRecord& request,
                                 const review_integrity::Scope& scope, const QJsonArray& changes) {
    markState(request, QStringLiteral("failed"), QStringLiteral("review_tampered"));
    const auto reviewer = host_->agentConversation(sessionId);
    const QString summary = review_integrity::describe(changes);
    qWarning("Review %s by %s changed the reviewed branch: %s", qUtf8Printable(request.id),
             qUtf8Printable(sessionId), qUtf8Printable(summary));
    // The working conversation reads what changed; nothing is reverted.
    if (const auto working = host_->agentConversation(scope.workingSession))
        host_->appendAgentNotice(
            working->sessionId, working->generation,
            {{QStringLiteral("message_id"), request.id},
             {QStringLiteral("kind"), QStringLiteral("review")},
             {QStringLiteral("state"), QStringLiteral("review_tampered")},
             {QStringLiteral("reason"), QStringLiteral("review_tampered")},
             {QStringLiteral("direction"), QStringLiteral("outgoing")},
             {QStringLiteral("peer_number"), reviewer ? reviewer->number : request.toNumber},
             {QStringLiteral("peer_title"), reviewer ? reviewer->title : QString{}},
             {QStringLiteral("changes"), changes},
             {QStringLiteral("summary"), summary}});
    // ADR 0114 §2: the team's history and its leader.
    if (request.fromSession.isEmpty())
        host_->teamReviewTampered(sessionId, request, changes);
}

} // namespace ketplus::daemon
