#pragma once

// A fake runtime for the team coordinator: conversations, worktrees and the messages a team
// sends, all in memory (ADR 0114).
#include "teams/TeamCoordinator.h"

#include <QHash>
#include <QSet>

namespace team_test {

using namespace ketplus::daemon::teams;

class FakeHost final : public TeamHostPort {
  public:
    std::optional<TeamConversation> teamConversation(const QString& sessionId) const override {
        const auto it = conversations.constFind(sessionId);
        return it == conversations.cend() ? std::nullopt : std::optional(it.value());
    }
    std::optional<TaskWorktree> createTaskWorktree(const Team& team, const Task& task,
                                                   QString* error) override {
        (void)error;
        return TaskWorktree{.branch = QStringLiteral("team/%1/%2").arg(team.id).arg(task.id),
                            .worktree = QStringLiteral("/wt/") + task.id,
                            .baseCommit = QStringLiteral("base")};
    }
    QString pointConversationAt(const QString& sessionId, const QString& worktree) override {
        pointed.insert(sessionId, worktree);
        return {};
    }
    std::optional<bool> hasUncommittedWork(const QString& worktree) const override {
        return dirty.contains(worktree);
    }
    QString taskCommit(const Team&, const Task& task) const override {
        return heads.value(task.branch, task.branch + QStringLiteral("@head"));
    }
    std::optional<TaskDiff> taskDiff(const Team& team, const Task& task) const override {
        (void)team;
        if (unreadable.contains(task.id))
            return std::nullopt;
        return TaskDiff{.diff = QStringLiteral("+++ b/web/%1.txt\n+change of %1").arg(task.id),
                        .files = {QStringLiteral("web/%1.txt").arg(task.id)}};
    }
    std::optional<QStringList> changedPaths(const Team& team, const Task& task) const override {
        (void)team;
        const auto it = changed.constFind(task.id);
        return it == changed.cend() ? std::nullopt : std::optional(it.value());
    }
    std::optional<bool> isMerged(const Team& team, const Task& task) const override {
        (void)team;
        return merged.contains(task.id);
    }
    QString postTeamMessage(const TeamMessage& message) override {
        messages.append(message);
        return {};
    }
    void noteInConversation(const QString& sessionId, const QJsonObject& note) override {
        notes.append({sessionId, note});
    }

    void add(const QString& id, const QString& provider = QStringLiteral("codex")) {
        conversations.insert(id, {.sessionId = id,
                                  .generation = 1,
                                  .number = id,
                                  .title = id,
                                  .provider = provider,
                                  .repository = QStringLiteral("/repo"),
                                  .idle = true});
    }
    void setIdle(const QString& id, const bool idle) { conversations[id].idle = idle; }
    [[nodiscard]] QList<TeamMessage> of(const QString& kind) const {
        QList<TeamMessage> result;
        for (const TeamMessage& message : messages)
            if (message.kind == kind)
                result.append(message);
        return result;
    }

    QHash<QString, TeamConversation> conversations;
    QHash<QString, QString> pointed;
    QHash<QString, QString> heads;
    QSet<QString> dirty;
    QSet<QString> unreadable;
    QHash<QString, QStringList> changed;
    QSet<QString> merged;
    QList<TeamMessage> messages;
    QList<std::pair<QString, QJsonObject>> notes;
};

// The reviewer of `session`'s team answers the request its review in flight is bound to.
inline void finishReview(TeamCoordinator& teams, const QString& session, const QString& text,
                         const bool noFindings, const QString& failure = {}) {
    const Team* team = teams.teamOf(session);
    teams.reviewFinished(session, team ? team->reviewingMessage : QString{}, text, noFindings,
                         failure);
}

inline Team standardTeam() {
    Team team;
    team.repository = QStringLiteral("/repo");
    team.name = QStringLiteral("Checkout");
    team.integrationBranch = QStringLiteral("integration");
    // The first plan is approved unless a test is about it (ADR 0114, open point 5).
    team.planApproved = true;
    team.members = {{.sessionId = "lead", .role = role::leader, .name = "Lead"},
                    {.sessionId = "dev1",
                     .role = role::developer,
                     .name = "Dev 1",
                     .lane = "Web",
                     .paths = {"web"}},
                    {.sessionId = "dev2",
                     .role = role::developer,
                     .name = "Dev 2",
                     .lane = "Api",
                     .paths = {"api"}},
                    {.sessionId = "rev", .role = role::reviewer, .name = "Reviewer"}};
    return team;
}

} // namespace team_test
