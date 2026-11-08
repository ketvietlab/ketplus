#include "agent/AgentWorkspaceView.h"

#include <QDir>
#include <QFileInfo>
#include <QUrl>

namespace ketplus {
QVariantMap AgentWorkspaceView::inspectAtlasResult(const QString& link) const {
    const QUrl url(link);
    const auto unavailable = [](const QString& reason) {
        return QVariantMap{{"available", false}, {"error", reason}};
    };
    if (!url.isValid() || !url.host().isEmpty() || url.hasQuery() || url.hasFragment() ||
        (!url.scheme().isEmpty() && !url.isLocalFile()))
        return unavailable(tr("This is not a local Atlas manifest."));
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.path();
    if (!path.endsWith(".ketatlas/atlas.json"))
        return unavailable(tr("This is not an Atlas manifest."));
    if (executionContextSwitched_ && QDir::isRelativePath(path))
        return unavailable(tr("Worktree source unavailable. Open this Atlas from the Atlas list."));
    const QString root = QFileInfo(executionPath()).canonicalFilePath();
    const QString absolute = QFileInfo(QDir(root).absoluteFilePath(path)).canonicalFilePath();
    if (root.isEmpty() || absolute.isEmpty() || !absolute.startsWith(root + QDir::separator()))
        return unavailable(tr("Atlas unavailable in this worktree."));
    if (!atlasResultInspector_)
        return unavailable(tr("Atlas actions are unavailable in this view."));
    auto result = atlasResultInspector_(absolute, root);
    result.insert("path", absolute);
    result.insert("root", root);
    return result;
}

void AgentWorkspaceView::openAtlasResult(const QString& link, const bool inBrowser) {
    // Re-resolve at click time: a manifest may have disappeared or become a symlink.
    const auto result = inspectAtlasResult(link);
    if (!result.value("available").toBool()) {
        emit composerErrorOccurred(result.value("error").toString());
        return;
    }
    emit atlasResultOpenRequested(result.value("path").toString(), result.value("root").toString(),
                                  inBrowser);
}
} // namespace ketplus
