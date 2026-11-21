#include "schedules/ScheduleWindow.h"

#include "schedules/ScheduleClient.h"
#include "schedules/ScheduleRules.h"

#include "KetAppearance.h"
#include "KetBadge.h"
#include "KetButton.h"
#include "KetEmptyState.h"
#include "KetIcon.h"
#include "KetSelect.h"
#include "KetTextArea.h"
#include "KetTextField.h"
#include "KetToggle.h"
#include "KetTokens.h"

#include <algorithm>

#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QPainter>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace ketplus {
namespace {

int scaled(const qreal logical) { return qRound(ket::Appearance::instance().scaled(logical)); }

class ScheduleRow final : public QWidget {
  public:
    ket::Checkbox* enabled{nullptr};

    ScheduleRow(const QJsonObject& schedule, const QJsonObject& latest, const bool selected,
                QWidget* parent)
        : QWidget(parent), selected_(selected) {
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(scaled(12), scaled(8), scaled(12), scaled(8));
        row->setSpacing(scaled(8));
        const QJsonObject target = schedule.value(QStringLiteral("target")).toObject();
        const QString kind = target.value(QStringLiteral("kind")).toString();
        const QString iconName =
            kind == QLatin1String("team") || kind == QLatin1String("team_start")
                ? QStringLiteral("users")
                : QStringLiteral("chat");
        auto* text = new QVBoxLayout;
        text->setSpacing(scaled(4));
        auto* name = new QLabel(schedule.value(QStringLiteral("name")).toString(), this);
        QFont nameFont = name->font();
        nameFont.setWeight(QFont::DemiBold);
        name->setFont(nameFont);
        name->setWordWrap(true);
        auto* targetLine = new QHBoxLayout;
        targetLine->setSpacing(scaled(6));
        targetLine->addWidget(new ket::Icon(iconName, 14, this));
        auto* targetLabel = new QLabel(
            scheduleTargetLabel(target, schedule.value(QStringLiteral("created_by")).toString()),
            this);
        targetLabel->setWordWrap(true);
        targetLine->addWidget(targetLabel, 1);
        auto* rule = new QLabel(
            scheduleRuleSummary(schedule.value(QStringLiteral("rule")).toString()), this);
        rule->setObjectName(QStringLiteral("scheduleRowRule"));
        rule->setWordWrap(true);
        const bool on = schedule.value(QStringLiteral("enabled")).toBool();
        const QString nextAt = schedule.value(QStringLiteral("next_run_at")).toString();
        const auto parsed = scheduleRuleFromText(schedule.value(QStringLiteral("rule")).toString());
        const QString next =
            !on ? tr("Paused")
            : nextAt.isEmpty()
                ? tr("Not scheduled")
                : tr("Next: %1")
                      .arg(scheduleWhenLabel(nextAt, parsed ? parsed->timezone : QString{}));
        auto* times =
            new QLabel(next + QStringLiteral(" · ") + scheduleLastLabel(schedule, latest), this);
        times->setObjectName(QStringLiteral("scheduleRowTimes"));
        times->setWordWrap(true);
        text->addWidget(name);
        text->addLayout(targetLine);
        text->addWidget(rule);
        text->addWidget(times);
        enabled = new ket::Checkbox({}, this);
        enabled->setFixedSize(scaled(18), scaled(18));
        enabled->setChecked(on);
        row->addLayout(text, 1);
        row->addWidget(enabled, 0, Qt::AlignTop);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const auto theme = ket::Appearance::instance().theme();
        if (selected_)
            painter.fillRect(rect(), ket::Tokens::color(u"nav-selected", theme));
        painter.setPen(ket::Tokens::color(u"panel-border", theme));
        painter.drawLine(0, height() - 1, width() - 1, height() - 1);
    }

  private:
    bool selected_{false};
};

void clearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

} // namespace

bool ScheduleWindow::matches(const QJsonObject& schedule) const {
    const bool enabled = schedule.value(QStringLiteral("enabled")).toBool();
    if (filter_->currentIndex() == 1 && !enabled)
        return false;
    if (filter_->currentIndex() == 2 && enabled)
        return false;
    if (filter_->currentIndex() == 3 &&
        schedule.value(QStringLiteral("created_by")).toString() == QLatin1String("user"))
        return false;
    const QString query = search_->text().trimmed();
    if (query.isEmpty())
        return true;
    const QString haystack =
        (schedule.value(QStringLiteral("name")).toString() + QLatin1Char(' ') +
         schedule.value(QStringLiteral("prompt")).toString() + QLatin1Char(' ') +
         scheduleTargetLabel(schedule.value(QStringLiteral("target")).toObject(),
                             schedule.value(QStringLiteral("created_by")).toString()) +
         QLatin1Char(' ') + scheduleRuleSummary(schedule.value(QStringLiteral("rule")).toString()))
            .toLower();
    return haystack.contains(query.toLower());
}

void ScheduleWindow::reload() {
    refreshBanner();
    const QList<schedule::Schedule> all = client_->schedules();
    count_->setText(QString::number(all.size()));
    if (!client_->isAvailable() || all.isEmpty() ||
        std::none_of(all.cbegin(), all.cend(), [this](const schedule::Schedule& schedule) {
            return matches(schedule::toJson(schedule));
        })) {
        list_->clear();
        const QString listError = client_->listError();
        if (!client_->isAvailable()) {
            empty_->setTitle(tr("Schedules"));
            empty_->setMessage(tr("Schedules need a KetPlus Daemon that supports them."));
            empty_->action()->hide();
        } else if (all.isEmpty()) {
            empty_->setTitle(listError.isEmpty() ? tr("No schedules yet")
                                                 : tr("Schedules are unavailable"));
            empty_->setMessage(listError.isEmpty()
                                   ? tr("Create schedules from New conversation or New team.")
                                   : listError);
            empty_->action()->hide();
        } else {
            empty_->setTitle(tr("No schedules found"));
            empty_->setMessage(tr("Try a different name, or show every schedule."));
            empty_->action()->setText(tr("Clear filters"));
            empty_->action()->show();
        }
        stack_->setCurrentWidget(empty_);
        reloadDetail();
        return;
    }
    if (selectedId_.isEmpty() || !client_->schedule(selectedId_)) {
        for (const schedule::Schedule& schedule : all) {
            const QJsonObject document = schedule::toJson(schedule);
            if (!matches(document))
                continue;
            selectedId_ = document.value(QStringLiteral("id")).toString();
            break;
        }
    }
    stack_->setCurrentIndex(1);
    reloadList();
    reloadDetail();
    if (!selectedId_.isEmpty() && !client_->runsLoaded(selectedId_))
        client_->loadRuns(selectedId_, false);
}

void ScheduleWindow::reloadList() {
    rebuilding_ = true;
    list_->clear();
    for (const schedule::Schedule& schedule : client_->schedules()) {
        const QJsonObject document = schedule::toJson(schedule);
        if (!matches(document))
            continue;
        const QString id = document.value(QStringLiteral("id")).toString();
        auto* item = new QListWidgetItem(list_);
        item->setData(Qt::UserRole, id);
        const schedule::Run latest = client_->latestRun(id);
        auto* row = new ScheduleRow(document,
                                    latest.id.isEmpty() ? QJsonObject{} : schedule::toJson(latest),
                                    id == selectedId_, list_);
        int width = list_->viewport()->width() - scaled(28);
        if (width < scaled(220) || width > scaled(320))
            width = scaled(300);
        row->setFixedWidth(width);
        const int column = width - scaled(56);
        const auto wrap = [column](QLabel* label) {
            if (label == nullptr)
                return;
            label->setFixedWidth(column);
            label->setMinimumHeight(label->heightForWidth(column));
        };
        wrap(row->findChild<QLabel*>(QStringLiteral("scheduleRowRule")));
        wrap(row->findChild<QLabel*>(QStringLiteral("scheduleRowTimes")));
        item->setSizeHint(row->sizeHint());
        list_->setItemWidget(item, row);
        connect(row->enabled, &QAbstractButton::toggled, this, [this, id](bool enabled) {
            if (!rebuilding_)
                applyEnabled(id, enabled);
        });
        if (id == selectedId_)
            list_->setCurrentItem(item);
    }
    rebuilding_ = false;
}

void ScheduleWindow::reloadDetail() {
    const QJsonObject schedule = selectedSchedule();
    const bool has = !schedule.isEmpty();
    rebuilding_ = true;
    detailName_->setText(has ? schedule.value(QStringLiteral("name")).toString()
                             : tr("Select a schedule"));
    {
        QSignalBlocker blocker(detailEnabled_);
        detailEnabled_->setChecked(schedule.value(QStringLiteral("enabled")).toBool());
        detailEnabled_->setVisible(has);
    }
    const QString rule = schedule.value(QStringLiteral("rule")).toString();
    const std::optional<ScheduleRule> parsed = scheduleRuleFromText(rule);
    const QString timezone = parsed ? parsed->timezone : QString{};
    const bool enabled = schedule.value(QStringLiteral("enabled")).toBool();
    target_->setText(
        has ? scheduleTargetLabel(schedule.value(QStringLiteral("target")).toObject(),
                                  schedule.value(QStringLiteral("created_by")).toString())
            : QString{});
    repeat_->setText(has ? scheduleRuleSummary(rule) : QString{});
    busy_->setText(has ? scheduleBusyLabel(schedule.value(QStringLiteral("busy_policy")).toString())
                       : QString{});
    missed_->setText(
        has ? scheduleMissedLabel(schedule.value(QStringLiteral("missed_policy")).toString())
            : QString{});
    createdBy_->setText(
        has ? scheduleCreatedByLabel(schedule.value(QStringLiteral("created_by")).toString())
            : QString{});
    nextRun_->setText(
        !has ? QString{}
        : enabled
            ? scheduleWhenLabel(schedule.value(QStringLiteral("next_run_at")).toString(), timezone)
            : tr("Paused — enable to resume"));
    prompt_->setText(schedule.value(QStringLiteral("prompt")).toString());
    prompt_->setVisible(has);
    pause_->setText(enabled ? tr("Pause") : tr("Resume"));
    for (ket::Button* button : {runNowButton_, pause_, edit_, remove_})
        button->setEnabled(has);
    clearLayout(history_->layout());
    if (has) {
        const QList<schedule::Run> runs = client_->runs(selectedId_);
        if (runs.isEmpty()) {
            history_->layout()->addWidget(new QLabel(tr("No runs yet"), history_));
        } else {
            for (const schedule::Run& item : runs) {
                const QJsonObject run = schedule::toJson(item);
                auto* line = new QWidget(history_);
                auto* row = new QHBoxLayout(line);
                row->setContentsMargins(0, 0, 0, 0);
                auto* label = new QLabel(scheduleRunLabel(run), line);
                label->setObjectName(QStringLiteral("scheduleRun"));
                label->setWordWrap(true);
                row->addWidget(label, 1);
                const QString session = run.value(QStringLiteral("session_id")).toString();
                if (!session.isEmpty()) {
                    auto* open = new ket::Button(tr("Conversation #%1 →").arg(session),
                                                 ket::ButtonVariant::Ghost, line);
                    connect(open, &QAbstractButton::clicked, this,
                            [this, session] { emit conversationRequested(session); });
                    row->addWidget(open);
                }
                history_->layout()->addWidget(line);
            }
        }
    }
    showMore_->setVisible(has && client_->runsHaveMore(selectedId_));
    rebuilding_ = false;
}

} // namespace ketplus
