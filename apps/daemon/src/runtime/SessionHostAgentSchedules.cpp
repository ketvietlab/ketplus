#include "runtime/SessionHost.h"

#include "agents/AgentSchedule.h"
#include "protocol/ScheduleProtocol.h"
#include "runtime/ScheduleFromRecord.h"
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
        // The runner may already have disarmed. A new or removed schedule has to re-arm it,
        // or the only schedule never fires and a nearer one waits for the old alarm.
        noteSchedulesChanged();
    }
    return result;
}

void SessionHost::disableAgentSchedules(const QString& sessionId) {
    const QStringList ids = agent_schedule::disableAgentSchedules(*journal_, sessionId);
    for (const QString& id : ids) {
        const auto record = journal_->schedule(id);
        if (!record)
            continue;
        ::ketplus::schedule::Failure failure;
        const auto schedule = scheduleFromRecord(*record, &failure);
        if (!schedule)
            continue;
        emit scheduleEvent(QString::fromLatin1(protocol::scheduleChangedEvent),
                           protocol::scheduleChangedPayload(QStringLiteral("updated"), *schedule));
    }
    if (!ids.isEmpty())
        noteSchedulesChanged();
}

} // namespace ketplus::daemon
