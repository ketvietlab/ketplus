#include "teams/TeamScheduleFields.h"

#include "schedules/ScheduleRules.h"

#include "KetButton.h"
#include "KetFieldRow.h"
#include "KetSelect.h"
#include "KetTextArea.h"
#include "KetTextField.h"
#include "KetToggle.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QTimeZone>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QStringList zones(const QString& current) {
    QStringList list{QStringLiteral("Asia/Ho_Chi_Minh"), QStringLiteral("UTC"),
                     QStringLiteral("Asia/Bangkok"),     QStringLiteral("Asia/Singapore"),
                     QStringLiteral("Asia/Tokyo"),       QStringLiteral("Europe/London"),
                     QStringLiteral("America/New_York"), QStringLiteral("America/Los_Angeles"),
                     QStringLiteral("Australia/Sydney")};
    const QString system = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    if (!system.isEmpty() && !list.contains(system))
        list.prepend(system);
    if (!current.isEmpty() && !list.contains(current))
        list.prepend(current);
    return list;
}

void fillZones(ket::Select* select, const QString& current) {
    select->clear();
    for (const QString& zone : zones(current))
        select->addItem(zone, zone);
    const int index =
        select->findData(current.isEmpty() ? QStringLiteral("Asia/Ho_Chi_Minh") : current);
    select->setCurrentIndex(index >= 0 ? index : 0);
}

QDateTime civil(const QString& date, const QString& time, const QString& zone) {
    const QDate day = QDate::fromString(date, Qt::ISODate);
    const QTime clock = QTime::fromString(time, QStringLiteral("HH:mm"));
    const QTimeZone area(zone.toUtf8());
    if (!day.isValid() || !clock.isValid() || !area.isValid())
        return {};
    return QDateTime(day, clock, area);
}

} // namespace

TeamScheduleFields::TeamScheduleFields(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("teamScheduleFields"));
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel(tr("Schedule"), this);
    title->setObjectName(QStringLiteral("teamScheduleHeading"));
    column->addWidget(title);

    now_ = new ket::Radio(tr("Now"), this);
    scheduled_ = new ket::Radio(tr("Scheduled"), this);
    now_->setObjectName(QStringLiteral("teamStartNow"));
    scheduled_->setObjectName(QStringLiteral("teamStartScheduled"));
    auto* group = new QButtonGroup(this);
    group->addButton(now_);
    group->addButton(scheduled_);
    now_->setChecked(true);
    auto* choice = new QHBoxLayout;
    choice->addWidget(now_);
    choice->addWidget(scheduled_);
    choice->addStretch();
    column->addLayout(choice);

    startDetails_ = new QWidget(this);
    auto* start = new QVBoxLayout(startDetails_);
    start->setContentsMargins(0, 0, 0, 0);
    auto* presets = new QHBoxLayout;
    auto* tonight = new ket::Button(tr("Tonight at 22:00"), ket::ButtonVariant::Secondary, this);
    auto* tomorrow = new ket::Button(tr("Tomorrow at 08:00"), ket::ButtonVariant::Secondary, this);
    auto* custom = new ket::Button(tr("Custom"), ket::ButtonVariant::Secondary, this);
    tonight->setObjectName(QStringLiteral("teamStartTonight"));
    tomorrow->setObjectName(QStringLiteral("teamStartTomorrow"));
    custom->setObjectName(QStringLiteral("teamStartCustom"));
    presets->addWidget(tonight);
    presets->addWidget(tomorrow);
    presets->addWidget(custom);
    presets->addStretch();
    start->addLayout(presets);
    date_ = new ket::TextField(startDetails_);
    time_ = new ket::TextField(startDetails_);
    zone_ = new ket::Select(startDetails_);
    date_->setObjectName(QStringLiteral("teamStartDate"));
    time_->setObjectName(QStringLiteral("teamStartTime"));
    zone_->setObjectName(QStringLiteral("teamStartZone"));
    date_->setPlaceholderText(QStringLiteral("YYYY-MM-DD"));
    time_->setPlaceholderText(QStringLiteral("HH:mm"));
    fillZones(zone_, QStringLiteral("Asia/Ho_Chi_Minh"));
    start->addWidget(new ket::FieldRow(
        tr("Date"), date_, tr("YYYY-MM-DD · 1 minute to 30 days from now"), startDetails_));
    start->addWidget(new ket::FieldRow(tr("Time"), time_, {}, startDetails_));
    start->addWidget(new ket::FieldRow(tr("Time zone"), zone_, {}, startDetails_));
    column->addWidget(startDetails_);
    connect(tonight, &QAbstractButton::clicked, this, [this] { preset(false); });
    connect(tomorrow, &QAbstractButton::clicked, this, [this] { preset(true); });
    connect(custom, &QAbstractButton::clicked, this, [this] { date_->setFocus(); });
    connect(scheduled_, &QAbstractButton::toggled, this, [this](bool) { applyStartMode(); });

    repeat_ = new ket::Select(this);
    repeat_->setObjectName(QStringLiteral("teamRepeat"));
    repeat_->addItem(tr("Off"), QStringLiteral("off"));
    repeat_->addItem(tr("Every day"), QStringLiteral("daily"));
    repeat_->addItem(tr("Weekdays"), QStringLiteral("weekdays"));
    repeat_->addItem(tr("Weekly"), QStringLiteral("weekly"));
    repeat_->addItem(tr("Every N hours"), QStringLiteral("interval"));
    column->addWidget(new ket::FieldRow(tr("Repeat"), repeat_, {}, this));

    repeatDetails_ = new QWidget(this);
    auto* details = new QVBoxLayout(repeatDetails_);
    details->setContentsMargins(0, 0, 0, 0);
    days_ = new QWidget(repeatDetails_);
    auto* dayRow = new QHBoxLayout(days_);
    dayRow->setContentsMargins(0, 0, 0, 0);
    const QStringList labels{tr("Mon"), tr("Tue"), tr("Wed"), tr("Thu"),
                             tr("Fri"), tr("Sat"), tr("Sun")};
    const QStringList keys{QStringLiteral("mon"), QStringLiteral("tue"), QStringLiteral("wed"),
                           QStringLiteral("thu"), QStringLiteral("fri"), QStringLiteral("sat"),
                           QStringLiteral("sun")};
    for (int index = 0; index < labels.size(); ++index) {
        day_[index] = new ket::Checkbox(labels.at(index), days_);
        day_[index]->setObjectName(QStringLiteral("teamRepeatDay_") + keys.at(index));
        day_[index]->setChecked(index < 5);
        dayRow->addWidget(day_[index]);
    }
    details->addWidget(days_);
    repeatTime_ = new ket::TextField(repeatDetails_);
    repeatTime_->setObjectName(QStringLiteral("teamRepeatTime"));
    repeatTime_->setText(QStringLiteral("08:00"));
    repeatZone_ = new ket::Select(repeatDetails_);
    repeatZone_->setObjectName(QStringLiteral("teamRepeatZone"));
    fillZones(repeatZone_, QStringLiteral("Asia/Ho_Chi_Minh"));
    hours_ = new ket::Select(repeatDetails_);
    hours_->setObjectName(QStringLiteral("teamRepeatHours"));
    for (int hour = 1; hour <= 24; ++hour)
        hours_->addItem(tr("Every %1 hours").arg(hour), hour);
    hours_->setCurrentIndex(3);
    details->addWidget(new ket::FieldRow(tr("Time"), repeatTime_, {}, repeatDetails_));
    details->addWidget(new ket::FieldRow(tr("Time zone"), repeatZone_, {}, repeatDetails_));
    details->addWidget(new ket::FieldRow(tr("Every N hours"), hours_, {}, repeatDetails_));
    column->addWidget(repeatDetails_);

    order_ = new ket::TextArea(this);
    order_->setObjectName(QStringLiteral("teamWorkOrder"));
    orderRow_ = new ket::FieldRow(tr("Work order each run"), order_,
                                  tr("The leader receives this work order on every run."), this);
    static_cast<ket::FieldRow*>(orderRow_)->setStacked(true);
    column->addWidget(orderRow_);
    connect(order_, &ket::TextArea::textChanged, this, [this] { orderEdited_ = true; });
    connect(repeat_, &QComboBox::currentIndexChanged, this, [this](int) { applyRepeatMode(); });

    problem_ = new QLabel(this);
    problem_->setObjectName(QStringLiteral("teamScheduleProblem"));
    problem_->setWordWrap(true);
    problem_->hide();
    column->addWidget(problem_);
    applyStartMode();
    applyRepeatMode();
}

void TeamScheduleFields::setGoal(const QString& goal) {
    if (orderEdited_)
        return;
    const QSignalBlocker blocker(order_);
    order_->setText(goal);
}

void TeamScheduleFields::setStartOnly(const bool startOnly) {
    startOnly_ = startOnly;
    if (QWidget* row = repeat_->parentWidget())
        row->setVisible(!startOnly);
    applyRepeatMode();
    setScheduled(true);
}

void TeamScheduleFields::setScheduled(const bool scheduled) {
    scheduled_->setChecked(scheduled);
    now_->setChecked(!scheduled);
    applyStartMode();
}

void TeamScheduleFields::setStart(const QString& date, const QString& time, const QString& zone) {
    date_->setText(date);
    time_->setText(time);
    fillZones(zone_, zone);
}

QJsonObject TeamScheduleFields::startAt() const {
    if (!scheduled_->isChecked())
        return {};
    return {{QStringLiteral("at"),
             date_->text().trimmed() + QLatin1Char('T') + time_->text().trimmed()},
            {QStringLiteral("timezone"), zone()}};
}

QJsonObject TeamScheduleFields::repeat() const {
    const QString kind = repeat_->currentData().toString();
    if (startOnly_ || kind == QLatin1String("off") || kind.isEmpty())
        return {};
    ScheduleRule rule;
    rule.kind = kind;
    rule.timezone = repeatZone();
    rule.time = repeatTime_->text().trimmed();
    if (kind == QLatin1String("weekly")) {
        const QStringList keys{QStringLiteral("mon"), QStringLiteral("tue"), QStringLiteral("wed"),
                               QStringLiteral("thu"), QStringLiteral("fri"), QStringLiteral("sat"),
                               QStringLiteral("sun")};
        for (int index = 0; index < keys.size(); ++index)
            if (day_[index]->isChecked())
                rule.days.append(keys.at(index));
    }
    if (kind == QLatin1String("interval")) {
        rule.everyHours = hours_->currentData().toInt();
        const QString today = QDateTime::currentDateTime().date().toString(Qt::ISODate);
        rule.anchor = today + QLatin1Char('T') + rule.time;
    }
    return {{QStringLiteral("rule"), scheduleRuleText(rule)},
            {QStringLiteral("work_order"), order_->text().trimmed()}};
}

QString TeamScheduleFields::problem(const QDateTime& now) const {
    if (scheduled_->isChecked()) {
        const QDateTime when = civil(date_->text().trimmed(), time_->text().trimmed(), zone());
        if (!when.isValid())
            return tr("Enter a date YYYY-MM-DD, a time HH:mm and a time zone.");
        const qint64 lead = now.toUTC().msecsTo(when.toUTC());
        if (lead < 60 * 1000)
            return tr("The team can start at least one minute from now.");
        if (lead > 30LL * 24 * 60 * 60 * 1000)
            return tr("The team can start at most 30 days from now.");
    }
    const QString kind = repeat_->currentData().toString();
    if (!startOnly_ && kind != QLatin1String("off") && !kind.isEmpty()) {
        if (order_->text().trimmed().isEmpty())
            return tr("Work order is required.");
        if (kind == QLatin1String("weekly")) {
            bool any = false;
            for (ket::Checkbox* day : day_)
                any = any || day->isChecked();
            if (!any)
                return tr("Choose a day.");
        }
        if (!QTime::fromString(repeatTime_->text().trimmed(), QStringLiteral("HH:mm")).isValid())
            return tr("Enter a time HH:mm.");
    }
    return {};
}

void TeamScheduleFields::showProblem(const QDateTime& now) {
    const QString text = problem(now);
    problem_->setText(text);
    problem_->setVisible(!text.isEmpty());
}

void TeamScheduleFields::applyStartMode() { startDetails_->setVisible(scheduled_->isChecked()); }

void TeamScheduleFields::applyRepeatMode() {
    const bool on = !startOnly_ && repeat_->currentData().toString() != QLatin1String("off");
    const QString kind = repeat_->currentData().toString();
    repeatDetails_->setVisible(on);
    days_->setVisible(kind == QLatin1String("weekly"));
    hours_->parentWidget()->setVisible(kind == QLatin1String("interval"));
    orderRow_->setVisible(on);
}

void TeamScheduleFields::preset(const bool tomorrow) {
    setScheduled(true);
    const QDate today = QDateTime::currentDateTime().date();
    date_->setText((tomorrow ? today.addDays(1) : today).toString(Qt::ISODate));
    time_->setText(tomorrow ? QStringLiteral("08:00") : QStringLiteral("22:00"));
}

QString TeamScheduleFields::zone() const { return zone_->currentData().toString(); }

QString TeamScheduleFields::repeatZone() const { return repeatZone_->currentData().toString(); }

} // namespace ketplus
