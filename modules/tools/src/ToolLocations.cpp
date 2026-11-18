#include "ToolLocations.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace ketplus::tool_locations {

QStringList commonToolDirectories() {
#if defined(Q_OS_WIN)
    QStringList directories;
    for (const char* variable : {"ProgramFiles", "ProgramFiles(x86)"}) {
        const QString base = qEnvironmentVariable(variable);
        if (base.isEmpty())
            continue;
        directories << QDir(base).filePath(QStringLiteral("Git/cmd"))
                    << QDir(base).filePath(QStringLiteral("nodejs"));
    }
    if (const QString appData = qEnvironmentVariable("APPDATA");
        !appData.isEmpty() && QDir::isAbsolutePath(appData))
        directories << QDir(appData).filePath(QStringLiteral("npm"));
    return directories;
#else
    return {QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin")};
#endif
}

QString findTool(const QString& name) {
    const QString onPath = QStandardPaths::findExecutable(name);
    return onPath.isEmpty() ? QStandardPaths::findExecutable(name, commonToolDirectories())
                            : onPath;
}

namespace {

QString binScript(const QDir& packageDirectory, const QString& binName) {
    QFile file(packageDirectory.filePath(QStringLiteral("package.json")));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonValue bin =
        QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("bin"));
    const QString relative =
        bin.isString() ? bin.toString() : bin.toObject().value(binName).toString();
    if (relative.isEmpty())
        return {};
    const QString script = QDir::cleanPath(packageDirectory.filePath(relative));
    const QString root = QDir::cleanPath(packageDirectory.absolutePath()) + QLatin1Char('/');
    return script.startsWith(root) && QFileInfo(script).isFile() ? script : QString();
}

} // namespace

std::optional<Command> npmCliCommand(const QString& executable, const QString& package,
                                     const QString& binName) {
    const QFileInfo shim(executable);
    const QString suffix = shim.suffix().toLower();
    if (suffix != QLatin1String("cmd") && suffix != QLatin1String("bat"))
        return Command{executable, {}};
    const QDir directory = shim.absoluteDir();
    const QString script =
        binScript(QDir(directory.filePath(QStringLiteral("node_modules/") + package)), binName);
    if (script.isEmpty())
        return std::nullopt;
    // nvm for Windows and the Node installer keep node.exe beside the global bin directory
    // or in `nodejs`; the PATH comes last.
    QString node = directory.filePath(QStringLiteral("node.exe"));
    if (!QFileInfo::exists(node))
        node = findTool(QStringLiteral("node"));
    if (node.isEmpty())
        return std::nullopt;
    return Command{node, {QDir::toNativeSeparators(script)}};
}

} // namespace ketplus::tool_locations
