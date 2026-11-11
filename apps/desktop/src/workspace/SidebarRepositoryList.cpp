#include "workspace/SidebarRepositoryList.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetConversationRow.h"
#include "KetTreeItem.h"
#include "ui/ChromeGeometry.h"
#include "ui/InterfaceZoom.h"

#include "workspace/SymbolIcons.h"

#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace ketplus {
namespace {

constexpr int kConversationSpacing = 2;
constexpr int kGroupSpacing = 12;
constexpr int kPreviewConversationCount = 5;
constexpr int kShowMoreHeight = 26;
const auto kCollapsedSetting = QStringLiteral("sidebar/collapsedRepositories");

ket::ConversationStatus conversationStatus(const QVariantMap& conversation) {
    using Status = ket::ConversationStatus;
    const auto status = conversation.value(QStringLiteral("status")).toString().toLower();
    if (conversation.value(QStringLiteral("needsInput")).toBool() || status.contains("input") ||
        status.contains("question") || status == QStringLiteral("waiting for your answer…"))
        return Status::NeedsInput;
    if (conversation.value(QStringLiteral("awaitingReply")).toBool())
        return Status::AwaitingReply;
    if (conversation.value(QStringLiteral("running")).toBool())
        return Status::Working;
    if (status.contains("fail") || status.contains("error") || status.contains("disconnect") ||
        status.contains("missing") || status.contains("not found"))
        return Status::Error;
    if (status.contains("stop"))
        return Status::Stopped;
    return conversation.value(QStringLiteral("started")).toBool() ? Status::Completed
                                                                  : Status::NotStarted;
}

void updateConversation(ket::ConversationRow* row, const QVariantMap& conversation, bool seen) {
    const auto status = conversationStatus(conversation);
    row->setStatus(status);
    const bool unread = status == ket::ConversationStatus::Completed && !seen;
    row->setProperty("unread", unread);
    QString label =
        unread ? QObject::tr("Completed, unread") : ket::conversationStatusLabel(status);
    const QString sender = conversation.value(QStringLiteral("agentTurnNumber")).toString();
    if (status == ket::ConversationStatus::Working && !sender.isEmpty())
        label += QObject::tr(" · from #%1").arg(sender);
    row->setStatusLabel(label);
    row->setAccessibleDescription(label);
    // Only a team member's locked role is drawn; conversations show no user tags (ADR 0117).
    QList<ket::ConversationTag> tags;
    for (const auto& value : conversation.value(QStringLiteral("tags")).toList()) {
        const auto tag = value.toMap();
        const auto text = tag.value(QStringLiteral("label")).toString().trimmed();
        if (text.isEmpty() || !tag.value(QStringLiteral("locked")).toBool())
            continue;
        QColor color(tag.value(QStringLiteral("color")).toString());
        if (!color.isValid())
            color = ket::conversationStatusRecipe(ket::ConversationStatus::Working,
                                                  ket::Appearance::instance().theme())
                        .iconColor;
        tags.append({text, color, true});
    }
    row->setTags(tags);
}

// Everything that needs new widgets. Selection and turn status are updated in place.
// The conversation whose message started a running turn; empty once the turn ends.
QString runningAgentSender(const QVariantMap& conversation) {
    return conversation.value(QStringLiteral("running")).toBool()
               ? conversation.value(QStringLiteral("agentTurnNumber")).toString()
               : QString();
}

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
        if (seenChanged)
            updateRows(repositories_);
        return;
    }
    const QStringList structure = structureOf(repositories);
    repositories_ = repositories;
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
        if (groupIndex > 0)
            chrome_geometry::addSpacing(layout_, kGroupSpacing);
        auto* group = new QWidget(this);
        group->setObjectName(QStringLiteral("productRepositoryGroup"));
        group->setProperty("repositoryPath", repository.path);
        auto* groupLayout = new QVBoxLayout(group);
        chrome_geometry::margins(groupLayout, 0, 0, 0, 0);
        chrome_geometry::spacing(groupLayout, kConversationSpacing);

        auto* header = new ket::GroupHeader(repository.name, group);
        header->setObjectName(QStringLiteral("productRepositoryGroupHeader"));
        auto* name = header->toggle();
        name->setObjectName(QStringLiteral("productRepositoryGroupButton"));
        name->setToolTip(repository.path);
        name->setProperty("activeRepository", repository.active);
        header->setSelected(repository.active);
        header->setProperty("activeRepository", repository.active);
        connect(name, &QAbstractButton::clicked, this,
                [this, path = repository.path] { onRepositoryHeaderClicked(path); });
        auto* create = header->action();
        create->setObjectName(QStringLiteral("productNewConversationButton"));
        create->setToolTip(tr("New conversation in %1").arg(repository.name));
        connect(header, &ket::GroupHeader::actionTriggered, this,
                [this, path = repository.path, create] { onRepositoryAction(path, create); });
        headers_.append(header);
        groupLayout->addWidget(header);
        for (const SidebarTeam& team : repository.teams)
            groupLayout->addWidget(createTeamFolder(group, repository, team));

        for (const QVariant& value : repository.conversations)
            groupLayout->addWidget(createRow(group, value.toMap()));
        if (repository.conversations.size() > kPreviewConversationCount) {
            // A QPushButton honours text-align, so the label starts where the mock's does.
            auto* showMore = new QPushButton(group);
            showMore->setObjectName(QStringLiteral("productShowMoreConversations"));
            showMore->setProperty("kvRole", QStringLiteral("productShowMoreConversations"));
            showMore->setProperty("repositoryPath", repository.path);
            showMore->setText(tr("Show more"));
            showMore->setFlat(true);
            showMore->setCursor(Qt::PointingHandCursor);
            chrome_geometry::fixedHeight(showMore, kShowMoreHeight);
            showMore->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            connect(showMore, &QToolButton::clicked, this, [this, path = repository.path] {
                showAllPaths_.insert(path);
                applyPresentation();
            });
            groupLayout->addWidget(showMore);
        }
        groups_.append(group);
        layout_->addWidget(group);
    }
    updateTeamFolders(repositories);
    applyPresentation();
    setUpdatesEnabled(true);
}

ket::ConversationRow* SidebarRepositoryList::createRow(QWidget* group,
                                                       const QVariantMap& conversation) {
    const QString id = conversation.value(QStringLiteral("id")).toString();
    const QString worktreePath = conversation.value(QStringLiteral("path")).toString();
    QString title = conversation.value(QStringLiteral("title")).toString();
    if (title.isEmpty())
        title = tr("New conversation");
    const bool seen =
        conversation.value(QStringLiteral("seen")).toBool() || seenConversationIds_.contains(id);
    const bool selected = conversation.value(QStringLiteral("selected")).toBool();

    auto* button = new ket::ConversationRow(title, group);
    button->setObjectName(QStringLiteral("productConversationButton"));
    button->setProperty("kvRole", QStringLiteral("productConversation"));
    button->setProperty("worktreePath", worktreePath);
    button->setProperty("conversationId", id);
    button->setProperty("sessionId", conversation.value(QStringLiteral("sessionId")));
    button->installEventFilter(this);
    button->setCheckable(true);
    button->setChecked(selected);
    button->setProperty("selectedConversation", selected);
    button->setContextMenuPolicy(Qt::CustomContextMenu);
    // The worktree is an attribute of the conversation; name it without a machine path.
    // ADR 0027: agents and the user call a conversation by this number.
    const QString number = conversation.value(QStringLiteral("number")).toString();
    const QString numbered =
        number.isEmpty() ? title : QLatin1Char('#') + number + QLatin1Char(' ') + title;
    const QString sender = runningAgentSender(conversation);
    QString origin;
    if (!sender.isEmpty()) {
        const QString senderTitle = conversation.value(QStringLiteral("agentTurnTitle")).toString();
        origin =
            (conversation.value(QStringLiteral("agentTurnReply")).toBool() ? tr("Reply from #%1")
                                                                           : tr("Message from #%1"))
                .arg(sender) +
            (senderTitle.isEmpty() ? QString() : QStringLiteral(" · ") + senderTitle);
    }
    button->setToolTip(numbered + QLatin1Char('\n') +
                       (origin.isEmpty() ? QString() : origin + QLatin1Char('\n')) +
                       QFileInfo(worktreePath).fileName());
    button->setAccessibleName(numbered);
    button->setNumber(number);
    updateConversation(button, conversation, seen);
    connect(button, &QAbstractButton::clicked, this, [this, button, worktreePath, id] {
        markConversationSeen(id);
        // Selection follows the session manager; the refresh re-checks the row.
        button->setChecked(button->property("selectedConversation").toBool());
        emit conversationRequested(worktreePath, id);
    });
    connect(button, &QWidget::customContextMenuRequested, this,
            [this, worktreePath, id] { emit conversationContextMenuRequested(worktreePath, id); });
    rows_.append(button);
    return button;
}

void SidebarRepositoryList::updateRows(const QVector<SidebarRepository>& repositories) {
    for (qsizetype group = 0; group < repositories.size() && group < headers_.size(); ++group) {
        auto* header = qobject_cast<ket::GroupHeader*>(headers_.at(group));
        header->setSelected(repositories.at(group).active);
        header->toggle()->setProperty("activeRepository", repositories.at(group).active);
        header->setProperty("activeRepository", repositories.at(group).active);
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
            const QVariantMap conversation = value.toMap();
            const bool selected = conversation.value(QStringLiteral("selected")).toBool();
            const QString id = conversation.value(QStringLiteral("id")).toString();
            const bool seen = conversation.value(QStringLiteral("seen")).toBool() ||
                              seenConversationIds_.contains(id);
            auto* button = rows_.at(row++);
            button->setProperty("selectedConversation", selected);
            button->setChecked(selected);
            updateConversation(button, conversation, seen);
        }
    }
    updateTeamFolders(repositories);
}

void SidebarRepositoryList::markConversationSeen(const QString& conversationId) {
    if (conversationId.isEmpty() || seenConversationIds_.contains(conversationId))
        return;
    seenConversationIds_.insert(conversationId);
    if (built_)
        updateRows(repositories_);
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
    int height = 0;
    for (qsizetype index = 0; index < groups_.size() && index < repositories_.size(); ++index) {
        if (index > 0)
            height += px(kGroupSpacing);
        QWidget* group = groups_.at(index);
        const SidebarRepository& repository = repositories_.at(index);
        const bool collapsed =
            (!repository.conversations.isEmpty() || !repository.teams.isEmpty()) &&
            collapsedPaths_.contains(repository.path);
        const bool showAll = showAllPaths_.contains(repository.path);
        const QSet<QString> preview = previewedConversationIds(repository.conversations, showAll);
        int visibleRows = 0;
        // A team's members are laid out by their folder.
        for (auto* button : group->findChildren<ket::ConversationRow*>(
                 QStringLiteral("productConversationButton"), Qt::FindDirectChildrenOnly)) {
            const bool visible =
                !collapsed && preview.contains(button->property("conversationId").toString());
            button->setFixedHeight(button->sizeHint().height());
            button->setVisible(visible);
            if (visible)
                ++visibleRows;
        }
        auto* showMore =
            group->findChild<QPushButton*>(QStringLiteral("productShowMoreConversations"));
        const bool moreVisible = showMore != nullptr && !collapsed && !showAll &&
                                 repository.conversations.size() > preview.size();
        if (showMore != nullptr)
            showMore->setVisible(moreVisible);
        if (auto* name = group->findChild<QAbstractButton*>(
                QStringLiteral("productRepositoryGroupButton"))) {
            name->setAccessibleDescription(repository.conversations.isEmpty() ? QString()
                                           : collapsed                        ? tr("Collapsed")
                                                                              : tr("Expanded"));
        }
        const int extra = moreVisible ? px(kShowMoreHeight) + px(kConversationSpacing) : 0;
        auto* header = qobject_cast<ket::GroupHeader*>(headers_.at(index));
        header->setExpanded(!collapsed);
        const int rowHeight = rows_.isEmpty() ? 0 : rows_.first()->sizeHint().height();
        group->setFixedHeight(
            presentTeamFolders(group, collapsed, rowHeight) + header->sizeHint().height() +
            visibleRows * ((rows_.isEmpty() ? 0 : rows_.first()->sizeHint().height()) +
                           px(kConversationSpacing)) +
            extra);
        height += group->height();
    }
    setFixedHeight(height);
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
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        // Every repository offers a new conversation; another repository opens first.
        if (auto* action =
                qobject_cast<QWidget*>(watched->property("hoverAction").value<QObject*>()))
            action->setVisible(event->type() == QEvent::Enter);
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace ketplus
