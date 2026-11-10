// ADR 0112 (#464): teams in the sidebar. The daemon owns each team; the window shows its
// folder, offers its menus and sends what the user chose back as team requests.
#include "DaemonClient.h"
#include "KetDialog.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "app/MainWindow.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamMenus.h"
#include "workspace/ActivityBar.h"

#include <QAction>
#include <QCursor>
#include <QJsonArray>
#include <QMenu>
#include <QPointer>
#include <QStatusBar>

#include <algorithm>

namespace ketplus {

void MainWindow::setupTeams() {
    teams_ = new TeamDirectory(this);
    auto* daemon = accountDaemon();
    teams_->setTransport([daemon](const QString& type, const QJsonObject& payload,
                                  TeamDirectory::Reply reply) {
        daemon->call(type, payload, {}, -1,
                     [reply = std::move(reply)](const QJsonObject& result, const QString& error) {
                         if (reply)
                             reply(result, error);
                     });
    });
    // The daemon reports its disconnect while the window is torn down; the list may be gone.
    const QPointer<SidebarRepositoryList> list(activityBar_->repositoryList());
    const auto update = [this, daemon, list] {
        if (!list)
            return;
        teams_->setAvailable(daemon->isReady() && daemon->hasFeature(QStringLiteral("teams/1")));
        list->setTeamsAvailable(teams_->isAvailable());
        scheduleSidebarRefresh();
    };
    connect(daemon, &remote::DaemonClient::readyChanged, list, update);
    connect(daemon, &remote::DaemonClient::teamEventReceived, teams_, &TeamDirectory::applyEvent);
    connect(teams_, &TeamDirectory::changed, this, [this] { scheduleSidebarRefresh(); });
    connect(list, &SidebarRepositoryList::teamRequested, this, &MainWindow::openTeam);
    connect(list, &SidebarRepositoryList::teamMenuRequested, this, &MainWindow::showTeamMenu);
    connect(list, &SidebarRepositoryList::teamJoinRequested, this, &MainWindow::joinTeam);
    connect(list, &SidebarRepositoryList::newTeamRequested, this, &MainWindow::showNewTeamDialog);
    update();
}

QVector<SidebarTeam> MainWindow::takeSidebarTeams(const QString& repository,
                                                  QVariantList& conversations) const {
    QVector<SidebarTeam> folders;
    if (teams_ == nullptr || !teams_->isAvailable())
        return folders;
    for (const QJsonObject& team : teams_->teams(repository)) {
        SidebarTeam folder{
            .id = team.value(QStringLiteral("id")).toString(),
            .name = team.value(QStringLiteral("name")).toString(),
            .openDecisions = team.value(QStringLiteral("open_decisions")).toInt(),
            .full = TeamDirectory::count(team, QStringLiteral("developer")) >=
                    team_roles::maximumDevelopers,
        };
        QJsonArray members = team.value(QStringLiteral("members")).toArray();
        QList<QJsonObject> ordered;
        for (const auto& value : members)
            ordered.append(value.toObject());
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
            return team_roles::order(left.value(QStringLiteral("role")).toString()) <
                   team_roles::order(right.value(QStringLiteral("role")).toString());
        });
        for (const QJsonObject& member : std::as_const(ordered)) {
            const QString sessionId = member.value(QStringLiteral("session_id")).toString();
            const auto found =
                std::find_if(conversations.begin(), conversations.end(), [&](const QVariant& row) {
                    return row.toMap().value(QStringLiteral("sessionId")).toString() == sessionId;
                });
            // A member whose conversation this window does not list yet appears once it does.
            if (found == conversations.end())
                continue;
            QVariantMap row = found->toMap();
            conversations.erase(found);
            const QString role = member.value(QStringLiteral("role")).toString();
            row.insert(QStringLiteral("teamId"), folder.id);
            row.insert(QStringLiteral("teamRole"), role);
            // The role is the first tag, set by the team (#467).
            QVariantList tags{QVariantMap{{QStringLiteral("label"), team_roles::label(role)},
                                          {QStringLiteral("color"), team_roles::color(role).name()},
                                          {QStringLiteral("locked"), true}}};
            tags += row.value(QStringLiteral("tags")).toList();
            row.insert(QStringLiteral("tags"), tags);
            // #125: the leader waits for the user while a decision is open.
            if (role == QStringLiteral("leader") && folder.openDecisions > 0 &&
                !row.value(QStringLiteral("running")).toBool())
                row.insert(QStringLiteral("needsInput"), true);
            folder.members.append(row);
        }
        folders.append(folder);
    }
    return folders;
}

QString MainWindow::conversationSessionId(const QString& worktreePath,
                                          const QString& conversationId) const {
    if (agentManager_ == nullptr)
        return {};
    for (const QVariant& value : agentManager_->conversations(worktreePath)) {
        const QVariantMap conversation = value.toMap();
        if (conversation.value(QStringLiteral("id")).toString() == conversationId)
            return conversation.value(QStringLiteral("sessionId")).toString();
    }
    return {};
}

void MainWindow::showTeamMenu(const QString& teamId) {
    const auto team = teams_ ? teams_->team(teamId) : std::nullopt;
    if (!team)
        return;
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("teamFolderMenu"));
    const auto actions = addTeamFolderMenuActions(menu, *team);
    QAction* chosen = menu.exec(QCursor::pos());
    if (chosen == actions.open)
        openTeam(teamId);
    else if (chosen == actions.addMember)
        showAddTeamMemberDialog(teamId);
    else if (chosen == actions.remove)
        deleteTeam(teamId);
}

void MainWindow::openTeam(const QString& teamId) {
    const auto team = teams_ ? teams_->team(teamId) : std::nullopt;
    if (!team || agentManager_ == nullptr)
        return;
    // Until the team page (#466): the leader's conversation.
    QString leader;
    for (const auto& value : team->value(QStringLiteral("members")).toArray())
        if (value.toObject().value(QStringLiteral("role")).toString() == QStringLiteral("leader"))
            leader = value.toObject().value(QStringLiteral("session_id")).toString();
    for (const QString& path : agentManager_->conversationPaths().values())
        for (const QVariant& value : agentManager_->conversations(path)) {
            const QVariantMap conversation = value.toMap();
            if (conversation.value(QStringLiteral("sessionId")).toString() == leader) {
                openSidebarConversation(path, conversation.value(QStringLiteral("id")).toString());
                return;
            }
        }
}

void MainWindow::joinTeam(const QString& teamId, const QString& sessionId) {
    if (teams_ == nullptr || sessionId.isEmpty())
        return;
    teams_->request(
        QStringLiteral("team.member.join"),
        {{QStringLiteral("team_id"), teamId}, {QStringLiteral("session_id"), sessionId}},
        [this](const QJsonObject&, const QString& error) {
            if (!error.isEmpty())
                statusBar()->showMessage(error, 7000);
        });
}

void MainWindow::leaveTeam(const QString& teamId, const QString& sessionId) {
    const auto team = teams_ ? teams_->team(teamId) : std::nullopt;
    if (!team)
        return;
    const QString name =
        TeamDirectory::member(*team, sessionId).value(QStringLiteral("name")).toString();
    auto* dialog = teamLeaveConfirmation(this, *team, name);
    connect(dialog, &ket::Dialog::accepted, this, [this, teamId, sessionId] {
        teams_->request(
            QStringLiteral("team.member.remove"),
            {{QStringLiteral("team_id"), teamId}, {QStringLiteral("session_id"), sessionId}},
            [this](const QJsonObject&, const QString& error) {
                if (!error.isEmpty())
                    statusBar()->showMessage(error, 7000);
            });
    });
    dialog->open();
}

void MainWindow::deleteTeam(const QString& teamId) {
    const auto team = teams_ ? teams_->team(teamId) : std::nullopt;
    if (!team)
        return;
    auto* dialog = teamDeleteConfirmation(this, *team);
    connect(dialog, &ket::Dialog::accepted, this, [this, teamId] {
        teams_->request(QStringLiteral("team.delete"), {{QStringLiteral("team_id"), teamId}},
                        [this](const QJsonObject&, const QString& error) {
                            if (!error.isEmpty())
                                statusBar()->showMessage(error, 7000);
                        });
    });
    dialog->open();
}

bool MainWindow::handleConversationTeamAction(QAction* chosen, const QString& sessionId) {
    if (chosen == nullptr || chosen->data().toString().isEmpty())
        return false;
    if (chosen->objectName() == QStringLiteral("joinTeamAction")) {
        joinTeam(chosen->data().toString(), sessionId);
        return true;
    }
    if (chosen->objectName() == QStringLiteral("leaveTeamAction")) {
        leaveTeam(chosen->data().toString(), sessionId);
        return true;
    }
    return false;
}

void MainWindow::showNewTeamDialog(const QString& repository) {
    // The New team dialog arrives with #465.
    Q_UNUSED(repository);
    statusBar()->showMessage(tr("New team is not available yet."), 5000);
}

void MainWindow::showAddTeamMemberDialog(const QString& teamId) {
    // The Add developer dialog arrives with #465.
    Q_UNUSED(teamId);
    statusBar()->showMessage(tr("Add member is not available yet."), 5000);
}

} // namespace ketplus
