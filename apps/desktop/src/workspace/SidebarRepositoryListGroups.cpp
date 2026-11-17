// A repository's group in the sidebar tree: its header, its teams and the conversations it
// previews. Groups are rebuilt one at a time, when what they show changes; a long-lived install
// holds thousands of conversations, and rebuilding every row for one new title froze the window.
#include "KetButton.h"
#include "KetSidebarTree.h"
#include "ui/ChromeGeometry.h"
#include "workspace/SidebarRepositoryList.h"
#include "workspace/SidebarRepositoryListDetail.h"

#include <QMenu>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace ketplus {

namespace sidebar_list {

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

} // namespace sidebar_list

using sidebar_list::previewedConversationIds;
using sidebar_list::runningAgentSender;

QStringList SidebarRepositoryList::groupStructure(const SidebarRepository& repository) const {
    // The active repository is state, not structure: switching repositories must not rebuild.
    QStringList structure{repository.path, repository.name,
                          repository.pinnable ? QStringLiteral("pinnable") : QString(),
                          QString::number(repository.pinOrder),
                          teamsAvailable_ ? QStringLiteral("teams") : QString()};
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
    // Only the rows that are built: a title further down the history changes nothing shown.
    const QSet<QString> preview =
        previewedConversationIds(repository.conversations, showAllPaths_.contains(repository.path));
    structure << QStringLiteral("more:%1").arg(repository.conversations.size() >
                                               sidebar_list::kPreviewConversationCount);
    for (const QVariant& value : repository.conversations) {
        const QVariantMap conversation = value.toMap();
        const QString id = conversation.value(QStringLiteral("id")).toString();
        if (!preview.contains(id))
            continue;
        structure << id << conversation.value(QStringLiteral("path")).toString()
                  << conversation.value(QStringLiteral("title")).toString()
                  << conversation.value(QStringLiteral("number")).toString()
                  << runningAgentSender(conversation);
    }
    return structure;
}

QWidget* SidebarRepositoryList::createGroup(const SidebarRepository& repository) {
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
    if (repository.pinnable) {
        header->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(header, &QWidget::customContextMenuRequested, this,
                [this, path = repository.path, header](const QPoint& at) {
                    QMenu* menu = createRepositoryMenu(path, header);
                    if (menu == nullptr)
                        return;
                    menu->setAttribute(Qt::WA_DeleteOnClose);
                    menu->popup(header->mapToGlobal(at));
                });
    }
    presentPinMark(header, repository.pinOrder >= 0);
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
    const QSet<QString> preview =
        previewedConversationIds(repository.conversations, showAllPaths_.contains(repository.path));
    for (const QVariant& value : repository.conversations) {
        const QVariantMap conversation = value.toMap();
        if (!preview.contains(conversation.value(QStringLiteral("id")).toString()))
            continue;
        auto* row = createRow(held, conversation, 0);
        row->setProperty("repositoryPath", repository.path);
        held->addPart(row);
    }
    if (repository.conversations.size() > sidebar_list::kPreviewConversationCount) {
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
            // The rest of the history is built now, in this group: the button stays.
            expandGroup(path);
            applyPresentation();
        });
        held->addPart(showMore);
    }
    return group;
}

void SidebarRepositoryList::presentGroups(const QVector<SidebarRepository>& repositories) {
    QHash<QString, qsizetype> previous;
    for (qsizetype index = 0; index < groups_.size(); ++index)
        previous.insert(groups_.at(index)->property("repositoryPath").toString(), index);

    QVector<QWidget*> groups;
    QVector<ket::SidebarRepositoryHeader*> headers;
    QVector<QStringList> structures;
    QVector<QHash<QString, ket::SidebarRow*>> rows;
    QSet<QWidget*> kept;
    bool changed = groups_.size() != repositories.size();
    for (qsizetype index = 0; index < repositories.size(); ++index) {
        const SidebarRepository& repository = repositories.at(index);
        QStringList structure = groupStructure(repository);
        const auto found = previous.constFind(repository.path);
        if (found != previous.cend() && groupStructures_.at(*found) == structure &&
            !kept.contains(groups_.at(*found))) {
            kept.insert(groups_.at(*found));
            groups.append(groups_.at(*found));
            headers.append(headers_.at(*found));
            rows.append(groupRows_.at(*found));
            changed = changed || *found != index;
        } else {
            QHash<QString, ket::SidebarRow*> built;
            buildingRows_ = &built;
            QWidget* group = createGroup(repository);
            buildingRows_ = nullptr;
            groups.append(group);
            headers.append(group->findChild<ket::SidebarRepositoryHeader*>(
                QStringLiteral("productRepositoryGroupButton"), Qt::FindDirectChildrenOnly));
            rows.append(built);
            changed = true;
        }
        structures.append(structure);
    }
    if (!changed && built_) {
        // Only state changed (selection, turn status): the rows are updated in place.
        updateRows(repositories);
        applyPresentation();
        return;
    }

    setUpdatesEnabled(false);
    // Kept groups move; the others go with their rows, folders and connections.
    while (auto* item = layout_->takeAt(0))
        delete item;
    for (QWidget* group : std::as_const(groups_))
        if (!kept.contains(group))
            delete group;
    groups_ = groups;
    headers_ = headers;
    groupStructures_ = structures;
    groupRows_ = rows;
    folders_.clear();
    for (qsizetype index = 0; index < groups_.size(); ++index) {
        QWidget* group = groups_.at(index);
        headers_.at(index)->setDivided(index > 0);
        folders_ += group->findChildren<QWidget*>(QStringLiteral("productTeamFolder"));
        layout_->addWidget(group);
        // Children, and so the Tab order and accessibility tree, in the order shown: a kept
        // group keeps its place among the children while the groups around it change.
        group->raise();
        // A layout shows a widget added to a shown parent only on the next event loop turn, so
        // until then the rebuilt sidebar was empty; with agents starting and ending turns it
        // flickered. Show it now, before updates are enabled again.
        group->show();
    }
    orderFocusChain();
    built_ = true;
    updateRows(repositories);
    applyPresentation();
    setUpdatesEnabled(true);
}

void SidebarRepositoryList::expandGroup(const QString& path) {
    const qsizetype index = std::find_if(groups_.cbegin(), groups_.cend(),
                                         [&path](const QWidget* group) {
                                             return group->property("repositoryPath") == path;
                                         }) -
                            groups_.cbegin();
    if (index >= groups_.size() || index >= repositories_.size())
        return;
    const SidebarRepository& repository = repositories_.at(index);
    auto* held = groups_.at(index)->findChild<ket::SidebarTreeGuide*>(
        QStringLiteral("productRepositoryHeld"), Qt::FindDirectChildrenOnly);
    if (held == nullptr)
        return;
    QHash<QString, ket::SidebarRow*>& rows = groupRows_[index];
    QVBoxLayout* parts = held->partsLayout();
    // New rows go between the built ones, in the repository's order, above "Show N more".
    int position = -1;
    for (int part = 0; part < parts->count(); ++part)
        if (auto* row = qobject_cast<ket::SidebarRow*>(parts->itemAt(part)->widget());
            row != nullptr && !row->property("teamMember").toBool()) {
            position = part;
            break;
        }
    if (position < 0)
        position = std::max(0, parts->count() - 1);
    buildingRows_ = &rows;
    for (const QVariant& value : repository.conversations) {
        const QVariantMap conversation = value.toMap();
        if (auto* row = rows.value(conversation.value(QStringLiteral("id")).toString())) {
            position = parts->indexOf(row) + 1;
            continue;
        }
        auto* row = createRow(held, conversation, 0);
        row->setProperty("repositoryPath", repository.path);
        parts->insertWidget(position++, row);
        row->show();
    }
    buildingRows_ = nullptr;
    groupStructures_[index] = groupStructure(repository);
    orderFocusChain();
}

namespace {

// The widgets Tab stops at, in the order their layouts show them. Children are created in the
// order they were built, which "Show N more" and partial rebuilds do not keep.
void tabStops(QWidget* widget, QList<QWidget*>& stops) {
    if ((widget->focusPolicy() & Qt::TabFocus) != 0)
        stops.append(widget);
    QList<QWidget*> children = widget->findChildren<QWidget*>(Qt::FindDirectChildrenOnly);
    if (QLayout* layout = widget->layout()) {
        std::stable_sort(children.begin(), children.end(), [layout](QWidget* left, QWidget* right) {
            const auto place = [layout](QWidget* child) {
                const int index = layout->indexOf(child);
                return index < 0 ? std::numeric_limits<int>::max() : index;
            };
            return place(left) < place(right);
        });
    }
    for (QWidget* child : std::as_const(children))
        if (!child->isWindow())
            tabStops(child, stops);
}

} // namespace

void SidebarRepositoryList::orderFocusChain() {
    // Widgets join the focus chain when they are created; a group built after its neighbours
    // came last on Tab. Thread the tab stops in the order the groups show them.
    QList<QWidget*> stops;
    for (QWidget* group : std::as_const(groups_))
        tabStops(group, stops);
    for (qsizetype index = 1; index < stops.size(); ++index)
        QWidget::setTabOrder(stops.at(index - 1), stops.at(index));
}

} // namespace ketplus
