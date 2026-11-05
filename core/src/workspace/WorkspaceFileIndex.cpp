#include "workspace/WorkspaceFileIndex.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>

#include <optional>

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

// Lists what Git would show under `root`: tracked files plus untracked ones that .gitignore
// does not exclude. Nothing when `root` is not inside a working tree or Git fails.
std::optional<QStringList> gitListedPaths(const QString& root, const std::atomic_bool* cancelled) {
    QProcess git;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    git.setProcessEnvironment(environment);
    git.start(QStringLiteral("git"),
              {QStringLiteral("-c"), QStringLiteral("core.fsmonitor=false"), QStringLiteral("-C"),
               root, QStringLiteral("ls-files"), QStringLiteral("--cached"),
               QStringLiteral("--others"), QStringLiteral("--exclude-standard"),
               QStringLiteral("-z")},
              QIODevice::ReadOnly);
    if (!git.waitForStarted(2000)) {
        return std::nullopt;
    }
    QByteArray output;
    while (!git.waitForFinished(100)) {
        output += git.readAllStandardOutput();
        if (git.state() == QProcess::NotRunning) {
            break;
        }
        if (cancelled != nullptr && cancelled->load()) {
            git.kill();
            git.waitForFinished();
            return std::nullopt;
        }
    }
    output += git.readAllStandardOutput();
    if (git.exitStatus() != QProcess::NormalExit || git.exitCode() != 0) {
        return std::nullopt;
    }
    QStringList paths;
    for (const QByteArray& path : output.split('\0')) {
        if (!path.isEmpty()) {
            paths.append(QString::fromUtf8(path));
        }
    }
    // --cached lists a file once per conflict stage.
    paths.removeDuplicates();
    return paths;
}

// Appends the files under `start` (relative to `root`), skipping dependency, build and hidden
// folders. Returns false once `maximumFiles` is reached or the walk was cancelled.
bool walkFiles(const QDir& root, const QString& start, const int maximumFiles,
               const std::atomic_bool* cancelled, QStringList* relativePaths) {
    QStringList pendingDirectories{start};
    while (!pendingDirectories.isEmpty()) {
        if (cancelled != nullptr && cancelled->load()) {
            return false;
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
            if (relativePaths->size() >= maximumFiles) {
                return false;
            }
            relativePaths->append(root.relativeFilePath(entry.absoluteFilePath()));
        }
    }
    return true;
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

    // In a repository, .gitignore decides: a committed vendor/ is listed and an ignored
    // cmake-build-debug/ is not. Elsewhere, a walk skips the usual dependency and build folders.
    if (const auto listed = gitListedPaths(root.absolutePath(), cancelled)) {
        for (const QString& relative : *listed) {
            if (cancelled != nullptr && cancelled->load()) {
                result.truncated = true;
                break;
            }
            const QFileInfo entry(root.filePath(relative));
            if (entry.isSymLink()) {
                continue;
            }
            if (entry.isDir()) {
                // A submodule is listed as one entry; its files come from a walk.
                if (!walkFiles(root, entry.absoluteFilePath(), maximumFiles, cancelled,
                               &result.relativePaths)) {
                    result.truncated = true;
                    break;
                }
                continue;
            }
            if (!entry.isFile()) {
                continue; // Deleted from disk but still in the index.
            }
            if (result.relativePaths.size() >= maximumFiles) {
                result.truncated = true;
                break;
            }
            result.relativePaths.append(relative);
        }
    } else if (cancelled != nullptr && cancelled->load()) {
        result.truncated = true;
    } else {
        result.truncated =
            !walkFiles(root, root.absolutePath(), maximumFiles, cancelled, &result.relativePaths);
    }
    std::sort(result.relativePaths.begin(), result.relativePaths.end());
    return result;
}

} // namespace ketplus
