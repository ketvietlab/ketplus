// ADR 0114 and #464: a team is a branch of its repository's tree, before the repository's other
// conversations. Its chevron folds it, its name opens the team page, and while a decision waits it
// shows the needs-input mark and a semibold name. A conversation dragged onto a team of its own
// repository joins as a developer.
#include "KetButton.h"
#include "KetIcons.h"
#include "KetSidebarTree.h"
#include "teams/TeamMenus.h"
#include "workspace/SidebarRepositoryList.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QSettings>
#include <QVBoxLayout>

#include <optional>

namespace ketplus {
namespace {

const auto kCollapsedTeamsSetting = QStringLiteral("sidebar/collapsedTeams");

struct DraggedConversation {
    QString sessionId;
    QString repository;
};

std::optional<DraggedConversation> dragged(const QMimeData* data) {
    if (data == nullptr || !data->hasFormat(SidebarRepositoryList::kConversationMimeType))
        return std::nullopt;
    const QJsonObject json =
        QJsonDocument::fromJson(data->data(SidebarRepositoryList::kConversationMimeType)).object();
    return DraggedConversation{json.value(QStringLiteral("session_id")).toString(),
                               json.value(QStringLiteral("repository")).toString()};
}

ket::SidebarTeamBranch* branchOf(QWidget* folder) {
    return folder->findChild<ket::SidebarTeamBranch*>(QStringLiteral("productTeamFolderHeader"),
                                                      Qt::FindDirectChildrenOnly);
}

ket::SidebarTreeGuide* membersOf(QWidget* folder) {
    return folder->findChild<ket::SidebarTreeGuide*>(QStringLiteral("productTeamMembers"),
                                                     Qt::FindDirectChildrenOnly);
}

void setDropping(QWidget* folder, const bool dropping) {
    folder->setProperty("dropping", dropping);
    if (auto* branch = branchOf(folder))
        branch->setSelected(dropping);
}

} // namespace

QWidget* SidebarRepositoryList::createTeamFolder(QWidget* group,
                                                 const SidebarRepository& repository,
                                                 const SidebarTeam& team) {
    if (collapsedTeams_.isEmpty()) {
        const QStringList stored = QSettings().value(kCollapsedTeamsSetting).toStringList();
        collapsedTeams_ = QSet<QString>(stored.cbegin(), stored.cend());
    }
    // The folder takes the drops; the branch and its members' guide are what it shows.
    auto* folder = new QWidget(group);
    folder->setObjectName(QStringLiteral("productTeamFolder"));
    folder->setProperty("teamId", team.id);
    folder->setProperty("repositoryPath", repository.path);
    folder->setAccessibleName(tr("Team %1").arg(team.name));
    folder->setAcceptDrops(true);
    folder->installEventFilter(this);
    auto* layout = new QVBoxLayout(folder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* branch = new ket::SidebarTeamBranch(team.name, folder);
    branch->setObjectName(QStringLiteral("productTeamFolderHeader"));
    branch->setLevel(1);
    branch->nameButton()->setObjectName(QStringLiteral("productTeamFolderName"));
    branch->nameButton()->setToolTip(tr("Open the team page of %1").arg(team.name));
    branch->nameButton()->setContextMenuPolicy(Qt::CustomContextMenu);
    branch->foldButton()->setObjectName(QStringLiteral("productTeamFolderFold"));
    // Members join from the team page or by a drag; the branch offers no add of its own.
    branch->addAction()->hide();
    connect(branch, &ket::SidebarTeamBranch::activated, this,
            [this, id = team.id] { emit teamRequested(id); });
    // A click, not toggled: presenting the stored fold must not store it again.
    connect(branch->foldButton(), &QAbstractButton::clicked, this, [this, id = team.id] {
        if (!collapsedTeams_.remove(id))
            collapsedTeams_.insert(id);
        QStringList values(collapsedTeams_.cbegin(), collapsedTeams_.cend());
        values.sort();
        QSettings().setValue(kCollapsedTeamsSetting, values);
        applyPresentation();
    });
    connect(branch->nameButton(), &QWidget::customContextMenuRequested, this,
            [this, id = team.id] { emit teamMenuRequested(id); });
    auto* menu = branch->menuAction();
    menu->setObjectName(QStringLiteral("productTeamFolderMenu"));
    menu->setLabel(tr("Team actions"));
    menu->setToolTip(tr("Team actions"));
    connect(branch, &ket::SidebarTeamBranch::menuTriggered, this,
            [this, id = team.id] { emit teamMenuRequested(id); });
    layout->addWidget(branch);

    auto* members = new ket::SidebarTreeGuide(1, folder);
    members->setObjectName(QStringLiteral("productTeamMembers"));
    layout->addWidget(members);
    for (const QVariant& value : team.members) {
        auto* member = createRow(members, value.toMap(), 1);
        member->setProperty("teamRole", value.toMap().value(QStringLiteral("teamRole")));
        member->setProperty("teamMember", true);
        member->setProperty("repositoryPath", repository.path);
        members->addPart(member);
    }
    folders_.append(folder);
    return folder;
}

void SidebarRepositoryList::updateTeamFolders(const QVector<SidebarRepository>& repositories) {
    for (QWidget* folder : std::as_const(folders_)) {
        const QString id = folder->property("teamId").toString();
        for (const SidebarRepository& repository : repositories)
            for (const SidebarTeam& team : repository.teams) {
                if (team.id != id)
                    continue;
                folder->setProperty("full", team.full);
                if (auto* branch = branchOf(folder)) {
                    branch->setMemberCount(static_cast<int>(team.members.size()));
                    branch->setDecisionCount(team.openDecisions);
                }
            }
    }
}

void SidebarRepositoryList::presentTeamFolders(QWidget* group) {
    for (QWidget* folder : group->findChildren<QWidget*>(QStringLiteral("productTeamFolder"),
                                                         Qt::FindDirectChildrenOnly)) {
        const bool folded = collapsedTeams_.contains(folder->property("teamId").toString());
        auto* branch = branchOf(folder);
        branch->setExpanded(!folded);
        auto* members = membersOf(folder);
        members->setVisible(!folded && !members->findChildren<ket::SidebarRow*>().isEmpty());
    }
}

bool SidebarRepositoryList::acceptsDrop(const QString& teamId, const QString& repositoryPath,
                                        const QString& sessionId) const {
    if (teamId.isEmpty() || sessionId.isEmpty())
        return false;
    bool found = false;
    for (const SidebarRepository& repository : repositories_)
        for (const SidebarTeam& team : repository.teams) {
            for (const QVariant& member : team.members)
                if (member.toMap().value(QStringLiteral("sessionId")).toString() == sessionId)
                    return false; // one team per conversation
            if (team.id == teamId)
                found = repository.path == repositoryPath && !team.full;
        }
    return found;
}

bool SidebarRepositoryList::handleDrag(QObject* watched, QEvent* event) {
    if (auto* row = qobject_cast<ket::SidebarRow*>(watched)) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                pressedRow_ = row;
                pressPosition_ = mouse->globalPosition().toPoint();
            }
            return false;
        }
        if (event->type() != QEvent::MouseMove || pressedRow_ != row)
            return false;
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const QString sessionId = row->property("sessionId").toString();
        // A member leaves from its menu; only a conversation on no team is dragged.
        if (!(mouse->buttons() & Qt::LeftButton) || sessionId.isEmpty() ||
            row->property("teamMember").toBool() ||
            (mouse->globalPosition().toPoint() - pressPosition_).manhattanLength() <
                QApplication::startDragDistance())
            return false;
        pressedRow_.clear();
        auto* data = new QMimeData;
        data->setData(kConversationMimeType,
                      QJsonDocument(QJsonObject{{QStringLiteral("session_id"), sessionId},
                                                {QStringLiteral("repository"),
                                                 row->property("repositoryPath").toString()}})
                          .toJson(QJsonDocument::Compact));
        auto* drag = new QDrag(row);
        drag->setMimeData(data);
        drag->setPixmap(row->grab());
        row->setDown(false);
        drag->exec(Qt::MoveAction);
        return true;
    }
    auto* folder = qobject_cast<QWidget*>(watched);
    if (folder == nullptr || folder->objectName() != QStringLiteral("productTeamFolder"))
        return false;
    const QString teamId = folder->property("teamId").toString();
    const QString repository = folder->property("repositoryPath").toString();
    switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove: {
        auto* drop = static_cast<QDragMoveEvent*>(event);
        const auto conversation = dragged(drop->mimeData());
        const bool accepts = conversation && conversation->repository == repository &&
                             acceptsDrop(teamId, repository, conversation->sessionId);
        setDropping(folder, accepts);
        if (accepts)
            drop->acceptProposedAction();
        else
            drop->ignore();
        return true;
    }
    case QEvent::DragLeave:
        setDropping(folder, false);
        return true;
    case QEvent::Drop: {
        auto* drop = static_cast<QDropEvent*>(event);
        setDropping(folder, false);
        const auto conversation = dragged(drop->mimeData());
        if (!conversation || conversation->repository != repository ||
            !acceptsDrop(teamId, repository, conversation->sessionId)) {
            drop->ignore();
            return true;
        }
        drop->acceptProposedAction();
        emit teamJoinRequested(teamId, conversation->sessionId);
        return true;
    }
    default:
        return false;
    }
}

void SidebarRepositoryList::onRepositoryAction(const QString& path, QWidget* anchor) {
    if (!teamsAvailable_) {
        emit newConversationRequested(path);
        return;
    }
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("productRepositoryAddMenu"));
    const auto actions = addRepositoryAddMenuActions(menu);
    QAction* chosen = menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
    if (chosen == actions.conversation)
        emit newConversationRequested(path);
    else if (chosen == actions.team)
        afterMenuCloses(this, [this, path] { emit newTeamRequested(path); });
}

} // namespace ketplus
