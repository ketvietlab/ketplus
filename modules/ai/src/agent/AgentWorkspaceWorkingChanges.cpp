#include "agent/AgentWorkspaceView.h"

#include <QDir>
#include <QFileInfo>

namespace ketplus {
namespace {
QString checkoutPath(const QString& path) {
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}
} // namespace

void AgentWorkspaceView::setWorkingChanges(const QString& root, const QStringList& relativePaths) {
    const QString checkout = checkoutPath(root);
    const QSet<QString> changes(relativePaths.cbegin(), relativePaths.cend());
    if (checkout == workingChangesRoot_ && changes == workingChanges_)
        return;
    workingChangesRoot_ = checkout;
    workingChanges_ = changes;
    ++workingChangesRevision_;
    emit workingChangesChanged();
}

void AgentWorkspaceView::executionCheckoutChanged() {
    ++workingChangesRevision_;
    emit workingChangesChanged();
}

bool AgentWorkspaceView::hasUncommittedChange(const QStringList& paths) const {
    if (workingChangesRoot_.isEmpty() || workingChangesRoot_ != checkoutPath(executionPath()))
        return false;
    const QDir root(workingChangesRoot_);
    for (const QString& path : paths) {
        if (path.isEmpty())
            continue;
        const QString relative =
            QDir::isAbsolutePath(path) ? root.relativeFilePath(path) : QDir::cleanPath(path);
        if (workingChanges_.contains(relative))
            return true;
    }
    return false;
}

} // namespace ketplus
