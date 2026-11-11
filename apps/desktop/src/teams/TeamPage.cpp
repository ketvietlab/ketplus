#include "teams/TeamPage.h"

#include "KetBanner.h"
#include "KetButton.h"
#include "KetEmptyState.h"
#include "KetIcons.h"
#include "KetSegmentedControl.h"
#include "KetStyle.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamMenus.h"
#include "teams/TeamTaskDialog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
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

QString tr(const char* text) { return QCoreApplication::translate("TeamPage", text); }

constexpr int kQueueBannerDevelopers = 4;
constexpr int kUpdatesShown = 50;

QLabel* text(QWidget* parent, const QString& value, const char* name = nullptr,
             bool muted = false) {
    auto* label = new QLabel(value, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    if (name)
        label->setObjectName(QLatin1String(name));
    if (muted)
        label->setProperty("kvRole", QStringLiteral("muted"));
    return label;
}

QLabel* heading(QWidget* parent, const QString& value, const char* name) {
    auto* label = text(parent, value, name);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    label->setAccessibleName(value);
    return label;
}

QString memberName(const QJsonObject& team, const QString& sessionId) {
    const QJsonObject member = TeamDirectory::member(team, sessionId);
    if (member.isEmpty())
        return sessionId.isEmpty() ? tr("Unassigned") : sessionId;
    const QString number = member.value(QStringLiteral("number")).toString();
    const QString name = member.value(QStringLiteral("name")).toString();
    return number.isEmpty() ? name : QStringLiteral("%1 · #%2").arg(name, number);
}

QString pausedReason(const QString& reason) {
    if (reason == QStringLiteral("budget"))
        return tr("The team used its automatic turns. Answer its decision or resume.");
    if (reason == QStringLiteral("empty_role"))
        return tr("A role has no member. Add one, then resume.");
    return tr("You paused it. Messages wait until you resume.");
}

QString leaderOf(const QJsonObject& team) {
    for (const auto& value : team.value(QStringLiteral("members")).toArray())
        if (value.toObject().value(QStringLiteral("role")).toString() == QStringLiteral("leader"))
            return value.toObject().value(QStringLiteral("session_id")).toString();
    return {};
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
    outer->addWidget(scroll_);
    connect(teams_, &TeamDirectory::changed, this, [this] {
        rebuild();
        readUpdates();
    });
    rebuild();
    readUpdates();
}

void TeamPage::send(const QString& type, QJsonObject payload, QLabel* error) {
    payload.insert(QStringLiteral("team_id"), teamId_);
    const QPointer<QLabel> label(error);
    teams_->request(type, payload, [label](const QJsonObject&, const QString& failure) {
        if (label) {
            label->setText(failure);
            label->setVisible(!failure.isEmpty());
        }
    });
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
                            if (updates == alive->updates_)
                                return;
                            alive->updates_ = updates;
                            alive->rebuild();
                        });
    });
}

void TeamPage::rebuild() {
    const int position = scroll_->verticalScrollBar()->value();
    content_ = new QWidget;
    content_->setObjectName(QStringLiteral("teamPageContent"));
    layout_ = new QVBoxLayout(content_);
    layout_->setContentsMargins(24, 20, 24, 24);
    layout_->setSpacing(12);
    tasks_ = nullptr;
    const auto team = teams_->team(teamId_);
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
}

void TeamPage::buildHeading(const QJsonObject& team) {
    auto* row = new QWidget(content_);
    auto* line = new QHBoxLayout(row);
    line->setContentsMargins(0, 0, 0, 0);
    auto* title = heading(row, team.value(QStringLiteral("name")).toString(), "teamPageName");
    QFont big = title->font();
    // The kit sets fonts in pixels, so scale whichever size the font carries.
    if (big.pixelSize() > 0)
        big.setPixelSize(qRound(big.pixelSize() * 1.4));
    else
        big.setPointSizeF(big.pointSizeF() * 1.4);
    title->setFont(big);
    line->addWidget(title, 1);
    auto* add = new ket::Button(tr("Add member"), ket::ButtonVariant::Secondary, row);
    add->setObjectName(QStringLiteral("teamPageAddMember"));
    const bool full =
        TeamDirectory::count(team, QStringLiteral("developer")) >= team_roles::maximumDevelopers;
    add->setEnabled(!full);
    add->setToolTip(full ? tr("A team has at most five developers.") : QString{});
    connect(add, &QAbstractButton::clicked, this, [this] { emit addMemberRequested(teamId_); });
    line->addWidget(add);
    auto* more = new ket::IconButton(ket::Icons::icon(u"more"), tr("Team actions"), row);
    more->setObjectName(QStringLiteral("teamPageMenu"));
    connect(more, &QAbstractButton::clicked, this,
            [this, more] { showMenu(more, *teams_->team(teamId_)); });
    line->addWidget(more);
    layout_->addWidget(row);
    if (const QString goal = team.value(QStringLiteral("goal")).toString(); !goal.isEmpty())
        layout_->addWidget(text(content_, goal, "teamPageGoal"));
    const QJsonArray tasks = team.value(QStringLiteral("tasks")).toArray();
    const int done = static_cast<int>(std::count_if(tasks.begin(), tasks.end(), [](auto value) {
        return value.toObject().value(QStringLiteral("state")).toString() == QStringLiteral("done");
    }));
    const QString branch = team.value(QStringLiteral("integration_branch")).toString();
    QStringList facts{QFileInfo(team.value(QStringLiteral("repository")).toString()).fileName(),
                      tr("into %1").arg(branch.isEmpty() ? tr("the current branch") : branch),
                      team.value(QStringLiteral("merge_policy")).toString() ==
                              QStringLiteral("user")
                          ? tr("you merge tasks")
                          : tr("the leader merges tasks"),
                      tr("%1 of %2 tasks done").arg(done).arg(tasks.size())};
    layout_->addWidget(text(content_, facts.join(QStringLiteral(" · ")), "teamPageFacts", true));
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
    menu.addSeparator();
    auto* remove = menu.addAction(ket::Icons::icon(u"trash"), tr("Delete team…"));
    remove->setProperty(ket::Style::kDangerProperty, true);
    QAction* chosen = menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
    if (chosen == create)
        afterMenuCloses(this,
                        [this, repository = team.value(QStringLiteral("repository")).toString()] {
                            emit newTeamRequested(repository);
                        });
    else if (chosen == merges)
        send(QStringLiteral("team.update"),
             {{QStringLiteral("merge_policy"),
               userMerges ? QStringLiteral("leader") : QStringLiteral("user")}});
    else if (chosen == pause)
        send(QStringLiteral("team.update"), {{QStringLiteral("paused"), !paused}});
    else if (chosen == remove)
        afterMenuCloses(this, [this, id = teamId_] { emit deleteRequested(id); });
}

void TeamPage::buildBanners(const QJsonObject& team) {
    if (team.value(QStringLiteral("paused")).toBool()) {
        auto* banner = new ket::Banner(
            ket::BannerTone::Warning, tr("Automatic turns are paused"),
            pausedReason(team.value(QStringLiteral("paused_reason")).toString()), content_);
        banner->setObjectName(QStringLiteral("teamPausedBanner"));
        connect(banner->setAction(tr("Resume")), &QAbstractButton::clicked, this, [this] {
            send(QStringLiteral("team.update"), {{QStringLiteral("paused"), false}});
        });
        layout_->addWidget(banner);
    }
    const int developers = TeamDirectory::count(team, QStringLiteral("developer"));
    if (developers >= kQueueBannerDevelopers) {
        auto* banner = new ket::Banner(
            ket::BannerTone::Info, tr("The reviewer is the queue"),
            tr("%1 developers share one reviewer, so finished tasks wait their turn. A second "
               "team gives the next developers their own.")
                .arg(developers),
            content_);
        banner->setObjectName(QStringLiteral("teamQueueBanner"));
        connect(banner->setAction(tr("New team…")), &QAbstractButton::clicked, this,
                [this, repository = team.value(QStringLiteral("repository")).toString()] {
                    emit newTeamRequested(repository);
                });
        layout_->addWidget(banner);
    }
}

void TeamPage::buildDecisions(const QJsonObject& team) {
    QList<QJsonObject> open;
    for (const auto& value : team.value(QStringLiteral("decisions")).toArray())
        if (value.toObject().value(QStringLiteral("state")).toString() == QStringLiteral("open"))
            open.append(value.toObject());
    // Absent when nothing waits.
    if (open.isEmpty())
        return;
    layout_->addWidget(heading(content_, tr("Waiting for you"), "teamWaitingHeading"));
    for (const QJsonObject& decision : std::as_const(open)) {
        const QString id = decision.value(QStringLiteral("id")).toString();
        auto* card = new QFrame(content_);
        card->setObjectName(QStringLiteral("teamDecision_") + id);
        card->setFrameShape(QFrame::StyledPanel);
        card->setAccessibleName(decision.value(QStringLiteral("question")).toString());
        auto* column = new QVBoxLayout(card);
        column->addWidget(heading(card, decision.value(QStringLiteral("question")).toString(),
                                  "teamDecisionQuestion"));
        if (const QString context = decision.value(QStringLiteral("context")).toString();
            !context.isEmpty())
            column->addWidget(text(card, context, "teamDecisionContext"));
        const QString raisedBy = decision.value(QStringLiteral("raised_by")).toString();
        QStringList about{
            tr("Raised by %1")
                .arg(raisedBy == QStringLiteral("daemon") ? tr("KetPlus")
                                                          : memberName(team, raisedBy)),
            QDateTime::fromString(decision.value(QStringLiteral("raised_at")).toString(),
                                  Qt::ISODateWithMs)
                .toLocalTime()
                .toString(QStringLiteral("d MMM, HH:mm"))};
        if (const QString task = decision.value(QStringLiteral("task_id")).toString();
            !task.isEmpty())
            about.append(tr("blocks %1").arg(task));
        column->addWidget(text(card, about.join(QStringLiteral(" · ")), "teamDecisionAbout", true));
        auto* choices = new QWidget(card);
        auto* row = new QHBoxLayout(choices);
        row->setContentsMargins(0, 0, 0, 0);
        QStringList ordered;
        for (const auto& value : decision.value(QStringLiteral("choices")).toArray())
            ordered.append(value.toString());
        const QString recommended = decision.value(QStringLiteral("recommended")).toString();
        if (ordered.removeAll(recommended) > 0)
            ordered.prepend(recommended);
        auto* error = text(card, {}, "teamDecisionError");
        error->setVisible(false);
        for (const QString& choice : std::as_const(ordered)) {
            const bool best = choice == recommended;
            auto* button = new ket::Button(
                best ? tr("%1 (recommended)").arg(choice) : choice,
                best ? ket::ButtonVariant::Primary : ket::ButtonVariant::Secondary, choices);
            button->setObjectName(QStringLiteral("teamDecisionChoice"));
            button->setProperty("choice", choice);
            connect(button, &QAbstractButton::clicked, this, [this, id, choice, error] {
                send(QStringLiteral("team.decision.answer"),
                     {{QStringLiteral("decision_id"), id}, {QStringLiteral("choice"), choice}},
                     error);
            });
            row->addWidget(button);
        }
        auto* elsewhere =
            new ket::Button(tr("Answer in the conversation"), ket::ButtonVariant::Ghost, choices);
        elsewhere->setObjectName(QStringLiteral("teamDecisionInConversation"));
        connect(elsewhere, &QAbstractButton::clicked, this,
                [this, leader = leaderOf(team)] { emit conversationRequested(leader); });
        row->addWidget(elsewhere);
        row->addStretch();
        column->addWidget(choices);
        column->addWidget(error);
        layout_->addWidget(card);
    }
}

void TeamPage::buildTasks(const QJsonObject& team) {
    layout_->addWidget(heading(content_, tr("Tasks"), "teamTasksHeading"));
    QStringList labels{tr("All")};
    for (const QString& state : team_states::all())
        labels.append(team_states::label(state));
    auto* filter = new ket::SegmentedControl(labels, content_);
    filter->setObjectName(QStringLiteral("teamTaskFilter"));
    filter->setAccessibleName(tr("Show tasks"));
    filter->setCurrentIndex(filter_.isEmpty() ? 0 : team_states::all().indexOf(filter_) + 1);
    connect(filter, &ket::SegmentedControl::currentChanged, this, [this](const int index) {
        filter_ = index <= 0 ? QString{} : team_states::all().value(index - 1);
        fillTasks();
    });
    layout_->addWidget(filter, 0, Qt::AlignLeft);
    tasks_ = new QTableWidget(content_);
    tasks_->setObjectName(QStringLiteral("teamTasksTable"));
    tasks_->setColumnCount(7);
    tasks_->setHorizontalHeaderLabels({tr("Task"), tr("Title"), tr("Criteria"), tr("Owner"),
                                       tr("Lane"), tr("State"), tr("Pull request")});
    tasks_->verticalHeader()->hide();
    tasks_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    tasks_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tasks_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tasks_->setSelectionMode(QAbstractItemView::SingleSelection);
    tasks_->setProperty("team", team);
    connect(tasks_, &QTableWidget::cellActivated, this,
            [this](const int row, int) { openTask(tasks_->item(row, 0)->text()); });
    fillTasks();
    layout_->addWidget(tasks_);
}

void TeamPage::fillTasks() {
    if (tasks_ == nullptr)
        return;
    const QJsonObject team = tasks_->property("team").toJsonObject();
    QList<QJsonObject> shown;
    for (const auto& value : team.value(QStringLiteral("tasks")).toArray())
        if (filter_.isEmpty() ||
            value.toObject().value(QStringLiteral("state")).toString() == filter_)
            shown.append(value.toObject());
    tasks_->setRowCount(static_cast<int>(shown.size()));
    for (int row = 0; row < shown.size(); ++row) {
        const QJsonObject& task = shown.at(row);
        const int criteria =
            static_cast<int>(task.value(QStringLiteral("criteria")).toArray().size());
        QString state = team_states::label(task.value(QStringLiteral("state")).toString());
        // #461: a flag, not a state.
        if (!task.value(QStringLiteral("outside_lane")).toArray().isEmpty())
            state += tr(" · Outside its lane");
        const QStringList cells{task.value(QStringLiteral("id")).toString(),
                                task.value(QStringLiteral("title")).toString(),
                                criteria == 0
                                    ? QStringLiteral("—")
                                    : QStringLiteral("%1/%2")
                                          .arg(task.value(QStringLiteral("criteria_met")).toInt())
                                          .arg(criteria),
                                memberName(team, task.value(QStringLiteral("owner")).toString()),
                                task.value(QStringLiteral("lane")).toString(),
                                state,
                                task.value(QStringLiteral("pull_request")).toString()};
        for (int column = 0; column < cells.size(); ++column)
            tasks_->setItem(row, column, new QTableWidgetItem(cells.at(column)));
    }
    tasks_->setFixedHeight(tasks_->horizontalHeader()->height() + 4 +
                           qMax(1, tasks_->rowCount()) *
                               tasks_->verticalHeader()->defaultSectionSize());
}

void TeamPage::setTaskFilter(const QString& state) {
    filter_ = state;
    if (auto* filter = content_->findChild<ket::SegmentedControl*>("teamTaskFilter"))
        filter->setCurrentIndex(state.isEmpty() ? 0 : team_states::all().indexOf(state) + 1);
    fillTasks();
}

ket::Dialog* TeamPage::openTask(const QString& taskId) {
    return TeamTaskDialog::present(this, teams_, teamId_, taskId);
}

void TeamPage::buildMembers(const QJsonObject& team) {
    layout_->addWidget(heading(content_, tr("Members"), "teamMembersHeading"));
    for (const auto& value : team.value(QStringLiteral("members")).toArray()) {
        const QJsonObject member = value.toObject();
        const QString session = member.value(QStringLiteral("session_id")).toString();
        const QString role = member.value(QStringLiteral("role")).toString();
        auto* row = new QWidget(content_);
        row->setObjectName(QStringLiteral("teamMember_") + session);
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto* tag = text(row, team_roles::label(role), "teamMemberRole");
        tag->setStyleSheet(
            QStringLiteral("color: %1; font-weight: 600;").arg(team_roles::color(role).name()));
        line->addWidget(tag);
        QStringList facts{memberName(team, session)};
        if (const QString provider = member.value(QStringLiteral("provider")).toString();
            !provider.isEmpty())
            facts.append(provider);
        if (role == QStringLiteral("developer")) {
            facts.append(member.value(QStringLiteral("lane")).toString());
            QStringList paths;
            for (const auto& path : member.value(QStringLiteral("paths")).toArray())
                paths.append(path.toString());
            if (!paths.isEmpty())
                facts.append(paths.join(QStringLiteral(", ")));
        }
        QString doing = tr("Idle");
        for (const auto& task : team.value(QStringLiteral("tasks")).toArray())
            if (task.toObject().value(QStringLiteral("owner")).toString() == session &&
                task.toObject().value(QStringLiteral("state")).toString() ==
                    QStringLiteral("in_progress"))
                doing = tr("On %1").arg(task.toObject().value(QStringLiteral("id")).toString());
        if (role == QStringLiteral("reviewer") &&
            !team.value(QStringLiteral("reviewing")).toString().isEmpty())
            doing = tr("Reviewing %1").arg(team.value(QStringLiteral("reviewing")).toString());
        facts.append(doing);
        line->addWidget(text(row, facts.join(QStringLiteral(" · ")), "teamMemberFacts"), 1);
        auto* open = new ket::Button(tr("Open"), ket::ButtonVariant::Ghost, row);
        open->setObjectName(QStringLiteral("teamMemberOpen"));
        open->setAccessibleName(tr("Open %1").arg(memberName(team, session)));
        connect(open, &QAbstractButton::clicked, this,
                [this, session] { emit conversationRequested(session); });
        line->addWidget(open);
        if (role == QStringLiteral("developer")) {
            auto* remove = new ket::Button(tr("Remove…"), ket::ButtonVariant::GhostDanger, row);
            remove->setObjectName(QStringLiteral("teamMemberRemove"));
            const QString refusal = leaveRefusal(team, session);
            remove->setEnabled(refusal.isEmpty());
            remove->setToolTip(refusal);
            connect(remove, &QAbstractButton::clicked, this,
                    [this, session] { emit removeMemberRequested(teamId_, session); });
            line->addWidget(remove);
        }
        layout_->addWidget(row);
    }
}

void TeamPage::buildUpdates() {
    layout_->addWidget(heading(content_, tr("Updates"), "teamUpdatesHeading"));
    QList<QJsonObject> updates;
    for (const auto& value : updates_)
        updates.append(value.toObject());
    std::sort(updates.begin(), updates.end(),
              [](const QJsonObject& left, const QJsonObject& right) {
                  return left.value(QStringLiteral("id")).toInteger() >
                         right.value(QStringLiteral("id")).toInteger();
              });
    if (updates.isEmpty())
        layout_->addWidget(text(content_, tr("Nothing yet."), "teamUpdatesEmpty", true));
    for (qsizetype index = 0; index < updates.size() && index < kUpdatesShown; ++index) {
        const QJsonObject& update = updates.at(index);
        const QString at =
            QDateTime::fromString(update.value(QStringLiteral("at")).toString(), Qt::ISODateWithMs)
                .toLocalTime()
                .toString(QStringLiteral("HH:mm"));
        layout_->addWidget(
            text(content_,
                 QStringLiteral("%1  %2").arg(at, update.value(QStringLiteral("text")).toString()),
                 "teamUpdate"));
    }
}

void TeamPage::buildEmpty() {
    auto* empty = new ket::EmptyState(
        QStringLiteral("users"), tr("No team here yet"),
        tr("A team is a leader, developers and a reviewer working toward one goal, each in its "
           "own worktree. The leader plans the tasks; the team asks you only when it needs a "
           "decision."),
        content_);
    empty->setObjectName(QStringLiteral("teamPageEmpty"));
    connect(empty->setAction(tr("New team…"), QStringLiteral("users")), &QAbstractButton::clicked,
            this, [this] { emit newTeamRequested(repository_); });
    layout_->addWidget(empty);
}

} // namespace ketplus
