#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace ketplus::tool_locations {

// Directories where developer tools usually live although a GUI app started from the
// Finder or the Start menu does not have them on PATH.
[[nodiscard]] QStringList commonToolDirectories();

// `name` on PATH, else in commonToolDirectories(); empty when it is not installed.
[[nodiscard]] QString findTool(const QString& name);

struct Command final {
    QString program;
    QStringList leadingArguments;
};

// How to start an npm-installed CLI found at `executable`, whose package is `package` and
// whose bin entry is `binName` (for example "@openai/codex" and "codex").
//
// A plain executable starts as itself. An npm `.cmd` or `.bat` shim (how Windows installs
// global packages) always runs through cmd.exe, which reinterprets quotes and `& | % ^` in
// the arguments (BatBadBut), so it is never started: the shim is replaced by
// `node <package bin script>`, read from the package's package.json next to the shim.
// nullopt when that cannot be resolved (no package.json, no bin entry, a script outside the
// package, or no node); callers report an error and do not fall back to the shim.
[[nodiscard]] std::optional<Command> npmCliCommand(const QString& executable,
                                                   const QString& package, const QString& binName);

} // namespace ketplus::tool_locations
