#pragma once

#include <QJsonObject>
#include <QProcessEnvironment>
#include <QStringList>

#include <functional>
#include <optional>

namespace ketplus::daemon::copilot_cli {

enum class Platform { MacOS, Linux, Windows };

// Discovery has no implicit dependency on the process environment, cwd or filesystem.
struct DiscoveryContext final {
    Platform platform;
    QProcessEnvironment environment;
    QString homeDirectory;
    std::function<bool(const QString&)> isRunnableFile;
};

struct DiscoveryResult final {
    QString executable;
    QString error;
};

[[nodiscard]] DiscoveryContext systemDiscoveryContext();
[[nodiscard]] QStringList wellKnownCandidates(const DiscoveryContext& context);
// Overrides must be absolute file paths. Invalid overrides do not fall through to discovery.
[[nodiscard]] DiscoveryResult findExecutable(const QString& explicitPath,
                                             const DiscoveryContext& context);
[[nodiscard]] QString missingExecutableMessage();

struct ProcessInvocation final {
    QString program;
    QStringList arguments;
    // On Windows, pass this to QProcess::setNativeArguments as well as setArguments.
    // cmd.exe requires its quoted command to survive Qt's normal argument escaping.
    QString nativeArguments;
    QString error;
};

// Windows script wrappers require the daemon's environment to locate SystemRoot/System32;
// never resolve cmd.exe or powershell.exe through the conversation's cwd.
[[nodiscard]] ProcessInvocation processInvocation(const QString& executable, Platform platform,
                                                  const QProcessEnvironment& environment = {});

struct Failure final {
    QJsonObject acpError;
    QString standardError;
    std::optional<int> exitCode;
};

// A generic ACP error/exit code is not an authentication signal. stderr is considered only
// after an unsuccessful exit; callers may inspect an explicit ACP failure before process exit.
[[nodiscard]] bool isAuthenticationFailure(const Failure& failure);
[[nodiscard]] QString notAuthenticatedMessage();
// Empty for unrelated failures, allowing the adapter to retain its normal error handling.
[[nodiscard]] QString authenticationGuidance(const Failure& failure);

} // namespace ketplus::daemon::copilot_cli
