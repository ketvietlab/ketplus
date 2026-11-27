#include "BundledBrowserRuntime.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <utility>

namespace ketplus::browser_tools {

namespace {

constexpr qint64 maxManifestBytes = 64 * 1024;

// A manifest path must be relative and resolve inside the runtime root.
QString containedFile(const QDir& root, const QString& relative) {
    if (relative.isEmpty() || QDir::isAbsolutePath(relative))
        return {};
    const QFileInfo info(root.absoluteFilePath(relative));
    if (!info.isFile())
        return {};
    const QString canonical = info.canonicalFilePath();
    const QString rootPath = root.canonicalPath();
    if (canonical.isEmpty() || !canonical.startsWith(rootPath + QLatin1Char('/')))
        return {};
    return canonical;
}

} // namespace

std::optional<BundledRuntime> loadBundledBrowserRuntime(const QString& rootDir) {
    const QDir root(rootDir);
    if (rootDir.isEmpty() || !root.exists())
        return std::nullopt;
    QFile manifestFile(root.filePath(QStringLiteral("runtime.json")));
    if (!manifestFile.open(QIODevice::ReadOnly) || manifestFile.size() > maxManifestBytes)
        return std::nullopt;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    const QJsonObject manifest = document.object();
    if (manifest.value(QStringLiteral("name")).toString() != QLatin1String(bundledRuntimeDirName) ||
        !manifest.value(QStringLiteral("protocols"))
             .toArray()
             .contains(QJsonValue(QLatin1String(browserV0Protocol))))
        return std::nullopt;

    const QJsonObject entrypoints = manifest.value(QStringLiteral("entrypoints")).toObject();
#ifdef Q_OS_WIN
    const QString defaultNode = QStringLiteral("bin/node.exe");
#else
    const QString defaultNode = QStringLiteral("bin/node");
#endif
    BundledRuntime runtime;
    runtime.rootDir = root.canonicalPath();
    runtime.nodePath =
        containedFile(root, manifest.value(QStringLiteral("node")).toString(defaultNode));
    runtime.mcpEntry = containedFile(root, entrypoints.value(QStringLiteral("mcp")).toString());
    runtime.jsonRpcEntry =
        containedFile(root, entrypoints.value(QStringLiteral("jsonrpc")).toString());
    runtime.skillFile = containedFile(root, manifest.value(QStringLiteral("skill")).toString());
    runtime.version = manifest.value(QStringLiteral("version")).toString();
    runtime.nodeVersion = manifest.value(QStringLiteral("nodeVersion")).toString();
    if (runtime.nodePath.isEmpty() || !QFileInfo(runtime.nodePath).isExecutable() ||
        runtime.mcpEntry.isEmpty() || runtime.jsonRpcEntry.isEmpty() ||
        runtime.skillFile.isEmpty() || runtime.version.isEmpty())
        return std::nullopt;
    return runtime;
}

QStringList bundledBrowserRuntimeCandidates(const QString& applicationDirPath) {
    if (applicationDirPath.isEmpty())
        return {};
    const QDir appDir(applicationDirPath);
    const QString name = QLatin1String(bundledRuntimeDirName);
    QStringList candidates{
        QDir::cleanPath(appDir.filePath(QStringLiteral("../Resources/") + name)),
        QDir::cleanPath(appDir.filePath(QStringLiteral("../Helpers/") + name)),
        QDir::cleanPath(appDir.filePath(name)),
        QDir::cleanPath(appDir.filePath(QStringLiteral("../lib/ketplus/") + name)),
    };
    candidates.removeDuplicates();
    return candidates;
}

std::optional<BundledRuntime> findBundledBrowserRuntime(const QString& applicationDirPath) {
    for (const QString& candidate : bundledBrowserRuntimeCandidates(applicationDirPath)) {
        if (auto runtime = loadBundledBrowserRuntime(candidate))
            return runtime;
    }
    return std::nullopt;
}

QString defaultBrowserCacheDir() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        return {};
    return QDir(base).filePath(QStringLiteral("Ketsuite/KetPlus/browser-runtime/chrome"));
}

BundledRuntimeProvider::BundledRuntimeProvider(QString applicationDirPath, QString browserCacheDir)
    : applicationDirPath_(std::move(applicationDirPath)),
      browserCacheDir_(std::move(browserCacheDir)) {}

std::optional<ToolLaunch> BundledRuntimeProvider::resolve(const QString& toolId,
                                                          const QString& protocol) const {
    if (toolId != QLatin1String(browserToolId) || protocol != QLatin1String(mcpStdioProtocol))
        return std::nullopt;
    const auto runtime = findBundledBrowserRuntime(applicationDirPath_);
    if (!runtime)
        return std::nullopt;
    ToolLaunch launch;
    launch.command = runtime->nodePath;
    launch.args = {runtime->mcpEntry};
    launch.workingDirectory = runtime->rootDir;
    // Codex starts MCP servers with the inherited environment plus this map, which can only
    // add or override. Empty values neutralize preloads (--require/--import) and Playwright's
    // download host overrides for the bundled node (ADR 0009).
    for (const auto* name :
         {"NODE_OPTIONS", "PLAYWRIGHT_DOWNLOAD_HOST", "PLAYWRIGHT_CHROMIUM_DOWNLOAD_HOST"})
        launch.env.insert(QLatin1String(name), QString());
    if (!browserCacheDir_.isEmpty()) {
        // The Chrome for Testing cache holds an executable: owner-only.
        if (QDir().mkpath(browserCacheDir_))
            QFile::setPermissions(browserCacheDir_, QFileDevice::ReadOwner |
                                                        QFileDevice::WriteOwner |
                                                        QFileDevice::ExeOwner);
        launch.env.insert(QStringLiteral("KETPLUS_BROWSER_CACHE"), browserCacheDir_);
    }
    launch.packageVersion = runtime->version;
    launch.source = QLatin1String(sourceName);
    return launch;
}

} // namespace ketplus::browser_tools
