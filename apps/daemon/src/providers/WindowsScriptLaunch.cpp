#include "providers/WindowsScriptLaunch.h"

#include "ToolLocations.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace ketplus::daemon {
namespace {
// cmd.exe expands %VAR% even inside quotes and treats a quote as a boundary it cannot escape
// reliably; refuse rather than run a different command than the one asked for.
bool unsafeForCmd(const QString& text) {
    return text.contains(QLatin1Char('"')) || text.contains(QLatin1Char('%')) ||
           text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r')) ||
           text.contains(QChar::Null);
}

QString quoted(const QString& text) {
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9_.:=/\\\\,+@-]+$"));
    return plain.match(text).hasMatch() ? text : QStringLiteral("\"%1\"").arg(text);
}

QString windowsPath(QString path) {
    path.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return path;
}

// A Windows path, judged the same on every host so the policy is testable anywhere.
bool windowsAbsolute(const QString& path) {
    static const QRegularExpression drive(QStringLiteral("^[A-Za-z]:[\\\\/]"));
    return drive.match(path).hasMatch() || path.startsWith(QStringLiteral("\\\\"));
}

ScriptLaunch refused(const QString& message) { return {{}, {}, {}, message}; }
} // namespace

ScriptLaunch scriptLaunch(const QString& executable, const QStringList& arguments,
                          const bool windows, const QString& systemRoot) {
    if (!windows)
        return {executable, arguments, {}, {}};
    const QString suffix = QFileInfo(executable).suffix().toLower();
    const bool command = suffix == QStringLiteral("cmd") || suffix == QStringLiteral("bat");
    const bool powershell = suffix == QStringLiteral("ps1");
    if (!command && !powershell)
        return {executable, arguments, {}, {}};
    if (!windowsAbsolute(systemRoot))
        return refused(QStringLiteral("Windows SystemRoot must be an absolute path to launch a "
                                      "script wrapper."));
    const QString system = QDir::cleanPath(systemRoot + QStringLiteral("/System32"));
    const QString nativePath = windowsPath(executable);
    if (powershell)
        return {system + QStringLiteral("/WindowsPowerShell/v1.0/powershell.exe"),
                QStringList{QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                            QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                            QStringLiteral("-File"), nativePath} +
                    arguments,
                {},
                {}};
    if (command && QFileInfo(executable)
                           .completeBaseName()
                           .compare(QStringLiteral("codex"), Qt::CaseInsensitive) == 0) {
        // npm's shim only starts node on the package's script: start that directly, so no
        // argument reaches cmd.exe's parser (the shared rule in modules/tools). There is no
        // fallback to the shim; it is refused when node.exe or the script cannot be found.
        const auto npm = tool_locations::npmCliCommand(executable, QStringLiteral("@openai/codex"),
                                                       QStringLiteral("codex"));
        if (!npm)
            return refused(QStringLiteral(
                "Codex's npm command wrapper cannot be started safely because node.exe or the "
                "package's script was not found next to it. Point the provider at codex.exe or "
                "reinstall @openai/codex."));
        return {npm->program, npm->leadingArguments + arguments, {}, {}};
    }
    if (unsafeForCmd(nativePath))
        return refused(QStringLiteral("A Windows command-wrapper path cannot contain percent or "
                                      "quote characters. Point the provider at its .exe instead."));
    QStringList words;
    for (const QString& argument : arguments) {
        if (unsafeForCmd(argument))
            return refused(QStringLiteral("This provider is a .cmd wrapper, which cannot carry "
                                          "arguments containing quotes or percent signs. Point "
                                          "the provider at its .exe instead."));
        words.append(quoted(argument));
    }
    // cmd /d /v:off /s /c ""path" args": /s keeps the outer quotes as a pair, /v:off disables
    // delayed expansion, /d skips AutoRun.
    return {system + QStringLiteral("/cmd.exe"),
            {QStringLiteral("/d"), QStringLiteral("/v:off"), QStringLiteral("/s"),
             QStringLiteral("/c")},
            QStringLiteral("\"\"%1\" %2\"").arg(nativePath, words.join(QLatin1Char(' '))),
            {}};
}

ScriptLaunch scriptLaunchForHost(const QString& executable, const QStringList& arguments) {
#if defined(Q_OS_WIN)
    return scriptLaunch(executable, arguments, true, qEnvironmentVariable("SystemRoot"));
#else
    return scriptLaunch(executable, arguments, false, {});
#endif
}
} // namespace ketplus::daemon
