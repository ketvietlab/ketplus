#include "cloud/RemoteAudit.h"

#include <QHash>
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
                                     QStringLiteral("session.task.output"),
                                     QStringLiteral("session.pending_approvals"),
                                     QStringLiteral("session.output.read"),
                                     QStringLiteral("session.media.read"),
                                     QStringLiteral("atlas.list")};
    // Uploading an attachment takes a command per chunk; the message that sends it is recorded.
    return !reads.contains(kind) && !kind.startsWith(QStringLiteral("attachment.stage."));
}

QJsonArray remoteAuditJson(const QList<RemoteCommandAudit>& entries,
                           const QHash<QString, QString>& titles) {
    QJsonArray result;
    for (const RemoteCommandAudit& entry : entries) {
        const auto title = titles.constFind(entry.sessionId);
        result.append(QJsonObject{
            {QStringLiteral("session_exists"), title != titles.cend()},
            {QStringLiteral("session_title"), title != titles.cend() ? *title : QString()},
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
