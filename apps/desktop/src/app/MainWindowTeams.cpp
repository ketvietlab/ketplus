// ADR 0114 (#464): teams in the sidebar. The daemon owns each team; the window shows its
// folder, offers its menus and sends what the user chose back as team requests.
#include "DaemonClient.h"
#include "KetDialog.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "app/AgentWorktreeDialog.h"
#include "app/MainWindow.h"
#include "git/GitChangesPanel.h"
#include "teams/TeamCommands.h"
#include "teams/TeamDialogs.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamMenus.h"
#include "teams/TeamPage.h"
#include "terminal/TerminalPanel.h"
#include "workspace/ActivityBar.h"
#include "workspace/ExplorerPanel.h"

#include <QAction>
#include <QCursor>
#include <QJsonArray>
#include <QMenu>
#include <QPointer>
#include <QSplitter>
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
        // A daemon that went away keeps its teams, stale, so the sidebar does not scatter a
        // team's members as loose conversations while it reconnects (ADR 0115).
        if (!daemon->isReady())
            teams_->setDisconnected();
        else
            teams_->setAvailable(daemon->hasFeature(QStringLiteral("teams/1")));
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
    if (teams_ == nullptr || !teams_->hasTeams())
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
            // The role is the row's only tag, locked: the team sets it (#467). Conversations
            // carry no user tags in the UI (ADR 0117).
            row.insert(
                QStringLiteral("tags"),
                QVariantList{QVariantMap{{QStringLiteral("label"), team_roles::label(role)},
                                         {QStringLiteral("color"), team_roles::color(role).name()},
                                         {QStringLiteral("locked"), true}}});
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

void MainWindow::updateTeamRoles() {
    if (agentWorkspace_ == nullptr)
        return;
    QVariantMap roles;
    if (teams_ != nullptr && teams_->isAvailable())
        for (const QJsonObject& team : teams_->teams())
            for (const auto& value : team.value(QStringLiteral("members")).toArray()) {
                const QJsonObject member = value.toObject();
                const QString role = member.value(QStringLiteral("role")).toString();
                roles.insert(
                    member.value(QStringLiteral("session_id")).toString(),
                    QVariantMap{
                        {QStringLiteral("team_id"), team.value(QStringLiteral("id")).toString()},
                        {QStringLiteral("team_name"),
                         team.value(QStringLiteral("name")).toString()},
                        {QStringLiteral("role"), role},
                        {QStringLiteral("label"), team_roles::label(role)},
                        {QStringLiteral("color"), team_roles::color(role).name()}});
            }
    agentWorkspace_->setTeamRoles(roles);
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
        afterMenuCloses(this, [this, teamId] { showAddTeamMemberDialog(teamId); });
    else if (chosen == actions.remove)
        afterMenuCloses(this, [this, teamId] { deleteTeam(teamId); });
}

void MainWindow::openTeam(const QString& teamId) {
    const auto team = teams_ ? teams_->team(teamId) : std::nullopt;
    if (!team)
        return;
    showTeamPage(teamId, team->value(QStringLiteral("repository")).toString());
}

// #468: a transcript block's Open T-n shows the task on its team's page.
void MainWindow::openTeamTask(const QString& teamId, const QString& taskId) {
    openTeam(teamId);
    if (teamPage_ != nullptr && teamPage_->teamId() == teamId)
        teamPage_->openTask(taskId);
}

void MainWindow::openTeamMember(const QString& sessionId) {
    if (agentManager_ == nullptr || sessionId.isEmpty())
        return;
    for (const QString& path : agentManager_->conversationPaths().values())
        for (const QVariant& value : agentManager_->conversations(path)) {
            const QVariantMap conversation = value.toMap();
            if (conversation.value(QStringLiteral("sessionId")).toString() == sessionId) {
                openSidebarConversation(path, conversation.value(QStringLiteral("id")).toString());
                return;
            }
        }
    statusBar()->showMessage(tr("That conversation is not listed in this window yet."), 5000);
}

void MainWindow::openConversationNumber(const QString& number) {
    if (agentManager_ == nullptr || number.isEmpty())
        return;
    for (const QString& path : agentManager_->conversationPaths().values())
        for (const QVariant& value : agentManager_->conversations(path)) {
            const QVariantMap conversation = value.toMap();
            if (conversation.value(QStringLiteral("number")).toString() == number) {
                openSidebarConversation(path, conversation.value(QStringLiteral("id")).toString());
                return;
            }
        }
    statusBar()->showMessage(tr("@agent/%1 is not a conversation on this computer.").arg(number),
                             5000);
}

void MainWindow::showTeamPage(const QString& teamId, const QString& repository) {
    if (teams_ == nullptr)
        return;
    hideUtilityWorkspaces();
    setRemoteWorkspaceVisible(false);
    setAtlasWorkspaceVisible(false);
    setAgentWorkspaceVisible(false);
    teamPage_ = new TeamPage(teams_, teamId, repository, workspaceSplit_);
    workspaceSplit_->addWidget(teamPage_);
    workspaceSplit_->setStretchFactor(workspaceSplit_->indexOf(teamPage_), 1);
    connect(teamPage_, &TeamPage::conversationRequested, this, &MainWindow::openTeamMember);
    if (agentManager_ != nullptr)
        teamPage_->setConversationDirectory(
            [this] { return agentManager_->numberedConversations(); });
    connect(teamPage_, &TeamPage::mentionRequested, this, &MainWindow::openConversationNumber);
    connect(teamPage_, &TeamPage::addMemberRequested, this, &MainWindow::showAddTeamMemberDialog);
    connect(teamPage_, &TeamPage::addReviewerRequested, this,
            &MainWindow::showAddTeamReviewerDialog);
    connect(teamPage_, &TeamPage::newTeamRequested, this, &MainWindow::showNewTeamDialog);
    connect(teamPage_, &TeamPage::deleteRequested, this, &MainWindow::deleteTeam);
    connect(teamPage_, &TeamPage::removeMemberRequested, this, &MainWindow::leaveTeam);
    for (QWidget* other :
         {static_cast<QWidget*>(explorer_), static_cast<QWidget*>(gitChanges_),
          static_cast<QWidget*>(terminal_), static_cast<QWidget*>(agentDetailPane_)})
        if (other != nullptr)
            other->hide();
    editorSplit_->hide();
    teamPage_->show();
    workspaceSplit_->show();
    mainSplit_->show();
    QList<int> sizes(workspaceSplit_->count(), 0);
    sizes[workspaceSplit_->indexOf(teamPage_)] = qMax(800, workspaceSplit_->width());
    workspaceSplit_->setSizes(sizes);
    teamPage_->setFocus();
}

void MainWindow::joinTeam(const QString& teamId, const QString& sessionId) {
    if (teams_ == nullptr || sessionId.isEmpty())
        return;
    teams_->commands()->send(
        TeamCommands::key(teamId, QStringLiteral("member/%1/join").arg(sessionId)), {},
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
        // The page's Remove… for this member waits for the answer (TeamCommands).
        teams_->commands()->send(
            TeamCommands::key(teamId, QStringLiteral("member/%1/remove").arg(sessionId)), {},
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
    QHash<QString, QVariantList> listed;
    if (agentManager_ != nullptr)
        for (const QString& path : agentManager_->conversationPaths().values())
            listed.insert(path, agentManager_->conversations(path));
    const TeamMemberConversations members = teamMemberConversations(*team, listed);
    auto* dialog = teamDeleteConfirmation(this, *team, members);
    connect(dialog, &ket::Dialog::accepted, this, [this, teamId, dialog, members] {
        const bool withConversations = deletesTeamConversations(dialog);
        teams_->commands()->send(
            TeamCommands::key(teamId, QStringLiteral("delete")), {}, QStringLiteral("team.delete"),
            {{QStringLiteral("team_id"), teamId}},
            [this, withConversations, members](const QJsonObject&, const QString& error) {
                if (!error.isEmpty()) {
                    statusBar()->showMessage(error, 7000);
                    return;
                }
                // After the team is gone, so a deleted leader or reviewer does not pause it
                // first. Each deletion reports its own failure (a turn started meanwhile).
                if (!withConversations || agentManager_ == nullptr)
                    return;
                for (const auto& [path, id] : members.conversations)
                    agentManager_->deleteConversation(path, id);
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
    if (teams_ == nullptr || !teams_->isAvailable() || agentManager_ == nullptr ||
        agentDialogOpen(this, QStringLiteral("newTeamDialog")))
        return;
    const auto open = [this, repository](const QVariantList& models) {
        NewTeamForm::present(
            this, teams_, repository, models,
            [this](const QJsonObject& team) {
                statusBar()->showMessage(
                    tr("Team %1 created.").arg(team.value(QStringLiteral("name")).toString()),
                    5000);
            },
            [this] { return agentManager_->numberedConversations(); });
    };
    // The roles choose among the models the app lists; read them first if it has none yet.
    if (!agentManager_->models().isEmpty()) {
        open(agentManager_->models());
        return;
    }
    auto* once = new QObject(this);
    connect(agentManager_, &AgentSessionManager::modelsLoaded, once,
            [once, open](const QVariantList& models) {
                once->deleteLater();
                open(models);
            });
    agentManager_->loadModels();
}

void MainWindow::showAddTeamMemberDialog(const QString& teamId) {
    const auto team = teams_ ? teams_->team(teamId) : std::nullopt;
    if (!team || agentManager_ == nullptr ||
        agentDialogOpen(this, QStringLiteral("addMemberDialog")))
        return;
    AddMemberForm::present(this, teams_, *team, agentManager_->models());
}

void MainWindow::showAddTeamReviewerDialog(const QString& teamId) {
    const auto team = teams_ ? teams_->team(teamId) : std::nullopt;
    if (!team || agentManager_ == nullptr ||
        agentDialogOpen(this, QStringLiteral("addMemberDialog")))
        return;
    // T-23: the same Add a member dialog, on Reviewer.
    AddMemberForm::present(this, teams_, *team, agentManager_->models(),
                           QStringLiteral("reviewer"));
}

} // namespace ketplus
