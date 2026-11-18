#include "agent/ToolLocations.h"

#include <QDir>
#include <QFileInfo>
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
    if (const QString appData = qEnvironmentVariable("APPDATA"); !appData.isEmpty())
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

Command codexCommand(const QString& executable) {
#if defined(Q_OS_WIN)
    const QFileInfo shim(executable);
    const QString suffix = shim.suffix().toLower();
    if (suffix == QLatin1String("cmd") || suffix == QLatin1String("bat")) {
        const QDir directory = shim.absoluteDir();
        const QString entry =
            directory.filePath(QStringLiteral("node_modules/@openai/codex/bin/codex.js"));
        // nvm for Windows and the Node installer put node.exe next to the shim or in
        // `nodejs`; fall back to PATH.
        QString node = directory.filePath(QStringLiteral("node.exe"));
        if (!QFileInfo::exists(node))
            node = findTool(QStringLiteral("node"));
        if (QFileInfo::exists(entry) && !node.isEmpty())
            return {node, {QDir::toNativeSeparators(entry)}};
    }
#endif
    return {executable, {}};
}

} // namespace ketplus::tool_locations
