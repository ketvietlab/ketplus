// ADR 0112: the session runtime's side of agent teams.
#include "runtime/SessionHost.h"

#include <QFileInfo>
#include <QProcess>

namespace ketplus::daemon {

QString SessionHost::repositoryOf(const QString& worktreePath) const {
    if (worktreePath.isEmpty())
        return {};
    if (const auto cached = repositoryByWorktree_.constFind(worktreePath);
        cached != repositoryByWorktree_.cend())
        return cached.value();
    QProcess process;
    process.setWorkingDirectory(worktreePath);
    process.start(QStringLiteral("git"),
                  {QStringLiteral("--no-optional-locks"), QStringLiteral("rev-parse"),
                   QStringLiteral("--path-format=absolute"), QStringLiteral("--git-common-dir")});
    QString repository;
    if (process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit &&
        process.exitCode() == 0) {
        const QFileInfo common(QString::fromUtf8(process.readAllStandardOutput()).trimmed());
        // The main worktree holds the common ".git"; a bare repository is its own key.
        repository = common.fileName() == QStringLiteral(".git")
                         ? QFileInfo(common.absolutePath()).canonicalFilePath()
                         : common.canonicalFilePath();
    }
    if (repository.isEmpty())
        repository = QFileInfo(worktreePath).canonicalFilePath();
    if (repository.isEmpty())
        repository = worktreePath;
    repositoryByWorktree_.insert(worktreePath, repository);
    return repository;
}

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
                                   .repository = repositoryOf(it->stored.worktreePath),
                                   .idle = agent.idle};
}

bool SessionHost::onTeam(const QString& sessionId) const {
    return teams_ && teams_->isMember(sessionId);
}

void SessionHost::startTeams() {
    teams_ = new teams::TeamCoordinator(journal_, static_cast<teams::TeamHostPort*>(this), this);
    teams_->load();
}

} // namespace ketplus::daemon
