#include "providers/WindowsScriptLaunch.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

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

// npm's `codex.cmd` only starts node on `node_modules/@openai/codex/bin/codex.js` next to it.
// Starting node on that script ourselves keeps every argument out of cmd.exe's parser.
// Empty when this is not that shim or node cannot be found; the caller then uses cmd.exe.
QString nodeForNpmShim(const QString& executable, QString* script) {
    const QFileInfo shim(executable);
    if (shim.completeBaseName().compare(QStringLiteral("codex"), Qt::CaseInsensitive) != 0)
        return {};
    const QString entry =
        shim.absoluteDir().filePath(QStringLiteral("node_modules/@openai/codex/bin/codex.js"));
    if (!QFileInfo(entry).isFile())
        return {};
    QStringList candidates{shim.absoluteDir().filePath(QStringLiteral("node.exe")),
                           QStandardPaths::findExecutable(QStringLiteral("node"))};
    const QString programFiles = qEnvironmentVariable("ProgramFiles");
    if (!programFiles.isEmpty())
        candidates.append(QDir(programFiles).filePath(QStringLiteral("nodejs/node.exe")));
    for (const QString& candidate : candidates)
        if (!candidate.isEmpty() && QFileInfo(candidate).isFile()) {
            *script = entry;
            return candidate;
        }
    return {};
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
    if (!QFileInfo(systemRoot).isAbsolute())
        return refused(QStringLiteral("Windows SystemRoot must be an absolute path to launch a "
                                      "script wrapper."));
    const QString system = QDir::cleanPath(systemRoot + QStringLiteral("/System32"));
    const QString nativePath = QDir::toNativeSeparators(executable);
    if (powershell)
        return {system + QStringLiteral("/WindowsPowerShell/v1.0/powershell.exe"),
                QStringList{QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                            QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                            QStringLiteral("-File"), nativePath} +
                    arguments,
                {},
                {}};
    if (command) {
        QString entry;
        const QString node = nodeForNpmShim(executable, &entry);
        if (!node.isEmpty())
            return {node, QStringList{QDir::toNativeSeparators(entry)} + arguments, {}, {}};
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
