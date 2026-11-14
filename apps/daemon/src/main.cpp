#include "agents/AgentMcpServer.h"
#include "cloud/CloudControlClient.h"
#include "service/DaemonService.h"
#include "service/UnixSignals.h"

#include "KetPlusBuildIdentity.h"
#include "KetPlusProductVersion.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QTextStream>

#include <utility>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#endif

using namespace ketplus::daemon;

namespace {

void addBundledPluginPath() {
#if defined(Q_OS_MACOS)
    const QDir executableDirectory(QCoreApplication::applicationDirPath());
    const QString pluginPath =
        QDir(executableDirectory.filePath(QStringLiteral("../PlugIns"))).canonicalPath();
    if (!pluginPath.isEmpty()) {
        QCoreApplication::addLibraryPath(pluginPath);
    }
#endif
}

} // namespace

int main(int argc, char* argv[]) {
#if defined(Q_OS_UNIX)
    // Everything the daemon creates (journal, SQLite WAL/SHM, token, credentials, attachment
    // staging) holds transcripts or secrets. Owner-only from creation, not chmod afterwards.
    ::umask(077);
#endif
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ketplus-daemon"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KETPLUS_PRODUCT_VERSION));
    QCoreApplication::setOrganizationDomain(QStringLiteral("ketsuite.com"));
    QCoreApplication::setOrganizationName(QStringLiteral("Ketsuite"));
    addBundledPluginPath();

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("KetPlus local session daemon"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOptions(
        {{{QStringLiteral("s"), QStringLiteral("socket")},
          QStringLiteral("Local socket name or path."),
          QStringLiteral("name")},
         {QStringLiteral("state"), QStringLiteral("SQLite journal path."), QStringLiteral("path")},
         {QStringLiteral("token-file"), QStringLiteral("Local credential path."),
          QStringLiteral("path")},
         {QStringLiteral("codex"), QStringLiteral("Codex executable path."),
          QStringLiteral("path")},
         {QStringLiteral("claude"), QStringLiteral("Claude Agent SDK sidecar path."),
          QStringLiteral("path")},
         {QStringLiteral("cursor"), QStringLiteral("Cursor CLI (`agent`) executable path."),
          QStringLiteral("path")},
         {QStringLiteral("copilot"), QStringLiteral("GitHub Copilot CLI executable path."),
          QStringLiteral("path")},
         {QStringLiteral("check-runtime"),
          QStringLiteral("Verify that required bundled Qt plugins can be loaded.")},
         {QStringLiteral("build-info"),
          QStringLiteral("Print the version, source revision and relay this build uses.")},
         {QStringLiteral("mcp-agents"),
          QStringLiteral("Serve one conversation's ketplus_agents MCP tools on stdio.")}});
    parser.process(application);

    // ADR 0027: a provider launches this mode as the conversation's agent messaging server.
    if (parser.isSet(QStringLiteral("mcp-agents")))
        return agent_mcp::run();

    if (parser.isSet(QStringLiteral("build-info"))) {
        const QJsonObject info{
            {QStringLiteral("product_version"), QStringLiteral(KETPLUS_PRODUCT_VERSION)},
            {QStringLiteral("build_revision"), QStringLiteral(KETPLUS_BUILD_REVISION)},
            {QStringLiteral("control_url"), CloudControlClient::configuredControlUrl().toString()},
#if defined(KETPLUS_REMOTE_DEMO_AVAILABLE)
            {QStringLiteral("control_url_overridable"), true},
#else
            {QStringLiteral("control_url_overridable"), false},
#endif
        };
        QTextStream(stdout) << QJsonDocument(info).toJson(QJsonDocument::Compact) << '\n';
        return 0;
    }

    if (parser.isSet(QStringLiteral("check-runtime"))) {
        if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
            QTextStream(stderr) << "KetPlus Daemon runtime check failed: QSQLITE is unavailable.\n";
            return 1;
        }
        QTextStream(stdout) << "ketplus-daemon runtime ok\n";
        return 0;
    }

    DaemonConfiguration configuration = DaemonService::defaultConfiguration();
    if (parser.isSet(QStringLiteral("socket"))) {
        configuration.socketName = parser.value(QStringLiteral("socket"));
    }
    if (parser.isSet(QStringLiteral("state"))) {
        configuration.statePath = parser.value(QStringLiteral("state"));
    }
    if (parser.isSet(QStringLiteral("token-file"))) {
        configuration.tokenPath = parser.value(QStringLiteral("token-file"));
    }
    if (parser.isSet(QStringLiteral("codex"))) {
        configuration.codexExecutable = parser.value(QStringLiteral("codex"));
    }
    if (parser.isSet(QStringLiteral("claude"))) {
        configuration.claudeExecutable = parser.value(QStringLiteral("claude"));
    }
    if (parser.isSet(QStringLiteral("cursor"))) {
        configuration.cursorExecutable = parser.value(QStringLiteral("cursor"));
    }
    if (parser.isSet(QStringLiteral("copilot"))) {
        configuration.copilotExecutable = parser.value(QStringLiteral("copilot"));
    }

    // One line a support log can be matched to a build and a relay by.
    qInfo("ketplus-daemon %s (%s) relay %s", KETPLUS_PRODUCT_VERSION, KETPLUS_BUILD_REVISION,
          qUtf8Printable(CloudControlClient::configuredControlUrl().toString()));
#if defined(Q_OS_UNIX)
    UnixSignals shutdownSignals;
    if (!shutdownSignals.isReady()) {
        QTextStream(stderr) << "Unable to install daemon shutdown handlers.\n";
        return 1;
    }
#endif
    DaemonService service(std::move(configuration));
    if (!service.start()) {
        QTextStream(stderr) << "KetPlus Daemon failed to start: " << service.errorString() << '\n';
        return 1;
    }
    return application.exec();
}
