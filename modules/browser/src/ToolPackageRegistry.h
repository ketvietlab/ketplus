#pragma once

#include "CodexBrowserTools.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

// P0 tool package abstraction (ADR 0008). A provider turns a tool id + launch protocol into a
// concrete process launch. Stage 0 ships only SourceCheckoutProvider; a downloadable QAPack
// provider (qapack.json manifest, signed archives, bundled Node, Chromium on first use) is
// planned for ADR 0009 and plugs in as another provider without changing callers.
namespace ketplus::browser_tools {

inline constexpr auto browserToolId = "ketplus.browser";
inline constexpr auto mcpStdioProtocol = "mcp-stdio";

struct ToolLaunch final {
    QString command;
    QStringList args;
    QMap<QString, QString> env; // package-provided; callers add policy variables
    QString workingDirectory;
    QString packageVersion; // empty when the package does not declare one
    QString source;         // provider name, e.g. "source-checkout"
};

// `-c mcp_servers.ketplus_browser.{command,args,cwd,env}=…` for a resolved launch. Env names
// outside [A-Z0-9_] are dropped (they would be written as TOML bare keys); cwd is emitted only
// when the launch has a working directory.
[[nodiscard]] QStringList mcpServerConfigArguments(const ToolLaunch& launch);

class ToolPackageProvider {
  public:
    virtual ~ToolPackageProvider() = default;
    [[nodiscard]] virtual std::optional<ToolLaunch> resolve(const QString& toolId,
                                                            const QString& protocol) const = 0;
};

// Today's behaviour: apps/browser from a source checkout (saved path or auto-detected upward
// from searchStart), node from the saved nodePath, entry src/mcp.mjs. File checks only.
class SourceCheckoutProvider final : public ToolPackageProvider {
  public:
    static constexpr auto sourceName = "source-checkout";

    SourceCheckoutProvider(Settings settings, QString searchStart);
    [[nodiscard]] std::optional<ToolLaunch> resolve(const QString& toolId,
                                                    const QString& protocol) const override;

  private:
    Settings settings_;
    QString searchStart_;
};

class ToolPackageRegistry final {
  public:
    void addProvider(std::unique_ptr<ToolPackageProvider> provider);
    // First provider (in registration order) that resolves wins.
    [[nodiscard]] std::optional<ToolLaunch> resolve(const QString& toolId,
                                                    const QString& protocol) const;

  private:
    std::vector<std::unique_ptr<ToolPackageProvider>> providers_;
};

} // namespace ketplus::browser_tools
