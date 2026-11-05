#include "CodexBrowserTools.h"
#include "BrowserToolLaunch.h"

#include "ToolPackageRegistry.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QVersionNumber>

#include <algorithm>

#include "BundledBrowserRuntime.h"

namespace ketplus::browser_tools {
namespace {

QString dataRoot() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("Ketsuite/KetPlus"));
}

int nodeMajor(const QString& version) {
    const QString trimmed = version.trimmed();
    return QVersionNumber::fromString(trimmed.startsWith(QLatin1Char('v')) ? trimmed.mid(1)
                                                                           : trimmed)
        .majorVersion();
}

bool hasModules(const QString& appPath) {
    const QDir modules(QDir(appPath).filePath(QStringLiteral("node_modules")));
    return QFileInfo::exists(modules.filePath(QStringLiteral("@modelcontextprotocol/server"))) &&
           QFileInfo::exists(modules.filePath(QStringLiteral("playwright-core")));
}

QString mcpScript(const QString& appPath) {
    return QDir(appPath).filePath(QStringLiteral("src/mcp.mjs"));
}

QProcessEnvironment nodeEnvironment() {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("NODE_OPTIONS"));
    return environment;
}

} // namespace

QString settingsFilePath() {
    const QString overridePath = qEnvironmentVariable("KETPLUS_BROWSER_TOOLS_CONFIG");
    return overridePath.isEmpty() ? QDir(dataRoot()).filePath(QStringLiteral("browser-tools.json"))
                                  : overridePath;
}

QString defaultArtifactsDirectory() {
    return QDir(dataRoot()).filePath(QStringLiteral("browser/artifacts"));
}

namespace {

// Missing file: an empty object. Existing but unreadable, oversized or not a JSON object: nullopt.
std::optional<QJsonObject> readSettingsObject(const QString& path) {
    if (!QFileInfo::exists(path))
        return QJsonObject{};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximumSettingsFileBytes)
        return std::nullopt;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    return document.object();
}

} // namespace

Settings loadSettings(const QString& path) {
    Settings settings;
    const std::optional<QJsonObject> read = readSettingsObject(path);
    if (!read) {
        // Never fail open: a damaged file must not become "enabled, all origins".
        settings.enabled = false;
        settings.loadError = QStringLiteral("%1 is damaged or too large; browser tools are off "
                                            "until it is saved again.")
                                 .arg(QDir::toNativeSeparators(path));
        return settings;
    }
    const QJsonObject& object = *read;
    const bool legacy = object.value(QStringLiteral("schemaVersion")).toInt(0) < 2;
    // Stage-0 files never recorded an explicit choice (every save wrote enabled/origins), so they
    // migrate to the new defaults; version 2 files keep what the user chose.
    settings.enabled = legacy || object.value(QStringLiteral("enabled")).toBool(true);
    const QString origins = object.value(QStringLiteral("allowedOrigins")).toString();
    if (!origins.trimmed().isEmpty() &&
        !(legacy && origins.trimmed() == QStringLiteral("loopback")))
        settings.allowedOrigins = origins;
    settings.browserAppPath = object.value(QStringLiteral("browserAppPath")).toString();
    settings.nodePath = object.value(QStringLiteral("nodePath")).toString();
    return settings;
}

namespace {

QJsonObject readObject(const QString& path) {
    return readSettingsObject(path).value_or(QJsonObject{});
}

bool writeObject(const QJsonObject& object, const QString& path) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return file.commit();
}

} // namespace

bool saveSettings(const Settings& settings, const QString& path) {
    // Merge so the prerequisite cache written next to the settings survives.
    QJsonObject object = readObject(path);
    object.insert(QStringLiteral("schemaVersion"), settingsSchemaVersion);
    object.insert(QStringLiteral("enabled"), settings.enabled);
    object.insert(QStringLiteral("allowedOrigins"), settings.allowedOrigins);
    object.insert(QStringLiteral("browserAppPath"), settings.browserAppPath);
    object.insert(QStringLiteral("nodePath"), settings.nodePath);
    return writeObject(object, path);
}

QString prerequisiteCacheKey(const Settings& settings, const QString& searchStart) {
    const QString appPath = settings.browserAppPath.isEmpty() ? detectBrowserAppPath(searchStart)
                                                              : settings.browserAppPath;
    const QFileInfo node(settings.nodePath);
    QByteArray lockHash;
    QFile lock(QDir(appPath).filePath(QStringLiteral("package-lock.json")));
    if (!appPath.isEmpty() && lock.open(QIODevice::ReadOnly)) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&lock);
        lockHash = hash.result().toHex();
    }
    QString playwright = qEnvironmentVariable("PLAYWRIGHT_BROWSERS_PATH");
    if (playwright.isEmpty())
#if defined(Q_OS_MACOS)
        playwright = QDir::home().filePath(QStringLiteral("Library/Caches/ms-playwright"));
#else
        playwright = QDir::home().filePath(QStringLiteral(".cache/ms-playwright"));
#endif
    const QFileInfo playwrightInfo(playwright);
    const QStringList parts{
        QStringLiteral("engine-probe-v2"),
        qEnvironmentVariable("KETPLUS_BROWSER_ENGINE"),
        qEnvironmentVariable("KETPLUS_BROWSER_DOWNLOAD"),
        qEnvironmentVariable("KETPLUS_BROWSER_CACHE"),
        appPath,
        settings.nodePath,
        node.exists() ? QString::number(node.lastModified().toMSecsSinceEpoch()) : QString(),
        node.exists() ? QString::number(node.size()) : QString(),
        QString::fromLatin1(lockHash),
        QString::number(!appPath.isEmpty() && hasModules(appPath)),
        playwrightInfo.exists() ? QString::number(playwrightInfo.lastModified().toMSecsSinceEpoch())
                                : QString()};
    return QString::fromLatin1(
        QCryptographicHash::hash(parts.join(QChar(0x1f)).toUtf8(), QCryptographicHash::Sha256)
            .toHex());
}

std::optional<PrerequisiteCache> loadPrerequisiteCache(const QString& path) {
    const QJsonObject cache = readObject(path).value(QStringLiteral("prereqCache")).toObject();
    const QJsonObject result = cache.value(QStringLiteral("result")).toObject();
    if (cache.value(QStringLiteral("key")).toString().isEmpty() || result.isEmpty())
        return std::nullopt;
    PrerequisiteCache out;
    out.key = cache.value(QStringLiteral("key")).toString();
    out.checkedAt = QDateTime::fromString(cache.value(QStringLiteral("checkedAt")).toString(),
                                          Qt::ISODateWithMs);
    out.result.browserAppPath = result.value(QStringLiteral("browserAppPath")).toString();
    out.result.nodePath = result.value(QStringLiteral("nodePath")).toString();
    out.result.nodeVersion = result.value(QStringLiteral("nodeVersion")).toString();
    out.result.nodeOk = result.value(QStringLiteral("nodeOk")).toBool();
    out.result.modulesOk = result.value(QStringLiteral("modulesOk")).toBool();
    out.result.chromiumOk = result.value(QStringLiteral("chromiumOk")).toBool();
    out.result.reason = result.value(QStringLiteral("reason")).toString();
    return out;
}

bool savePrerequisiteCache(const QString& key, const Prerequisites& result, const QString& path) {
    // Writing only the cache into a damaged file would drop schemaVersion and re-enable the
    // tools as a legacy file; leave it for an explicit save from Settings.
    const std::optional<QJsonObject> existing = readSettingsObject(path);
    if (!existing)
        return false;
    QJsonObject object = *existing;
    object.insert(
        QStringLiteral("prereqCache"),
        QJsonObject{{QStringLiteral("key"), key},
                    {QStringLiteral("checkedAt"),
                     QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                    {QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("browserAppPath"), result.browserAppPath},
                                 {QStringLiteral("nodePath"), result.nodePath},
                                 {QStringLiteral("nodeVersion"), result.nodeVersion},
                                 {QStringLiteral("nodeOk"), result.nodeOk},
                                 {QStringLiteral("modulesOk"), result.modulesOk},
                                 {QStringLiteral("chromiumOk"), result.chromiumOk},
                                 {QStringLiteral("reason"), result.reason}}}});
    return writeObject(object, path);
}

QString normalizeAllowedOrigins(const QString& value, QString* error) {
    QStringList result;
    bool all = false;
    for (const QString& raw : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString entry = raw.trimmed();
        if (entry.isEmpty())
            continue;
        QString normalized;
        if (entry == QStringLiteral("loopback")) {
            normalized = entry;
        } else if (entry == QLatin1String(allOrigins)) {
            all = true; // keep validating the remaining entries
            continue;
        } else {
            const QUrl url(entry, QUrl::StrictMode);
            const QString scheme = url.scheme().toLower();
            const bool pathless = url.path().isEmpty() || url.path() == QStringLiteral("/");
            if (!url.isValid() ||
                (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) ||
                url.host().isEmpty() || !url.userInfo().isEmpty() || !pathless || url.hasQuery() ||
                url.hasFragment()) {
                if (error)
                    *error = QStringLiteral("Not an http(s) origin: %1").arg(entry);
                return {};
            }
            const QString host = url.host().toLower();
            // QUrl::host() drops IPv6 brackets; an origin needs them (http://[::1]:3000).
            normalized =
                scheme + QStringLiteral("://") +
                (host.contains(QLatin1Char(':')) ? QLatin1Char('[') + host + QLatin1Char(']')
                                                 : host);
            if (url.port() != -1)
                normalized += QLatin1Char(':') + QString::number(url.port());
        }
        if (!result.contains(normalized))
            result.append(normalized);
    }
    if (error)
        error->clear();
    if (all)
        return QLatin1String(allOrigins);
    return result.isEmpty() ? QStringLiteral("loopback") : result.join(QLatin1Char(','));
}

QString tomlString(const QString& value) {
    QString out;
    out.reserve(value.size() + 2);
    out.append(QLatin1Char('"'));
    for (const QChar character : value) {
        switch (character.unicode()) {
        case '"':
            out.append(QStringLiteral("\\\""));
            break;
        case '\\':
            out.append(QStringLiteral("\\\\"));
            break;
        case '\b':
            out.append(QStringLiteral("\\b"));
            break;
        case '\t':
            out.append(QStringLiteral("\\t"));
            break;
        case '\n':
            out.append(QStringLiteral("\\n"));
            break;
        case '\f':
            out.append(QStringLiteral("\\f"));
            break;
        case '\r':
            out.append(QStringLiteral("\\r"));
            break;
        default:
            if (character.unicode() < 0x20 || character.unicode() == 0x7F)
                out.append(QStringLiteral("\\u%1").arg(static_cast<uint>(character.unicode()), 4,
                                                       16, QLatin1Char('0')));
            else
                out.append(character);
        }
    }
    out.append(QLatin1Char('"'));
    return out;
}

QString detectBrowserAppPath(const QString& startDirectory) {
    // Walk path strings, not QDir::cdUp: the start (e.g. a build directory) need not exist.
    QString directory = QDir::cleanPath(QDir(startDirectory).absolutePath());
    for (int depth = 0; depth < 12 && !directory.isEmpty(); ++depth) {
        const QString candidate = QDir(directory).filePath(QStringLiteral("apps/browser"));
        if (QFileInfo::exists(mcpScript(candidate)))
            return QDir(candidate).canonicalPath();
        const QString parent = QFileInfo(directory).path();
        if (parent == directory)
            break;
        directory = parent;
    }
    return {};
}

Probes systemProbes() {
    Probes probes;
    probes.nodeCandidates = [] {
        QStringList candidates{QStringLiteral("/opt/homebrew/bin/node"),
                               QStringLiteral("/usr/local/bin/node")};
        const QString onPath = QStandardPaths::findExecutable(QStringLiteral("node"));
        if (!onPath.isEmpty())
            candidates.append(onPath);
        QString nvm = qEnvironmentVariable("NVM_DIR");
        if (nvm.isEmpty())
            nvm = QDir::home().filePath(QStringLiteral(".nvm"));
        QDir versions(QDir(nvm).filePath(QStringLiteral("versions/node")));
        QStringList installed = versions.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        std::sort(installed.begin(), installed.end(), [](const QString& a, const QString& b) {
            return QVersionNumber::fromString(a.mid(1)) > QVersionNumber::fromString(b.mid(1));
        });
        for (const QString& version : installed)
            candidates.append(versions.filePath(version + QStringLiteral("/bin/node")));
        return candidates;
    };
    probes.nodeVersion = [](const QString& node) {
        if (!QFileInfo(node).isExecutable())
            return QString();
        QProcess process;
        process.setProcessEnvironment(nodeEnvironment());
        process.start(node, {QStringLiteral("--version")});
        if (!process.waitForFinished(nodeProbeTimeoutMs) || process.exitCode() != 0) {
            process.kill();
            return QString();
        }
        return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    };
    probes.chromiumInstalled = [](const QString& node, const QString& appPath) {
        // Shares runtime engine selection (Chrome/Edge/cache/first-use download),
        // without launching a browser or downloading anything in Settings.
        QProcess process;
        process.setProcessEnvironment(nodeEnvironment());
        process.setWorkingDirectory(appPath);
        process.start(node, {QDir(appPath).filePath(QStringLiteral("src/check-engine.mjs"))});
        if (!process.waitForFinished(20000) || process.exitCode() != 0) {
            process.kill();
            process.waitForFinished(1000);
            return false;
        }
        return QJsonDocument::fromJson(process.readAllStandardOutput())
            .object()
            .value(QStringLiteral("available"))
            .toBool();
    };
    return probes;
}

Prerequisites detectPrerequisites(const Settings& settings, const QString& searchStart,
                                  const Probes& probes) {
    Prerequisites result;
    result.browserAppPath = settings.browserAppPath.isEmpty() ? detectBrowserAppPath(searchStart)
                                                              : settings.browserAppPath;
    if (result.browserAppPath.isEmpty() || !QFileInfo::exists(mcpScript(result.browserAppPath))) {
        result.reason = QStringLiteral("Browser app not found. Browser tools need a KetPlus "
                                       "source checkout with apps/browser.");
        return result;
    }
    QStringList candidates;
    if (!settings.nodePath.isEmpty())
        candidates.append(settings.nodePath);
    if (probes.nodeCandidates)
        candidates.append(probes.nodeCandidates());
    QSet<QString> seen;
    for (const QString& candidate : std::as_const(candidates)) {
        if (candidate.isEmpty() || seen.contains(candidate))
            continue;
        if (seen.size() >= maximumNodeCandidates)
            break;
        seen.insert(candidate);
        const QString version = probes.nodeVersion ? probes.nodeVersion(candidate) : QString();
        if (!version.isEmpty() && nodeMajor(version) >= minimumNodeMajor) {
            result.nodePath = candidate;
            result.nodeVersion = version;
            result.nodeOk = true;
            break;
        }
    }
    if (!result.nodeOk) {
        result.reason = QStringLiteral("Node.js %1 or newer was not found.").arg(minimumNodeMajor);
        return result;
    }
    result.modulesOk = hasModules(result.browserAppPath);
    if (!result.modulesOk) {
        result.reason = QStringLiteral("Dependencies are missing. Run `npm ci` in apps/browser.");
        return result;
    }
    result.chromiumOk = probes.chromiumInstalled &&
                        probes.chromiumInstalled(result.nodePath, result.browserAppPath);
    if (!result.chromiumOk)
        result.reason = QStringLiteral(
            "No usable browser engine. Install Chrome or Edge, enable first-use downloads, "
            "or check KETPLUS_BROWSER_ENGINE. For the playwright engine, run `npm run "
            "install:browser`.");
    return result;
}

QStringList codexArguments(const Settings& settings, const QString& searchStart,
                           const QString& artifactsDirectory) {
    const auto launch = configuredBrowserLaunch(settings, searchStart, artifactsDirectory);
    return launch ? mcpServerConfigArguments(*launch) : QStringList{};
}

std::optional<ToolLaunch> resolveBrowserLaunch(const Settings& settings,
                                               const QString& searchStart) {
    ToolPackageRegistry registry;
    // A runtime shipped inside the app bundle wins over a source checkout.
    registry.addProvider(std::make_unique<BundledRuntimeProvider>());
    registry.addProvider(std::make_unique<SourceCheckoutProvider>(settings, searchStart));
    return registry.resolve(QLatin1String(browserToolId), QLatin1String(mcpStdioProtocol));
}

QString effectiveNodePath(const Settings& settings, const QString& path) {
    if (!settings.nodePath.isEmpty())
        return QFileInfo(settings.nodePath).isExecutable() ? settings.nodePath : QString();
    const auto cache = loadPrerequisiteCache(path);
    if (cache && cache->result.nodeOk && QFileInfo(cache->result.nodePath).isExecutable())
        return cache->result.nodePath;
    return {};
}

QStringList mcpServerConfigArguments(const ToolLaunch& launch) {
    const QString prefix = QStringLiteral("mcp_servers.%1.").arg(QLatin1String(serverName));
    QStringList args;
    for (const QString& arg : std::as_const(launch.args))
        args.append(tomlString(arg));
    // Env names are written as TOML bare keys; anything outside [A-Z0-9_] is not emitted.
    static const QRegularExpression envName(QStringLiteral("^[A-Z0-9_]+$"));
    QStringList env;
    for (auto it = launch.env.cbegin(); it != launch.env.cend(); ++it) {
        if (envName.match(it.key()).hasMatch())
            env.append(it.key() + QLatin1Char('=') + tomlString(it.value()));
    }
    QStringList arguments{
        QStringLiteral("-c"), prefix + QStringLiteral("command=") + tomlString(launch.command),
        QStringLiteral("-c"),
        prefix + QStringLiteral("args=[") + args.join(QLatin1Char(',')) + QLatin1Char(']')};
    // `mcp_servers.<name>.cwd` is accepted by codex-cli (verified with 0.154.0).
    if (!launch.workingDirectory.isEmpty())
        arguments << QStringLiteral("-c")
                  << prefix + QStringLiteral("cwd=") + tomlString(launch.workingDirectory);
    // Browser tools run without a per-call approval prompt (product decision, ADR 0008). Under
    // `approvalPolicy=on-request` Codex otherwise asks before every MCP tool call. codex-cli
    // 0.154.0 accepts `auto`, `prompt`, `writes` or `approve` and refuses to start on others.
    arguments << QStringLiteral("-c")
              << prefix + QStringLiteral("default_tools_approval_mode=") +
                     tomlString(QLatin1String(defaultToolsApprovalMode));
    arguments << QStringLiteral("-c")
              << prefix + QStringLiteral("env={") + env.join(QLatin1Char(',')) + QLatin1Char('}');
    return arguments;
}

QStringList codexArgumentsFromUserSettings(const QString& searchStart) {
    const auto launch = browserLaunchFromUserSettings(searchStart);
    return launch ? mcpServerConfigArguments(*launch) : QStringList{};
}

} // namespace ketplus::browser_tools
