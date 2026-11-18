#pragma once

#include <QProcessEnvironment>
#include <QString>

namespace ketplus {

// The command line that runs a file in the user's terminal shell. The quoting depends on the
// shell actually running, so it is kept apart from the terminal widgets and testable anywhere.
enum class ShellKind { Posix, PowerShell, Cmd };

// By the shell's file name, without .exe/.cmd/.bat/.com: powershell / pwsh (PowerShell), cmd
// (cmd.exe), anything else Posix.
[[nodiscard]] ShellKind shellKindFor(const QString& shellPath);

struct RunCommand final {
    QString line;  // Without the final Return.
    QString error; // Not empty: nothing must be typed.
};

// `interpreter` is the shell path used to run a file that is not directly executable on POSIX
// (`sh file`); Windows runs such a file by its path, as the shell would.
//
// POSIX: single quotes, as it always was (nothing is refused). PowerShell and cmd show the path
// with native separators and refuse control characters (a typed line break would run the line).
// PowerShell: `& '<path>'` with ' (and the typographic quotes it treats
// as ') doubled. cmd: `"<path>"`; a path with a double quote or line break, or a `%name%` that
// cmd would expand (a variable of `environment`, or one cmd defines itself), is refused,
// because cmd cannot take those literally on a command line.
[[nodiscard]] RunCommand
runCommandFor(ShellKind kind, const QString& interpreter, const QString& absolutePath,
              bool directlyExecutable,
              const QProcessEnvironment& environment = QProcessEnvironment::systemEnvironment());

} // namespace ketplus
