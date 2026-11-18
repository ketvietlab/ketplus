#include "runner/remote/RelayMessages.h"

#include "runner/link/OfferCheck.h"

namespace ketplus::daemon::runner::remote {

RelayRoute relayRoute(const QJsonObject& message,
                      const std::function<bool(const QString& connectionId)>& ownsConnection) {
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QLatin1StringView("job.changed"))
        return message.value(QStringLiteral("jobId")).isString() ? RelayRoute::JobChanged
                                                                 : RelayRoute::Device;
    if (type == QLatin1StringView("signal.offer"))
        return link::isRunnerOffer(message) ? RelayRoute::RunnerLink : RelayRoute::Device;
    if (type == QLatin1StringView("signal.candidate") || type == QLatin1StringView("signal.end") ||
        type == QLatin1StringView("signal.error")) {
        const QJsonValue id = message.value(QStringLiteral("connectionId"));
        if (id.isString() && ownsConnection && ownsConnection(id.toString()))
            return RelayRoute::RunnerLink;
    }
    return RelayRoute::Device;
}

} // namespace ketplus::daemon::runner::remote
