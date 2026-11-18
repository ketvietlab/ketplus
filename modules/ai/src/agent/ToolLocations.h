#pragma once

#include <QString>
#include <QStringList>

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

// How to start the Codex CLI found at `executable`. Windows installs it as an npm `.cmd`
// shim, which always runs through cmd.exe and would reinterpret quotes and `& | % ^` in
// the arguments (BatBadBut); the shim is replaced by `node <package>/bin/codex.js`.
// Elsewhere, and when the package entry point or node cannot be found, the executable
// itself is the program.
[[nodiscard]] Command codexCommand(const QString& executable);

} // namespace ketplus::tool_locations
