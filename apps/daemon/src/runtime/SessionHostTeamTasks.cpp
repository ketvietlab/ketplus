// ADR 0114 §6: the task side of teams in the session runtime — the task's worktree, the
// owner's conversation, team messages, and the teams.tasks/1 requests.
#include "runtime/SessionHost.h"

#include "protocol/DaemonProtocol.h"
#include "runtime/TeamGit.h"
#include "runtime/TeamPullRequests.h"
#include "teams/TeamBranchNames.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUuid>

#include <algorithm>

namespace ketplus::daemon {
namespace {

QJsonObject refused(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

QStringList strings(const QJsonValue& value) {
    QStringList result;
    for (const auto& item : value.toArray())
        result.append(item.toString());
    return result;
}

} // namespace

std::optional<teams::TaskWorktree>
SessionHost::createTaskWorktree(const teams::Team& team, const teams::Task& task, QString* error) {
    teams::WorktreeAnswer answer = team_git::createWorktree(team, task);
    *error = answer.error;
    return answer.worktree;
}

// T-90: a short local git check (no fetch), on the caller's thread like conversationRepository.
teams::RepositoryCheck SessionHost::checkRepository(const QString& path,
                                                    const QString& integrationBranch) const {
    return team_git::checkRepository(path, integrationBranch);
}

// T-93: answered from the last listing of the repository's branches, never by git on the
// event loop (a slow git would hold every reply). The listing is asked again on its own thread
// when it is missing or old, and an old one still answers meanwhile. Teams' repositories are
// listed when the teams load; until a repository's first listing arrives the answer is unknown,
// the task's history says so, and the worktree's creation, which runs git off this thread
// anyway, checks the name for good. An empty `branch` only asks for the listing.
std::optional<bool> SessionHost::branchTaken(const QString& repository,
                                             const QString& branch) const {
    if (repository.isEmpty())
        return std::nullopt;
    BranchNames& known = branchNames_[repository];
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!known.reading && (!known.read.isValid() || known.read.msecsTo(now) > branchNamesAgeMs)) {
        known.reading = true;
        auto* self = const_cast<SessionHost*>(this);
        self->teamBranchNames_.start([guard = QPointer<SessionHost>(self), repository]() {
            const auto names = team_git::branchNames(repository);
            QMetaObject::invokeMethod(
                guard,
                [guard, repository, names] {
                    if (!guard)
                        return;
                    BranchNames& entry = guard->branchNames_[repository];
                    entry.reading = false;
                    if (!names)
                        return;
                    entry.names = *names;
                    entry.read = QDateTime::currentDateTimeUtc();
                },
                Qt::QueuedConnection);
        });
    }
    if (!known.read.isValid() || branch.isEmpty())
        return std::nullopt;
    return teams::branch_names::clashesWithAny(branch, known.names);
}

// A prepared command's git runs on the thread of the team's other questions, in their order.
void SessionHost::askWorktree(const teams::Team& team, const teams::Task& task,
                              teams::WorktreeDone done) {
    teamGit_.start(
        [guard = QPointer<SessionHost>(this), team, task, done = std::move(done)]() mutable {
            teams::WorktreeAnswer answer = team_git::createWorktree(team, task);
            QMetaObject::invokeMethod(
                guard, [done = std::move(done), answer = std::move(answer)] { done(answer); },
                Qt::QueuedConnection);
        });
}

void SessionHost::askHandoff(const teams::Team& team, const teams::Task& task,
                             teams::HandoffDone done) {
    teamGit_.start(
        [guard = QPointer<SessionHost>(this), team, task, done = std::move(done)]() mutable {
            teams::HandoffFacts facts = team_git::handoff(team, task);
            QMetaObject::invokeMethod(
                guard, [done = std::move(done), facts = std::move(facts)] { done(facts); },
                Qt::QueuedConnection);
        });
}

QJsonObject SessionHost::pointRequest(const QString& sessionId, const QString& worktree) const {
    const auto it = sessions_.constFind(sessionId);
    return QJsonObject{
        {QStringLiteral("protocol_version"), protocol::currentVersion},
        {QStringLiteral("message_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("message_type"), QStringLiteral("session.worktree.switch")},
        {QStringLiteral("sent_at"), protocol::timestamp()},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("session_generation"), it->stored.generation},
        {QStringLiteral("payload"),
         QJsonObject{{QStringLiteral("worktree_path"), worktree},
                     {QStringLiteral("expected_context_revision"), it->stored.contextRevision}}}};
}

namespace {

QString pointError(const QJsonObject& result) {
    return result.value(QStringLiteral("ok")).toBool()
               ? QString{}
               : result.value(QStringLiteral("error_message")).toString();
}

} // namespace

QString SessionHost::pointConversationAt(const QString& sessionId, const QString& worktree) {
    const auto it = sessions_.constFind(sessionId);
    if (it == sessions_.cend())
        return QStringLiteral("The conversation no longer exists.");
    if (QDir::cleanPath(it->stored.worktreePath) == QDir::cleanPath(worktree))
        return {};
    return pointError(switchWorktree(pointRequest(sessionId, worktree)));
}

// T-28: the checks that need no git run here, git runs on the team's git thread, and the
// switch is checked again and stored here once git answers: the session may have changed.
void SessionHost::askPointConversationAt(const QString& sessionId, const QString& worktree,
                                         teams::PointDone done) {
    const auto it = sessions_.constFind(sessionId);
    if (it == sessions_.cend()) {
        done(QStringLiteral("The conversation no longer exists."));
        return;
    }
    if (QDir::cleanPath(it->stored.worktreePath) == QDir::cleanPath(worktree)) {
        done({});
        return;
    }
    Session* session = nullptr;
    QString target;
    if (const QJsonObject refused =
            switchRefusal(pointRequest(sessionId, worktree), &session, &target);
        !refused.isEmpty()) {
        done(pointError(refused));
        return;
    }
    teamGit_.start([guard = QPointer<SessionHost>(this), sessionId, worktree, target,
                    done = std::move(done)]() mutable {
        WorktreeFacts facts = worktreeFacts(target);
        QMetaObject::invokeMethod(
            guard,
            [guard, sessionId, worktree, target, facts = std::move(facts), done = std::move(done)] {
                if (!guard->sessions_.contains(sessionId)) {
                    done(QStringLiteral("The conversation no longer exists."));
                    return;
                }
                const QJsonObject request = guard->pointRequest(sessionId, worktree);
                Session* current = nullptr;
                QString again;
                if (const QJsonObject refused = guard->switchRefusal(request, &current, &again);
                    !refused.isEmpty()) {
                    done(pointError(refused));
                    return;
                }
                if (again != target) {
                    done(QStringLiteral("The worktree changed while git was asked."));
                    return;
                }
                done(pointError(
                    guard->finishSwitch(*current, target, facts,
                                        request.value(QStringLiteral("message_id")).toString())));
            },
            Qt::QueuedConnection);
    });
}

std::optional<bool> SessionHost::hasUncommittedWork(const QString& worktree) const {
    return team_git::hasUncommittedWork(worktree);
}

std::optional<teams::TaskDiff> SessionHost::taskDiff(const teams::Team& team,
                                                     const teams::Task& task) const {
    return team_git::taskDiff(team, task);
}

// A review's diff is read on the thread of the team's other questions to git.
void SessionHost::askDiff(const teams::Team& team, const teams::Task& task,
                          teams::TaskDiffDone done) {
    teamGit_.start(
        [guard = QPointer<SessionHost>(this), team, task, done = std::move(done)]() mutable {
            std::optional<teams::TaskDiff> diff = team_git::taskDiff(team, task);
            QMetaObject::invokeMethod(
                guard, [done = std::move(done), diff = std::move(diff)] { done(diff); },
                Qt::QueuedConnection);
        });
}

QString SessionHost::taskCommit(const teams::Team& team, const teams::Task& task) const {
    return team_git::taskCommit(team, task);
}

std::optional<QString> SessionHost::ownHead(const teams::Team& team,
                                            const teams::Task& task) const {
    return team_git::ownHead(team, task);
}

std::optional<QStringList> SessionHost::changedPaths(const teams::Team& team,
                                                     const teams::Task& task) const {
    return team_git::changedPaths(team, task);
}

std::optional<bool> SessionHost::isMerged(const teams::Team& team, const teams::Task& task) const {
    return team_git::isMerged(team, task);
}

// The daemon's own checks ask git on another thread: a slow repository holds no client and no
// other conversation. The team and the tasks are copies; the answer comes back queued.
void SessionHost::askTasks(const teams::Team& team, const QList<teams::Task>& tasks,
                           const teams::TaskQuestion question, teams::TaskFactsDone done) {
    teamGit_.start([guard = QPointer<SessionHost>(this), team, tasks, question,
                    done = std::move(done)]() mutable {
        QHash<QString, teams::TaskFacts> facts = team_git::facts(team, tasks, question);
        QMetaObject::invokeMethod(
            guard, [done = std::move(done), facts = std::move(facts)] { done(facts); },
            Qt::QueuedConnection);
    });
}

// ADR 0118: the remote is asked on its own worker, bounded per request and per batch, so a
// slow network holds no question to git and no client.
void SessionHost::askPullRequests(const teams::Team& team, const QStringList& urls,
                                  teams::PullRequestsDone done) {
    teamRemote_.start([guard = QPointer<SessionHost>(this), directory = team.repository, urls,
                       stopping = teamRemoteStopping_, done = std::move(done)]() mutable {
        QHash<QString, teams::PullRequestFact> facts =
            team_pull_requests::ask(directory, urls, {}, {}, stopping.get());
        QMetaObject::invokeMethod(
            guard, [done = std::move(done), facts = std::move(facts)] { done(facts); },
            Qt::QueuedConnection);
    });
}

// T-40: on the same worker as the pull request probe, bounded, so a slow network holds no
// question to git and no client.
void SessionHost::askPostReviewComment(const teams::Team& team, const teams::Task& task,
                                       const QString& body, teams::ReviewCommentDone done) {
    teamRemote_.start([guard = QPointer<SessionHost>(this),
                       directory = teams::taskScope(team, task).repository, url = task.pullRequest,
                       body, stopping = teamRemoteStopping_, done = std::move(done)]() mutable {
        teams::ReviewComment posted =
            team_pull_requests::comment(directory, url, body, {}, 30000, stopping.get());
        QMetaObject::invokeMethod(
            guard, [done = std::move(done), posted = std::move(posted)] { done(posted); },
            Qt::QueuedConnection);
    });
}

void SessionHost::teamRequestFinished(const QString& sessionId, const AgentMessageRecord& request,
                                      const QString& text, const QString& failure) {
    if (teams_ && request.kind == QStringLiteral("review"))
        teams_->reviewFinished(sessionId, request.id, text, AgentBroker::verdictHasNoFindings(text),
                               failure);
}

// ADR 0114 §8: a member that asked a conversation outside its team would wait for an answer
// that never comes. Messages inside the team are the team's own and are handled there.
void SessionHost::agentMessageFailed(const AgentMessageRecord& message, const QString& reason) {
    const teams::Team* team = teams_ ? teams_->teamOf(message.fromSession) : nullptr;
    if (!team || team->member(message.toSession))
        return;
    teams_->outsideMessageFailed(message.fromSession, QLatin1Char('#') + message.toNumber, reason);
}

QString SessionHost::postTeamMessage(const teams::TeamMessage& message) {
    return agents_->postTeamMessage(
        message.fromSession, QStringLiteral("Team %1").arg(message.teamName), message.toSession,
        message.kind, message.body, message.expectReply, nullptr, message.id);
}

bool SessionHost::teamLimitsMessage(const AgentMessageRecord& message) const {
    const teams::Team* team = teams_ ? teams_->teamOf(message.toSession) : nullptr;
    // An empty sender is the team itself; a conversation always sends with its session id.
    return team && (message.fromSession.isEmpty() || team->member(message.fromSession));
}

QString SessionHost::takeTeamTurn(const QString& sessionId) {
    return teams_ ? teams_->takeMemberTurn(sessionId) : QString{};
}

void SessionHost::releaseTeamHeldMessages(const QStringList& sessionIds) {
    agents_->releaseTeamHeldMessages(sessionIds);
}

bool SessionHost::teamMessagePosted(const QString& id) const {
    return !id.isEmpty() && journal_->agentMessage(id).has_value();
}

void SessionHost::noteInConversation(const QString& sessionId, const QJsonObject& note) {
    const auto it = sessions_.constFind(sessionId);
    if (it != sessions_.cend())
        appendAgentNotice(sessionId, it->stored.generation, note);
}

// The user's task commands that reach git: assign, hand over, and move, which reads the
// worktree when the task goes to review. handleCommand refuses what it always refused.
void SessionHost::prepareTeamTask(const QJsonObject& request, std::function<void()> then) {
    using Kind = teams::TeamCoordinator::TaskRequest::Kind;
    const QString type = request.value(QStringLiteral("message_type")).toString();
    std::optional<Kind> kind;
    if (type == QStringLiteral("team.task.assign"))
        kind = Kind::Assign;
    else if (type == QStringLiteral("team.task.handover"))
        kind = Kind::Handover;
    else if (type == QStringLiteral("team.task.move"))
        kind = Kind::Move;
    if (!kind || !teams_ || !teams_->isLoaded()) {
        then();
        return;
    }
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    teams_->prepare({.kind = *kind,
                     .teamId = payload.value(QStringLiteral("team_id")).toString().toLongLong(),
                     .taskId = payload.value(QStringLiteral("task_id")).toString(),
                     .actor = QStringLiteral("user"),
                     .owner = payload.value(QStringLiteral("owner")).toString(),
                     .reason = payload.value(QStringLiteral("reason")).toString(),
                     .to = payload.value(QStringLiteral("state")).toString()},
                    std::move(then));
}

// teams.tasks/1, for trusted local clients: the user acts on the team's tasks.
QJsonObject SessionHost::teamTaskCommand(const QString& type, const QJsonObject& request) {
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const qint64 teamId = payload.value(QStringLiteral("team_id")).toString().toLongLong();
    const QString taskId = payload.value(QStringLiteral("task_id")).toString();
    const auto answer = [&](const teams::TeamResult& result, const QString& id) {
        if (!result.ok())
            return refused(result.error->code, result.error->message);
        const teams::Team* team = teams_->team(result.teamId);
        const teams::Task* task = team ? team->task(id) : nullptr;
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("team"), team ? teams_->teamJson(*team) : QJsonObject{}},
            {QStringLiteral("task"), task ? teams::taskPageJson(*task) : QJsonObject{}}};
    };
    const teams::Team* team = teams_->team(teamId);
    if (!team)
        return refused(QStringLiteral("team_not_found"),
                       QStringLiteral("The team no longer exists."));
    const QString user = QStringLiteral("user");
    if (type == QStringLiteral("team.task.list")) {
        QJsonArray tasks;
        const QString wanted = payload.value(QStringLiteral("state")).toString();
        for (const teams::Task& task : team->tasks)
            if (wanted.isEmpty() || task.state == wanted)
                tasks.append(teams::toJson(task, false));
        return {{QStringLiteral("ok"), true}, {QStringLiteral("tasks"), tasks}};
    }
    if (type == QStringLiteral("team.task.get")) {
        const teams::Task* task = team->task(taskId);
        if (!task)
            return refused(QStringLiteral("task_not_found"),
                           QStringLiteral("There is no task %1.").arg(taskId));
        // ADR 0115: one page of history, newest last; history_before reads the page before.
        const QJsonValue before = payload.value(QStringLiteral("history_before"));
        return {
            {QStringLiteral("ok"), true},
            {QStringLiteral("task"),
             teams::taskPageJson(
                 *task, payload.value(QStringLiteral("history_limit")).toInt(teams::defaultPage),
                 before.isDouble() ? before.toInteger() : -1)}};
    }
    if (type == QStringLiteral("team.task.create")) {
        QString created;
        const auto result = teams_->createTask(
            teamId,
            {.title = payload.value(QStringLiteral("title")).toString(),
             .description = payload.value(QStringLiteral("description")).toString(),
             .lane = payload.value(QStringLiteral("lane")).toString(),
             .dependsOn = strings(payload.value(QStringLiteral("depends_on"))),
             .criteria = strings(payload.value(QStringLiteral("criteria"))),
             // T-93: optional, as on the leader's team_task_create.
             .branchType = payload.value(QStringLiteral("branch_type")).toString(),
             .branchSlug = payload.value(QStringLiteral("branch_slug")).toString()},
            user, &created);
        return answer(result, created);
    }
    if (type == QStringLiteral("team.task.update"))
        return answer(teams_->updateTask(teamId, taskId, payload, user), taskId);
    if (type == QStringLiteral("team.task.assign"))
        return answer(teams_->assignTask(teamId, taskId,
                                         payload.value(QStringLiteral("owner")).toString(), user),
                      taskId);
    if (type == QStringLiteral("team.task.handover"))
        return answer(
            teams_->handoverTask(teamId, taskId, payload.value(QStringLiteral("owner")).toString(),
                                 payload.value(QStringLiteral("reason")).toString(), user),
            taskId);
    if (type == QStringLiteral("team.task.move"))
        return answer(teams_->moveTask(teamId, taskId,
                                       payload.value(QStringLiteral("state")).toString(), user,
                                       payload.value(QStringLiteral("note")).toString()),
                      taskId);
    if (type == QStringLiteral("team.task.criteria.set"))
        return answer(teams_->setCriteria(teamId, taskId,
                                          strings(payload.value(QStringLiteral("criteria"))), user),
                      taskId);
    // ADR 0114 §3: decisions, answered only by the user through a client.
    const QString decisionId = payload.value(QStringLiteral("decision_id")).toString();
    if (type == QStringLiteral("team.decision.list")) {
        // ADR 0115: newest first, one page; before_id reads older ones.
        const QString wanted = payload.value(QStringLiteral("state")).toString();
        const QString before = payload.value(QStringLiteral("before_id")).toString();
        const int limit =
            std::clamp(payload.value(QStringLiteral("limit")).toInt(teams::defaultPage), 1,
                       teams::maximumPage);
        QJsonArray decisions;
        bool reached = before.isEmpty();
        bool more = false;
        for (auto it = team->decisions.crbegin(); it != team->decisions.crend(); ++it) {
            if (!reached) {
                reached = it->id == before;
                continue;
            }
            if (!wanted.isEmpty() && it->state != wanted)
                continue;
            if (decisions.size() == limit) {
                more = true;
                break;
            }
            decisions.append(teams::toJson(*it));
        }
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("decisions"), decisions},
                {QStringLiteral("has_more"), more}};
    }
    if (type == QStringLiteral("team.decision.answer")) {
        const QString choice = payload.value(QStringLiteral("choice")).toString();
        const auto result = teams_->answerDecision(
            teamId, decisionId,
            choice.isEmpty() ? payload.value(QStringLiteral("text")).toString() : choice, user);
        if (!result.ok())
            return refused(result.error->code, result.error->message);
        const teams::Team* current = teams_->team(teamId);
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("team"), current ? teams_->teamJson(*current) : QJsonObject{}}};
    }
    return refused(QStringLiteral("unknown_operation"),
                   QStringLiteral("Unknown team request %1.").arg(type));
}

} // namespace ketplus::daemon
