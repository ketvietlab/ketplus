#include "schedules/ScheduleEditor.h"

#include "schedules/ScheduleClient.h"
#include "schedules/ScheduleRules.h"

#include "KetAppearance.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetSegmentedControl.h"
#include "KetSelect.h"
#include "KetTextArea.h"
#include "KetTextField.h"
#include "KetToggle.h"
#include "KetTokens.h"
#include "Schedule.h"

#include <QLabel>
#include <QPalette>
#include <QTextEdit>
#include <QTimeZone>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QStringList zoneChoices(const QString& current) {
    QStringList zones{QStringLiteral("Asia/Ho_Chi_Minh"), QStringLiteral("UTC"),
                      QStringLiteral("Asia/Bangkok"),     QStringLiteral("Asia/Singapore"),
                      QStringLiteral("Asia/Tokyo"),       QStringLiteral("Europe/London"),
                      QStringLiteral("America/New_York"), QStringLiteral("America/Los_Angeles"),
                      QStringLiteral("Australia/Sydney")};
    const QString system = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    if (!system.isEmpty() && !zones.contains(system))
        zones.prepend(system);
    if (!current.isEmpty() && !zones.contains(current))
        zones.prepend(current);
    return zones;
}

ket::FieldRow* stackedRow(const QString& label, QWidget* control, QWidget* parent) {
    auto* row = new ket::FieldRow(label, control, {}, parent);
    row->setStacked(true);
    return row;
}

void choose(ket::Select* select, const QVariant& value) {
    const int index = select->findData(value);
    select->setCurrentIndex(index >= 0 ? index : 0);
}

} // namespace

ScheduleEditor::ScheduleEditor(ScheduleClient* client, const QJsonObject& schedule, QWidget* anchor)
    : QObject(anchor), client_(client), schedule_(schedule),
      dialog_(new ket::Dialog(tr("Edit schedule"), anchor)) {
    dialog_->setObjectName(QStringLiteral("scheduleEditor"));
    dialog_->setCardWidth(560);
    teamStart_ = schedule.value(QStringLiteral("target"))
                     .toObject()
                     .value(QStringLiteral("kind"))
                     .toString() == QLatin1String("team_start");

    auto* body = new QWidget;
    auto* column = new QVBoxLayout(body);
    banner_ = new QLabel(body);
    banner_->setObjectName(QStringLiteral("scheduleEditorError"));
    banner_->setWordWrap(true);
    banner_->hide();
    column->addWidget(banner_);

    name_ = new ket::TextField(body);
    name_->setObjectName(QStringLiteral("scheduleNameEdit"));
    nameRow_ = stackedRow(tr("Name"), name_, body);
    column->addWidget(nameRow_);

    auto* target =
        new QLabel(scheduleTargetLabel(schedule.value(QStringLiteral("target")).toObject()), body);
    target->setObjectName(QStringLiteral("scheduleTargetReadOnly"));
    target->setWordWrap(true);
    target->setTextInteractionFlags(Qt::TextSelectableByMouse);
    column->addWidget(stackedRow(tr("Target"), target, body));

    prompt_ = new ket::TextArea(body);
    prompt_->setObjectName(QStringLiteral("schedulePromptEdit"));
    prompt_->editor()->setReadOnly(teamStart_);
    promptRow_ = stackedRow(tr("Prompt"), prompt_, body);
    if (teamStart_)
        promptRow_->setHelp(tr("A team start has no prompt."));
    column->addWidget(promptRow_);

    repeat_ = new ket::SegmentedControl(
        {tr("Once"), tr("Daily"), tr("Weekdays"), tr("Weekly"), tr("Every N hours")}, body);
    repeat_->setObjectName(QStringLiteral("scheduleRepeat"));
    repeat_->setExpanding(true);
    repeat_->setEnabled(!teamStart_);
    ruleRow_ = stackedRow(tr("Repeat"), repeat_, body);
    column->addWidget(ruleRow_);

    date_ = new ket::TextField(body);
    date_->setPlaceholderText(QStringLiteral("YYYY-MM-DD"));
    date_->setObjectName(QStringLiteral("scheduleDateEdit"));
    onceRow_ = stackedRow(tr("Date"), date_, body);
    column->addWidget(onceRow_);

    time_ = new ket::TextField(body);
    time_->setPlaceholderText(QStringLiteral("HH:mm"));
    time_->setObjectName(QStringLiteral("scheduleTimeEdit"));
    timeRow_ = stackedRow(tr("Time"), time_, body);
    column->addWidget(timeRow_);

    daysRow_ = new QWidget(body);
    auto* daysLayout = new QHBoxLayout(daysRow_);
    daysLayout->setContentsMargins(0, 0, 0, 0);
    const QStringList dayLabels{tr("Mon"), tr("Tue"), tr("Wed"), tr("Thu"),
                                tr("Fri"), tr("Sat"), tr("Sun")};
    for (int index = 0; index < dayLabels.size(); ++index) {
        days_[index] = new ket::Checkbox(dayLabels.at(index), daysRow_);
        daysLayout->addWidget(days_[index]);
        connect(days_[index], &QAbstractButton::toggled, this, [this] { updateSummary(); });
    }
    column->addWidget(daysRow_);

    hours_ = new ket::Select(body);
    hours_->setObjectName(QStringLiteral("scheduleHours"));
    for (int hour = 1; hour <= 24; ++hour)
        hours_->addItem(tr("Every %1 hours").arg(hour), hour);
    hoursRow_ = stackedRow(tr("Interval"), hours_, body);
    column->addWidget(hoursRow_);

    anchorDate_ = new ket::TextField(body);
    anchorDate_->setPlaceholderText(QStringLiteral("YYYY-MM-DD"));
    anchorTime_ = new ket::TextField(body);
    anchorTime_->setPlaceholderText(QStringLiteral("HH:mm"));
    auto* anchorFields = new QWidget(body);
    auto* anchorLayout = new QHBoxLayout(anchorFields);
    anchorLayout->setContentsMargins(0, 0, 0, 0);
    anchorLayout->addWidget(anchorDate_);
    anchorLayout->addWidget(anchorTime_);
    anchorRow_ = stackedRow(tr("Starting"), anchorFields, body);
    column->addWidget(anchorRow_);

    timezone_ = new ket::Select(body);
    timezone_->setObjectName(QStringLiteral("scheduleTimezone"));
    zoneRow_ = stackedRow(tr("Timezone"), timezone_, body);
    column->addWidget(zoneRow_);

    raw_ = new QLabel(body);
    raw_->setObjectName(QStringLiteral("scheduleRawRule"));
    raw_->setWordWrap(true);
    raw_->hide();
    column->addWidget(raw_);

    busy_ = new ket::Select(body);
    busy_->addItem(scheduleBusyLabel(QStringLiteral("queue")), QStringLiteral("queue"));
    busy_->addItem(scheduleBusyLabel(QStringLiteral("skip")), QStringLiteral("skip"));
    busyRow_ = stackedRow(tr("When busy"), busy_, body);
    column->addWidget(busyRow_);

    missed_ = new ket::Select(body);
    missed_->setObjectName(QStringLiteral("scheduleMissed"));
    missed_->addItem(scheduleMissedLabel(QStringLiteral("run_once")), QStringLiteral("run_once"));
    missed_->addItem(scheduleMissedLabel(QStringLiteral("skip")), QStringLiteral("skip"));
    missed_->setEnabled(!teamStart_);
    missedRow_ = stackedRow(tr("When missed"), missed_, body);
    if (teamStart_)
        missedRow_->setHelp(tr("A team start catches up once."));
    column->addWidget(missedRow_);

    summary_ = new QLabel(body);
    summary_->setObjectName(QStringLiteral("scheduleRuleSummary"));
    summary_->setWordWrap(true);
    column->addWidget(summary_);

    dialog_->setBody(body);
    auto* cancel = dialog_->addButton(tr("Cancel"));
    connect(cancel, &QAbstractButton::clicked, dialog_, &ket::Dialog::reject);
    auto* save = dialog_->addButton(tr("Save"), ket::ButtonVariant::Primary);
    save->setObjectName(QStringLiteral("scheduleSave"));
    connect(save, &QAbstractButton::clicked, this, &ScheduleEditor::save);
    connect(dialog_, &ket::Dialog::finished, this, [this](bool) { deleteLater(); });

    const auto refresh = [this] {
        applyKind();
        updateSummary();
    };
    connect(repeat_, &ket::SegmentedControl::currentChanged, this, [refresh](int) { refresh(); });
    connect(hours_, &QComboBox::currentIndexChanged, this, [refresh](int) { refresh(); });
    for (ket::TextField* field : {name_, date_, time_, anchorDate_, anchorTime_})
        connect(field, &QLineEdit::textChanged, this, [this](const QString&) { updateSummary(); });
    connect(timezone_, &QComboBox::currentIndexChanged, this, [this](int) { updateSummary(); });
    connect(prompt_, &ket::TextArea::textChanged, this, [this] { updateSummary(); });
    fill();
}

ScheduleEditor::~ScheduleEditor() { delete dialog_.data(); }

ket::Dialog* ScheduleEditor::dialog() const { return dialog_; }

QString ScheduleEditor::scheduleId() const {
    return schedule_.value(QStringLiteral("id")).toString();
}

void ScheduleEditor::open() { dialog_->open(); }

void ScheduleEditor::fill() {
    const QString ruleText = schedule_.value(QStringLiteral("rule")).toString();
    const std::optional<ScheduleRule> rule = scheduleRuleFromText(ruleText);
    preserved_ = !rule.has_value();
    preservedRule_ = preserved_ ? ruleText : QString{};
    name_->setText(schedule_.value(QStringLiteral("name")).toString());
    prompt_->setText(teamStart_ ? QString{} : schedule_.value(QStringLiteral("prompt")).toString());
    choose(busy_, schedule_.value(QStringLiteral("busy_policy")).toString());
    choose(missed_, teamStart_ ? QStringLiteral("run_once")
                               : schedule_.value(QStringLiteral("missed_policy")).toString());

    const ScheduleRule fields = rule.value_or(ScheduleRule{});
    const QString timezone = fields.timezone.isEmpty() ? QStringLiteral("UTC") : fields.timezone;
    timezone_->clear();
    for (const QString& zone : zoneChoices(timezone))
        timezone_->addItem(zone, zone);
    choose(timezone_, timezone);
    const QStringList kinds{QStringLiteral("once"), QStringLiteral("daily"),
                            QStringLiteral("weekdays"), QStringLiteral("weekly"),
                            QStringLiteral("interval")};
    const int kind = qMax(0, kinds.indexOf(fields.kind));
    repeat_->setCurrentIndex(kind);
    const QString onceAt = fields.at;
    date_->setText(onceAt.section(QLatin1Char('T'), 0, 0));
    time_->setText(fields.kind == QLatin1String("once") ? onceAt.section(QLatin1Char('T'), 1, 1)
                                                        : fields.time);
    const QStringList dayKeys = scheduleDayKeys();
    for (int index = 0; index < dayKeys.size(); ++index)
        days_[index]->setChecked(fields.days.contains(dayKeys.at(index)));
    const int hours = qBound(1, fields.everyHours == 0 ? 4 : fields.everyHours, 24);
    choose(hours_, hours);
    anchorDate_->setText(fields.anchor.section(QLatin1Char('T'), 0, 0));
    anchorTime_->setText(fields.anchor.section(QLatin1Char('T'), 1, 1));
    if (preserved_) {
        raw_->setText(tr("This rule stays as stored.\n%1").arg(ruleText));
        raw_->show();
    }
    applyKind();
    updateSummary();
}

void ScheduleEditor::applyKind() {
    const int kind = repeat_->currentIndex();
    const bool edit = !preserved_;
    ruleRow_->setVisible(edit);
    onceRow_->setVisible(edit && kind == 0);
    timeRow_->setVisible(edit && kind != 4);
    daysRow_->setVisible(edit && kind == 3);
    hoursRow_->setVisible(edit && kind == 4);
    anchorRow_->setVisible(edit && kind == 4);
    zoneRow_->setVisible(edit);
    raw_->setVisible(preserved_);
}

ScheduleRule ScheduleEditor::collect() const {
    ScheduleRule rule;
    const QStringList kinds{QStringLiteral("once"), QStringLiteral("daily"),
                            QStringLiteral("weekdays"), QStringLiteral("weekly"),
                            QStringLiteral("interval")};
    rule.kind = kinds.value(repeat_->currentIndex(), QStringLiteral("once"));
    rule.timezone = timezone_->currentData().toString();
    rule.time = time_->text().trimmed();
    rule.at = date_->text().trimmed() + QLatin1Char('T') + rule.time;
    const QStringList dayKeys = scheduleDayKeys();
    for (int index = 0; index < dayKeys.size(); ++index)
        if (days_[index]->isChecked())
            rule.days.append(dayKeys.at(index));
    rule.everyHours = hours_->currentData().toInt();
    rule.anchor = anchorDate_->text().trimmed() + QLatin1Char('T') + anchorTime_->text().trimmed();
    return rule;
}

void ScheduleEditor::updateSummary() {
    if (preserved_) {
        summary_->setText(tr("Custom rule"));
        return;
    }
    summary_->setText(scheduleRuleSummary(scheduleRuleText(collect())));
}

void ScheduleEditor::showIssue(const QString& code, const QString& message) {
    QString field;
    if (code == QLatin1String(schedule::emptyPromptCode) ||
        code == QLatin1String(schedule::promptNotAllowedCode))
        field = QStringLiteral("prompt");
    else if (code == QLatin1String(schedule::invalidRuleCode))
        field = QStringLiteral("rule");
    else if (code == QLatin1String(schedule::teamStartConstraintCode))
        field = QStringLiteral("missed_policy");
    else if (code == QLatin1String(schedule::invalidPayloadCode) &&
             message.contains(QStringLiteral("name")))
        field = QStringLiteral("name");
    else if (code == QLatin1String(schedule::invalidEnumCode) &&
             message.contains(QStringLiteral("busy_policy")))
        field = QStringLiteral("busy_policy");
    else if (code == QLatin1String(schedule::invalidEnumCode) &&
             message.contains(QStringLiteral("missed_policy")))
        field = QStringLiteral("missed_policy");
    nameRow_->setError(field == QLatin1String("name") ? message : QString{});
    promptRow_->setError(field == QLatin1String("prompt") ? message : QString{});
    ruleRow_->setError(field == QLatin1String("rule") ? message : QString{});
    busyRow_->setError(field == QLatin1String("busy_policy") ? message : QString{});
    missedRow_->setError(field == QLatin1String("missed_policy") ? message : QString{});
    // The prompt's note sits under the native text editor, so the message also leads the card.
    banner_->setText(message);
    if (!message.isEmpty()) {
        QPalette palette = banner_->palette();
        const QColor danger = ket::Tokens::color(u"danger", ket::Appearance::instance().theme());
        for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
            palette.setColor(group, QPalette::WindowText, danger);
        banner_->setPalette(palette);
    }
    const bool show = !message.isEmpty();
    const bool moved = banner_->isVisible() != show;
    banner_->setVisible(show);
    if (moved)
        dialog_->setCardWidth(560);
}

void ScheduleEditor::save() {
    ScheduleEdit edit;
    edit.name = name_->text();
    edit.prompt = prompt_->text();
    edit.busyPolicy = busy_->currentData().toString();
    edit.missedPolicy = missed_->currentData().toString();
    if (preserved_)
        edit.preservedRule = preservedRule_;
    else
        edit.rule = collect();
    const EditIssue invalid = validateScheduleEdit(schedule_, edit);
    if (!invalid.ok()) {
        showIssue(invalid.code, invalid.message);
        return;
    }
    showIssue({}, {});
    const QPointer<ScheduleEditor> alive(this);
    client_->request(QStringLiteral("schedule.update"), scheduleUpdatePayload(schedule_, edit),
                     [alive](const QJsonObject& result, const QString& error) {
                         if (!alive)
                             return;
                         if (!error.isEmpty()) {
                             const QString code =
                                 result.value(QStringLiteral("error_code")).toString();
                             alive->showIssue(code, scheduleFailureText(result, error));
                             return;
                         }
                         alive->dialog_->accept();
                     });
}

} // namespace ketplus
