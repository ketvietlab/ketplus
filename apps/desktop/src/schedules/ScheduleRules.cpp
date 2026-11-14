#include "schedules/ScheduleRules.h"

#include "Schedule.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimeZone>

namespace ketplus {
namespace {

QStringList keysFor(const QString& kind) {
    if (kind == QLatin1String("once"))
        return {QStringLiteral("kind"), QStringLiteral("at"), QStringLiteral("timezone")};
    if (kind == QLatin1String("daily") || kind == QLatin1String("weekdays"))
        return {QStringLiteral("kind"), QStringLiteral("time"), QStringLiteral("timezone")};
    if (kind == QLatin1String("weekly"))
        return {QStringLiteral("kind"), QStringLiteral("days"), QStringLiteral("time"),
                QStringLiteral("timezone")};
    if (kind == QLatin1String("interval"))
        return {QStringLiteral("kind"), QStringLiteral("every_hours"), QStringLiteral("anchor"),
                QStringLiteral("timezone")};
    return {};
}

bool localDateTime(const QString& text) {
    return QDateTime::fromString(text, QStringLiteral("yyyy-MM-ddTHH:mm")).isValid();
}

bool clockTime(const QString& text) {
    return QTime::fromString(text, QStringLiteral("HH:mm")).isValid();
}

QString zoneOffset(const QString& timezone) {
    const QTimeZone zone(timezone.toUtf8());
    if (!zone.isValid())
        return timezone;
    const int seconds = zone.offsetFromUtc(QDateTime::currentDateTimeUtc());
    const int absolute = seconds < 0 ? -seconds : seconds;
    const QString sign = seconds < 0 ? QStringLiteral("-") : QStringLiteral("+");
    const int minutes = (absolute % 3600) / 60;
    if (minutes == 0)
        return QStringLiteral("GMT%1%2").arg(sign).arg(absolute / 3600);
    return QStringLiteral("GMT%1%2:%3")
        .arg(sign)
        .arg(absolute / 3600, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'));
}

EditIssue issue(const char* code, const QString& message, const QString& field) {
    return {QString::fromLatin1(code), message, field};
}

EditIssue ruleIssue(const QString& message) {
    return issue(schedule::invalidRuleCode, message, QStringLiteral("rule"));
}

QString ruleTextOf(const ScheduleEdit& edit) {
    return edit.preservedRule.isEmpty() ? scheduleRuleText(edit.rule) : edit.preservedRule;
}

} // namespace

QStringList scheduleDayKeys() {
    return {QStringLiteral("mon"), QStringLiteral("tue"), QStringLiteral("wed"),
            QStringLiteral("thu"), QStringLiteral("fri"), QStringLiteral("sat"),
            QStringLiteral("sun")};
}

std::optional<ScheduleRule> scheduleRuleFromText(const QString& rule) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(rule.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    const QJsonObject object = document.object();
    const QString kind = object.value(QStringLiteral("kind")).toString();
    const QStringList allowed = keysFor(kind);
    if (allowed.isEmpty())
        return std::nullopt;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        if (!allowed.contains(it.key()))
            return std::nullopt;

    ScheduleRule parsed;
    parsed.kind = kind;
    parsed.timezone = object.value(QStringLiteral("timezone")).toString();
    if (object.contains(QStringLiteral("timezone")) &&
        !object.value(QStringLiteral("timezone")).isString())
        return std::nullopt;
    parsed.at = object.value(QStringLiteral("at")).toString();
    parsed.time = object.value(QStringLiteral("time")).toString();
    parsed.anchor = object.value(QStringLiteral("anchor")).toString();
    if (object.contains(QStringLiteral("every_hours"))) {
        const QJsonValue hours = object.value(QStringLiteral("every_hours"));
        if (!hours.isDouble() || hours.toInt() != hours.toDouble())
            return std::nullopt;
        parsed.everyHours = hours.toInt();
    }
    if (object.contains(QStringLiteral("days"))) {
        if (!object.value(QStringLiteral("days")).isArray())
            return std::nullopt;
        const QStringList known = scheduleDayKeys();
        for (const QJsonValue value : object.value(QStringLiteral("days")).toArray()) {
            const QString day = value.toString();
            if (!known.contains(day))
                return std::nullopt;
            parsed.days.append(day);
        }
    }
    return parsed;
}

QString scheduleRuleText(const ScheduleRule& rule) {
    QJsonObject object{{QStringLiteral("kind"), rule.kind},
                       {QStringLiteral("timezone"), rule.timezone}};
    if (rule.kind == QLatin1String("once"))
        object.insert(QStringLiteral("at"), rule.at);
    if (rule.kind == QLatin1String("daily") || rule.kind == QLatin1String("weekdays") ||
        rule.kind == QLatin1String("weekly"))
        object.insert(QStringLiteral("time"), rule.time);
    if (rule.kind == QLatin1String("weekly")) {
        QJsonArray days;
        for (const QString& day : scheduleDayKeys())
            if (rule.days.contains(day))
                days.append(day);
        object.insert(QStringLiteral("days"), days);
    }
    if (rule.kind == QLatin1String("interval")) {
        object.insert(QStringLiteral("every_hours"), rule.everyHours);
        object.insert(QStringLiteral("anchor"), rule.anchor);
    }
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString scheduleRuleSummary(const QString& rule) {
    const std::optional<ScheduleRule> parsed = scheduleRuleFromText(rule);
    if (!parsed)
        return QStringLiteral("Custom rule");
    QString body;
    if (parsed->kind == QLatin1String("once")) {
        QString at = parsed->at;
        at.replace(QLatin1Char('T'), QLatin1Char(' '));
        body = QStringLiteral("Once · %1").arg(at);
    } else if (parsed->kind == QLatin1String("daily"))
        body = QStringLiteral("Daily %1").arg(parsed->time);
    else if (parsed->kind == QLatin1String("weekdays"))
        body = QStringLiteral("Weekdays %1").arg(parsed->time);
    else if (parsed->kind == QLatin1String("weekly")) {
        QStringList labels;
        const QStringList names{QStringLiteral("Mon"), QStringLiteral("Tue"), QStringLiteral("Wed"),
                                QStringLiteral("Thu"), QStringLiteral("Fri"), QStringLiteral("Sat"),
                                QStringLiteral("Sun")};
        const QStringList keys = scheduleDayKeys();
        for (int index = 0; index < keys.size(); ++index)
            if (parsed->days.contains(keys.at(index)))
                labels.append(names.at(index));
        body = QStringLiteral("%1 %2").arg(labels.join(QStringLiteral(", ")), parsed->time);
    } else if (parsed->kind == QLatin1String("interval")) {
        body = parsed->everyHours == 1 ? QStringLiteral("Every hour")
                                       : QStringLiteral("Every %1 hours").arg(parsed->everyHours);
    }
    if (parsed->timezone.isEmpty())
        return body;
    return QStringLiteral("%1 (%2)").arg(body, zoneOffset(parsed->timezone));
}

QString scheduleTargetLabel(const QJsonObject& target, const QString& createdBy) {
    const QString kind = target.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("conversation")) {
        const QString session = target.value(QStringLiteral("session_id")).toString();
        if (!createdBy.isEmpty() && createdBy != QLatin1String("user"))
            return QStringLiteral("#%1 · agent-created").arg(session);
        return QStringLiteral("#%1").arg(session);
    }
    if (kind == QLatin1String("new")) {
        const QString provider = target.value(QStringLiteral("provider")).toString();
        QString name = provider;
        if (provider == QLatin1String("claude"))
            name = QStringLiteral("Claude");
        else if (provider == QLatin1String("codex"))
            name = QStringLiteral("Codex");
        else if (provider == QLatin1String("cursor"))
            name = QStringLiteral("Cursor");
        else if (name.isEmpty())
            name = target.value(QStringLiteral("workspace")).toString();
        return QStringLiteral("New conversation each run · %1").arg(name);
    }
    if (kind == QLatin1String("team"))
        return QStringLiteral("Team %1 → leader")
            .arg(target.value(QStringLiteral("team_id")).toString());
    if (kind == QLatin1String("team_start"))
        return QStringLiteral("Team start · once");
    return kind;
}

QString scheduleWhenLabel(const QString& iso, const QString& timezone) {
    if (iso.isEmpty())
        return QStringLiteral("Not scheduled");
    QDateTime when = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!when.isValid())
        when = QDateTime::fromString(iso, Qt::ISODate);
    if (!when.isValid())
        return iso;
    const QTimeZone zone(timezone.toUtf8());
    if (zone.isValid())
        when = when.toTimeZone(zone);
    return when.toString(QStringLiteral("dd/MM HH:mm"));
}

QString scheduleRunLabel(const QJsonObject& run) {
    const QString when =
        scheduleWhenLabel(run.value(QStringLiteral("scheduled_for")).toString(), {});
    const QString status = run.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("succeeded"))
        return QStringLiteral("✓ %1 Succeeded").arg(when);
    if (status == QLatin1String("failed"))
        return QStringLiteral("✗ %1 Failed: %2")
            .arg(when, run.value(QStringLiteral("error_message")).toString());
    if (status == QLatin1String("skipped"))
        return QStringLiteral("↷ %1 Skipped").arg(when);
    if (status == QLatin1String("missed"))
        return QStringLiteral("⚠ %1 Missed").arg(when);
    if (status == QLatin1String("running"))
        return QStringLiteral("%1 Running").arg(when);
    if (status == QLatin1String("queued"))
        return QStringLiteral("%1 Queued").arg(when);
    return QStringLiteral("%1 %2").arg(when, status);
}

QString scheduleLastLabel(const QJsonObject& schedule, const QJsonObject& latestRun) {
    if (!latestRun.isEmpty())
        return scheduleRunLabel(latestRun);
    const QString last = schedule.value(QStringLiteral("last_run_at")).toString();
    if (last.isEmpty())
        return QStringLiteral("Not run yet");
    const std::optional<ScheduleRule> rule =
        scheduleRuleFromText(schedule.value(QStringLiteral("rule")).toString());
    return QStringLiteral("Last %1").arg(
        scheduleWhenLabel(last, rule ? rule->timezone : QString{}));
}

QString scheduleCreatedByLabel(const QString& createdBy) {
    if (createdBy == QLatin1String("user"))
        return QStringLiteral("You");
    if (createdBy.isEmpty())
        return QStringLiteral("Unknown");
    return QStringLiteral("Agent · %1").arg(createdBy);
}

QString scheduleBusyLabel(const QString& policy) {
    if (policy == QLatin1String("queue"))
        return QStringLiteral("Queue until finished");
    if (policy == QLatin1String("skip"))
        return QStringLiteral("Skip this run");
    return policy;
}

QString scheduleMissedLabel(const QString& policy) {
    if (policy == QLatin1String("run_once"))
        return QStringLiteral("Catch up once when the computer wakes");
    if (policy == QLatin1String("skip"))
        return QStringLiteral("Skip");
    return policy;
}

EditIssue validateScheduleEdit(const QJsonObject& schedule, const ScheduleEdit& edit) {
    const QString name = edit.name.trimmed();
    if (name.isEmpty())
        return issue(schedule::invalidPayloadCode, QStringLiteral("name is required."),
                     QStringLiteral("name"));
    if (name.size() > schedule::maximumScheduleNameLength)
        return issue(schedule::invalidPayloadCode, QStringLiteral("name is too long."),
                     QStringLiteral("name"));

    const auto kind = schedule::targetKindFromName(schedule.value(QStringLiteral("target"))
                                                       .toObject()
                                                       .value(QStringLiteral("kind"))
                                                       .toString());
    if (!kind)
        return issue(schedule::missingTargetCode, QStringLiteral("target is required."),
                     QStringLiteral("rule"));
    const bool teamStart = *kind == schedule::TargetKind::TeamStart;
    if (const schedule::Failure promptError = schedule::validatePrompt(edit.prompt, *kind);
        !promptError.ok())
        return {promptError.code, promptError.message, QStringLiteral("prompt")};

    const QString rule = ruleTextOf(edit);
    if (edit.preservedRule.isEmpty()) {
        if (teamStart && edit.rule.kind != QLatin1String("once"))
            return ruleIssue(QStringLiteral("A team start accepts only a once rule."));
        if (edit.rule.timezone.isEmpty() ||
            !QTimeZone::isTimeZoneIdAvailable(edit.rule.timezone.toUtf8()))
            return ruleIssue(QStringLiteral("Choose an IANA timezone."));
        if (edit.rule.kind == QLatin1String("once") && !localDateTime(edit.rule.at))
            return ruleIssue(QStringLiteral("Choose a date and time."));
        if ((edit.rule.kind == QLatin1String("daily") ||
             edit.rule.kind == QLatin1String("weekdays") ||
             edit.rule.kind == QLatin1String("weekly")) &&
            !clockTime(edit.rule.time))
            return ruleIssue(QStringLiteral("Choose a time as HH:mm."));
        if (edit.rule.kind == QLatin1String("weekly") && edit.rule.days.isEmpty())
            return ruleIssue(QStringLiteral("Choose at least one weekday."));
        if (edit.rule.kind == QLatin1String("interval")) {
            if (edit.rule.everyHours < 1 || edit.rule.everyHours > 24)
                return ruleIssue(QStringLiteral("Every N hours must be from 1 to 24."));
            if (!localDateTime(edit.rule.anchor))
                return ruleIssue(QStringLiteral("Choose a start time."));
        }
    }
    if (const schedule::Failure ruleError = schedule::validateRule(rule, teamStart);
        !ruleError.ok())
        return ruleIssue(ruleError.message);

    if (!schedule::busyPolicyFromName(edit.busyPolicy))
        return issue(
            schedule::invalidEnumCode,
            QStringLiteral("busy_policy \"%1\" is not a known value.").arg(edit.busyPolicy),
            QStringLiteral("busy_policy"));
    if (!schedule::missedPolicyFromName(edit.missedPolicy))
        return issue(
            schedule::invalidEnumCode,
            QStringLiteral("missed_policy \"%1\" is not a known value.").arg(edit.missedPolicy),
            QStringLiteral("missed_policy"));
    if (teamStart && edit.missedPolicy != QLatin1String("run_once"))
        return issue(schedule::teamStartConstraintCode,
                     QStringLiteral("A team start uses missed policy run_once."),
                     QStringLiteral("missed_policy"));
    return {};
}

QJsonObject scheduleUpdatePayload(const QJsonObject& schedule, const ScheduleEdit& edit) {
    const bool teamStart = schedule.value(QStringLiteral("target"))
                               .toObject()
                               .value(QStringLiteral("kind"))
                               .toString() == QLatin1String("team_start");
    QJsonObject payload{{QStringLiteral("id"), schedule.value(QStringLiteral("id"))},
                        {QStringLiteral("name"), edit.name.trimmed()},
                        {QStringLiteral("prompt"), teamStart ? QString{} : edit.prompt},
                        {QStringLiteral("rule"), ruleTextOf(edit)},
                        {QStringLiteral("busy_policy"), edit.busyPolicy},
                        {QStringLiteral("missed_policy"), edit.missedPolicy}};
    if (teamStart)
        payload.insert(QStringLiteral("target"), schedule.value(QStringLiteral("target")));
    return payload;
}

QString scheduleFailureText(const QJsonObject& result, const QString& error) {
    const QString code = result.value(QStringLiteral("error_code")).toString();
    const QString message =
        !error.isEmpty() ? error : result.value(QStringLiteral("error_message")).toString();
    if (code == QLatin1String("not_implemented"))
        return message.isEmpty() ? QStringLiteral("This action is not available yet.") : message;
    if (!message.isEmpty())
        return message;
    if (!code.isEmpty())
        return code;
    return QStringLiteral("Something went wrong.");
}

} // namespace ketplus
