#include "ToolPackageRegistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace ketplus::browser_tools {

SourceCheckoutProvider::SourceCheckoutProvider(Settings settings, QString searchStart)
    : settings_(std::move(settings)), searchStart_(std::move(searchStart)) {}

std::optional<ToolLaunch> SourceCheckoutProvider::resolve(const QString& toolId,
                                                          const QString& protocol) const {
    if (toolId != QLatin1String(browserToolId) || protocol != QLatin1String(mcpStdioProtocol))
        return std::nullopt;
    const QString appPath = settings_.browserAppPath.isEmpty() ? detectBrowserAppPath(searchStart_)
                                                               : settings_.browserAppPath;
    if (appPath.isEmpty())
        return std::nullopt;
    const QDir app(appPath);
    const QString entry = app.absoluteFilePath(QStringLiteral("src/mcp.mjs"));
    const QDir modules(app.filePath(QStringLiteral("node_modules")));
    if (!QFileInfo::exists(entry) ||
        !QFileInfo::exists(modules.filePath(QStringLiteral("@modelcontextprotocol/server"))) ||
        !QFileInfo::exists(modules.filePath(QStringLiteral("playwright-core"))) ||
        !QFileInfo(settings_.nodePath).isExecutable())
        return std::nullopt;
    ToolLaunch launch;
    launch.command = settings_.nodePath;
    launch.args = {entry};
    launch.workingDirectory = app.absolutePath();
    launch.source = QLatin1String(sourceName);
    QFile manifest(app.filePath(QStringLiteral("package.json")));
    if (manifest.open(QIODevice::ReadOnly) && manifest.size() <= 1024 * 1024)
        launch.packageVersion = QJsonDocument::fromJson(manifest.readAll())
                                    .object()
                                    .value(QStringLiteral("version"))
                                    .toString();
    return launch;
}

void ToolPackageRegistry::addProvider(std::unique_ptr<ToolPackageProvider> provider) {
    if (provider)
        providers_.push_back(std::move(provider));
}

std::optional<ToolLaunch> ToolPackageRegistry::resolve(const QString& toolId,
                                                       const QString& protocol) const {
    for (const auto& provider : providers_) {
        if (auto launch = provider->resolve(toolId, protocol))
            return launch;
    }
    return std::nullopt;
}

} // namespace ketplus::browser_tools
