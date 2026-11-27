// T-6: the page follows its team section by section. A team changes with every turn a member
// starts or ends; building the whole page again each time kept the interface busy while the
// daemon's frames were read. Each section lives in a box of its own and is built again only when
// what it shows of the team changed; the others keep their widgets, scroll position and focus.
#include "TeamLanguage.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPage.h"
#include "teams/TeamPageParts.h"

#include <QJsonArray>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>
#include <QVBoxLayout>

#include <initializer_list>

namespace ketplus {
namespace team_language = remote::team_language;
namespace {

// Without a '_': focusPlace() takes a name with one for a row that scopes the focus.
constexpr std::array<const char*, TeamPage::kSections> kBoxNames{
    "teamSectionHeading",       "teamSectionBanners", "teamSectionDecisions", "teamSectionTasks",
    "teamSectionDoneWorktrees", "teamSectionMembers", "teamSectionUpdates"};

// The fields `keys` of each object in `rows`.
QJsonArray pick(const QJsonArray& rows, const std::initializer_list<QLatin1StringView> keys) {
    QJsonArray picked;
    for (const auto& value : rows) {
        const QJsonObject row = value.toObject();
        QJsonObject kept;
        for (const QLatin1StringView key : keys)
            if (row.contains(key))
                kept.insert(key, row.value(key));
        picked.append(kept);
    }
    return picked;
}

QJsonArray members(const QJsonObject& team, const std::initializer_list<QLatin1StringView> keys) {
    return pick(team.value(QLatin1StringView("members")).toArray(), keys);
}

QJsonArray tasks(const QJsonObject& team, const std::initializer_list<QLatin1StringView> keys) {
    return pick(team.value(QLatin1StringView("tasks")).toArray(), keys);
}

// The members as a row names them (TeamPage::memberName).
QJsonArray names(const QJsonObject& team) {
    return members(team, {QLatin1StringView("session_id"), QLatin1StringView("name"),
                          QLatin1StringView("number")});
}

} // namespace

QJsonValue TeamPage::sectionInput(const Section section, const QJsonObject& team) {
    using L = QLatin1StringView;
    switch (section) {
    case Section::Heading: {
        // The team's own settings and facts. What only another section shows, and what changes
        // with every turn without being shown, is left out; a field not named here counts.
        QJsonObject heading = team;
        for (const L key : {L("tasks"),
                            L("decisions"),
                            L("decisions_total"),
                            L("open_decisions"),
                            L("members"),
                            L("reviews"),
                            L("reviewing"),
                            L("reviewing_message"),
                            L("review_queue"),
                            L("review_waiting"),
                            L("relocations"),
                            L("held"),
                            L("outbox"),
                            L("next_task"),
                            L("next_decision"),
                            L("tasks_done"),
                            L("revision"),
                            L("settings_revision"),
                            L("members_revision"),
                            L("changed_at"),
                            L("last_routing_decision"),
                            L("directory"),
                            L("tasks_total"),
                            L("shared_paths")})
            heading.remove(key);
        // How far the team is, and whether it has room for another member.
        heading.insert(L("tasks"), tasks(team, {L("state")}));
        heading.insert(L("members"), members(team, {L("role")}));
        return heading;
    }
    case Section::Banners:
        return QJsonObject{{L("paused"), team.value(L("paused"))},
                           {L("paused_reason"), team.value(L("paused_reason"))},
                           {L("members"), members(team, {L("role")})}};
    case Section::Decisions: {
        QJsonArray open;
        for (const auto& value : team.value(L("decisions")).toArray())
            if (value.toObject().value(L("state")).toString() == L("open"))
                open.append(value);
        return QJsonObject{{L("decisions"), open}, {L("members"), names(team)}};
    }
    case Section::Tasks:
        return QJsonObject{
            {L("tasks"),
             tasks(team, {L("id"), L("title"), L("criteria"), L("criteria_met"), L("owner"),
                          L("lane"), L("state"), L("outside_lane"), L("pull_request")})},
            {L("members"), names(team)}};
    case Section::DoneWorktrees:
        return QJsonObject{{L("tasks"), tasks(team, {L("id"), L("title")})}};
    case Section::Members:
        return QJsonObject{{L("members"), team.value(L("members"))},
                           {L("tasks"), tasks(team, {L("id"), L("owner"), L("state")})},
                           {L("reviews"), team.value(L("reviews"))},
                           {L("reviewing"), team.value(L("reviewing"))},
                           {L("shared_paths"), team.value(L("shared_paths"))}};
    case Section::Updates:
        return QJsonObject{{L("members"), names(team)}};
    }
    return {};
}

void TeamPage::buildSection(const Section section, const QJsonObject& team, const int at) {
    const int index = static_cast<int>(section);
    auto* column = static_cast<QVBoxLayout*>(column_->layout());
    auto* box = new QWidget(column_);
    box->setObjectName(QLatin1String(kBoxNames.at(index)));
    auto* lines = new QVBoxLayout(box);
    lines->setContentsMargins(0, 0, 0, 0);
    lines->setSpacing(column->spacing());
    // The build functions add to layout_: here, the section's own box.
    layout_ = lines;
    switch (section) {
    case Section::Heading:
        buildHeading(team);
        break;
    case Section::Banners:
        buildBanners(team);
        break;
    case Section::Decisions:
        buildDecisions(team);
        break;
    case Section::Tasks:
        tasks_ = nullptr;
        taskRows_ = nullptr;
        taskHint_ = nullptr;
        buildTasks(team);
        break;
    case Section::DoneWorktrees:
        buildDoneWorktrees(team);
        break;
    case Section::Members:
        buildMembers(team);
        break;
    case Section::Updates:
        buildUpdates();
        break;
    }
    layout_ = column;
    column->insertWidget(at, box);
    // An empty section takes no room, nor the spacing around it.
    box->setVisible(!lines->isEmpty());
    sections_.at(index) = box;
    inputs_.at(index) = sectionInput(section, team);
    ++builds_.at(index);
}

void TeamPage::followTeam() {
    const auto team = teams_->team(teamId_);
    const QJsonObject now = team.value_or(QJsonObject{});
    // team.get answers with the Updates; only a change of this team can add one.
    if (now != readFor_)
        readUpdates();
    if (now == shown_ && content_)
        return;
    // The page without a team, a team appearing or going, or another language: all of it.
    if (!team || !sections_.front() ||
        now.value(QStringLiteral("language")) != shown_.value(QStringLiteral("language"))) {
        rebuild();
        return;
    }
    QList<Section> changed;
    for (int section = 0; section < kSections; ++section)
        if (sectionInput(static_cast<Section>(section), now) != inputs_.at(section))
            changed.append(static_cast<Section>(section));
    shown_ = now;
    rebuildSections(changed);
}

void TeamPage::rebuildSections(const QList<Section>& sections) {
    const auto team = teams_->team(teamId_);
    if (sections.isEmpty() || !team || !sections_.front())
        return;
    // A word mid-composition lives only in its field: rebuild() waits for it (composingDraft).
    if (sections.contains(Section::Decisions) && composingDraft()) {
        rebuild();
        return;
    }
    const team_language::Scope speaking(teamLanguage());
    const int position = scroll_->verticalScrollBar()->value();
    // The focus comes back only when its widget is rebuilt; elsewhere it stays where it is.
    QWidget* focused = window()->focusWidget();
    bool focusRebuilt = false;
    for (const Section section : sections)
        if (QWidget* box = sections_.at(static_cast<int>(section));
            focused && box && box->isAncestorOf(focused))
            focusRebuilt = true;
    const FocusPlace focus = focusRebuilt ? focusPlace() : FocusPlace{};
    const QString currentTask = sections.contains(Section::Tasks) && tasks_ &&
                                        tasks_->currentRow() >= 0 &&
                                        tasks_->item(tasks_->currentRow(), 0)
                                    ? tasks_->item(tasks_->currentRow(), 0)->text()
                                    : QString{};
    auto* column = static_cast<QVBoxLayout*>(column_->layout());
    for (const Section section : sections) {
        QWidget* old = sections_.at(static_cast<int>(section));
        const int at = column->indexOf(old);
        column->removeWidget(old);
        delete old;
        buildSection(section, *team, at);
    }
    content_->adjustSize();
    scroll_->verticalScrollBar()->setValue(position);
    if (tasks_ && !currentTask.isEmpty())
        for (int row = 0; row < tasks_->rowCount(); ++row)
            if (tasks_->item(row, 0)->text() == currentTask) {
                tasks_->setCurrentCell(row, 0);
                break;
            }
    if (focusRebuilt)
        restoreFocus(focus);
}

} // namespace ketplus
