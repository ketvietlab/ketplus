#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

// Helpers shared by the CloudControlClient translation units.
namespace ketplus::daemon::cloud_control {

inline QJsonObject object(const QJsonValue& value) {
    return value.isObject() ? value.toObject() : QJsonObject{};
}

inline QString responseCode(const QJsonValue& value, const QString& fallback) {
    return object(value).value(QStringLiteral("code")).toString(fallback);
}

// The capabilities this daemon lists in each heartbeat. `events.forward_only/1` tells the service
// that the daemon follows its answer: while the answer lists it, events reach clients only as
// frames and the daemon uploads none.
inline QJsonArray daemonCapabilities() {
    return QJsonArray{"transport.webrtc/1", "teams.remote/1", "session.prepare/1", "runner.relay/1",
                      "events.forward_only/1"};
}

// Keyed with a per-installation random salt so the relay cannot confirm guessed paths.
// An empty salt (unreadable credential store) yields an empty id, which never matches a scope.
[[nodiscard]] QString opaquePathId(const QByteArray& kind, const QString& path,
                                   const QByteArray& salt);

// A deleted worktree keeps the repository recorded for it, so its conversations and any
// command naming them keep the scope they were published under.
[[nodiscard]] QJsonObject sourceContext(const QString& worktreePath, const QByteArray& salt,
                                        const QString& recordedRepository = {});

} // namespace ketplus::daemon::cloud_control
