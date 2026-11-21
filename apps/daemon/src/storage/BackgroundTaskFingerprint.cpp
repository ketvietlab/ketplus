#include "storage/BackgroundTaskLedger.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

namespace ketplus::daemon {
QByteArray backgroundTaskFingerprint(QJsonObject request, const QJsonObject& defaults) {
    auto execution = request.value("execution_spec").toObject();
    if (request.value("kind") == "agent")
        for (const auto& key : {"provider", "model", "effort"})
            if (!execution.contains(key))
                execution.insert(key, defaults.value(key));
    if (!execution.contains("cwd"))
        execution.insert("cwd", defaults.value("cwd"));
    const auto cwd = execution.value("cwd").toString();
    const auto resolved = defaults.value("resolved_cwd").toString();
    if (!resolved.isEmpty() &&
        (cwd == defaults.value("requested_cwd").toString() || cwd == resolved)) {
        // Accepted cwd/spec never follows a later symlink retarget or deletion.
        execution.insert("cwd", resolved);
    } else {
        const auto path = QDir(defaults.value("cwd").toString()).absoluteFilePath(cwd);
        const auto canonical = QFileInfo(path).canonicalFilePath();
        execution.insert("cwd", canonical.isEmpty() ? QDir::cleanPath(path) : canonical);
    }
    request.insert("execution_spec", execution);
    return QCryptographicHash::hash(QJsonDocument(request).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256);
}

} // namespace ketplus::daemon
