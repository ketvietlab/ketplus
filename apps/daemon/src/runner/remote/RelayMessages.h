#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

// Which part of the daemon a message of the Relay socket belongs to. Runner signaling goes to the
// runner links, a job.changed hint to the remote job records, and everything else, device
// signaling included, keeps its existing path.
namespace ketplus::daemon::runner::remote {

enum class RelayRoute {
    Device,     // the existing path: device signaling and every other message
    RunnerLink, // a runner offer, or signaling of a connection the runner links own
    JobChanged, // a hint that a remote job of this machine changed
};

// `ownsConnection` answers whether the runner links hold a connection id.
[[nodiscard]] RelayRoute
relayRoute(const QJsonObject& message,
           const std::function<bool(const QString& connectionId)>& ownsConnection);

} // namespace ketplus::daemon::runner::remote
