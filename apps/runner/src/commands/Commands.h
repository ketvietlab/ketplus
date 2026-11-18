#pragma once

#include "ExitCode.h"

#include <QString>
#include <QStringList>

#include <optional>

// The ketplus-runner commands. Each prints through the environment and returns its exit code.
namespace ketplus::runner::app {

struct Environment;

struct CommandOptions {
    QString command;
    QStringList positional;
    QString stateDir;
    QString tokenFile;
    std::optional<QString> name;
    QStringList labels;
    bool localOnly = false;
};

[[nodiscard]] ExitCode registerRunner(const CommandOptions& options,
                                      const Environment& environment);
[[nodiscard]] ExitCode pairDesktop(const CommandOptions& options, const Environment& environment);
[[nodiscard]] ExitCode unpairDesktop(const CommandOptions& options, const Environment& environment);
[[nodiscard]] ExitCode deregisterRunner(const CommandOptions& options,
                                        const Environment& environment);
[[nodiscard]] ExitCode showStatus(const CommandOptions& options, const Environment& environment);
[[nodiscard]] ExitCode runRunner(const CommandOptions& options, const Environment& environment);

// Parses `arguments` (without the program name) and runs the command.
[[nodiscard]] int runCli(const QStringList& arguments, const Environment& environment);
[[nodiscard]] QString helpText();

} // namespace ketplus::runner::app
