#pragma once

#include <QString>
#include <QStringList>

namespace ketplus::daemon {
// Launch description for a provider CLI on any host. On Windows an npm-style `.cmd`/`.bat`
// shim is started through `cmd.exe` with `nativeArguments`, because Qt's normal quoting does
// not follow cmd.exe's parsing; a `.ps1` goes through PowerShell. Everything else, and every
// other host, launches the file directly.
struct ScriptLaunch final {
    QString program;
    QStringList arguments;
    // Apply with QProcess::setNativeArguments on Windows; empty otherwise.
    QString nativeArguments;
    // Set when the command cannot be passed safely; the other fields are then empty.
    QString error;
};

// `windows` and `systemRoot` are parameters so the policy is testable on any host;
// production callers use scriptLaunchForHost().
[[nodiscard]] ScriptLaunch scriptLaunch(const QString& executable, const QStringList& arguments,
                                        bool windows, const QString& systemRoot);
[[nodiscard]] ScriptLaunch scriptLaunchForHost(const QString& executable,
                                               const QStringList& arguments);
} // namespace ketplus::daemon
