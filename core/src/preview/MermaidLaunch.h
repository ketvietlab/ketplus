#pragma once

#include <QString>
#include <QStringList>

namespace ketplus {

// How to start mmdc. The rules are for Windows, where mmdc is an npm `.cmd` shim, but the
// function is platform independent so they can be tested everywhere.
struct MermaidLaunch final {
    QString program;
    QStringList arguments;
    // Windows only: the whole command line handed to cmd.exe, bypassing Qt's quoting (which
    // does not follow cmd's rules). Empty when `arguments` are used.
    QString nativeArguments;
    QString error; // Not empty: do not start, show this.
};

// Runs mmdc.cmd/.bat as `node <cli.js> args` when the npm package next to the shim and a node
// binary are found (no cmd.exe at all). Otherwise runs `cmd /D /S /C ""shim" "arg" ..."`, which
// is only safe for paths without `%`, `"` and line breaks; those fail with an error because cmd
// cannot take them literally on a command line. `nodeCandidates` are tried in order.
[[nodiscard]] MermaidLaunch mermaidWindowsLaunch(const QString& shellPath, const QString& shim,
                                                 const QStringList& arguments,
                                                 const QStringList& nodeCandidates);

// The command line `"program" "arg" ...` for use after `cmd /S /C "`; empty with `ok` false
// when a value holds a character that cannot be quoted for cmd.
[[nodiscard]] QString cmdCommandLine(const QString& program, const QStringList& arguments,
                                     bool* ok);

} // namespace ketplus
