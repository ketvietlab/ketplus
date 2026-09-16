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

bool hasBinaryFileSuffix(const QString& path) {
    static const QSet<QString> binarySuffixes{
        // Images
        QStringLiteral("jpg"),    QStringLiteral("jpeg"),  QStringLiteral("png"),
        QStringLiteral("gif"),    QStringLiteral("bmp"),   QStringLiteral("ico"),
        QStringLiteral("icns"),   QStringLiteral("webp"),  QStringLiteral("avif"),
        QStringLiteral("heic"),   QStringLiteral("tif"),   QStringLiteral("tiff"),
        QStringLiteral("psd"),    QStringLiteral("ai"),    QStringLiteral("sketch"),
        QStringLiteral("svgz"),
        // Documents and archives
        QStringLiteral("pdf"),    QStringLiteral("doc"),   QStringLiteral("docx"),
        QStringLiteral("xls"),    QStringLiteral("xlsx"),  QStringLiteral("ppt"),
        QStringLiteral("pptx"),   QStringLiteral("odt"),   QStringLiteral("zip"),
        QStringLiteral("gz"),     QStringLiteral("tgz"),   QStringLiteral("bz2"),
        QStringLiteral("xz"),     QStringLiteral("zst"),   QStringLiteral("7z"),
        QStringLiteral("rar"),    QStringLiteral("jar"),   QStringLiteral("war"),
        // Media
        QStringLiteral("mp3"),    QStringLiteral("wav"),   QStringLiteral("ogg"),
        QStringLiteral("flac"),   QStringLiteral("m4a"),   QStringLiteral("mp4"),
        QStringLiteral("m4v"),    QStringLiteral("mov"),   QStringLiteral("avi"),
        QStringLiteral("mkv"),    QStringLiteral("webm"),  QStringLiteral("wmv"),
        // Fonts
        QStringLiteral("woff"),   QStringLiteral("woff2"), QStringLiteral("ttf"),
        QStringLiteral("otf"),    QStringLiteral("eot"),
        // Compiled and opaque data
        QStringLiteral("exe"),    QStringLiteral("dll"),   QStringLiteral("so"),
        QStringLiteral("dylib"),  QStringLiteral("a"),     QStringLiteral("o"),
        QStringLiteral("obj"),    QStringLiteral("lib"),   QStringLiteral("pdb"),
        QStringLiteral("class"),  QStringLiteral("pyc"),   QStringLiteral("wasm"),
        QStringLiteral("bin"),    QStringLiteral("dat"),   QStringLiteral("db"),
        QStringLiteral("sqlite"), QStringLiteral("sqlite3"), QStringLiteral("dmg"),
        QStringLiteral("iso"),    QStringLiteral("pack"),  QStringLiteral("idx")};

    const qsizetype dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot + 1 >= path.size()) {
        return false;
    }
    // A dot in a folder name says nothing about the file itself.
    const qsizetype separator =
        qMax(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    if (dot < separator) {
        return false;
    }
    return binarySuffixes.contains(path.mid(dot + 1).toLower());
}

WorkspaceFileList collectWorkspaceFiles(const QString& rootPath, const int maximumFiles,
                                        const std::atomic_bool* cancelled) {
    WorkspaceFileList result;
    const QDir root(rootPath);
    if (!root.exists()) {
        return result;
    }

    QStringList pendingDirectories{root.absolutePath()};
    while (!pendingDirectories.isEmpty()) {
        if (cancelled != nullptr && cancelled->load()) {
            result.truncated = true;
            break;
        }
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
