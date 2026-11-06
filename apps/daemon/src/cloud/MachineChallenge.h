#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace ketplus::daemon::machine_challenge {
// Empty on an invalid or legacy response. Never sign relay-selected raw bytes with the
// identity key: the fixed domain binds this signature to daemon authentication only.
[[nodiscard]] QByteArray signingBytes(const QJsonObject& challenge, const QString& machineId,
                                      int keyVersion);
} // namespace ketplus::daemon::machine_challenge
