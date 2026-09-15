#include "workspace/WorkspaceFileIndex.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace ketplus {
namespace {

bool isSkippedDirectory(const QString& name) {
    static const QSet<QString> skipped{
        QStringLiteral("node_modules"), QStringLiteral("build"),       QStringLiteral("dist"),
        QStringLiteral("out"),          QStringLiteral("target"),      QStringLiteral("__pycache__"),
        QStringLiteral("venv"),         QStringLiteral("Pods"),        QStringLiteral("DerivedData"),
        QStringLiteral("vendor"),       QStringLiteral("third_party"),
    };
    // Hidden folders cover .git, .cache, .idea, .venv and similar tool state.
    return name.startsWith(QLatin1Char('.')) || skipped.contains(name);
}

} // namespace

WorkspaceFileList collectWorkspaceFiles(const QString& rootPath, const int maximumFiles) {
    WorkspaceFileList result;
    const QDir root(rootPath);
    if (!root.exists()) {
        return result;
    }

    QStringList pendingDirectories{root.absolutePath()};
    while (!pendingDirectories.isEmpty()) {
        const QDir directory(pendingDirectories.takeLast());
        const auto entries = directory.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks,
            QDir::Name);
        for (const QFileInfo& entry : entries) {
            if (entry.isDir()) {
                if (!isSkippedDirectory(entry.fileName())) {
                    pendingDirectories.append(entry.absoluteFilePath());
                }
                continue;
            }
            if (result.relativePaths.size() >= maximumFiles) {
                result.truncated = true;
                break;
            }
            result.relativePaths.append(root.relativeFilePath(entry.absoluteFilePath()));
        }
        if (result.truncated) {
            break;
        }
    }
    std::sort(result.relativePaths.begin(), result.relativePaths.end());
    return result;
}

} // namespace ketplus
