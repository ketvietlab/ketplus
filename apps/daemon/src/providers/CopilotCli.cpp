#include "providers/CopilotCli.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace ketplus::daemon::copilot_cli {
namespace {

QString normalizedPath(QString path, Platform platform) {
    if (platform == Platform::Windows)
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const bool unc = platform == Platform::Windows && path.startsWith(QStringLiteral("//"));
    path = QDir::cleanPath(path);
    // Keep Windows UNC semantics when the resolver is tested on a Unix host.
    if (unc && !path.startsWith(QStringLiteral("//")))
        path.prepend(QLatin1Char('/'));
    return path;
}

bool isAbsolute(const QString& path, Platform platform) {
    if (platform != Platform::Windows)
        return path.startsWith(QLatin1Char('/'));
    return path.startsWith(QStringLiteral("//")) ||
           (path.size() >= 3 && path.at(0).isLetter() && path.at(1) == QLatin1Char(':') &&
            path.at(2) == QLatin1Char('/'));
}

QString environmentValue(const DiscoveryContext& context, const QString& key) {
    if (context.platform == Platform::Windows) {
        for (const auto& candidate : context.environment.keys())
            if (candidate.compare(key, Qt::CaseInsensitive) == 0)
                return context.environment.value(candidate);
    }
    return context.environment.value(key);
}

QString joinedPath(const QString& directory, const QString& name, Platform platform) {
    if (directory.isEmpty())
        return {};
    return normalizedPath(directory + QLatin1Char('/') + name, platform);
}

QStringList pathNames(const DiscoveryContext& context) {
    if (context.platform != Platform::Windows)
        return {QStringLiteral("copilot")};
    const QStringList supported{QStringLiteral(".exe"), QStringLiteral(".com"),
                                QStringLiteral(".bat"), QStringLiteral(".cmd"),
                                QStringLiteral(".ps1")};
    QStringList extensions;
    for (const auto& extension : environmentValue(context, QStringLiteral("PATHEXT"))
                                     .toLower()
                                     .split(QLatin1Char(';'), Qt::SkipEmptyParts))
        if (supported.contains(extension) && !extensions.contains(extension))
            extensions.append(extension);
    // Include supported script wrappers even when a GUI process has no PATHEXT.
    for (const auto& extension : supported)
        if (!extensions.contains(extension))
            extensions.append(extension);
    QStringList names;
    for (const auto& extension : extensions)
        names.append(QStringLiteral("copilot") + extension);
    return names;
}

bool hasAuthDiagnostic(const QString& text) {
    // Deliberately anchored: model, proxy and network failures can mention authentication
    // or suggest login without being a signed-out Copilot account. Bound untrusted stderr.
    static const QRegularExpression diagnostic(
        QStringLiteral(
            "^(?:error:\\s*)?(?:authentication required|not authenticated|not logged in|"
            "no authentication information found|no valid authentication token found)[.!]?"
            "(?:\\s+(?:please\\s+)?run\\s+['\"\\x60]?copilot login['\"\\x60]?"
            "(?:\\s+to (?:authenticate|sign in|log in))?\\.?)?$"),
        QRegularExpression::CaseInsensitiveOption);
    QString tail = text.right(8192);
    if (text.size() > tail.size()) {
        const auto newline = tail.indexOf(QLatin1Char('\n'));
        if (newline < 0)
            return false;
        tail.remove(0, newline + 1); // A truncated line must not become an exact diagnostic.
    }
    for (const auto& line : tail.split(QLatin1Char('\n')))
        if (diagnostic.match(line.trimmed()).hasMatch())
            return true;
    return false;
}

} // namespace

DiscoveryContext systemDiscoveryContext() {
#if defined(Q_OS_WIN)
    constexpr auto platform = Platform::Windows;
#elif defined(Q_OS_MACOS)
    constexpr auto platform = Platform::MacOS;
#else
    constexpr auto platform = Platform::Linux;
#endif
    return {platform, QProcessEnvironment::systemEnvironment(), QDir::homePath(),
            [](const QString& path) {
                const QFileInfo file(path);
                if (!file.isFile())
                    return false;
#if defined(Q_OS_WIN)
                return QStringList{QStringLiteral("exe"), QStringLiteral("com"),
                                   QStringLiteral("cmd"), QStringLiteral("bat"),
                                   QStringLiteral("ps1")}
                    .contains(file.suffix().toLower());
#else
                return file.isExecutable();
#endif
            }};
}

QStringList wellKnownCandidates(const DiscoveryContext& context) {
    QStringList candidates;
    const auto append = [&](const QString& directory, const QString& name) {
        const QString path = joinedPath(directory, name, context.platform);
        if (isAbsolute(path, context.platform))
            candidates.append(path);
    };
    if (context.platform == Platform::Windows) {
        const QString roaming = environmentValue(context, QStringLiteral("APPDATA"));
        append(roaming, QStringLiteral("npm/copilot.cmd"));
        append(roaming, QStringLiteral("npm/copilot.ps1"));
        append(environmentValue(context, QStringLiteral("LOCALAPPDATA")),
               QStringLiteral("Microsoft/WinGet/Links/copilot.exe"));
    } else {
        append(context.homeDirectory, QStringLiteral(".local/bin/copilot"));
        if (context.platform == Platform::MacOS)
            candidates.append(QStringLiteral("/opt/homebrew/bin/copilot"));
        else
            candidates.append(QStringLiteral("/home/linuxbrew/.linuxbrew/bin/copilot"));
        candidates.append(QStringLiteral("/usr/local/bin/copilot"));
        if (context.platform == Platform::Linux)
            candidates.append(QStringLiteral("/usr/bin/copilot"));
    }
    return candidates;
}

QString missingExecutableMessage() {
    return QStringLiteral(
        "GitHub Copilot CLI was not found. Install Copilot CLI using the GitHub installation "
        "instructions, then retry. For a custom install, set the daemon's --copilot path.\n\n"
        "https://docs.github.com/en/copilot/how-tos/copilot-cli/set-up-copilot-cli/"
        "install-copilot-cli");
}

DiscoveryResult findExecutable(const QString& explicitPath, const DiscoveryContext& context) {
    const auto runnable = [&](const QString& path) {
        return isAbsolute(path, context.platform) && context.isRunnableFile &&
               context.isRunnableFile(path);
    };
    const QString override = explicitPath.isEmpty()
                                 ? environmentValue(context, QStringLiteral("KETPLUS_COPILOT"))
                                 : explicitPath;
    if (!override.isEmpty()) {
        const QString path = normalizedPath(override, context.platform);
        if (runnable(path))
            return {path, {}};
        return {{},
                QStringLiteral("GitHub Copilot CLI override is not a runnable absolute file "
                               "path. Check --copilot or KETPLUS_COPILOT.\n\n%1")
                    .arg(missingExecutableMessage())};
    }
    QSet<QString> visited;
    const auto probe = [&](const QString& path) {
        const QString key = context.platform == Platform::Windows ? path.toLower() : path;
        if (visited.contains(key))
            return false;
        visited.insert(key);
        return runnable(path);
    };
    const QChar separator =
        context.platform == Platform::Windows ? QLatin1Char(';') : QLatin1Char(':');
    for (QString directory : environmentValue(context, QStringLiteral("PATH")).split(separator)) {
        if (context.platform == Platform::Windows && directory.startsWith(QLatin1Char('"')) &&
            directory.endsWith(QLatin1Char('"')))
            directory = directory.mid(1, directory.size() - 2);
        if (directory.isEmpty())
            continue;
        directory = normalizedPath(directory, context.platform);
        // Never implicitly execute a repository-local CLI via '.' or a relative PATH entry.
        if (!isAbsolute(directory, context.platform))
            continue;
        for (const auto& name : pathNames(context)) {
            const QString path = joinedPath(directory, name, context.platform);
            if (probe(path))
                return {path, {}};
        }
    }
    for (const auto& path : wellKnownCandidates(context))
        if (probe(path))
            return {path, {}};
    return {{}, missingExecutableMessage()};
}

ProcessInvocation processInvocation(const QString& executable, Platform platform,
                                    const QProcessEnvironment& environment) {
    const QString path = normalizedPath(executable, platform);
    if (executable.isEmpty() || !isAbsolute(path, platform) || path.contains(QChar::Null) ||
        path.contains(QLatin1Char('\r')) || path.contains(QLatin1Char('\n')))
        return {{}, {}, {}, QStringLiteral("Copilot CLI requires an absolute executable path.")};
    const QStringList arguments{QStringLiteral("--acp"), QStringLiteral("--stdio")};
    if (platform == Platform::Windows) {
        const QString suffix = QFileInfo(path).suffix().toLower();
        QString systemDirectory;
        if (suffix == QStringLiteral("cmd") || suffix == QStringLiteral("bat") ||
            suffix == QStringLiteral("ps1")) {
            const DiscoveryContext context{platform, environment, {}, {}};
            systemDirectory = joinedPath(environmentValue(context, QStringLiteral("SystemRoot")),
                                         QStringLiteral("System32"), platform);
            if (!isAbsolute(systemDirectory, platform))
                return {{},
                        {},
                        {},
                        QStringLiteral("Windows SystemRoot must be an absolute path "
                                       "to launch the Copilot CLI script wrapper.")};
        }
        QString nativePath = path;
        nativePath.replace(QLatin1Char('/'), QLatin1Char('\\'));
        if (suffix == QStringLiteral("cmd") || suffix == QStringLiteral("bat")) {
            // Percent expansion happens even inside quotes; refuse rather than execute a
            // different path. Delayed expansion is disabled so literal '!' remains safe.
            if (nativePath.contains(QLatin1Char('%')) || nativePath.contains(QLatin1Char('"')))
                return {{},
                        {},
                        {},
                        QStringLiteral("Copilot's Windows command-wrapper path "
                                       "cannot contain percent or quote characters. "
                                       "Set --copilot to another install path.")};
            return {systemDirectory + QStringLiteral("/cmd.exe"),
                    {QStringLiteral("/d"), QStringLiteral("/v:off"), QStringLiteral("/s"),
                     QStringLiteral("/c")},
                    QStringLiteral("\"\"%1\" --acp --stdio\"").arg(nativePath),
                    {}};
        }
        if (suffix == QStringLiteral("ps1"))
            return {systemDirectory + QStringLiteral("/WindowsPowerShell/v1.0/powershell.exe"),
                    {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                     QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                     QStringLiteral("-File"), nativePath, arguments.at(0), arguments.at(1)},
                    {},
                    {}};
    }
    return {path, arguments, {}, {}};
}

bool isAuthenticationFailure(const Failure& failure) {
    if (hasAuthDiagnostic(failure.acpError.value(QStringLiteral("message")).toString()))
        return true;
    const auto data = failure.acpError.value(QStringLiteral("data")).toObject();
    for (const auto& key : {QStringLiteral("message"), QStringLiteral("details")})
        if (hasAuthDiagnostic(data.value(key).toString()))
            return true;
    return failure.exitCode.has_value() && *failure.exitCode != 0 &&
           hasAuthDiagnostic(failure.standardError);
}

QString notAuthenticatedMessage() {
    return QStringLiteral(
        "GitHub Copilot CLI needs sign-in. Run `copilot login` in a terminal on the machine "
        "running the KetPlus daemon, then retry. For headless use, configure a supported "
        "Copilot token in that daemon's environment.");
}

QString authenticationGuidance(const Failure& failure) {
    return isAuthenticationFailure(failure) ? notAuthenticatedMessage() : QString();
}

} // namespace ketplus::daemon::copilot_cli
