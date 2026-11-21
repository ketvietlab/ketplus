// ADR 0114: the session runtime's side of agent teams.
#include "runtime/SessionHost.h"

#include "agents/AgentInstructions.h"
#include "protocol/DaemonProtocol.h"
#include "runtime/TeamGit.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUuid>

namespace ketplus::daemon {

namespace {

// The key of a team created at a path git could not name: the path itself.
QString ownPath(const QString& worktreePath) {
    const QString own = QFileInfo(worktreePath).canonicalFilePath();
    return own.isEmpty() ? worktreePath : own;
}

} // namespace

// Whether the path's repository is known without git: nothing to ask, asked before, a task
// worktree of a team, or a team is filed under the path itself. Such a team was created
// there, perhaps while git did not answer, and it and its members are found by it whatever
// git says later. A task worktree's answer, which comes from its team, is cached here.
bool SessionHost::repositoryKnown(const QString& worktreePath) const {
    // A team's task worktree is filed under the team's repository without git: a command
    // that names its owner is answered before any git runs (T-32).
    if (const QString repository = teams_ && !repositoryByWorktree_.contains(worktreePath)
                                       ? teams_->taskWorktreeRepository(worktreePath)
                                       : QString{};
        !repository.isEmpty())
        noteRepository(worktreePath, repository);
    return worktreePath.isEmpty() || repositoryByWorktree_.contains(worktreePath) ||
           failedRepositoryLookups_.contains(worktreePath) ||
           (teams_ && !teams_->teams(ownPath(worktreePath)).isEmpty());
}

// Only an answer is kept: git's, or the team's for a task worktree. A path that is no
// repository yet, or a git that did not answer in time, is asked again, but not before the
// event loop turns: a command names a path more than once, and each failed call can take
// the whole timeout.
void SessionHost::noteRepository(const QString& worktreePath, const QString& repository) const {
    if (!repository.isEmpty()) {
        repositoryByWorktree_.insert(worktreePath, repository);
        // The main worktree is its own repository: a member's conversation that starts there
        // is not asked about.
        repositoryByWorktree_.insert(repository, repository);
        return;
    }
    if (failedRepositoryLookups_.isEmpty())
        QTimer::singleShot(0, this, [this] { failedRepositoryLookups_.clear(); });
    failedRepositoryLookups_.insert(worktreePath);
}

QString SessionHost::repositoryOf(const QString& worktreePath) const {
    if (worktreePath.isEmpty())
        return {};
    if (const auto cached = repositoryByWorktree_.constFind(worktreePath);
        cached != repositoryByWorktree_.cend())
        return cached.value();
    // A path learnRepositories was not asked about is asked here, on the caller's thread.
    if (!repositoryKnown(worktreePath))
        noteRepository(worktreePath, team_git::repositoryOf(worktreePath));
    return repositoryByWorktree_.value(worktreePath, ownPath(worktreePath));
}

// Asks git about the paths it has not been asked about, on the thread of the team's other
// questions, and calls `then` on this one when repositoryOf answers each without git: at once
// when all are known.
void SessionHost::learnRepositories(const QStringList& paths, std::function<void()> then) {
    QStringList unknown;
    for (const QString& path : paths)
        if (!repositoryKnown(path) && !unknown.contains(path))
            unknown.append(path);
    if (unknown.isEmpty()) {
        then();
        return;
    }
    teamGit_.start([guard = QPointer<SessionHost>(this), unknown,
                    then = std::move(then)]() mutable {
        QList<std::pair<QString, QString>> found;
        for (const QString& path : std::as_const(unknown)) {
            // A path git just named as a main worktree is its own repository: a
            // repository is often named in two spellings, as sent and as resolved.
            const bool main = std::any_of(found.cbegin(), found.cend(),
                                          [&path](const auto& one) { return one.second == path; });
            found.append({path, main ? path : team_git::repositoryOf(path)});
        }
        QMetaObject::invokeMethod(
            guard,
            [guard, found = std::move(found), then = std::move(then)] {
                for (const auto& [path, repository] : found)
                    guard->noteRepository(path, repository);
                then();
            },
            Qt::QueuedConnection);
    });
}

// The paths a team command asks repositoryOf about: the repository it names, as it is sent
// and as team.create reads it, and where each conversation it names works.
QStringList SessionHost::teamCommandPaths(const QJsonObject& payload) const {
    QStringList paths;
    const QString repository = payload.value(QStringLiteral("repository")).toString();
    if (!repository.isEmpty())
        paths << repository << normalizedWorktree(repository);
    const std::function<void(const QJsonValue&)> collect = [&](const QJsonValue& value) {
        if (value.isString()) {
            const auto it = sessions_.constFind(value.toString());
            if (it != sessions_.cend())
                paths << it->stored.worktreePath;
        } else if (value.isArray()) {
            for (const QJsonValue& item : value.toArray())
                collect(item);
        } else if (value.isObject()) {
            const QJsonObject object = value.toObject();
            for (const QJsonValue& item : object)
                collect(item);
        }
    };
    collect(payload);
    return paths;
}

bool SessionHost::teamChecksPending() const { return teams_ && teams_->checksOpen(); }

std::optional<teams::TeamConversation>
SessionHost::teamConversation(const QString& sessionId) const {
    const auto it = sessions_.constFind(sessionId);
    if (it == sessions_.cend())
        return std::nullopt;
    const AgentConversation agent = agentConversationOf(*it);
    return teams::TeamConversation{.sessionId = sessionId,
                                   .generation = it->stored.generation,
                                   .number = agent.number,
                                   .title = it->stored.title,
                                   .provider = it->stored.adapter,
                                   .model = it->stored.model,
                                   .effort = it->stored.effort,
                                   .worktree = it->stored.worktreePath,
                                   .executionPolicy = it->stored.executionPolicy,
                                   .idle = agent.idle};
}

std::optional<teams::TeamConversation>
SessionHost::conversationByNumber(const QString& number) const {
    const auto session =
        number.isEmpty() ? std::nullopt : journal_->sessionForConversationNumber(number);
    if (!session)
        return std::nullopt;
    const auto conversation = teamConversation(session->first);
    // A number names one generation; an older one it once named is not that conversation.
    if (!conversation || conversation->generation != session->second)
        return std::nullopt;
    return conversation;
}

QString SessionHost::conversationRepository(const QString& sessionId) const {
    const auto it = sessions_.constFind(sessionId);
    return it == sessions_.cend() ? QString{} : repositoryOf(it->stored.worktreePath);
}

bool SessionHost::onTeam(const QString& sessionId) const {
    return teams_ && teams_->isMember(sessionId);
}

void SessionHost::loadTeams() {
    teams_->load();
    if (!teams_->isLoaded())
        return;
    // T-93 (review L1): list each team's branches now, so a new task's name is checked against
    // them from its first creation.
    for (const teams::Team* team : teams_->teams())
        (void)branchTaken(team->repository, QString());
    // T-26: earlier builds titled a member from its first agent message, so the conversation
    // read "[KetPlus agent message]". Only that shape is repaired; a user's rename stays.
    for (const teams::Team* team : teams_->teams()) {
        for (const teams::Member& member : team->members) {
            auto it = sessions_.find(member.sessionId);
            if (it == sessions_.end() || member.name.isEmpty() ||
                !agent_messaging::isDeliveryText(it->stored.title))
                continue;
            if (!updateSessionTitle(*it, member.name, false))
                continue;
            if (it->stored.adapter == QStringLiteral("codex")) {
                it->pendingProviderTitle = member.name;
                syncProviderTitle(*it);
            }
        }
    }
}

void SessionHost::startTeams() {
    teams_ = new teams::TeamCoordinator(journal_, static_cast<teams::TeamHostPort*>(this), this);
    // T-28: a team command is answered before the git and the messages it sets off.
    teams_->setDeferSending(true);
    loadTeams();
    connect(teams_, &teams::TeamCoordinator::teamChanged, this, [this](const QJsonObject& team) {
        emit teamEvent(QStringLiteral("team.changed"),
                       {{QStringLiteral("team"), team},
                        {QStringLiteral("revision"), team.value(QStringLiteral("revision"))}});
    });
    connect(teams_, &teams::TeamCoordinator::teamRemoved, this,
            [this](const QString& teamId, const QString& repository) {
                emit teamEvent(QStringLiteral("team.removed"),
                               {{QStringLiteral("team_id"), teamId},
                                {QStringLiteral("repository"), repository}});
            });
}

} // namespace ketplus::daemon
