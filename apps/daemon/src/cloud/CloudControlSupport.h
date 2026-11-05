#pragma once

#include <QByteArray>
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

// Keyed with a per-installation random salt so the relay cannot confirm guessed paths.
// An empty salt (unreadable credential store) yields an empty id, which never matches a scope.
[[nodiscard]] QString opaquePathId(const QByteArray& kind, const QString& path,
                                   const QByteArray& salt);

// A deleted worktree keeps the repository recorded for it, so its conversations and any
// command naming them keep the scope they were published under.
[[nodiscard]] QJsonObject sourceContext(const QString& worktreePath, const QByteArray& salt,
                                        const QString& recordedRepository = {});

} // namespace ketplus::daemon::cloud_control
