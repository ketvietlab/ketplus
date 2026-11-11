#include "teams/TeamMenus.h"

#include "KetButton.h"
#include "KetDialog.h"
#include "KetIcons.h"
#include "KetStyle.h"
#include "teams/TeamDirectory.h"

#include <QAction>
#include <QCoreApplication>
#include <QLabel>
#include <QMenu>
#include <QTimer>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("TeamMenus", text); }

QAction* entry(QMenu& menu, const QString& text, QStringView icon, const char* name) {
    auto* action = menu.addAction(ket::Icons::icon(icon), text);
    action->setObjectName(QLatin1String(name));
    return action;
}

void refuse(QAction* action, const QString& reason) {
    action->setEnabled(reason.isEmpty());
    action->setProperty(ket::Style::kDescriptionProperty, reason);
    action->setToolTip(reason);
}

QString name(const QJsonObject& team) { return team.value(QStringLiteral("name")).toString(); }

ket::Dialog* confirmation(QWidget* anchor, const QString& title, const QString& text,
                          const QString& confirm, const char* objectName) {
    auto* dialog = new ket::Dialog(title, anchor);
    dialog->setObjectName(QLatin1String(objectName));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* label = new QLabel(text, body);
    label->setObjectName(QStringLiteral("teamConfirmationText"));
    label->setWordWrap(true);
    layout->addWidget(label);
    dialog->setBody(body);
    dialog->setCardWidth(420);
    auto* cancel = dialog->addButton(tr("Cancel"));
    QObject::connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* accept = dialog->addButton(confirm, ket::ButtonVariant::Danger);
    accept->setObjectName(QStringLiteral("teamConfirmationAccept"));
    QObject::connect(accept, &QAbstractButton::clicked, dialog, &ket::Dialog::accept);
    return dialog;
}

} // namespace

QString team_roles::label(const QString& role) {
    if (role == QStringLiteral("leader"))
        return tr("Leader");
    if (role == QStringLiteral("reviewer"))
        return tr("Reviewer");
    return tr("Developer");
}

QColor team_roles::color(const QString& role) {
    // The mock's role colours (Fixtures.roleColors).
    if (role == QStringLiteral("leader"))
        return QColor(0xb4, 0x43, 0x6c);
    if (role == QStringLiteral("reviewer"))
        return QColor(0x7c, 0x5c, 0xd6);
    return QColor(0x5b, 0x6b, 0x7f);
}

int team_roles::order(const QString& role) {
    if (role == QStringLiteral("leader"))
        return 0;
    if (role == QStringLiteral("reviewer"))
        return 2;
    return 1;
}

QStringList team_states::all() {
    return {QStringLiteral("todo"),    QStringLiteral("in_progress"), QStringLiteral("in_review"),
            QStringLiteral("blocked"), QStringLiteral("ready"),       QStringLiteral("done")};
}

QString team_states::label(const QString& state) {
    if (state == QStringLiteral("todo"))
        return tr("To do");
    if (state == QStringLiteral("in_progress"))
        return tr("In progress");
    if (state == QStringLiteral("in_review"))
        return tr("In review");
    if (state == QStringLiteral("blocked"))
        return tr("Blocked");
    if (state == QStringLiteral("ready"))
        return tr("Ready to merge");
    if (state == QStringLiteral("done"))
        return tr("Done");
    return state;
}

TeamFolderMenuActions addTeamFolderMenuActions(QMenu& menu, const QJsonObject& team) {
    TeamFolderMenuActions actions;
    actions.open = entry(menu, tr("Open team"), u"users", "openTeamAction");
    actions.addMember = entry(menu, tr("Add member…"), u"plus", "addTeamMemberAction");
    refuse(actions.addMember,
           TeamDirectory::count(team, QStringLiteral("developer")) >= team_roles::maximumDevelopers
               ? tr("A team has at most five developers.")
               : QString{});
    menu.addSeparator();
    actions.remove = entry(menu, tr("Delete team…"), u"trash", "deleteTeamAction");
    actions.remove->setProperty(ket::Style::kDangerProperty, true);
    for (QAction* action : {actions.open, actions.addMember, actions.remove})
        action->setData(team.value(QStringLiteral("id")).toString());
    return actions;
}

RepositoryAddMenuActions addRepositoryAddMenuActions(QMenu& menu) {
    return {.conversation =
                entry(menu, tr("New conversation"), u"plus", "newRepositoryConversationAction"),
            .team = entry(menu, tr("New team…"), u"users", "newTeamAction")};
}

void afterMenuCloses(QObject* context, std::function<void()> action) {
    QTimer::singleShot(kMenuCloseMs, context, std::move(action));
}

QString joinRefusal(const QJsonObject& team, const QString& sessionId) {
    if (sessionId.isEmpty())
        return tr("Start the conversation before it joins a team.");
    if (!TeamDirectory::member(team, sessionId).isEmpty())
        return tr("Already on %1.").arg(name(team));
    if (TeamDirectory::count(team, QStringLiteral("developer")) >= team_roles::maximumDevelopers)
        return tr("%1 has five developers.").arg(name(team));
    return {};
}

QString leaveRefusal(const QJsonObject& team, const QString& sessionId) {
    const QString role =
        TeamDirectory::member(team, sessionId).value(QStringLiteral("role")).toString();
    if (role == QStringLiteral("leader"))
        return tr("The leader leads the team. Delete the team instead.");
    if (role == QStringLiteral("reviewer"))
        return tr("The team's reviewer reviews every task. Delete the team instead.");
    if (TeamDirectory::count(team, QStringLiteral("developer")) <= 1)
        return tr("A team keeps at least one developer.");
    return {};
}

ConversationTeamActions addConversationTeamActions(QMenu& menu, const QList<QJsonObject>& teams,
                                                   const QString& sessionId) {
    ConversationTeamActions actions;
    if (teams.isEmpty())
        return actions;
    menu.addSeparator();
    for (const QJsonObject& team : teams) {
        if (TeamDirectory::member(team, sessionId).isEmpty())
            continue;
        actions.leave =
            entry(menu, tr("Remove from %1…").arg(name(team)), u"users", "leaveTeamAction");
        actions.leave->setData(team.value(QStringLiteral("id")).toString());
        refuse(actions.leave, leaveRefusal(team, sessionId));
        return actions;
    }
    for (const QJsonObject& team : teams) {
        auto* join = entry(menu, tr("Add to %1").arg(name(team)), u"users", "joinTeamAction");
        join->setData(team.value(QStringLiteral("id")).toString());
        refuse(join, joinRefusal(team, sessionId));
        actions.join.append(join);
    }
    return actions;
}

ket::Dialog* teamDeleteConfirmation(QWidget* anchor, const QJsonObject& team) {
    return confirmation(anchor, tr("Delete %1?").arg(name(team)),
                        tr("The team, its tasks and its decisions are deleted. Its "
                           "conversations, their history and their worktrees stay, back among "
                           "the repository's conversations."),
                        tr("Delete team"), "teamDeleteDialog");
}

ket::Dialog* teamLeaveConfirmation(QWidget* anchor, const QJsonObject& team,
                                   const QString& memberName) {
    return confirmation(
        anchor, tr("Remove %1 from %2?").arg(memberName, name(team)),
        tr("The conversation and its history stay, back among the repository's "
           "conversations. Its open tasks are unassigned for the leader to give out again."),
        tr("Remove"), "teamLeaveDialog");
}

} // namespace ketplus
