#include "teams/TeamPage.h"

#include "KetAppearance.h"
#include "KetBanner.h"
#include "KetButton.h"
#include "KetEmptyState.h"
#include "KetExpandableText.h"
#include "KetIcon.h"
#include "KetIcons.h"
#include "KetStyle.h"
#include "KetTokens.h"
#include "teams/TeamCommands.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamMenus.h"
#include "teams/TeamPageParts.h"
#include "teams/TeamTurnBudget.h"

#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace ketplus {
namespace {

using team_page::px;
using team_page::Text;

constexpr int kQueueBannerDevelopers = 4;
// The mock's page: a column of at most 1120, 42 from the sides, roomy from 860.
constexpr int kColumnWidth = 1120;
constexpr int kRoomyWidth = 860;
constexpr int kSideMargin = 42;
constexpr int kNarrowSideMargin = 20;

QString pausedReason(const QString& reason) {
    if (reason == QStringLiteral("budget"))
        return TeamPage::tr("The team used its automatic turns. Answer its decision or resume.");
    if (reason == QStringLiteral("empty_role"))
        return TeamPage::tr("A role has no member. Add one, then resume.");
    return TeamPage::tr("You paused it. Messages wait until you resume.");
}

} // namespace

TeamPage::TeamPage(TeamDirectory* teams, const QString& teamId, const QString& repository,
                   QWidget* parent)
    : QWidget(parent), teams_(teams), teamId_(teamId), repository_(repository),
      scroll_(new QScrollArea(this)) {
    setObjectName(QStringLiteral("teamPage"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    scroll_->setObjectName(QStringLiteral("teamPageScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    // The page fits its width; nothing is reached by scrolling sideways.
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll_);
    connect(teams_, &TeamDirectory::changed, this, [this] {
        // The directory reports every team: only a change of this one rebuilds the page.
        if (teams_->team(teamId_).value_or(QJsonObject{}) != shown_)
            rebuild();
        readUpdates();
    });
    // Sizes follow the zoom and the task table's colours the theme.
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
            [this] { rebuild(); });
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this,
            [this] { rebuild(); });
    rebuild();
    readUpdates();
}

QString TeamPage::commandKey(const QString& what) const { return TeamCommands::key(teamId_, what); }

void TeamPage::send(const QString& what, const QString& source, const QString& type,
                    QJsonObject payload) {
    payload.insert(QStringLiteral("team_id"), teamId_);
    // A refusal shows in the key's status label, the one bound when the answer comes.
    teams_->commands()->send(commandKey(what), source, type, payload);
}

void TeamPage::readUpdates() {
    if (readPending_ || !teams_->team(teamId_))
        return;
    readPending_ = true;
    const QPointer<TeamPage> alive(this);
    // Once per burst of changes; team.get also notices a merge made outside the app.
    QTimer::singleShot(0, this, [this, alive] {
        if (!alive)
            return;
        teams_->request(QStringLiteral("team.get"), {{QStringLiteral("team_id"), teamId_}},
                        [alive](const QJsonObject& result, const QString& error) {
                            if (!alive)
                                return;
                            alive->readPending_ = false;
                            if (!error.isEmpty())
                                return;
                            QJsonArray updates = result.value(QStringLiteral("updates")).toArray();
                            const bool more =
                                result.value(QStringLiteral("updates_has_more")).toBool();
                            if (updates == alive->updates_ && more == alive->newestHaveMore_)
                                return;
                            alive->updates_ = updates;
                            alive->newestHaveMore_ = more;
                            alive->rebuild();
                        });
    });
}

void TeamPage::readOlderUpdates() {
    if (olderPending_)
        return;
    qint64 oldest = 0;
    for (const QJsonArray* page : {&updates_, &olderUpdates_})
        for (const auto& value : *page) {
            const qint64 id = value.toObject().value(QStringLiteral("id")).toInteger();
            oldest = oldest == 0 ? id : qMin(oldest, id);
        }
    if (oldest <= 0)
        return;
    olderPending_ = true;
    const QPointer<TeamPage> alive(this);
    teams_->request(QStringLiteral("team.updates.list"),
                    {{QStringLiteral("team_id"), teamId_},
                     {QStringLiteral("before_id"), oldest},
                     {QStringLiteral("limit"), 50}},
                    [alive](const QJsonObject& result, const QString& error) {
                        if (!alive)
                            return;
                        alive->olderPending_ = false;
                        if (!error.isEmpty())
                            return;
                        for (const auto& value : result.value(QStringLiteral("updates")).toArray())
                            alive->olderUpdates_.append(value);
                        alive->olderHaveMore_ = result.value(QStringLiteral("has_more")).toBool();
                        alive->rebuild();
                    });
}

QString TeamPage::memberName(const QJsonObject& team, const QString& sessionId) {
    const QJsonObject member = TeamDirectory::member(team, sessionId);
    if (member.isEmpty())
        return sessionId.isEmpty() ? tr("Unassigned") : sessionId;
    const QString number = member.value(QStringLiteral("number")).toString();
    const QString name = member.value(QStringLiteral("name")).toString();
    return number.isEmpty() ? name : QStringLiteral("%1 · #%2").arg(name, number);
}

bool TeamPage::roomy() const { return width() - 2 * px(kSideMargin) >= px(kRoomyWidth); }

int TeamPage::columnWidth() const {
    return qMin(width() - 2 * px(roomy() ? kSideMargin : kNarrowSideMargin), px(kColumnWidth));
}

bool TeamPage::compact() const { return columnWidth() < px(520); }

// The task filter sits beside the Tasks title as segments while both fit on the line.
bool TeamPage::filterFits() const { return segmentedWidth_ + px(220) <= columnWidth(); }

void TeamPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (roomy() != roomy_ || compact() != compact_ ||
        (segmentedWidth_ > 0 && filterFits() != segmented_))
        rebuild();
}

// The focused widget's own name, or its nearest named ancestor's and the part's place in it: a
// kit control keeps the focus on a part without a name. A decision's and a member's row carry
// their id.
TeamPage::FocusPlace TeamPage::focusPlace() const {
    QWidget* focused = content_ ? window()->focusWidget() : nullptr;
    if (!focused || !content_->isAncestorOf(focused))
        return {};
    QWidget* const part = focused;
    while (focused != content_ && focused->objectName().isEmpty())
        focused = focused->parentWidget();
    if (focused == content_)
        return {};
    FocusPlace place{.name = focused->objectName()};
    if (part != focused)
        place.part = static_cast<int>(focused->findChildren<QWidget*>().indexOf(part));
    const QWidget* scope = content_;
    for (QWidget* above = focused->parentWidget(); above != content_; above = above->parentWidget())
        if (above->objectName().contains(QLatin1Char('_'))) {
            place.row = above->objectName();
            scope = above;
            break;
        }
    place.index = static_cast<int>(scope->findChildren<QWidget*>(place.name).indexOf(focused));
    return place;
}

void TeamPage::restoreFocus(const FocusPlace& place) {
    if (place.index < 0)
        return;
    const QWidget* scope =
        place.row.isEmpty() ? content_ : content_->findChild<QWidget*>(place.row);
    const QList<QWidget*> same =
        scope ? scope->findChildren<QWidget*>(place.name) : QList<QWidget*>{};
    if (same.isEmpty()) {
        // What had the focus is gone, such as the decision just answered: the page keeps it,
        // where the arrows scroll and Tab goes on into the sections.
        scroll_->setFocus(Qt::OtherFocusReason);
        return;
    }
    QWidget* target = same.at(qMin(place.index, static_cast<int>(same.size()) - 1));
    // The same part of the rebuilt widget, when it still takes the keyboard there: a banner
    // does not act on a key, its button does.
    if (place.part >= 0) {
        const QList<QWidget*> parts = target->findChildren<QWidget*>();
        if (place.part < parts.size() && parts.at(place.part)->focusPolicy() != Qt::NoFocus)
            target = parts.at(place.part);
    }
    target->setFocus(Qt::OtherFocusReason);
}

void TeamPage::rebuild() {
    const int position = scroll_->verticalScrollBar()->value();
    const FocusPlace focus = focusPlace();
    const QString currentTask =
        tasks_ && tasks_->currentRow() >= 0 && tasks_->item(tasks_->currentRow(), 0)
            ? tasks_->item(tasks_->currentRow(), 0)->text()
            : QString{};
    roomy_ = roomy();
    compact_ = compact();
    segmented_ = true;
    segmentedWidth_ = 0;
    tasks_ = nullptr;
    taskRows_ = nullptr;
    taskHint_ = nullptr;
    content_ = new QWidget;
    content_->setObjectName(QStringLiteral("teamPageContent"));
    auto* centred = new QHBoxLayout(content_);
    const int side = px(roomy_ ? kSideMargin : kNarrowSideMargin);
    centred->setContentsMargins(side, px(roomy_ ? 38 : 20), side, px(40));
    // A widget narrower than its cell is centred in it.
    column_ = new QWidget(content_);
    column_->setObjectName(QStringLiteral("teamPageColumn"));
    column_->setMaximumWidth(px(kColumnWidth));
    centred->addWidget(column_);
    layout_ = new QVBoxLayout(column_);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(px(14));
    const auto team = teams_->team(teamId_);
    shown_ = team.value_or(QJsonObject{});
    if (!team) {
        buildEmpty();
    } else {
        buildHeading(*team);
        buildBanners(*team);
        buildDecisions(*team);
        buildTasks(*team);
        buildMembers(*team);
        buildUpdates();
    }
    layout_->addStretch();
    scroll_->setWidget(content_); // deletes the previous content
    // An update keeps the reader where it was.
    content_->adjustSize();
    scroll_->verticalScrollBar()->setValue(position);
    // And the keyboard where it was: the control that had the focus and the current task.
    if (tasks_ && !currentTask.isEmpty())
        for (int row = 0; row < tasks_->rowCount(); ++row)
            if (tasks_->item(row, 0)->text() == currentTask) {
                tasks_->setCurrentCell(row, 0);
                break;
            }
    restoreFocus(focus);
}

// A section's title, what it counts, and at the right what acts on the whole section.
void TeamPage::addSection(const QString& title, const QString& count, const char* name,
                          QWidget* trailing) {
    auto* row = new QWidget(column_);
    auto* line = new QHBoxLayout(row);
    line->setContentsMargins(0, px(14), 0, 0);
    line->setSpacing(px(10));
    auto* label = new Text(title, row, Text::Fit::Elide);
    label->setObjectName(QLatin1String(name));
    label->setType(17, QFont::Medium);
    label->setAccessibleName(title);
    line->addWidget(label);
    auto* counted = new Text(count, row, Text::Fit::Elide);
    counted->setTone(QStringLiteral("text-secondary"));
    counted->setVisible(!count.isEmpty());
    line->addWidget(counted, 1);
    if (!count.isEmpty())
        row->setAccessibleName(QStringLiteral("%1, %2").arg(title, count));
    else
        line->addStretch(1);
    if (trailing) {
        trailing->setParent(row);
        line->addWidget(trailing);
    }
    layout_->addWidget(row);
}

void TeamPage::setConversationDirectory(team_goal::ConversationDirectory directory) {
    conversations_ = std::move(directory);
    rebuild();
}

void TeamPage::buildHeading(const QJsonObject& team) {
    auto* row = new QWidget(column_);
    auto* line = new QHBoxLayout(row);
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(px(16));
    auto* titles = new QVBoxLayout;
    titles->setSpacing(px(8));
    auto* eyebrow =
        new Text(team.value(QStringLiteral("paused")).toBool() ? tr("TEAM · PAUSED") : tr("TEAM"),
                 row, Text::Fit::Elide);
    eyebrow->setObjectName(QStringLiteral("teamPageEyebrow"));
    eyebrow->setTone(QStringLiteral("accent-text"));
    eyebrow->setType(12, QFont::Medium);
    eyebrow->setLetterSpacing(1.6);
    titles->addWidget(eyebrow);
    const QString name = team.value(QStringLiteral("name")).toString();
    auto* title = new Text(name, row);
    title->setObjectName(QStringLiteral("teamPageName"));
    title->setType(roomy_ ? 30 : 24, QFont::DemiBold);
    title->setAccessibleName(name);
    titles->addWidget(title);
    line->addLayout(titles, 1);
    auto* add = new ket::Button(tr("Add member"), ket::ButtonVariant::Secondary, row);
    add->setObjectName(QStringLiteral("teamPageAddMember"));
    add->setIcon(ket::Icons::icon(u"plus"));
    const bool full =
        TeamDirectory::count(team, QStringLiteral("developer")) >= team_roles::maximumDevelopers;
    add->setEnabled(!full);
    add->setToolTip(full ? tr("A team has at most five developers.") : QString{});
    connect(add, &QAbstractButton::clicked, this, [this] { emit addMemberRequested(teamId_); });
    line->addWidget(add, 0, Qt::AlignTop);
    auto* more = new ket::IconButton(ket::Icons::icon(u"more"), tr("Team actions"), row);
    more->setObjectName(QStringLiteral("teamPageMenu"));
    connect(more, &QAbstractButton::clicked, this,
            [this, more] { showMenu(more, *teams_->team(teamId_)); });
    line->addWidget(more, 0, Qt::AlignTop);
    layout_->addWidget(row);
    if (const QString goal = team.value(QStringLiteral("goal")).toString(); !goal.isEmpty()) {
        auto* purpose = team_goal::foldedGoal(
            goal, conversations_ ? conversations_() : QVariantList{}, column_);
        purpose->setExpanded(goalExpanded_);
        connect(purpose, &ket::ExpandableText::expandedChanged, this,
                [this](const bool expanded) { goalExpanded_ = expanded; });
        connect(purpose, &ket::ExpandableText::mentionActivated, this, &TeamPage::mentionRequested);
        // The goal whole, under the heading at the column's width (ADR 0114).
        layout_->addWidget(purpose);
    }

    // Where the team works, who merges, and how far it is.
    const QJsonArray tasks = team.value(QStringLiteral("tasks")).toArray();
    const int done = static_cast<int>(std::count_if(tasks.begin(), tasks.end(), [](auto value) {
        return value.toObject().value(QStringLiteral("state")).toString() == QStringLiteral("done");
    }));
    const QString branch = team.value(QStringLiteral("integration_branch")).toString();
    auto* facts = new QWidget(column_);
    facts->setObjectName(QStringLiteral("teamPageFacts"));
    auto* flow = new team_page::Flow(facts, px(18), px(6));
    QStringList spoken;
    const auto fact = [&](const QString& icon, const QString& value, const char* objectName,
                          const bool mono = false) {
        auto* chip = new QWidget(facts);
        auto* pair = new QHBoxLayout(chip);
        pair->setContentsMargins(0, 0, 0, 0);
        pair->setSpacing(px(6));
        if (!icon.isEmpty()) {
            auto* glyph = new ket::Icon(icon, 13, chip);
            glyph->setColor(ket::Tokens::color(u"text-muted", ket::Appearance::instance().theme()));
            pair->addWidget(glyph);
        }
        auto* label = new Text(value, chip, Text::Fit::ElideMiddle);
        label->setObjectName(QLatin1String(objectName));
        label->setTone(QStringLiteral("text-secondary"));
        label->setType(mono ? 12 : 13, mono ? QFont::Medium : QFont::Normal, mono);
        pair->addWidget(label);
        flow->addWidget(chip);
        spoken.append(value);
    };
    fact(QStringLiteral("folder"),
         QFileInfo(team.value(QStringLiteral("repository")).toString()).fileName(),
         "teamPageRepository");
    fact(QStringLiteral("branch"), branch.isEmpty() ? tr("The current branch") : branch,
         "teamPageBranch", !branch.isEmpty());
    fact(QStringLiteral("pull-request"),
         team.value(QStringLiteral("merge_policy")).toString() == QStringLiteral("user")
             ? tr("You merge tasks")
             : tr("The leader merges tasks"),
         "teamPageMerges");
    fact(QStringLiteral("clock"), team_budget::usage(team), "teamPageTurns");
    if (!tasks.isEmpty())
        fact({}, tr("%1 of %2 tasks done").arg(done).arg(tasks.size()), "teamPageProgress");
    facts->setAccessibleName(spoken.join(QStringLiteral(", ")));
    layout_->addWidget(facts);
}

void TeamPage::showMenu(QWidget* anchor, const QJsonObject& team) {
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("teamPageActions"));
    auto* create = menu.addAction(ket::Icons::icon(u"users"), tr("New team…"));
    const bool userMerges =
        team.value(QStringLiteral("merge_policy")).toString() == QStringLiteral("user");
    auto* merges =
        menu.addAction(userMerges ? tr("Let the leader merge tasks") : tr("Merge tasks myself"));
    const bool paused = team.value(QStringLiteral("paused")).toBool();
    auto* pause =
        menu.addAction(paused ? tr("Resume automatic turns") : tr("Pause automatic turns"));
    auto* budget = menu.addAction(ket::Icons::icon(u"clock"), tr("Automatic turns…"));
    budget->setObjectName(QStringLiteral("teamPageTurnBudget"));
    // T-23: a second reviewer, at any team size; a team has at most two.
    auto* reviewer = menu.addAction(ket::Icons::icon(u"plus"), tr("Add a second reviewer…"));
    reviewer->setObjectName(QStringLiteral("teamPageAddReviewer"));
    reviewer->setEnabled(TeamDirectory::count(team, QStringLiteral("reviewer")) < 2);
    menu.addSeparator();
    auto* remove = menu.addAction(ket::Icons::icon(u"trash"), tr("Delete team…"));
    remove->setProperty(ket::Style::kDangerProperty, true);
    merges->setObjectName(QStringLiteral("teamPageMergePolicy"));
    pause->setObjectName(QStringLiteral("teamPagePause"));
    remove->setObjectName(QStringLiteral("teamPageDelete"));
    // A change still on its way to the daemon is not offered again (TeamCommands).
    TeamCommands* commands = teams_->commands();
    commands->bind(merges, commandKey(QStringLiteral("merge_policy")));
    commands->bind(pause, commandKey(QStringLiteral("paused")));
    commands->bind(budget, commandKey(QStringLiteral("turn_budget")));
    commands->bind(remove, commandKey(QStringLiteral("delete")));
    QAction* chosen = menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
    if (chosen == create)
        afterMenuCloses(this,
                        [this, repository = team.value(QStringLiteral("repository")).toString()] {
                            emit newTeamRequested(repository);
                        });
    else if (chosen == merges)
        send(QStringLiteral("merge_policy"), {}, QStringLiteral("team.update"),
             {{QStringLiteral("merge_policy"),
               userMerges ? QStringLiteral("leader") : QStringLiteral("user")}});
    else if (chosen == pause)
        send(QStringLiteral("paused"), QStringLiteral("menu"), QStringLiteral("team.update"),
             {{QStringLiteral("paused"), !paused}});
    else if (chosen == budget)
        afterMenuCloses(this, [this, team] { TeamTurnBudgetForm::present(this, teams_, team); });
    else if (chosen == reviewer)
        afterMenuCloses(this, [this] { emit addReviewerRequested(teamId_); });
    else if (chosen == remove)
        afterMenuCloses(this, [this, id = teamId_] { emit deleteRequested(id); });
}

void TeamPage::buildBanners(const QJsonObject& team) {
    if (team.value(QStringLiteral("paused")).toBool()) {
        auto* banner = new ket::Banner(
            ket::BannerTone::Warning, tr("Automatic turns are paused"),
            pausedReason(team.value(QStringLiteral("paused_reason")).toString()), column_);
        banner->setObjectName(QStringLiteral("teamPausedBanner"));
        QAbstractButton* resume = banner->setAction(tr("Resume"));
        resume->setObjectName(QStringLiteral("teamPausedResume"));
        teams_->commands()->bind(resume, commandKey(QStringLiteral("paused")),
                                 QStringLiteral("banner"));
        connect(resume, &QAbstractButton::clicked, this, [this] {
            send(QStringLiteral("paused"), QStringLiteral("banner"), QStringLiteral("team.update"),
                 {{QStringLiteral("paused"), false}});
        });
        layout_->addWidget(banner);
    }
    const int developers = TeamDirectory::count(team, QStringLiteral("developer"));
    // T-23: a second reviewer takes the queue away, so the banner offers one until it is there.
    if (developers >= kQueueBannerDevelopers &&
        TeamDirectory::count(team, QStringLiteral("reviewer")) < 2) {
        auto* banner = new ket::Banner(
            ket::BannerTone::Info, tr("The reviewer is the queue"),
            tr("%1 developers share one reviewer, so finished tasks wait their turn. A second "
               "reviewer reviews another task at the same time; a second team, from the team's "
               "menu, gives the next developers their own.")
                .arg(developers),
            column_);
        banner->setObjectName(QStringLiteral("teamQueueBanner"));
        QAbstractButton* second = banner->setAction(tr("Add a second reviewer"));
        second->setObjectName(QStringLiteral("teamQueueAddReviewer"));
        connect(second, &QAbstractButton::clicked, this,
                [this] { emit addReviewerRequested(teamId_); });
        layout_->addWidget(banner);
    }
}

void TeamPage::buildEmpty() {
    auto* empty = new ket::EmptyState(
        QStringLiteral("users"), tr("No team here yet"),
        tr("A team is a leader, developers and a reviewer working toward one goal, each in its "
           "own worktree. The leader plans the tasks; the team asks you only when it needs a "
           "decision."),
        column_);
    empty->setObjectName(QStringLiteral("teamPageEmpty"));
    connect(empty->setAction(tr("New team…"), QStringLiteral("users")), &QAbstractButton::clicked,
            this, [this] { emit newTeamRequested(repository_); });
    layout_->addWidget(empty);
}

} // namespace ketplus
