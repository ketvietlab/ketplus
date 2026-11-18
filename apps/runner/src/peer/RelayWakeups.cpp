#include "peer/RelayWakeups.h"

#include <QStringList>

#include <optional>

namespace ketplus::runner::peer {
namespace {

std::optional<QStringList> fields(const QJsonObject& message, const QStringList& names) {
    QStringList values;
    values.reserve(names.size());
    for (const QString& name : names) {
        const QJsonValue value = message.value(name);
        if (!value.isString() || value.toString().isEmpty())
            return std::nullopt;
        values.append(value.toString());
    }
    return values;
}

} // namespace

WakeupResult dispatchWakeup(const QJsonObject& message, WakeupListener& listener) {
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QLatin1StringView("job.available")) {
        const auto v = fields(message, {QStringLiteral("jobId")});
        if (!v)
            return WakeupResult::Malformed;
        listener.jobAvailable(v->at(0));
    } else if (type == QLatin1StringView("job.cancel")) {
        const auto v = fields(message, {QStringLiteral("jobId"), QStringLiteral("leaseId")});
        if (!v)
            return WakeupResult::Malformed;
        listener.jobCancel(v->at(0), v->at(1));
    } else if (type == QLatin1StringView("runner.paired")) {
        const auto v = fields(message, {QStringLiteral("ticketId"), QStringLiteral("pairState"),
                                        QStringLiteral("approvalSignature")});
        if (!v)
            return WakeupResult::Malformed;
        listener.runnerPaired(v->at(0), v->at(1), v->at(2));
    } else if (type == QLatin1StringView("runner.unpaired")) {
        const auto v = fields(message, {QStringLiteral("machineId")});
        if (!v)
            return WakeupResult::Malformed;
        listener.runnerUnpaired(v->at(0));
    } else {
        return WakeupResult::NotWakeup;
    }
    return WakeupResult::Delivered;
}

} // namespace ketplus::runner::peer
