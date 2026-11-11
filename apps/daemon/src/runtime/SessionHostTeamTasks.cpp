// ADR 0112 §6: the task side of teams in the session runtime — the task's worktree, the
// owner's conversation, team messages, and the teams.tasks/1 requests.
#include "runtime/SessionHost.h"

#include "protocol/DaemonProtocol.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUuid>

namespace ketplus::daemon {
namespace {

// A short synchronous git call for the team's bookkeeping; nothing when git failed.
std::optional<QString> gitOutput(const QString& directory, const QStringList& arguments) {
    if (directory.isEmpty())
        return std::nullopt;
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("--no-optional-locks")} + arguments);
    if (!process.waitForFinished(15000) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0)
        return std::nullopt;
    return QString::fromUtf8(process.readAllStandardOutput());
}

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

// ADR 0112, open point 1: `<repository>-team-<id>-t-<n>` beside the repository, as the
// worktrees of ADR 0111 are, on branch `team/<id>/t-<n>` from the integration branch.
std::optional<teams::TaskWorktree>
SessionHost::createTaskWorktree(const teams::Team& team, const teams::Task& task, QString* error) {
    const QString base =
        team.integrationBranch.isEmpty() ? QStringLiteral("HEAD") : team.integrationBranch;
    const auto commit =
        gitOutput(team.repository, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
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
    if (!gitOutput(team.repository,
                   {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-b"), branch,
                    QStringLiteral("--"), path, commit->trimmed()})) {
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
        gitOutput(worktree, {QStringLiteral("status"), QStringLiteral("--porcelain")});
    if (!status)
        return std::nullopt;
    return !status->trimmed().isEmpty();
}

QString SessionHost::headCommit(const QString& worktree) const {
    return gitOutput(worktree, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")})
        .value_or(QString{})
        .trimmed();
}

QString SessionHost::postTeamMessage(const teams::TeamMessage& message) {
    return agents_->postTeamMessage(
        message.fromSession, QStringLiteral("Team %1").arg(message.teamName), message.toSession,
        message.kind, message.body, message.expectReply);
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
    return refused(QStringLiteral("unknown_operation"),
                   QStringLiteral("Unknown team request %1.").arg(type));
}

} // namespace ketplus::daemon
