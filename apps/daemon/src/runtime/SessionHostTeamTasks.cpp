// ADR 0114 §6: the task side of teams in the session runtime — the task's worktree, the
// owner's conversation, team messages, and the teams.tasks/1 requests.
#include "runtime/SessionHost.h"

#include "protocol/DaemonProtocol.h"
#include "runtime/TeamGit.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUuid>

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

// ADR 0114, open point 1: `<repository>-team-<id>-t-<n>` beside the repository, as the
// worktrees of ADR 0111 are, on branch `team/<id>/t-<n>` from the integration branch.
std::optional<teams::TaskWorktree>
SessionHost::createTaskWorktree(const teams::Team& team, const teams::Task& task, QString* error) {
    const QString base =
        team.integrationBranch.isEmpty() ? QStringLiteral("HEAD") : team.integrationBranch;
    const auto commit =
        team_git::output(team.repository, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                           base + QStringLiteral("^{commit}")});
    if (!commit) {
        *error = QStringLiteral("The integration branch %1 does not exist in %2.")
                     .arg(base, team.repository);
        return std::nullopt;
    }
    const QString slug = task.id.toLower();
    const QString branch = QStringLiteral("team/%1/%2").arg(team.id).arg(slug);
    const QFileInfo repository(team.repository);
    const QString path = QDir::cleanPath(repository.dir().filePath(
        QStringLiteral("%1-team-%2-%3").arg(repository.fileName()).arg(team.id).arg(slug)));
    if (QFileInfo::exists(path)) {
        *error = QStringLiteral("The worktree path %1 already exists.").arg(path);
        return std::nullopt;
    }
    if (!team_git::output(team.repository,
                          {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-b"),
                           branch, QStringLiteral("--"), path, commit->trimmed()})) {
        *error = QStringLiteral("git could not create the worktree %1 on %2.").arg(path, branch);
        return std::nullopt;
    }
    return teams::TaskWorktree{.branch = branch,
                               .worktree = QFileInfo(path).canonicalFilePath(),
                               .baseCommit = commit->trimmed()};
}

QString SessionHost::pointConversationAt(const QString& sessionId, const QString& worktree) {
    const auto it = sessions_.constFind(sessionId);
    if (it == sessions_.cend())
        return QStringLiteral("The conversation no longer exists.");
    if (QDir::cleanPath(it->stored.worktreePath) == QDir::cleanPath(worktree))
        return {};
    const QJsonObject result = switchWorktree(
        {{QStringLiteral("protocol_version"), protocol::currentVersion},
         {QStringLiteral("message_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
         {QStringLiteral("message_type"), QStringLiteral("session.worktree.switch")},
         {QStringLiteral("sent_at"), protocol::timestamp()},
         {QStringLiteral("session_id"), sessionId},
         {QStringLiteral("session_generation"), it->stored.generation},
         {QStringLiteral("payload"),
          QJsonObject{{QStringLiteral("worktree_path"), worktree},
                      {QStringLiteral("expected_context_revision"), it->stored.contextRevision}}}});
    return result.value(QStringLiteral("ok")).toBool()
               ? QString{}
               : result.value(QStringLiteral("error_message")).toString();
}

std::optional<bool> SessionHost::hasUncommittedWork(const QString& worktree) const {
    const auto status =
        team_git::output(worktree, {QStringLiteral("status"), QStringLiteral("--porcelain")});
    if (!status)
        return std::nullopt;
    return !status->trimmed().isEmpty();
}

std::optional<teams::TaskDiff> SessionHost::taskDiff(const teams::Team& team,
                                                     const teams::Task& task) const {
    const QString directory = task.worktree.isEmpty() ? team.repository : task.worktree;
    // The branch against the integration branch it was cut from (ADR 0114 §2).
    const QString range = QStringLiteral("%1...%2").arg(
        team.integrationBranch.isEmpty() ? task.baseCommit : team.integrationBranch, task.branch);
    const auto diff =
        team_git::output(directory, {QStringLiteral("diff"), QStringLiteral("--no-color"),
                                     QStringLiteral("--no-ext-diff"), range});
    const auto names =
        team_git::output(directory, {QStringLiteral("diff"), QStringLiteral("--name-only"), range});
    if (!diff || !names)
        return std::nullopt;
    return teams::TaskDiff{.diff = *diff,
                           .files = names->split(QLatin1Char('\n'), Qt::SkipEmptyParts)};
}

QString SessionHost::taskCommit(const teams::Team& team, const teams::Task& task) const {
    return team_git::taskCommit(team, task);
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

void SessionHost::noteInConversation(const QString& sessionId, const QJsonObject& note) {
    const auto it = sessions_.constFind(sessionId);
    if (it != sessions_.cend())
        appendAgentNotice(sessionId, it->stored.generation, note);
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
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("team"), team ? teams_->teamJson(*team) : QJsonObject{}},
                           {QStringLiteral("task"), task ? teams::toJson(*task) : QJsonObject{}}};
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
        return {{QStringLiteral("ok"), true}, {QStringLiteral("task"), teams::toJson(*task)}};
    }
    if (type == QStringLiteral("team.task.create")) {
        QString created;
        const auto result = teams_->createTask(
            teamId,
            {.title = payload.value(QStringLiteral("title")).toString(),
             .description = payload.value(QStringLiteral("description")).toString(),
             .lane = payload.value(QStringLiteral("lane")).toString(),
             .dependsOn = strings(payload.value(QStringLiteral("depends_on"))),
             .criteria = strings(payload.value(QStringLiteral("criteria")))},
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
        QJsonArray decisions;
        const QString wanted = payload.value(QStringLiteral("state")).toString();
        for (const teams::Decision& decision : team->decisions)
            if (wanted.isEmpty() || decision.state == wanted)
                decisions.append(teams::toJson(decision));
        return {{QStringLiteral("ok"), true}, {QStringLiteral("decisions"), decisions}};
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
