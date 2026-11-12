#include "workspace/SidebarRepositoryList.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetSidebarTree.h"
#include "ui/ChromeGeometry.h"
#include "ui/InterfaceZoom.h"
#include "workspace/SidebarRepositoryListDetail.h"

#include <QEvent>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>

namespace ketplus {
namespace {

using sidebar_list::runningAgentSender;

constexpr int kPreviewConversationCount = 5;
constexpr int kShowMoreHeight = 28;
const auto kCollapsedSetting = QStringLiteral("sidebar/collapsedRepositories");

// Everything that needs new widgets. Selection and turn status are updated in place.
QStringList structureOf(const QVector<SidebarRepository>& repositories) {
    QStringList structure;
    for (const SidebarRepository& repository : repositories) {
        // The active repository is state, not structure: switching repositories must not rebuild.
        structure << repository.path << repository.name;
        for (const SidebarTeam& team : repository.teams) {
            structure << QStringLiteral("team:") + team.id << team.name;
            for (const QVariant& value : team.members) {
                const QVariantMap member = value.toMap();
                structure << member.value(QStringLiteral("id")).toString()
                          << member.value(QStringLiteral("title")).toString()
                          << member.value(QStringLiteral("number")).toString()
                          << member.value(QStringLiteral("teamRole")).toString()
                          << runningAgentSender(member);
            }
        }
        for (const QVariant& value : repository.conversations) {
            const QVariantMap conversation = value.toMap();
            structure << conversation.value(QStringLiteral("id")).toString()
                      << conversation.value(QStringLiteral("path")).toString()
                      << conversation.value(QStringLiteral("title")).toString()
                      << conversation.value(QStringLiteral("number")).toString()
                      << runningAgentSender(conversation);
        }
        structure << QString();
    }
    return structure;
}

// Newest rows stay first. Running and selected conversations keep a slot even
// when that pushes the visible set past five.
QSet<QString> previewedConversationIds(const QVariantList& conversations, const bool showAll) {
    QSet<QString> kept;
    if (showAll || conversations.size() <= kPreviewConversationCount) {
        for (const QVariant& value : conversations)
            kept.insert(value.toMap().value(QStringLiteral("id")).toString());
        return kept;
    }
    for (const QVariant& value : conversations) {
        const QVariantMap conversation = value.toMap();
        if (conversation.value(QStringLiteral("running")).toBool() ||
            conversation.value(QStringLiteral("selected")).toBool())
            kept.insert(conversation.value(QStringLiteral("id")).toString());
    }
    for (const QVariant& value : conversations) {
        if (kept.size() >= kPreviewConversationCount)
            break;
        kept.insert(value.toMap().value(QStringLiteral("id")).toString());
    }
    return kept;
}

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
    // The first conversation is the newest now, so it dates its repository.
    const auto latest = [](const SidebarRepository& repository) {
        return repository.conversations.isEmpty() ? 0
                                                  : turnStartedAt(repository.conversations.first());
    };
    std::sort(repositories.begin(), repositories.end(),
              [&latest](const SidebarRepository& left, const SidebarRepository& right) {
                  if (left.promoted != right.promoted)
                      return left.promoted;
                  if (latest(left) != latest(right))
                      return latest(left) > latest(right);
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
    // The parts follow the kit's zoom; the list's own height follows them.
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this,
            [this] { applyPresentation(); });
}

void SidebarRepositoryList::setRepositories(const QVector<SidebarRepository>& projected) {
    QVector<SidebarRepository> repositories = projected;
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
    const QStringList structure = structureOf(repositories);
    repositories_ = repositories;
    forgetMissingTeams(repositories);
    if (built_ && structure == structure_) {
        // Selecting a conversation only changes state; rebuilding would flash the sidebar.
        updateRows(repositories);
        applyPresentation();
        return;
    }
    built_ = true;
    structure_ = structure;
    rows_.clear();
    headers_.clear();
    groups_.clear();
    folders_.clear();
    setUpdatesEnabled(false);
    while (auto* item = layout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (qsizetype groupIndex = 0; groupIndex < repositories.size(); ++groupIndex) {
        const SidebarRepository& repository = repositories.at(groupIndex);
        auto* group = new QWidget(this);
        group->setObjectName(QStringLiteral("productRepositoryGroup"));
        group->setProperty("repositoryPath", repository.path);
        auto* groupLayout = new QVBoxLayout(group);
        chrome_geometry::margins(groupLayout, 0, 0, 0, 0);
        chrome_geometry::spacing(groupLayout, 0);

        // The repository is the root: its chevron folds it, + offers what it can start.
        auto* header = new ket::SidebarRepositoryHeader(repository.name, group);
        header->setObjectName(QStringLiteral("productRepositoryGroupButton"));
        header->setToolTip(repository.path);
        header->setDivided(groupIndex > 0);
        connect(header, &QAbstractButton::clicked, this,
                [this, path = repository.path] { onRepositoryHeaderClicked(path); });
        auto* create = header->action();
        create->setObjectName(QStringLiteral("productNewConversationButton"));
        const QString createText = teamsAvailable_
                                       ? tr("New conversation or team in %1").arg(repository.name)
                                       : tr("New conversation in %1").arg(repository.name);
        create->setToolTip(createText);
        create->setLabel(createText);
        connect(header, &ket::SidebarRepositoryHeader::actionTriggered, this,
                [this, path = repository.path, create] { onRepositoryAction(path, create); });
        headers_.append(header);
        groupLayout->addWidget(header);

        // What the repository holds, along the guide line under its chevron: its teams, then the
        // conversations of no team.
        auto* held = new ket::SidebarTreeGuide(0, group);
        held->setObjectName(QStringLiteral("productRepositoryHeld"));
        groupLayout->addWidget(held);
        for (const SidebarTeam& team : repository.teams)
            held->addPart(createTeamFolder(held, repository, team));
        if (!repository.teams.isEmpty()) {
            auto* section = new ket::SidebarSectionLabel(tr("Conversations"), held);
            section->setObjectName(QStringLiteral("productConversationsSection"));
            section->action()->setObjectName(QStringLiteral("productSectionNewConversation"));
            section->action()->setToolTip(tr("New conversation in %1").arg(repository.name));
            connect(section, &ket::SidebarSectionLabel::actionTriggered, this,
                    [this, path = repository.path] { emit newConversationRequested(path); });
            held->addPart(section);
        }
        for (const QVariant& value : repository.conversations) {
            auto* row = createRow(held, value.toMap(), 0);
            row->setProperty("repositoryPath", repository.path);
            held->addPart(row);
        }
        if (repository.conversations.size() > kPreviewConversationCount) {
            // A QPushButton honours text-align, so the label starts under the titles.
            auto* showMore = new QPushButton(held);
            showMore->setObjectName(QStringLiteral("productShowMoreConversations"));
            showMore->setProperty("kvRole", QStringLiteral("productShowMoreConversations"));
            showMore->setProperty("repositoryPath", repository.path);
            showMore->setFlat(true);
            showMore->setCursor(Qt::PointingHandCursor);
            showMore->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            connect(showMore, &QPushButton::clicked, this, [this, path = repository.path] {
                showAllPaths_.insert(path);
                applyPresentation();
            });
            held->addPart(showMore);
        }
        groups_.append(group);
        layout_->addWidget(group);
        // A layout shows a widget added to a shown parent only on the next event loop turn, so
        // until then the rebuilt sidebar was empty; with agents starting and ending turns it
        // flickered. Show it now, before updates are enabled again.
        group->show();
    }
    updateRows(repositories);
    applyPresentation();
    setUpdatesEnabled(true);
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
    qsizetype row = 0;
    for (const SidebarRepository& repository : repositories) {
        QVariantList listed;
        for (const SidebarTeam& team : repository.teams)
            listed += team.members;
        listed += repository.conversations;
        for (const QVariant& value : std::as_const(listed)) {
            if (row >= rows_.size())
                return;
            updateRow(rows_.at(row++), value.toMap());
        }
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
            showMore->setStyleSheet(QStringLiteral("padding-left: %1px;").arg(text));
            showMore->setFixedHeight(px(kShowMoreHeight));
        }
        presentTeamFolders(held);
    }
    for (QWidget* group : std::as_const(groups_))
        if (group->layout() != nullptr)
            group->layout()->activate();
    layout_->activate();
    setFixedHeight(layout_->sizeHint().height());
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
