#pragma once
#include <QJsonObject>
#include <QString>

namespace ketplus::daemon {
// Private allocation receipt. It is deliberately not a StoredSession or transcript row.
struct SessionPreparation final {
    QString sessionId;
    QString owner;
    QString requestId;
    QString worktreePath;
    QJsonObject options;
    qint64 expiresAt{0};
    QString state{QStringLiteral("prepared")};
};
inline constexpr qint64 sessionPreparationTtlMs = 24LL * 60 * 60 * 1000;
} // namespace ketplus::daemon
