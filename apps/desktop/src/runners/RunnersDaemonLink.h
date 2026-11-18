#pragma once

#include "runners/RunnersRequester.h"

namespace ketplus {
class SettingsRunnersPage;
namespace remote {
class DaemonClient;
} // namespace remote
} // namespace ketplus

namespace ketplus::runners {

// Sends the page's runner.* and remote.status requests over the desktop's daemon client.
[[nodiscard]] Requester daemonRequester(remote::DaemonClient* daemon);
// While both live: hands every runner.pairing.changed push of `daemon` to `page`, and
// refreshes a shown page when the daemon becomes ready.
void followDaemon(remote::DaemonClient* daemon, SettingsRunnersPage* page);

} // namespace ketplus::runners
