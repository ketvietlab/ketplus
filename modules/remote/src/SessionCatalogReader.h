#pragma once

#include "DaemonClient.h"
#include <QJsonArray>

namespace ketplus::remote {

// Streams tombstones without accumulating history, then returns a fresh live catalog.
// reset=true starts a new walk; a sink such as the CLI can discard its previous spool.
void readSessionCatalog(DaemonClient* client, QObject* context, std::function<bool()> current,
                        std::function<void(const QJsonArray&, bool reset)> page,
                        DaemonClient::ResponseCallback finished);

} // namespace ketplus::remote
