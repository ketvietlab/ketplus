#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

// KetPlus browser tools for Codex (docs/decisions/0008): launches the apps/browser MCP facade
// (bundled runtime or source checkout) as a Codex MCP server. On by default, all origins allowed,
// always an isolated temporary browser profile. Core-only so the daemon (ketplus_daemon_core) and
// the desktop (ketplus_ai) share one argument builder.
namespace ketplus::browser_tools {

inline constexpr auto serverName = "ketplus_browser";
inline constexpr auto defaultToolsApprovalMode = "approve";
inline constexpr int minimumNodeMajor = 22;
// browser-tools.json schema. Files without `schemaVersion` were written by the stage-0
// Experimental page, which saved `enabled:false` and `allowedOrigins:"loopback"` on every refresh;
// they are read as enabled with all origins (a custom origin list is kept). Version 2 files keep
// an explicit `enabled:false` (the user turned the tools off in Settings → Browser).
inline constexpr int settingsSchemaVersion = 2;
inline constexpr auto allOrigins = "*";
inline constexpr qint64 maximumSettingsFileBytes = 64 * 1024;

struct Settings final {
    bool enabled{true};
    QString allowedOrigins{QStringLiteral("*")};
    QString browserAppPath; // empty: auto-detect from the source checkout
    QString nodePath;       // empty: auto-detect
    // Set when browser-tools.json exists but is unreadable, not a JSON object or over the size
    // cap: the settings then load disabled (fail closed) and Settings → Browser shows this.
    QString loadError;
};

struct Prerequisites final {
    QString browserAppPath;
    QString nodePath;
    QString nodeVersion;
    bool nodeOk{false};
    bool modulesOk{false};
    bool chromiumOk{false}; // runtime engine ready, or first-use download allowed
    QString reason;         // first missing prerequisite, user-facing

    [[nodiscard]] bool ok() const noexcept { return nodeOk && modulesOk && chromiumOk; }
};

// Injectable probes so detection is unit-testable without node or Chromium.
struct Probes final {
    std::function<QStringList()> nodeCandidates;
    std::function<QString(const QString& node)> nodeVersion; // "v22.1.0" or empty
    std::function<bool(const QString& node, const QString& appPath)> chromiumInstalled;
};

// A missing file loads the defaults; a corrupt or oversized one loads disabled (fail closed).
// JSON settings file shared by desktop and daemon (both run as the same user):
// $GenericDataLocation/Ketsuite/KetPlus/browser-tools.json, overridable with
// KETPLUS_BROWSER_TOOLS_CONFIG for tests.
[[nodiscard]] QString settingsFilePath();
[[nodiscard]] QString defaultArtifactsDirectory();
[[nodiscard]] Settings loadSettings(const QString& path = settingsFilePath());
bool saveSettings(const Settings& settings, const QString& path = settingsFilePath());

// "*" (all origins; any "*" entry wins), or comma-separated http(s) origins and "loopback",
// normalized (lower-case, no trailing slash, IPv6 brackets kept). Empty becomes "loopback".
[[nodiscard]] QString normalizeAllowedOrigins(const QString& value, QString* error = nullptr);
[[nodiscard]] QString tomlString(const QString& value);

// Walks upward from startDirectory looking for apps/browser/src/mcp.mjs.
[[nodiscard]] QString detectBrowserAppPath(const QString& startDirectory);
inline constexpr int nodeProbeTimeoutMs = 1500;
inline constexpr int maximumNodeCandidates = 6;
[[nodiscard]] Probes systemProbes();

// Last detection result, stored in browser-tools.json as {prereqCache:{key,result,checkedAt}}.
struct PrerequisiteCache final {
    QString key;
    Prerequisites result;
    QDateTime checkedAt;
};

// Cheap (stat + hash of package-lock.json) fingerprint of everything detection depends on:
// resolved app path, node binary mtime+size, apps/browser/package-lock.json sha256, presence
// of node_modules and the Playwright browser cache mtime (PLAYWRIGHT_BROWSERS_PATH or the
// platform default).
[[nodiscard]] QString prerequisiteCacheKey(const Settings& settings, const QString& searchStart);
[[nodiscard]] std::optional<PrerequisiteCache>
loadPrerequisiteCache(const QString& path = settingsFilePath());
bool savePrerequisiteCache(const QString& key, const Prerequisites& result,
                           const QString& path = settingsFilePath());
[[nodiscard]] Prerequisites detectPrerequisites(const Settings& settings,
                                                const QString& searchStart,
                                                const Probes& probes = systemProbes());

// The node used by a source-checkout launch: the saved nodePath, else the Node recorded by the
// last prerequisite check. Empty when neither is executable.
[[nodiscard]] QString effectiveNodePath(const Settings& settings,
                                        const QString& path = settingsFilePath());

struct ToolLaunch;
// Registry order: BundledRuntimeProvider (when compiled in) first, SourceCheckoutProvider second.
[[nodiscard]] std::optional<ToolLaunch> resolveBrowserLaunch(const Settings& settings,
                                                             const QString& searchStart);

// Cheap launch-time check (file existence only; the Settings page runs full detection and
// records the resolved node path). Returns the extra `codex app-server` arguments, or an empty
// list when the feature is disabled or unusable (fail closed: Codex starts without the server).
[[nodiscard]] QStringList codexArguments(const Settings& settings, const QString& searchStart,
                                         const QString& artifactsDirectory);
// Convenience used by both Codex launch paths; creates the 0700 artifacts directory.
[[nodiscard]] QStringList codexArgumentsFromUserSettings(const QString& searchStart);

// In-process notification that browser-tools.json was saved (the Settings page calls it after a
// write). CodexLaunchReloader instances listen to it in addition to watching the file.
void notifySettingsChanged();

} // namespace ketplus::browser_tools
