#pragma once

#include "storage/LocalJournal.h"

#include <QHash>
#include <QJsonArray>
#include <QString>

namespace ketplus::daemon {

// The remote command audit (#86): what paired devices did on this Mac through the relay.
// Whether a command kind is recorded. Kinds known to read are not, nor the steps of uploading an
// attachment (the message that sends it is), so a kind this build does not know is recorded.
[[nodiscard]] bool remoteCommandMayChange(const QString& kind);
// The `remote.audit` reply entries: command_id, kind, device_id, device_key_version,
// session_id, result_code and recorded_at, plus session_exists and session_title from
// `titles` (by session id), so the desktop can name a conversation or say it was deleted.
[[nodiscard]] QJsonArray remoteAuditJson(const QList<RemoteCommandAudit>& entries,
                                         const QHash<QString, QString>& titles = {});

} // namespace ketplus::daemon
