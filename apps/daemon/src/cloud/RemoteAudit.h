#pragma once

#include "storage/LocalJournal.h"

#include <QJsonArray>
#include <QString>

namespace ketplus::daemon {

// The remote command audit (#86): what paired devices did on this Mac through the relay.
// Whether a command kind is recorded. Only kinds known to read are not, so a kind this build
// does not know is recorded.
[[nodiscard]] bool remoteCommandMayChange(const QString& kind);
// The `remote.audit` reply entries: command_id, kind, device_id, device_key_version,
// session_id, result_code and recorded_at.
[[nodiscard]] QJsonArray remoteAuditJson(const QList<RemoteCommandAudit>& entries);

} // namespace ketplus::daemon
