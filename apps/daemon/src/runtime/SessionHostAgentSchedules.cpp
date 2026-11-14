#include "runtime/SessionHost.h"

#include "agents/AgentSchedule.h"
#include "protocol/ScheduleProtocol.h"
#include "storage/LocalJournal.h"

#include <QDateTime>

namespace ketplus::daemon {

QJsonObject SessionHost::agentSchedule(const QString& sessionId, const QString& operation,
                                       const QJsonObject& payload) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject result;
    if (operation == QLatin1String("schedule_create"))
        result = agent_schedule::createAgentSchedule(*journal_, sessionId, payload, now);
    else if (operation == QLatin1String("schedule_list"))
        result = agent_schedule::listAgentSchedules(*journal_, sessionId);
    else if (operation == QLatin1String("schedule_cancel"))
        result = agent_schedule::cancelAgentSchedule(*journal_, sessionId, payload);
    else {
        return {
            {QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), QStringLiteral("unsupported_operation")},
            {QStringLiteral("error_message"), QStringLiteral("Unknown agent schedule operation.")}};
    }
    if (result.value(QStringLiteral("ok")).toBool() &&
        operation != QLatin1String("schedule_list")) {
        ::ketplus::schedule::Failure failure;
        const auto document = ::ketplus::schedule::scheduleFromJson(
            result.value(QStringLiteral("schedule")).toObject(), &failure);
        if (document)
            emit scheduleEvent(
                QString::fromLatin1(protocol::scheduleChangedEvent),
                protocol::scheduleChangedPayload(operation == QLatin1String("schedule_create")
                                                     ? QStringLiteral("created")
                                                     : QStringLiteral("deleted"),
                                                 *document));
    }
    return result;
}

void SessionHost::disableAgentSchedules(const QString& sessionId) {
    const QStringList ids = agent_schedule::disableAgentSchedules(*journal_, sessionId);
    for (const QString& id : ids) {
        const auto record = journal_->schedule(id);
        if (!record)
            continue;
        ::ketplus::schedule::Schedule schedule;
        schedule.id = record->id;
        schedule.name = record->name;
        schedule.enabled = record->enabled;
        schedule.prompt = record->prompt;
        schedule.target.kind = ::ketplus::schedule::TargetKind::Conversation;
        schedule.target.sessionId = record->target.sessionId;
        schedule.rule = record->ruleJson;
        schedule.createdBy = record->createdBy;
        schedule.createdAt = record->createdAt;
        schedule.updatedAt = record->updatedAt;
        schedule.nextRunAt = record->nextRunAt;
        schedule.lastRunAt = record->lastRunAt;
        emit scheduleEvent(QString::fromLatin1(protocol::scheduleChangedEvent),
                           protocol::scheduleChangedPayload(QStringLiteral("updated"), schedule));
    }
}

} // namespace ketplus::daemon
