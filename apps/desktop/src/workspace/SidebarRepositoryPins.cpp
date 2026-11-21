#include "workspace/SidebarRepositoryPins.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace ketplus {
namespace {

const auto kPinnedSetting = QStringLiteral("sidebar/pinnedRepositories");

} // namespace

QString SidebarRepositoryPins::key(const QString& path) {
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(path) : canonical;
}

void SidebarRepositoryPins::load() {
    const QStringList stored = QSettings().value(kPinnedSetting).toStringList();
    paths_.clear();
    for (const QString& path : stored) {
        // A repository that was moved or deleted is dropped, without asking.
        if (path.isEmpty() || !QFileInfo(path).isDir())
            continue;
        const QString pinned = key(path);
        if (!paths_.contains(pinned))
            paths_.append(pinned);
    }
    if (paths_ != stored)
        save();
}

int SidebarRepositoryPins::order(const QString& path) const {
    if (path.isEmpty() || paths_.isEmpty())
        return -1;
    return static_cast<int>(paths_.indexOf(key(path)));
}

void SidebarRepositoryPins::pin(const QString& path) {
    const QString pinned = key(path);
    if (pinned.isEmpty() || paths_.contains(pinned))
        return;
    paths_.append(pinned);
    save();
}

void SidebarRepositoryPins::unpin(const QString& path) {
    if (paths_.removeAll(key(path)) > 0)
        save();
}

void SidebarRepositoryPins::save() const {
    if (paths_.isEmpty())
        QSettings().remove(kPinnedSetting);
    else
        QSettings().setValue(kPinnedSetting, paths_);
}

} // namespace ketplus
