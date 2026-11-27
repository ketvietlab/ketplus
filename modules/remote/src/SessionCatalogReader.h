#pragma once

#include "DaemonClient.h"
#include <QJsonArray>

namespace ketplus::remote {

// Streams tombstones without accumulating history, then returns a fresh live catalog.
// reset=true starts a new walk; a sink such as the CLI can discard its previous spool.
// The live sessions are read in pages (session.catalog_pages/1) and returned as one "sessions"
// array, so the catalog of a long-lived install never exceeds a frame.
void readSessionCatalog(DaemonClient* client, QObject* context, std::function<bool()> current,
                        std::function<void(const QJsonArray&, bool reset)> page,
                        DaemonClient::ResponseCallback finished);

// The live catalog only (session_catalog_live_only, ADR 0092), in the same pages.
void readLiveSessions(DaemonClient* client, QObject* context, std::function<bool()> current,
                      DaemonClient::ResponseCallback finished);

} // namespace ketplus::remote
