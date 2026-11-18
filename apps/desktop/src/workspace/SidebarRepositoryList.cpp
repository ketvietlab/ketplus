#include "workspace/SidebarRepositoryList.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetSidebarTree.h"
#include "ui/ChromeGeometry.h"
#include "ui/InterfaceZoom.h"
#include "workspace/SidebarPinMark.h"
#include "workspace/SidebarRepositoryListDetail.h"

#include "workspace/SidebarFilterText.h"
#include <QEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>

namespace ketplus {
namespace {

using sidebar_list::previewedConversationIds;
using sidebar_list::runningAgentSender;

constexpr int kShowMoreHeight = 28;
const auto kCollapsedSetting = QStringLiteral("sidebar/collapsedRepositories");

int compareNames(const QString& left, const QString& right) {
    return QString::localeAwareCompare(left.toCaseFolded(), right.toCaseFolded());
}

qint64 turnStartedAt(const QVariant& conversation) {
    return conversation.toMap().value(QStringLiteral("lastTurnStartedAt")).toLongLong();
}

int conversationCount(const SidebarRepository& repository) {
    int count = static_cast<int>(repository.conversations.size());
    for (const SidebarTeam& team : repository.teams)
        count += static_cast<int>(team.members.size());
    return count;
}

ket::SidebarTreeGuide* heldOf(QWidget* group) {
    return group->findChild<ket::SidebarTreeGuide*>(QStringLiteral("productRepositoryHeld"),
                                                    Qt::FindDirectChildrenOnly);
}

} // namespace

void sortSidebarRepositories(QVector<SidebarRepository>& repositories) {
    // A repository merges its worktrees' conversations, so each worktree's own recency order is
    // not enough: the conversation whose turn started last leads the whole group.
    for (SidebarRepository& repository : repositories) {
        std::sort(repository.conversations.begin(), repository.conversations.end(),
                  [](const QVariant& left, const QVariant& right) {
                      if (turnStartedAt(left) != turnStartedAt(right))
                          return turnStartedAt(left) > turnStartedAt(right);
                      const QVariantMap leftMap = left.toMap();
                      const QVariantMap rightMap = right.toMap();
                      if (const int titles =
                              compareNames(leftMap.value(QStringLiteral("title")).toString(),
                                           rightMap.value(QStringLiteral("title")).toString());
                          titles != 0)
                          return titles < 0;
                      return leftMap.value(QStringLiteral("id")).toString() <
                             rightMap.value(QStringLiteral("id")).toString();
                  });
    }
    // Activity can reorder conversations within a group, never the repository groups.
    std::stable_sort(repositories.begin(), repositories.end(),
                     [](const SidebarRepository& left, const SidebarRepository& right) {
                         const bool leftPinned = left.pinOrder >= 0;
                         if (leftPinned != (right.pinOrder >= 0))
                             return leftPinned;
                         if (leftPinned && left.pinOrder != right.pinOrder)
                             return left.pinOrder < right.pinOrder;
                         if (const int names = compareNames(left.name, right.name); names != 0)
                             return names < 0;
                         return left.path < right.path;
                     });
}

SidebarRepositoryList::SidebarRepositoryList(QWidget* parent)
    : QWidget(parent), layout_(new QVBoxLayout(this)) {
    chrome_geometry::margins(layout_, 0, 0, 0, 0);
    chrome_geometry::spacing(layout_, 0);
    const QStringList collapsed = QSettings().value(kCollapsedSetting).toStringList();
    collapsedPaths_ = QSet<QString>(collapsed.cbegin(), collapsed.cend());
    pins_.load();
    filterEmpty_ = new QLabel(sidebar_filter::text(sidebar_filter::empty), this);
    filterEmpty_->setObjectName(QStringLiteral("sidebarFilterEmpty"));
    filterEmpty_->setWordWrap(true);
    layout_->addWidget(filterEmpty_);
    filterEmpty_->hide();
    // The parts follow the kit's zoom; the list's own height follows them.
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this,
            [this] { applyPresentation(); });
}

void SidebarRepositoryList::setRepositories(const QVector<SidebarRepository>& projected) {
    QVector<SidebarRepository> repositories = projected;
    // T-92: the pins are this list's; a repository that cannot be pinned never sorts as one.
    for (SidebarRepository& repository : repositories)
        repository.pinOrder = repository.pinnable ? pins_.order(repository.path) : -1;
    sortSidebarRepositories(repositories);
    bool seenChanged = false;
    for (const SidebarRepository& repository : repositories) {
        QVariantList listed = repository.conversations;
        for (const SidebarTeam& team : repository.teams)
            listed += team.members;
        for (const QVariant& value : std::as_const(listed)) {
            const QVariantMap conversation = value.toMap();
            const QString id = conversation.value(QStringLiteral("id")).toString();
            // Daemon-backed `seen` is authoritative across relaunch; drop stale local marks
            // when a background turn completes and the conversation becomes unread again.
            if (conversation.value(QStringLiteral("seen")).toBool() ||
                conversation.value(QStringLiteral("selected")).toBool()) {
                if (!id.isEmpty() && !seenConversationIds_.contains(id)) {
                    seenConversationIds_.insert(id);
                    seenChanged = true;
                }
            } else if (seenConversationIds_.remove(id)) {
                seenChanged = true;
            }
        }
    }
    // Streaming status updates often repeat the same projection; keep the rows stable.
    if (built_ && repositories == repositories_) {
        if (seenChanged) {
            updateRows(repositories_);
            applyPresentation();
        }
        return;
    }
    repositories_ = repositories;
    forgetMissingTeams(repositories);
    // Selecting a conversation only changes state; rebuilding would flash the sidebar. Only the
    // groups whose shown structure changed are built again.
    presentGroups(repositories);
}

void SidebarRepositoryList::updateRows(const QVector<SidebarRepository>& repositories) {
    for (qsizetype group = 0; group < repositories.size() && group < headers_.size(); ++group) {
        const SidebarRepository& repository = repositories.at(group);
        auto* header = headers_.at(group);
        header->setSelected(repository.active);
        header->setProperty("activeRepository", repository.active);
        header->setConversationCount(conversationCount(repository));
        header->setWaitingCount(waitingCount(repository));
    }
    for (qsizetype group = 0; group < repositories.size() && group < groupRows_.size(); ++group) {
        const SidebarRepository& repository = repositories.at(group);
        const QHash<QString, ket::SidebarRow*>& rows = groupRows_.at(group);
        const auto update = [this, &rows](const QVariantList& listed) {
            for (const QVariant& value : listed) {
                const QVariantMap conversation = value.toMap();
                if (auto* row = rows.value(conversation.value(QStringLiteral("id")).toString()))
                    updateRow(row, conversation);
            }
        };
        for (const SidebarTeam& team : repository.teams)
            update(team.members);
        update(repository.conversations);
    }
    updateTeamFolders(repositories);
}

int SidebarRepositoryList::waitingCount(const SidebarRepository& repository) const {
    QVariantList listed = repository.conversations;
    int waiting = 0;
    for (const SidebarTeam& team : repository.teams) {
        listed += team.members;
        waiting += team.openDecisions;
    }
    for (const QVariant& value : std::as_const(listed)) {
        const QVariantMap conversation = value.toMap();
        const bool seen = conversation.value(QStringLiteral("seen")).toBool() ||
                          seenConversationIds_.contains(conversation.value("id").toString());
        waiting += sidebar_list::waitsForUser(conversation, seen) ? 1 : 0;
    }
    return waiting;
}

void SidebarRepositoryList::markConversationSeen(const QString& conversationId) {
    if (conversationId.isEmpty() || seenConversationIds_.contains(conversationId))
        return;
    seenConversationIds_.insert(conversationId);
    if (built_) {
        updateRows(repositories_);
        applyPresentation();
    }
    emit iconPresentationChanged();
}

void SidebarRepositoryList::applyZoom(const int zoomPercent) {
    chrome_geometry::apply(this, zoomPercent);
    applyPresentation();
}

void SidebarRepositoryList::applyPresentation() {
    const auto px = [this](int n) {
        return InterfaceZoomMetrics::geometry(n, chrome_geometry::zoomPercent(this));
    };
    for (qsizetype index = 0; index < groups_.size() && index < repositories_.size(); ++index) {
        QWidget* group = groups_.at(index);
        group->show();
        const SidebarRepository& repository = repositories_.at(index);
        const bool collapsed =
            (!repository.conversations.isEmpty() || !repository.teams.isEmpty()) &&
            collapsedPaths_.contains(repository.path);
        const bool showAll = showAllPaths_.contains(repository.path);
        const QSet<QString> preview = previewedConversationIds(repository.conversations, showAll);
        headers_.at(index)->setExpanded(!collapsed);
        auto* held = heldOf(group);
        held->setVisible(!collapsed &&
                         (!repository.conversations.isEmpty() || !repository.teams.isEmpty()));
        // A team's members are laid out by their folder.
        for (auto* row : held->findChildren<ket::SidebarRow*>(
                 QStringLiteral("productConversationButton"), Qt::FindDirectChildrenOnly))
            row->setVisible(preview.contains(row->property("conversationId").toString()));
        if (auto* section = held->findChild<ket::SidebarSectionLabel*>(
                QStringLiteral("productConversationsSection"), Qt::FindDirectChildrenOnly)) {
            section->setCount(static_cast<int>(repository.conversations.size()));
            section->setVisible(!repository.conversations.isEmpty());
        }
        if (auto* showMore =
                held->findChild<QPushButton*>(QStringLiteral("productShowMoreConversations"))) {
            const auto hidden = repository.conversations.size() - preview.size();
            showMore->setText(tr("Show %1 more").arg(hidden));
            showMore->setVisible(!showAll && hidden > 0);
            // Under the titles of the rows it adds.
            const int text =
                qRound(ket::sidebarTreeColumns(0, ket::Appearance::instance().zoom()).text);
            // A style sheet is parsed and polished again on every set; most presentations only
            // change selection.
            const QString padding = QStringLiteral("padding-left: %1px;").arg(text);
            if (showMore->styleSheet() != padding)
                showMore->setStyleSheet(padding);
            showMore->setFixedHeight(px(kShowMoreHeight));
        }
        presentTeamFolders(held);
    }
    applyFilter();
    for (QWidget* group : std::as_const(groups_))
        if (group->layout() != nullptr)
            group->layout()->activate();
    layout_->activate();
    setFixedHeight(layout_->hasHeightForWidth() ? layout_->totalHeightForWidth(width())
                                                : layout_->sizeHint().height());
}

void SidebarRepositoryList::onRepositoryHeaderClicked(const QString& path) {
    const SidebarRepository* repository = nullptr;
    for (const SidebarRepository& candidate : repositories_) {
        if (candidate.path == path) {
            repository = &candidate;
            break;
        }
    }
    if (repository == nullptr ||
        (repository->conversations.isEmpty() && repository->teams.isEmpty())) {
        applyPresentation();
        emit repositoryRequested(path);
        return;
    }
    if (collapsedPaths_.contains(path)) {
        collapsedPaths_.remove(path);
    } else {
        collapsedPaths_.insert(path);
        showAllPaths_.remove(path);
    }
    persistCollapsedRepositories();
    applyPresentation();
}

void SidebarRepositoryList::setRepositoryPinned(const QString& path, const bool pinned) {
    if (pinned)
        pins_.pin(path);
    else
        pins_.unpin(path);
    // The order and the pin marks are structure: rebuild from what was last shown.
    if (built_)
        setRepositories(repositories_);
}

QMenu* SidebarRepositoryList::createRepositoryMenu(const QString& path, QWidget* parent) {
    const auto found = std::find_if(
        repositories_.cbegin(), repositories_.cend(),
        [&path](const SidebarRepository& repository) { return repository.path == path; });
    if (found == repositories_.cend() || !found->pinnable)
        return nullptr;
    auto* menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("productRepositoryMenu"));
    const bool pinned = pins_.isPinned(path);
    QAction* pin = menu->addAction(pinned ? tr("Unpin") : tr("Pin to top"));
    pin->setObjectName(QStringLiteral("productRepositoryPinAction"));
    connect(pin, &QAction::triggered, this,
            [this, path, pinned] { setRepositoryPinned(path, !pinned); });
    return menu;
}

void SidebarRepositoryList::presentPinMark(ket::SidebarRepositoryHeader* header,
                                           const bool pinned) const {
    if (!pinned)
        return;
    auto* mark = new SidebarPinMark(header);
    mark->setObjectName(QStringLiteral("productRepositoryPinMark"));
    mark->setToolTip(tr("Pinned to top"));
    mark->setAccessibleName(tr("Pinned"));
    mark->place();
    header->installEventFilter(mark);
    mark->show();
}

void SidebarRepositoryList::persistCollapsedRepositories() const {
    QStringList values(collapsedPaths_.cbegin(), collapsedPaths_.cend());
    values.sort();
    QSettings().setValue(kCollapsedSetting, values);
}

bool SidebarRepositoryList::eventFilter(QObject* watched, QEvent* event) {
    if (handleDrag(watched, event))
        return true;
    if (event->type() == QEvent::MouseButtonDblClick) {
        const QString conversationId = watched->property("conversationId").toString();
        if (!conversationId.isEmpty()) {
            emit conversationRenameRequested(watched->property("worktreePath").toString(),
                                             conversationId);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace ketplus
