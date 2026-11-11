// ADR 0114 and #464: a team is a folder inside its repository, before the repository's other
// conversations. Its name opens the team page, its icon folds it, its count is the open
// decisions. A conversation dragged onto a folder of its own repository joins as a developer.
#include "KetBadge.h"
#include "KetButton.h"
#include "KetConversationRow.h"
#include "KetIcons.h"
#include "KetTreeItem.h"
#include "teams/TeamMenus.h"
#include "ui/ChromeGeometry.h"
#include "ui/InterfaceZoom.h"
#include "workspace/SidebarRepositoryList.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace ketplus {
namespace {

constexpr int kFolderIndent = 12;
constexpr int kConversationSpacing = 2;
const auto kCollapsedTeamsSetting = QStringLiteral("sidebar/collapsedTeams");

QString decisionsLabel(const int count) {
    return count == 1 ? QObject::tr("1 decision waits for you")
                      : QObject::tr("%1 decisions wait for you").arg(count);
}

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

ket::GroupHeader* folderHeader(QWidget* folder) {
    return folder->findChild<ket::GroupHeader*>(QStringLiteral("productTeamFolderHeader"),
                                                Qt::FindDirectChildrenOnly);
}

void setDropping(QWidget* folder, const bool dropping) {
    folder->setProperty("dropping", dropping);
    if (auto* header = folderHeader(folder))
        header->setSelected(dropping);
}

} // namespace

QWidget* SidebarRepositoryList::createTeamFolder(QWidget* group,
                                                 const SidebarRepository& repository,
                                                 const SidebarTeam& team) {
    if (collapsedTeams_.isEmpty()) {
        const QStringList stored = QSettings().value(kCollapsedTeamsSetting).toStringList();
        collapsedTeams_ = QSet<QString>(stored.cbegin(), stored.cend());
    }
    auto* folder = new QWidget(group);
    folder->setObjectName(QStringLiteral("productTeamFolder"));
    folder->setProperty("teamId", team.id);
    folder->setProperty("repositoryPath", repository.path);
    folder->setAccessibleName(tr("Team %1").arg(team.name));
    folder->setAcceptDrops(true);
    folder->installEventFilter(this);
    auto* layout = new QVBoxLayout(folder);
    chrome_geometry::margins(layout, kFolderIndent, 0, 0, 0);
    chrome_geometry::spacing(layout, kConversationSpacing);

    auto* header = new ket::GroupHeader(team.name, folder);
    header->setObjectName(QStringLiteral("productTeamFolderHeader"));
    header->setActivatable(true);
    header->setIconCode(QStringLiteral("users"));
    header->nameButton()->setObjectName(QStringLiteral("productTeamFolderName"));
    header->nameButton()->setToolTip(tr("Open the team page of %1").arg(team.name));
    header->nameButton()->setContextMenuPolicy(Qt::CustomContextMenu);
    header->toggle()->setObjectName(QStringLiteral("productTeamFolderFold"));
    connect(header, &ket::GroupHeader::activated, this,
            [this, id = team.id] { emit teamRequested(id); });
    connect(header->toggle(), &QAbstractButton::clicked, this, [this, id = team.id] {
        if (!collapsedTeams_.remove(id))
            collapsedTeams_.insert(id);
        QStringList values(collapsedTeams_.cbegin(), collapsedTeams_.cend());
        values.sort();
        QSettings().setValue(kCollapsedTeamsSetting, values);
        applyPresentation();
    });
    connect(header->nameButton(), &QWidget::customContextMenuRequested, this,
            [this, id = team.id] { emit teamMenuRequested(id); });
    auto* menu = header->action();
    menu->setObjectName(QStringLiteral("productTeamFolderMenu"));
    menu->setIcon(ket::Icons::icon(u"more"));
    menu->setLabel(tr("Team actions"));
    menu->setToolTip(tr("Team actions"));
    connect(header, &ket::GroupHeader::actionTriggered, this,
            [this, id = team.id] { emit teamMenuRequested(id); });
    auto* count =
        new ket::Badge(QString::number(team.openDecisions), ket::BadgeTone::Accent, header);
    count->setObjectName(QStringLiteral("productTeamDecisionCount"));
    auto* row = static_cast<QHBoxLayout*>(header->layout());
    row->insertWidget(row->indexOf(static_cast<QWidget*>(menu)), count);
    layout->addWidget(header);

    for (const QVariant& value : team.members) {
        auto* member = createRow(folder, value.toMap());
        member->setProperty("teamRole", value.toMap().value(QStringLiteral("teamRole")));
        layout->addWidget(member);
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
                auto* count =
                    folder->findChild<ket::Badge*>(QStringLiteral("productTeamDecisionCount"));
                count->setText(QString::number(team.openDecisions));
                count->setVisible(team.openDecisions > 0);
                count->setAccessibleName(decisionsLabel(team.openDecisions));
                if (auto* header = folderHeader(folder))
                    header->nameButton()->setAccessibleDescription(
                        team.openDecisions > 0 ? decisionsLabel(team.openDecisions) : QString{});
            }
    }
}

int SidebarRepositoryList::presentTeamFolders(QWidget* group, const bool repositoryCollapsed,
                                              const int rowHeight) {
    const auto px = [this](int n) {
        return InterfaceZoomMetrics::geometry(n, chrome_geometry::zoomPercent(this));
    };
    int height = 0;
    for (QWidget* folder : group->findChildren<QWidget*>(QStringLiteral("productTeamFolder"),
                                                         Qt::FindDirectChildrenOnly)) {
        const bool folded = collapsedTeams_.contains(folder->property("teamId").toString());
        int shown = 0;
        int matching = 0;
        for (auto* row : folder->findChildren<ket::ConversationRow*>(
                 QStringLiteral("productConversationButton"), Qt::FindDirectChildrenOnly)) {
            const bool matches = rowMatchesFilter(row);
            matching += matches ? 1 : 0;
            row->setFixedHeight(row->sizeHint().height());
            row->setVisible(matches && !folded);
            shown += matches && !folded ? 1 : 0;
        }
        auto* header = folderHeader(folder);
        header->setExpanded(!folded);
        // A tag filter keeps only the folders with a matching member.
        const bool visible = !repositoryCollapsed && (tagFilter_.isEmpty() || matching > 0);
        folder->setVisible(visible);
        if (!visible)
            continue;
        folder->setFixedHeight(header->sizeHint().height() +
                               shown * (rowHeight + px(kConversationSpacing)));
        height += folder->height() + px(kConversationSpacing);
    }
    return height;
}

bool SidebarRepositoryList::rowMatchesFilter(const ket::ConversationRow* row) const {
    if (tagFilter_.isEmpty())
        return true;
    // A role is the team's, not one of the user's tags (#467).
    const auto tags = row->tags();
    return std::any_of(tags.cbegin(), tags.cend(), [this](const ket::ConversationTag& tag) {
        return !tag.locked && tag.label == tagFilter_;
    });
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
    if (auto* row = qobject_cast<ket::ConversationRow*>(watched)) {
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
            row->parentWidget()->objectName() == QStringLiteral("productTeamFolder") ||
            (mouse->globalPosition().toPoint() - pressPosition_).manhattanLength() <
                QApplication::startDragDistance())
            return false;
        pressedRow_.clear();
        auto* data = new QMimeData;
        data->setData(
            kConversationMimeType,
            QJsonDocument(QJsonObject{{QStringLiteral("session_id"), sessionId},
                                      {QStringLiteral("repository"),
                                       row->parentWidget()->property("repositoryPath").toString()}})
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
