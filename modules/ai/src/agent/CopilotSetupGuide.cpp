#include "agent/CopilotSetupGuide.h"
#include "agent/ProviderSetupGuide.h"

#include <QCoreApplication>
#include <QVariantList>

namespace ketplus::provider_setup {
namespace {
QString tr(const char* text) { return QCoreApplication::translate("ProviderSetupGuide", text); }
QVariantMap step(const QString& text, const QString& command = {}) {
    QVariantMap value{{"text", text}};
    if (!command.isEmpty())
        value.insert("command", command);
    return value;
}
} // namespace

QString copilotInstallCommand(const QString& platform) {
    if (platform == "windows")
        return QStringLiteral("winget install GitHub.Copilot");
    if (platform == "macos")
        return QStringLiteral("brew install --cask copilot-cli");
    return QStringLiteral("npm install -g @github/copilot");
}

QVariantMap copilotGuide(const QJsonObject& provider, const QJsonObject& turnError,
                         const QString& platform) {
    const auto readiness = provider.value("readiness").toObject();
    const bool fromTurn = !turnError.value("code").toString().isEmpty();
    const QString id = guideId("copilot", fromTurn ? turnError.value("code").toString()
                                                   : readiness.value("state").toString());
    const bool missing = id == "cli-missing";
    if (!missing && id != "signed-out" && id != "expired")
        return {};
    const QString name = QStringLiteral("GitHub Copilot");
    const QString login = readiness.value("login_command").toString("copilot login");
    const QString message =
        fromTurn ? turnError.value("message").toString() : readiness.value("message").toString();
    const QString install =
        readiness.value("install_command").toString(copilotInstallCommand(platform));
    QVariantList steps;
    if (missing) {
        steps << step(tr("Install GitHub Copilot CLI on the machine running the KetPlus daemon."),
                      install);
        if (install.startsWith("npm "))
            steps << step(tr("The npm installation requires Node.js 22 or later."));
        steps << step(tr("For another install location, set the daemon's --copilot absolute "
                         "path or KETPLUS_COPILOT environment variable."));
    }
    steps << step(tr("Sign in on the machine running the KetPlus daemon with your GitHub "
                     "Copilot account."),
                  login)
          << step(tr("For headless use, configure COPILOT_GITHUB_TOKEN, GH_TOKEN or GITHUB_TOKEN "
                     "in the daemon's environment. KetPlus does not store these credentials."))
          << step(tr("Check again, then retry. Your message stays in the composer."));
    QVariantList details;
    if (missing) {
        const QString locations =
            platform == "windows"
                ? tr("--copilot, KETPLUS_COPILOT, PATH, npm wrappers and WinGet Links")
            : platform == "macos"
                ? tr("--copilot, KETPLUS_COPILOT, PATH, ~/.local/bin and Homebrew locations")
                : tr("--copilot, KETPLUS_COPILOT, PATH, ~/.local/bin and system bin locations");
        details.append(QVariant(QVariantList{tr("Looked for"), locations}));
    } else {
        details.append(QVariant(QVariantList{tr("Signed in"), tr("No")}));
    }
    return {{"id", id},
            {"provider", "copilot"},
            {"name", name},
            {"tone", "danger"},
            {"blocking", missing},
            {"composer", !fromTurn},
            {"title", missing ? tr("GitHub Copilot CLI isn't installed")
                              : tr("GitHub Copilot CLI needs sign-in")},
            {"badge", missing ? tr("Not installed") : tr("Not signed in")},
            {"reason",
             missing
                 ? tr("KetPlus couldn't find GitHub Copilot CLI on the machine running the daemon.")
                 : tr("Run copilot login in a terminal on the machine running the KetPlus daemon, "
                      "then retry. Authentication belongs to Copilot CLI.")},
            {"error", message.section('\n', 0, 0)},
            {"source", "ketPlus"},
            {"primary", tr("Check again")},
            {"primaryAction", ""},
            {"secondary", tr("Use Codex instead")},
            {"secondaryAction", "switch:codex"},
            {"steps", steps},
            {"rows", details},
            {"still", tr("Run copilot --version on the daemon's machine. If it works there, "
                         "check --copilot or restart the daemon to refresh its environment.")},
            {"ready", tr("GitHub Copilot is ready")}};
}
} // namespace ketplus::provider_setup
