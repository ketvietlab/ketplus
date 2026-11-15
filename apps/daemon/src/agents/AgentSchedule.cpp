#include "agents/AgentSchedule.h"

#include "runtime/ScheduleFromRecord.h"
#include "schedule/ScheduleRule.h"
#include "storage/LocalJournal.h"
#include "storage/ScheduleRecord.h"

#include "Schedule.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUuid>

#include <cmath>

namespace ketplus::daemon::agent_schedule {
namespace {

using schedule::Kind;
using schedule::nextRunAfter;
using schedule::ScheduleRule;
using schedule::serialize;

QJsonObject failed(const char* code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), QString::fromLatin1(code)},
            {QStringLiteral("error_message"), message}};
}

QString stamp(const QDateTime& time) { return time.toUTC().toString(Qt::ISODateWithMs); }

QDateTime minute(QDateTime time) {
    time = time.toUTC();
    time.setTime(QTime(time.time().hour(), time.time().minute()));
    return time;
}

bool zoned(const QString& text) {
    static const QRegularExpression zone(QStringLiteral(R"((?:[Zz]|[+-]\d{2}:?\d{2})$)"));
    return zone.match(text).hasMatch();
}

QJsonObject document(const ScheduleRecord& record) {
    ketplus::schedule::Failure failure;
    const auto schedule = scheduleFromRecord(record, &failure);
    return schedule ? ketplus::schedule::toJson(*schedule) : QJsonObject{};
}

} // namespace

QJsonObject createAgentSchedule(LocalJournal& journal, const QString& sessionId,
                                const QJsonObject& payload, const QDateTime& now) {
    if (!ketplus::schedule::validateCreatedBy(sessionId).ok())
        return failed(ketplus::schedule::invalidPayloadCode,
                      QStringLiteral("Conversation id is invalid."));
    const QStringList allowed{QStringLiteral("prompt"), QStringLiteral("name"),
                              QStringLiteral("at"), QStringLiteral("inMinutes")};
    for (auto it = payload.constBegin(); it != payload.constEnd(); ++it) {
        if (!allowed.contains(it.key())) {
            if (it.key() == QLatin1String("rule") || it.key() == QLatin1String("kind") ||
                it.key() == QLatin1String("target"))
                return failed(onceOnlyCode,
                              QStringLiteral("An agent schedule is once, for this conversation."));
            return failed(ketplus::schedule::unknownFieldCode,
                          QStringLiteral("Unknown field \"%1\".").arg(it.key()));
        }
    }
    const bool hasAt = payload.contains(QStringLiteral("at"));
    const bool hasMinutes = payload.contains(QStringLiteral("inMinutes"));
    if (hasAt == hasMinutes)
        return failed(ketplus::schedule::invalidPayloadCode,
                      QStringLiteral("Send at or inMinutes, not both."));
    const QString prompt = payload.value(QStringLiteral("prompt")).toString();
    if (const auto issue =
            ketplus::schedule::validatePrompt(prompt, ketplus::schedule::TargetKind::Conversation);
        !issue.ok())
        return failed(issue.code.toLatin1().constData(), issue.message);
    QString name = QStringLiteral("Follow-up");
    if (payload.contains(QStringLiteral("name"))) {
        if (!payload.value(QStringLiteral("name")).isString())
            return failed(ketplus::schedule::invalidPayloadCode,
                          QStringLiteral("name must be a string."));
        name = payload.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty() || name.size() > ketplus::schedule::maximumScheduleNameLength)
            return failed(ketplus::schedule::invalidPayloadCode,
                          QStringLiteral("name is empty or too long."));
    }
    QDateTime fire;
    if (hasMinutes) {
        const QJsonValue minutes = payload.value(QStringLiteral("inMinutes"));
        if (!minutes.isDouble() || minutes.toDouble() != std::floor(minutes.toDouble()))
            return failed(ketplus::schedule::invalidPayloadCode,
                          QStringLiteral("inMinutes must be a whole number of minutes."));
        const int count = minutes.toInt();
        if (count < minimumLeadMinutes)
            return failed(tooSoonCode,
                          QStringLiteral("A schedule must be at least 1 minute ahead."));
        fire = minute(now.addSecs(count * 60));
    } else {
        const QString at = payload.value(QStringLiteral("at")).toString();
        if (!zoned(at))
            return failed(ketplus::schedule::invalidPayloadCode,
                          QStringLiteral("at must be an ISO-8601 timestamp with a UTC offset."));
        fire = minute(QDateTime::fromString(at, Qt::ISODateWithMs));
        if (!fire.isValid())
            fire = minute(QDateTime::fromString(at, Qt::ISODate));
        if (!fire.isValid())
            return failed(ketplus::schedule::invalidPayloadCode,
                          QStringLiteral("at must be an ISO-8601 timestamp with a UTC offset."));
    }
    // The once rule fires on the minute, so the one-minute lead is a whole minute too.
    const QDateTime earliest = minute(now).addSecs(minimumLeadMinutes * 60);
    const QDateTime latest = now.toUTC().addDays(maximumLeadDays);
    if (fire < earliest)
        return failed(tooSoonCode, QStringLiteral("A schedule must be at least 1 minute ahead."));
    if (fire > latest)
        return failed(tooFarCode, QStringLiteral("A schedule must be at most 7 days ahead."));

    ScheduleFilter filter;
    filter.createdBy = sessionId;
    const auto owned = journal.schedules(filter);
    if (!owned)
        return failed("journal_error", journal.errorString());
    int active = 0;
    for (const ScheduleRecord& row : *owned)
        if (row.enabled)
            ++active;
    if (active >= maximumActivePerConversation)
        return failed(scheduleLimitCode,
                      QStringLiteral("This conversation already has 5 active schedules."));

    const QTimeZone zone = QTimeZone::systemTimeZone();
    const QDateTime local = fire.toTimeZone(zone.isValid() ? zone : QTimeZone::utc());
    ScheduleRule rule;
    rule.kind = Kind::Once;
    rule.timeZone = zone.isValid() ? zone : QTimeZone::utc();
    rule.date = local.date();
    rule.time = QTime(local.time().hour(), local.time().minute());
    const auto next = nextRunAfter(rule, fire.addSecs(-1));
    if (!next)
        return failed(ketplus::schedule::invalidRuleCode,
                      QStringLiteral("The once rule does not fire."));

    ScheduleRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.name = name;
    record.enabled = true;
    record.prompt = prompt;
    record.target.kind = QStringLiteral("conversation");
    record.target.sessionId = sessionId;
    record.ruleJson = QString::fromUtf8(serialize(rule));
    record.busyPolicy = QStringLiteral("queue");
    record.missedPolicy = QStringLiteral("run_once");
    record.createdBy = sessionId;
    record.createdAt = stamp(now);
    record.updatedAt = record.createdAt;
    record.nextRunAt = stamp(*next);
    QString error;
    if (!journal.createSchedule(record, &error))
        return failed("journal_error", error);
    return {{QStringLiteral("ok"), true}, {QStringLiteral("schedule"), document(record)}};
}

QJsonObject listAgentSchedules(LocalJournal& journal, const QString& sessionId) {
    ScheduleFilter filter;
    filter.createdBy = sessionId;
    const auto owned = journal.schedules(filter);
    if (!owned)
        return failed("journal_error", journal.errorString());
    QJsonArray schedules;
    for (const ScheduleRecord& row : *owned)
        schedules.append(document(row));
    return {{QStringLiteral("ok"), true}, {QStringLiteral("schedules"), schedules}};
}

QJsonObject cancelAgentSchedule(LocalJournal& journal, const QString& sessionId,
                                const QJsonObject& payload) {
    for (auto it = payload.constBegin(); it != payload.constEnd(); ++it)
        if (it.key() != QLatin1String("id"))
            return failed(ketplus::schedule::unknownFieldCode,
                          QStringLiteral("Unknown field \"%1\".").arg(it.key()));
    const QString id = payload.value(QStringLiteral("id")).toString();
    if (const auto issue = ketplus::schedule::validateScheduleId(id, QStringLiteral("id"), false);
        !issue.ok())
        return failed(issue.code.toLatin1().constData(), issue.message);
    const auto found = journal.schedule(id);
    if (!found && !journal.errorString().isEmpty())
        return failed("journal_error", journal.errorString());
    if (!found)
        return failed("schedule_not_found", QStringLiteral("Schedule was not found."));
    if (found->createdBy != sessionId)
        return failed(notOwnerCode, QStringLiteral("This schedule belongs to someone else."));
    if (found->target.kind != QLatin1String("conversation") || found->target.sessionId != sessionId)
        return failed(foreignTargetCode,
                      QStringLiteral("An agent schedule stays in its conversation."));
    const QJsonObject body = document(*found);
    QString error;
    if (!journal.deleteSchedule(id, &error))
        return failed("journal_error", error);
    return {{QStringLiteral("ok"), true}, {QStringLiteral("schedule"), body}};
}

QStringList disableAgentSchedules(LocalJournal& journal, const QString& sessionId) {
    return journal.disableAgentSchedules(sessionId);
}

} // namespace ketplus::daemon::agent_schedule
