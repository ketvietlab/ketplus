#include "KetAppearance.h"
#include "KetBadge.h"
#include "KetDialog.h"
#include "KetIcon.h"
#include "KetSegmentedControl.h"
#include "KetSelect.h"
#include "KetTokens.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamMenus.h"
#include "teams/TeamPage.h"
#include "teams/TeamPageParts.h"
#include "teams/TeamTaskDialog.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QTableWidget>
#include <QVBoxLayout>

namespace ketplus {
namespace {

using team_page::px;
using team_page::Text;

ket::BadgeTone stateTone(const QString& state) {
    if (state == QStringLiteral("in_progress"))
        return ket::BadgeTone::Accent;
    if (state == QStringLiteral("in_review"))
        return ket::BadgeTone::Info;
    if (state == QStringLiteral("blocked"))
        return ket::BadgeTone::Warning;
    if (state == QStringLiteral("ready") || state == QStringLiteral("done"))
        return ket::BadgeTone::Success;
    return ket::BadgeTone::Neutral;
}

QString hex(const QStringView token) {
    return ket::Tokens::color(token, ket::Appearance::instance().theme()).name(QColor::HexArgb);
}

} // namespace

void TeamPage::buildTasks(const QJsonObject& team) {
    const QJsonArray tasks = team.value(QStringLiteral("tasks")).toArray();
    const auto counted = [&tasks](const QString& state) {
        return std::count_if(tasks.begin(), tasks.end(), [&state](const auto& value) {
            return value.toObject().value(QStringLiteral("state")).toString() == state;
        });
    };
    QStringList labels{tr("All %1").arg(tasks.size())};
    for (const QString& state : team_states::all())
        labels.append(QStringLiteral("%1 %2").arg(team_states::label(state)).arg(counted(state)));
    const int current = filter_.isEmpty() ? 0 : team_states::all().indexOf(filter_) + 1;
    const auto choose = [this](const int index) {
        filter_ = index <= 0 ? QString{} : team_states::all().value(index - 1);
        fillTasks();
    };
    // Segments beside the title while they fit; the same choices as a select when they do not.
    QWidget* filter = nullptr;
    auto* segments = new ket::SegmentedControl(labels);
    segmentedWidth_ = segments->sizeHint().width();
    segmented_ = filterFits();
    if (segmented_) {
        segments->setCurrentIndex(current);
        connect(segments, &ket::SegmentedControl::currentChanged, this, choose);
        filter = segments;
    } else {
        delete segments;
        auto* select = new ket::Select;
        select->addItems(labels);
        select->setCurrentIndex(current);
        connect(select, &QComboBox::activated, this, choose);
        filter = select;
    }
    filter->setObjectName(QStringLiteral("teamTaskFilter"));
    filter->setAccessibleName(tr("Show tasks"));
    filter->setVisible(!tasks.isEmpty());
    addSection(tr("Tasks"),
               tasks.isEmpty()
                   ? QString{}
                   : tr("%1 of %2 done").arg(counted(QStringLiteral("done"))).arg(tasks.size()),
               "teamTasksHeading", filter);

    auto* panel = new team_page::Panel(column_);
    panel->setObjectName(QStringLiteral("teamTasksPanel"));
    auto* box = new QVBoxLayout(panel);
    box->setContentsMargins(1, 1, 1, 1);
    box->setSpacing(0);
    tasks_ = new QTableWidget(panel);
    tasks_->setObjectName(QStringLiteral("teamTasksTable"));
    tasks_->setAccessibleName(tr("Tasks"));
    tasks_->setColumnCount(7);
    tasks_->setHorizontalHeaderLabels(
        {tr("Task"), tr("Title"), tr("Criteria"), tr("Owner"), tr("Lane"), tr("State"), tr("PR")});
    tasks_->setFrameShape(QFrame::NoFrame);
    tasks_->setShowGrid(false);
    tasks_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tasks_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tasks_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tasks_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tasks_->setSelectionMode(QAbstractItemView::SingleSelection);
    tasks_->setTabKeyNavigation(false);
    tasks_->verticalHeader()->hide();
    tasks_->verticalHeader()->setDefaultSectionSize(px(48));
    QHeaderView* header = tasks_->horizontalHeader();
    header->setMinimumSectionSize(px(40));
    header->setSectionResizeMode(QHeaderView::Fixed);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    header->setHighlightSections(false);
    header->setSectionsClickable(false);
    header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    header->setFixedHeight(px(34));
    tasks_->setColumnWidth(0, px(72));
    tasks_->setColumnWidth(2, px(72));
    tasks_->setColumnWidth(3, px(140));
    tasks_->setColumnWidth(4, px(140));
    tasks_->setColumnWidth(6, px(96));
    tasks_->setStyleSheet(
        QStringLiteral(
            "QTableWidget { background: transparent; border: none; }"
            "QHeaderView { background: transparent; border: none; }"
            "QHeaderView::section { background: transparent; border: none;"
            " border-bottom: 1px solid %1; color: %2; font-size: %3px; padding: 0 0 0 %4px; }"
            "QHeaderView::section:first { padding-left: %5px; }")
            .arg(hex(u"panel-border"), hex(u"text-muted"))
            .arg(px(12))
            .arg(px(8))
            .arg(px(18)));
    tasks_->setProperty("team", team);
    taskRows_ = new team_page::TaskRows(tasks_);
    connect(taskRows_, &team_page::TaskRows::chosen, this,
            [this](const int row) { openTask(tasks_->item(row, 0)->text()); });
    box->addWidget(tasks_);
    taskHint_ = new Text({}, panel);
    taskHint_->setObjectName(QStringLiteral("teamTasksEmpty"));
    taskHint_->setTone(QStringLiteral("text-secondary"));
    taskHint_->setContentsMargins(px(18), px(14), px(18), px(16));
    box->addWidget(taskHint_);
    fillTasks();
    layout_->addWidget(panel);
}

void TeamPage::fillTasks() {
    if (tasks_ == nullptr)
        return;
    const QJsonObject team = tasks_->property("team").toJsonObject();
    const QJsonArray all = team.value(QStringLiteral("tasks")).toArray();
    QList<QJsonObject> shown;
    for (const auto& value : all)
        if (filter_.isEmpty() ||
            value.toObject().value(QStringLiteral("state")).toString() == filter_)
            shown.append(value.toObject());
    tasks_->setRowCount(0); // also drops the badges of the rows before
    tasks_->setRowCount(static_cast<int>(shown.size()));
    int stateWidth = px(96);
    for (int row = 0; row < shown.size(); ++row) {
        const QJsonObject& task = shown.at(row);
        const int criteria =
            static_cast<int>(task.value(QStringLiteral("criteria")).toArray().size());
        const QString stateId = task.value(QStringLiteral("state")).toString();
        // #461: a flag, not a state.
        const bool outside = !task.value(QStringLiteral("outside_lane")).toArray().isEmpty();
        QString state = team_states::label(stateId);
        if (outside)
            state += tr(" · Outside its lane");
        const QString pullRequest = task.value(QStringLiteral("pull_request")).toString();
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
                                pullRequest.isEmpty() ? QStringLiteral("—") : pullRequest};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            // A cell elides; its whole value stays one hover away.
            item->setToolTip(cells.at(column));
            tasks_->setItem(row, column, item);
        }
        // The state as the kit's badge, over the cell whose text the delegate leaves out. Not
        // colour alone: the flag carries an icon and its words.
        auto* cell = new QWidget(tasks_);
        cell->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* line = new QHBoxLayout(cell);
        line->setContentsMargins(px(8), 0, px(8), 0);
        line->setSpacing(px(6));
        line->addWidget(new ket::Badge(team_states::label(stateId), stateTone(stateId), cell));
        if (outside) {
            auto* glyph = new ket::Icon(QStringLiteral("warning"), 13, cell);
            glyph->setColor(ket::Tokens::color(u"warning", ket::Appearance::instance().theme()));
            line->addWidget(glyph);
            auto* flag = new Text(tr("Outside its lane"), cell, Text::Fit::Elide);
            flag->setTone(QStringLiteral("warning"));
            flag->setType(12, QFont::Medium);
            line->addWidget(flag);
        }
        line->addStretch();
        stateWidth = qMax(stateWidth, cell->sizeHint().width());
        tasks_->setCellWidget(row, team_page::TaskRows::kStateColumn, cell);
    }
    tasks_->setColumnWidth(team_page::TaskRows::kStateColumn, stateWidth);
    tasks_->setFixedHeight(tasks_->horizontalHeader()->height() +
                           tasks_->rowCount() * tasks_->verticalHeader()->defaultSectionSize());
    taskHint_->setText(
        all.isEmpty() ? tr("No task yet. The leader breaks the goal into tasks, writes what done "
                           "means for each, then assigns them.")
                      : tr("No task is %1.").arg(team_states::label(filter_).toLower()));
    taskHint_->setVisible(shown.isEmpty());
    taskRows_->fit();
}

void TeamPage::setTaskFilter(const QString& state) {
    filter_ = state;
    const int index = state.isEmpty() ? 0 : team_states::all().indexOf(state) + 1;
    if (auto* segments = column_->findChild<ket::SegmentedControl*>("teamTaskFilter"))
        segments->setCurrentIndex(index);
    if (auto* select = column_->findChild<ket::Select*>("teamTaskFilter"))
        select->setCurrentIndex(index);
    fillTasks();
}

ket::Dialog* TeamPage::openTask(const QString& taskId) {
    auto* dialog = TeamTaskDialog::present(this, teams_, teamId_, taskId);
    if (auto* body = dialog->findChild<TeamTaskDialog*>())
        connect(body, &TeamTaskDialog::ownerRequested, this, &TeamPage::conversationRequested);
    return dialog;
}

} // namespace ketplus
