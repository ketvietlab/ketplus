#include "cloud/RemoteAudit.h"

#include <QJsonObject>
#include <QSet>

namespace ketplus::daemon {

bool remoteCommandMayChange(const QString& kind) {
    static const QSet<QString> reads{QStringLiteral("transcript.restore_window"),
                                     QStringLiteral("workspace.files.tree"),
                                     QStringLiteral("workspace.files.search"),
                                     QStringLiteral("workspace.files.file"),
                                     QStringLiteral("workspace.files.diff"),
                                     QStringLiteral("worktree.branches.read"),
                                     QStringLiteral("worktree.configuration.read"),
                                     QStringLiteral("session.configuration.read"),
                                     QStringLiteral("session.worktrees"),
                                     QStringLiteral("session.resource.resolve"),
                                     QStringLiteral("atlas.list")};
    return !reads.contains(kind);
}

QJsonArray remoteAuditJson(const QList<RemoteCommandAudit>& entries) {
    QJsonArray result;
    for (const RemoteCommandAudit& entry : entries) {
        result.append(QJsonObject{
            {QStringLiteral("command_id"), entry.commandId},
            {QStringLiteral("kind"), entry.kind},
            {QStringLiteral("device_id"), entry.deviceId},
            {QStringLiteral("device_key_version"), entry.deviceKeyVersion},
            {QStringLiteral("session_id"), entry.sessionId},
            {QStringLiteral("result_code"), entry.resultCode},
            {QStringLiteral("recorded_at"), entry.recordedAt.toUTC().toString(Qt::ISODateWithMs)}});
    }
    return result;
}

} // namespace ketplus::daemon
