#include "agent/AgentWorkspaceView.h"
#include "app/MainWindow.h"
#include "atlas/ManagedAtlasViewers.h"
#include "workspace/AtlasIndex.h"

#include <QFileInfo>
#include <QStatusBar>

namespace ketplus {
void MainWindow::connectAtlasResults(AgentWorkspaceView* chat) {
    connect(ManagedAtlasViewers::instance(), &ManagedAtlasViewers::changed, chat,
            &AgentWorkspaceView::atlasResultsChanged);
    chat->setAtlasResultInspector([](const QString& path, const QString& root) {
        const auto atlas = AtlasIndex::readManifest(path, root);
        if (atlas.path.isEmpty())
            return QVariantMap{{"available", false},
                               {"error", tr("Atlas manifest is missing, invalid or too large.")}};
        return QVariantMap{{"available", true},
                           {"title", atlas.title},
                           {"screens", atlas.screens.size()},
                           {"flows", atlas.flowCount},
                           {"relativePath", atlas.relativePath},
                           {"viewerError", ManagedAtlasViewers::instance()->errorFor(atlas.path)}};
    });
    connect(chat, &AgentWorkspaceView::atlasResultOpenRequested, this,
            [this, chat](const QString& path, const QString& root, bool inBrowser) {
                // Reject stale actions after navigation. Never move a result into another repo.
                if (QFileInfo(chat->executionPath()).canonicalFilePath() != root ||
                    QFileInfo(workspaceRoot_).canonicalFilePath() != root) {
                    statusBar()->showMessage(tr("Open the Atlas in its original worktree."));
                    return;
                }
                const auto atlas = AtlasIndex::readManifest(path, root);
                if (atlas.path.isEmpty()) {
                    statusBar()->showMessage(tr("Atlas manifest is no longer available."));
                    return;
                }
                if (inBrowser)
                    ManagedAtlasViewers::instance()->open(atlas);
                else
                    editAtlasInKetPlus(atlas);
            });
}
} // namespace ketplus
