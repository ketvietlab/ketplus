#pragma once

#include "ToolPackageRegistry.h"

#include <QCoreApplication>
#include <QString>

#include <optional>

// ADR 0009: the browser.v0 runtime shipped inside KetPlus.app at
// Contents/Resources/ketplus-browser (pinned Node, production node_modules, skill, runtime.json).
namespace ketplus::browser_tools {

struct BundledRuntime final {
    QString rootDir;      // absolute, canonical
    QString nodePath;     // executable
    QString mcpEntry;     // absolute path to src/mcp.mjs
    QString jsonRpcEntry; // absolute path to src/cli.mjs
    QString skillFile;    // absolute path to .claude/skills/ketplus-browser/SKILL.md
    QString version;      // runtime.json "version"
    QString nodeVersion;  // runtime.json "nodeVersion", e.g. "v24.21.0"
};

inline constexpr auto bundledRuntimeDirName = "ketplus-browser";
inline constexpr auto browserV0Protocol = "browser.v0";

// Validates <rootDir>/runtime.json (name "ketplus-browser", protocols containing "browser.v0",
// relative entrypoints that stay inside rootDir) and that node is executable and every
// referenced file exists. File checks only; nothing is executed.
[[nodiscard]] std::optional<BundledRuntime> loadBundledBrowserRuntime(const QString& rootDir);

// Directories searched, in order, for Desktop in Contents/MacOS and the daemon in
// Contents/Helpers alike: <appDir>/../Resources/ketplus-browser (the signed layout),
// <appDir>/../Helpers/ketplus-browser and <appDir>/ketplus-browser (sibling of the daemon,
// or a flat install). The first valid runtime wins.
[[nodiscard]] QStringList bundledBrowserRuntimeCandidates(const QString& applicationDirPath);
[[nodiscard]] std::optional<BundledRuntime> findBundledBrowserRuntime(
    const QString& applicationDirPath = QCoreApplication::applicationDirPath());

// Where the runtime keeps a downloaded Chrome for Testing when neither Google Chrome nor
// Microsoft Edge is installed: $GenericDataLocation/Ketsuite/KetPlus/browser-runtime/chrome.
[[nodiscard]] QString defaultBrowserCacheDir();

// Resolves ketplus.browser + mcp-stdio to the bundled node + src/mcp.mjs with
// KETPLUS_BROWSER_CACHE set (directory created 0700) and NODE_OPTIONS plus Playwright download
// host variables set to empty. Not registered anywhere by default; the registry owner decides
// the order relative to SourceCheckoutProvider.
class BundledRuntimeProvider final : public ToolPackageProvider {
  public:
    static constexpr auto sourceName = "bundled";

    explicit BundledRuntimeProvider(
        QString applicationDirPath = QCoreApplication::applicationDirPath(),
        QString browserCacheDir = defaultBrowserCacheDir());
    [[nodiscard]] std::optional<ToolLaunch> resolve(const QString& toolId,
                                                    const QString& protocol) const override;

  private:
    QString applicationDirPath_;
    QString browserCacheDir_;
};

} // namespace ketplus::browser_tools
