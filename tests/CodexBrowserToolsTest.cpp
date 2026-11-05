#include "CodexBrowserTools.h"
#include "CodexLaunchReloader.h"
#include "CodexServerRequests.h"
#include "ToolPackageRegistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::browser_tools;

namespace {

void touch(const QString& path, const QFileDevice::Permissions permissions = {}) {
    if (QFileInfo::exists(path))
        return; // the fake node is read/execute-only after the first create()
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    if (permissions)
        file.setPermissions(permissions);
}

// A fake source checkout: <root>/apps/browser with mcp.mjs and node_modules, plus a fake node.
struct Checkout final {
    QTemporaryDir root{QDir::tempPath() + QStringLiteral("/ketplus browser \"quoted\"-XXXXXX")};
    QString app() const { return root.filePath(QStringLiteral("apps/browser")); }
    QString node() const { return root.filePath(QStringLiteral("bin/node")); }
    void create(const bool modules = true) const {
        touch(app() + QStringLiteral("/src/mcp.mjs"));
        touch(node(), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        if (modules) {
            QDir().mkpath(app() + QStringLiteral("/node_modules/@modelcontextprotocol/server"));
            QDir().mkpath(app() + QStringLiteral("/node_modules/playwright-core"));
        }
    }
};

Probes fakeProbes(const QString& node, const QString& version, const bool chromium) {
    Probes probes;
    probes.nodeCandidates = [node] { return QStringList{QStringLiteral("/missing/node"), node}; };
    probes.nodeVersion = [node, version](const QString& candidate) {
        return candidate == node ? version : QString();
    };
    probes.chromiumInstalled = [chromium](const QString&, const QString&) { return chromium; };
    return probes;
}

} // namespace

class CodexBrowserToolsTest final : public QObject {
    Q_OBJECT

  private slots:
    void disabledProducesNoArguments();
    void enabledProducesEscapedMcpServerConfig();
    void failsClosedOnInvalidOriginsOrMissingRuntime();
    void normalizesOrigins();
    void tomlStringEscaping();
    void settingsRoundTripWithOwnerOnlyFile();
    void detectsPrerequisitesInOrder();
    void prerequisiteCacheKeyTracksInputsAndSurvivesSettingsSaves();
    void probesAtMostSixNodeCandidates();
    void sourceCheckoutProviderResolvesBrowserPackage();
    void registryUsesFirstResolvingProvider();
    void migratesStageZeroSettings();
    void damagedSettingsFileFailsClosed();
    void launchFallsBackToCachedNode();
    void reloaderDebouncesAndSkipsUnchangedArguments();
    void reloaderDefersRestartUntilIdle();
    void reloaderFollowsFileAndInProcessNotifications();
    void autoApprovesOnlyInjectedBrowserToolApprovals();
};

void CodexBrowserToolsTest::disabledProducesNoArguments() {
    Checkout checkout;
    checkout.create();
    Settings settings;
    settings.enabled = false;
    settings.nodePath = checkout.node();
    QVERIFY(codexArguments(settings, checkout.root.path(), QStringLiteral("/tmp/a")).isEmpty());
    // Defaults: on, all origins.
    settings.enabled = Settings{}.enabled;
    QVERIFY(settings.enabled);
    const QStringList defaults = codexArguments(settings, checkout.root.path(), "/tmp/a");
    QVERIFY(!defaults.isEmpty());
    QVERIFY2(defaults.last().contains(QStringLiteral("KETPLUS_BROWSER_ALLOWED_ORIGINS=\"*\"")),
             qPrintable(defaults.last()));
}

void CodexBrowserToolsTest::enabledProducesEscapedMcpServerConfig() {
    const QByteArray previousPlaywright = qgetenv("PLAYWRIGHT_BROWSERS_PATH");
    qunsetenv("PLAYWRIGHT_BROWSERS_PATH");
    Checkout checkout;
    checkout.create();
    Settings settings;
    settings.enabled = true;
    settings.nodePath = checkout.node();
    settings.allowedOrigins = QStringLiteral(" loopback , https://Staging.Example.com:8443/ ");
    const QString artifacts = QStringLiteral("/data/Ket Plus/\"art\"\\dir");
    const QStringList arguments = codexArguments(
        settings, checkout.root.filePath(QStringLiteral("build/apps/daemon")), artifacts);
    QCOMPARE(arguments.size(), 10);
    for (int index = 0; index < arguments.size(); index += 2)
        QCOMPARE(arguments.at(index), QStringLiteral("-c"));
    QCOMPARE(arguments.at(1),
             QStringLiteral("mcp_servers.ketplus_browser.command=") + tomlString(checkout.node()));
    const QString app = QDir(checkout.app()).canonicalPath();
    const QString script = QDir(app).filePath("src/mcp.mjs");
    QCOMPARE(arguments.at(3),
             QStringLiteral("mcp_servers.ketplus_browser.args=[") + tomlString(script) + "]");
    QVERIFY(arguments.at(3).contains(QStringLiteral("\\\"quoted\\\"")));
    QCOMPARE(arguments.at(5), QStringLiteral("mcp_servers.ketplus_browser.cwd=") + tomlString(app));
    QCOMPARE(arguments.at(7),
             QStringLiteral("mcp_servers.ketplus_browser.default_tools_approval_mode=\"approve\""));
    QCOMPARE(arguments.at(9),
             QStringLiteral("mcp_servers.ketplus_browser.env={KETPLUS_BROWSER_ALLOWED_ORIGINS="
                            "\"loopback,https://staging.example.com:8443\","
                            "KETPLUS_BROWSER_ARTIFACTS=\"/data/Ket Plus/\\\"art\\\"\\\\dir\"}"));

    // A Playwright cache found through PLAYWRIGHT_BROWSERS_PATH is forwarded to the server.
    qputenv("PLAYWRIGHT_BROWSERS_PATH", "/opt/pw browsers");
    const QStringList forwarded = codexArguments(settings, checkout.root.path(), artifacts);
    QVERIFY2(
        forwarded.last().contains(QStringLiteral("PLAYWRIGHT_BROWSERS_PATH=\"/opt/pw browsers\"")),
        qPrintable(forwarded.last()));
    if (previousPlaywright.isEmpty())
        qunsetenv("PLAYWRIGHT_BROWSERS_PATH");
    else
        qputenv("PLAYWRIGHT_BROWSERS_PATH", previousPlaywright);

    // Env names become TOML bare keys: only [A-Z0-9_] names are emitted; no cwd when unset.
    ToolLaunch launch;
    launch.command = QStringLiteral("/bin/node");
    launch.env = {{QStringLiteral("GOOD_NAME_1"), QStringLiteral("v")},
                  {QStringLiteral("bad-name"), QStringLiteral("x")},
                  {QStringLiteral("EVIL=1,X"), QStringLiteral("y")},
                  {QStringLiteral("lower"), QStringLiteral("z")}};
    const QStringList filtered = mcpServerConfigArguments(launch);
    QCOMPARE(filtered.size(), 8);
    QCOMPARE(filtered.at(5),
             QStringLiteral("mcp_servers.ketplus_browser.default_tools_approval_mode=\"approve\""));
    QCOMPARE(filtered.last(),
             QStringLiteral("mcp_servers.ketplus_browser.env={GOOD_NAME_1=\"v\"}"));
    launch.workingDirectory = QStringLiteral("/w d");
    QCOMPARE(mcpServerConfigArguments(launch).at(5),
             QStringLiteral("mcp_servers.ketplus_browser.cwd=\"/w d\""));
}

void CodexBrowserToolsTest::failsClosedOnInvalidOriginsOrMissingRuntime() {
    Checkout checkout;
    checkout.create(false);
    Settings settings;
    settings.enabled = true;
    settings.nodePath = checkout.node();
    QVERIFY(codexArguments(settings, checkout.root.path(), "/tmp/a").isEmpty()); // no modules
    Checkout ready;
    ready.create();
    settings.nodePath = ready.node();
    settings.allowedOrigins = QStringLiteral("loopback,ftp://example.com");
    QVERIFY(codexArguments(settings, ready.root.path(), "/tmp/a").isEmpty());
    settings.allowedOrigins = QStringLiteral("loopback");
    settings.nodePath = QStringLiteral("/missing/node");
    QVERIFY(codexArguments(settings, ready.root.path(), "/tmp/a").isEmpty());
    settings.nodePath = ready.node();
    QVERIFY(!codexArguments(settings, ready.root.path(), "/tmp/a").isEmpty());
}

void CodexBrowserToolsTest::normalizesOrigins() {
    QString error;
    QCOMPARE(normalizeAllowedOrigins(QString(), &error), QStringLiteral("loopback"));
    QCOMPARE(normalizeAllowedOrigins("http://LOCALHOST:5173, loopback,loopback", &error),
             QStringLiteral("http://localhost:5173,loopback"));
    QVERIFY(error.isEmpty());
    // IPv6 literals keep their brackets (origin-policy.mjs compares URL origins).
    QCOMPARE(normalizeAllowedOrigins("http://[::1]:3000, https://[2001:DB8::1]/", &error),
             QStringLiteral("http://[::1]:3000,https://[2001:db8::1]"));
    QVERIFY(error.isEmpty());
    // "*" allows every origin and absorbs the other (still validated) entries.
    QCOMPARE(normalizeAllowedOrigins(" * ", &error), QStringLiteral("*"));
    QVERIFY(error.isEmpty());
    QCOMPARE(normalizeAllowedOrigins("loopback, *, http://[::1]:3000", &error),
             QStringLiteral("*"));
    QVERIFY(error.isEmpty());
    QVERIFY(normalizeAllowedOrigins("*, ftp://example.com", &error).isEmpty());
    QVERIFY(!error.isEmpty());
    for (const QString bad : {"ftp://example.com", "https://example.com/path",
                              "https://user:pw@example.com", "example.com", "https://e.com?q=1"}) {
        error.clear();
        QVERIFY2(normalizeAllowedOrigins(bad, &error).isEmpty(), qPrintable(bad));
        QVERIFY(!error.isEmpty());
    }
}

void CodexBrowserToolsTest::tomlStringEscaping() {
    QCOMPARE(tomlString(QStringLiteral("a b")), QStringLiteral("\"a b\""));
    QCOMPARE(tomlString(QStringLiteral("C:\\x\"y")), QStringLiteral("\"C:\\\\x\\\"y\""));
    QCOMPARE(tomlString(QStringLiteral("l1\nl2\t\x01")), QStringLiteral("\"l1\\nl2\\t\\u0001\""));
}

void CodexBrowserToolsTest::settingsRoundTripWithOwnerOnlyFile() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("nested/browser-tools.json"));
    QCOMPARE(loadSettings(path).enabled, true);
    QCOMPARE(loadSettings(path).allowedOrigins, QStringLiteral("*"));
    Settings settings;
    settings.enabled = true;
    settings.allowedOrigins = QStringLiteral("https://example.com");
    settings.browserAppPath = QStringLiteral("/src/apps/browser");
    settings.nodePath = QStringLiteral("/opt/homebrew/bin/node");
    QVERIFY(saveSettings(settings, path));
    const Settings loaded = loadSettings(path);
    QVERIFY(loaded.enabled);
    QCOMPARE(loaded.allowedOrigins, settings.allowedOrigins);
    QCOMPARE(loaded.browserAppPath, settings.browserAppPath);
    QCOMPARE(loaded.nodePath, settings.nodePath);
#if defined(Q_OS_UNIX)
    QCOMPARE(QFile::permissions(path) & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
             QFileDevice::Permissions{});
#endif
    qputenv("KETPLUS_BROWSER_TOOLS_CONFIG", path.toUtf8());
    QCOMPARE(settingsFilePath(), path);
    qunsetenv("KETPLUS_BROWSER_TOOLS_CONFIG");
}

void CodexBrowserToolsTest::detectsPrerequisitesInOrder() {
    Checkout checkout;
    checkout.create(false);
    Settings settings;
    const QString start = checkout.root.filePath(QStringLiteral("build/apps/desktop"));

    Prerequisites result =
        detectPrerequisites(settings, start, fakeProbes(checkout.node(), "v20.11.0", true));
    QVERIFY(!result.ok());
    QVERIFY(result.reason.contains(QStringLiteral("Node.js 22")));

    result = detectPrerequisites(settings, start, fakeProbes(checkout.node(), "v22.16.0", true));
    QVERIFY(result.nodeOk);
    QCOMPARE(result.nodePath, checkout.node());
    QVERIFY(!result.modulesOk);
    QVERIFY(result.reason.contains(QStringLiteral("npm ci")));

    checkout.create(true);
    result = detectPrerequisites(settings, start, fakeProbes(checkout.node(), "v24.1.0", false));
    QVERIFY(result.modulesOk);
    QVERIFY(!result.chromiumOk);
    QVERIFY(result.reason.contains(QStringLiteral("install:browser")));

    result = detectPrerequisites(settings, start, fakeProbes(checkout.node(), "v24.1.0", true));
    QVERIFY(result.ok());
    QCOMPARE(result.browserAppPath, QDir(checkout.app()).canonicalPath());

    QTemporaryDir elsewhere;
    result = detectPrerequisites(settings, elsewhere.path(),
                                 fakeProbes(checkout.node(), "v24.1.0", true));
    QVERIFY(!result.ok());
    QVERIFY(result.reason.contains(QStringLiteral("source checkout")));
}

void CodexBrowserToolsTest::prerequisiteCacheKeyTracksInputsAndSurvivesSettingsSaves() {
    Checkout checkout;
    checkout.create();
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("browser-tools.json"));
    Settings settings;
    settings.nodePath = checkout.node();
    const QString start = checkout.root.path();
    const QString key = prerequisiteCacheKey(settings, start);
    QCOMPARE(prerequisiteCacheKey(settings, start), key);

    QVERIFY(!loadPrerequisiteCache(path).has_value());
    Prerequisites result;
    result.nodeOk = result.modulesOk = result.chromiumOk = true;
    result.nodePath = checkout.node();
    result.nodeVersion = QStringLiteral("v22.16.0");
    QVERIFY(savePrerequisiteCache(key, result, path));
    QVERIFY(saveSettings(settings, path)); // must not drop the cache
    auto cached = loadPrerequisiteCache(path);
    QVERIFY(cached.has_value());
    QCOMPARE(cached->key, key);
    QVERIFY(cached->result.ok());
    QCOMPARE(cached->result.nodeVersion, QStringLiteral("v22.16.0"));
    QVERIFY(cached->checkedAt.isValid());
    QCOMPARE(loadSettings(path).nodePath, checkout.node());

    // Node binary mtime change -> miss.
    QFile node(checkout.node());
    QVERIFY(node.open(QIODevice::ReadOnly));
    QVERIFY(node.setFileTime(QDateTime::currentDateTime().addSecs(-3600),
                             QFileDevice::FileModificationTime));
    node.close();
    const QString afterNode = prerequisiteCacheKey(settings, start);
    QVERIFY(afterNode != key);
    // package-lock.json content -> miss.
    QFile lock(checkout.app() + QStringLiteral("/package-lock.json"));
    QVERIFY(lock.open(QIODevice::WriteOnly));
    lock.write("{}");
    lock.close();
    const QString afterLock = prerequisiteCacheKey(settings, start);
    QVERIFY(afterLock != afterNode);
    // Playwright cache directory mtime -> miss.
    QTemporaryDir playwright;
    const QString browsers = playwright.filePath(QStringLiteral("ms-playwright"));
    qputenv("PLAYWRIGHT_BROWSERS_PATH", browsers.toUtf8());
    const QString withoutBrowsers = prerequisiteCacheKey(settings, start);
    QVERIFY(QDir().mkpath(browsers));
    const QString withBrowsers = prerequisiteCacheKey(settings, start);
    QVERIFY(withBrowsers != withoutBrowsers);
    QTest::qWait(1100);                                               // coarse-mtime file systems
    QVERIFY(QDir().mkpath(browsers + QStringLiteral("/chromium-1"))); // bumps the dir mtime
    QVERIFY(prerequisiteCacheKey(settings, start) != withBrowsers);
    qunsetenv("PLAYWRIGHT_BROWSERS_PATH");
}

void CodexBrowserToolsTest::probesAtMostSixNodeCandidates() {
    Checkout checkout;
    checkout.create();
    Probes probes;
    probes.nodeCandidates = [] {
        QStringList candidates;
        for (int index = 0; index < 20; ++index)
            candidates.append(QStringLiteral("/fake/node-%1").arg(index));
        return candidates;
    };
    int probed = 0;
    probes.nodeVersion = [&probed](const QString&) {
        ++probed;
        return QString();
    };
    probes.chromiumInstalled = [](const QString&, const QString&) { return true; };
    QVERIFY(!detectPrerequisites({}, checkout.root.path(), probes).nodeOk);
    QCOMPARE(probed, maximumNodeCandidates);
}

void CodexBrowserToolsTest::sourceCheckoutProviderResolvesBrowserPackage() {
    Checkout checkout;
    checkout.create();
    QFile manifest(checkout.app() + QStringLiteral("/package.json"));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(R"({"name":"@ketplus/browser","version":"0.3.1"})");
    manifest.close();
    Settings settings;
    settings.nodePath = checkout.node();
    const SourceCheckoutProvider provider(settings,
                                          checkout.root.filePath(QStringLiteral("build/apps")));
    const auto launch =
        provider.resolve(QLatin1String(browserToolId), QLatin1String(mcpStdioProtocol));
    QVERIFY(launch.has_value());
    const QString app = QDir(checkout.app()).canonicalPath();
    QCOMPARE(launch->command, checkout.node());
    QCOMPARE(launch->args, QStringList{QDir(app).filePath(QStringLiteral("src/mcp.mjs"))});
    QVERIFY(launch->env.isEmpty());
    QCOMPARE(launch->workingDirectory, app);
    QCOMPARE(launch->packageVersion, QStringLiteral("0.3.1"));
    QCOMPARE(launch->source, QStringLiteral("source-checkout"));

    QVERIFY(!provider.resolve(QStringLiteral("ketplus.other"), QLatin1String(mcpStdioProtocol)));
    QVERIFY(!provider.resolve(QLatin1String(browserToolId), QStringLiteral("json-rpc")));
    settings.nodePath = QStringLiteral("/missing/node");
    QVERIFY(!SourceCheckoutProvider(settings, checkout.root.path())
                 .resolve(QLatin1String(browserToolId), QLatin1String(mcpStdioProtocol)));
    Checkout bare;
    bare.create(false);
    settings.nodePath = bare.node();
    QVERIFY(!SourceCheckoutProvider(settings, bare.root.path())
                 .resolve(QLatin1String(browserToolId), QLatin1String(mcpStdioProtocol)));
}

namespace {
class FixedProvider final : public ToolPackageProvider {
  public:
    FixedProvider(QString tool, QString source, int* calls)
        : tool_(std::move(tool)), source_(std::move(source)), calls_(calls) {}
    std::optional<ToolLaunch> resolve(const QString& toolId, const QString&) const override {
        ++*calls_;
        if (toolId != tool_)
            return std::nullopt;
        ToolLaunch launch;
        launch.command = QStringLiteral("/bin/true");
        launch.source = source_;
        return launch;
    }

  private:
    QString tool_;
    QString source_;
    int* calls_;
};
} // namespace

void CodexBrowserToolsTest::registryUsesFirstResolvingProvider() {
    int calls = 0;
    ToolPackageRegistry registry;
    QVERIFY(!registry.resolve(QLatin1String(browserToolId), QLatin1String(mcpStdioProtocol)));
    registry.addProvider(nullptr);
    registry.addProvider(std::make_unique<FixedProvider>("other", "first", &calls));
    registry.addProvider(std::make_unique<FixedProvider>(browserToolId, "second", &calls));
    registry.addProvider(std::make_unique<FixedProvider>(browserToolId, "third", &calls));
    const auto launch =
        registry.resolve(QLatin1String(browserToolId), QLatin1String(mcpStdioProtocol));
    QVERIFY(launch.has_value());
    QCOMPARE(launch->source, QStringLiteral("second"));
    QCOMPARE(calls, 2);
    QVERIFY(!registry.resolve(QStringLiteral("unknown"), QLatin1String(mcpStdioProtocol)));
}

namespace {
void writeJson(const QString& path, const QJsonObject& object) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(object).toJson());
}
} // namespace

void CodexBrowserToolsTest::migratesStageZeroSettings() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("browser-tools.json"));
    // Stage 0 wrote enabled/origins on every save, so neither was an explicit choice.
    writeJson(path, {{"enabled", false},
                     {"allowedOrigins", "loopback"},
                     {"nodePath", "/opt/node"},
                     {"prereqCache", QJsonObject{{"key", "k"}}}});
    Settings loaded = loadSettings(path);
    QVERIFY(loaded.enabled);
    QCOMPARE(loaded.allowedOrigins, QStringLiteral("*"));
    QCOMPARE(loaded.nodePath, QStringLiteral("/opt/node"));
    // A custom stage-0 list is kept.
    writeJson(path, {{"enabled", true}, {"allowedOrigins", "https://staging.example.com"}});
    QCOMPARE(loadSettings(path).allowedOrigins, QStringLiteral("https://staging.example.com"));

    // Saving writes schema version 2; an explicit disable then sticks.
    loaded = loadSettings(path);
    loaded.enabled = false;
    QVERIFY(saveSettings(loaded, path));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(saved.value("schemaVersion").toInt(), settingsSchemaVersion);
    QVERIFY(!loadSettings(path).enabled);
    writeJson(path, {{"schemaVersion", 2}, {"allowedOrigins", "loopback"}});
    QVERIFY(loadSettings(path).enabled); // missing field: default on
    QCOMPARE(loadSettings(path).allowedOrigins, QStringLiteral("loopback"));
}

void CodexBrowserToolsTest::damagedSettingsFileFailsClosed() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("browser-tools.json"));
    // Missing: defaults (on, all origins), no error.
    QVERIFY(loadSettings(path).enabled);
    QVERIFY(loadSettings(path).loadError.isEmpty());

    const QByteArray garbage("{\"enabled\": tru");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(garbage);
    }
    Settings loaded = loadSettings(path);
    QVERIFY(!loaded.enabled);
    QVERIFY(!loaded.loadError.isEmpty());
    // A cache write must not turn the damaged file into an enabled legacy file.
    QVERIFY(!savePrerequisiteCache(QStringLiteral("key"), Prerequisites{}, path));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), garbage);
    }
    writeJson(path, QJsonObject{}); // valid JSON but not what a legacy file looked like: still fine
    QVERIFY(loadSettings(path).loadError.isEmpty());
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("[1, 2, 3]"); // not an object
    }
    QVERIFY(!loadSettings(path).enabled);

    // Oversized (a valid legacy object that would otherwise load enabled).
    writeJson(path, {{"allowedOrigins", "loopback"},
                     {"padding", QString(maximumSettingsFileBytes, QLatin1Char('x'))}});
    loaded = loadSettings(path);
    QVERIFY(!loaded.enabled);
    QVERIFY(!loaded.loadError.isEmpty());

    // An explicit save repairs the file.
    loaded.enabled = true;
    QVERIFY(saveSettings(loaded, path));
    QVERIFY(loadSettings(path).enabled);
    QVERIFY(loadSettings(path).loadError.isEmpty());
}

void CodexBrowserToolsTest::launchFallsBackToCachedNode() {
    Checkout checkout;
    checkout.create();
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("browser-tools.json"));
    Settings settings;
    QVERIFY(effectiveNodePath(settings, path).isEmpty());
    Prerequisites result;
    result.nodeOk = true;
    result.nodePath = checkout.node();
    QVERIFY(savePrerequisiteCache(QStringLiteral("key"), result, path));
    QCOMPARE(effectiveNodePath(settings, path), checkout.node());
    settings.nodePath = QStringLiteral("/missing/node"); // an explicit path is never replaced
    QVERIFY(effectiveNodePath(settings, path).isEmpty());
}

void CodexBrowserToolsTest::reloaderDebouncesAndSkipsUnchangedArguments() {
    QStringList arguments{QStringLiteral("-c"), QStringLiteral("a")};
    int computed = 0;
    CodexLaunchReloader reloader(
        [&] {
            ++computed;
            return arguments;
        },
        [] { return true; }, QString(), 30);
    QSignalSpy restarts(&reloader, &CodexLaunchReloader::restartRequested);
    // Not running: nothing to restart, the next launch reads settings anyway.
    reloader.settingsChanged();
    QTest::qWait(80);
    QCOMPARE(restarts.count(), 0);

    reloader.launched(arguments);
    for (int index = 0; index < 5; ++index)
        reloader.settingsChanged(); // a burst collapses into one evaluation
    QTRY_COMPARE(computed, 1);
    QCOMPARE(restarts.count(), 0); // same effective arguments

    arguments = QStringList{};
    reloader.settingsChanged();
    reloader.settingsChanged();
    QTRY_COMPARE(restarts.count(), 1);
    QVERIFY(!reloader.running());
    // Relaunched with the new arguments: a rewrite with the same content never loops.
    reloader.launched(arguments);
    reloader.settingsChanged();
    QTest::qWait(80);
    QCOMPARE(restarts.count(), 1);
}

void CodexBrowserToolsTest::reloaderDefersRestartUntilIdle() {
    QStringList arguments{QStringLiteral("old")};
    bool idle = false;
    CodexLaunchReloader reloader([&] { return arguments; }, [&] { return idle; }, QString(), 10);
    QSignalSpy restarts(&reloader, &CodexLaunchReloader::restartRequested);
    reloader.launched(arguments);
    arguments = QStringList{QStringLiteral("new")};
    reloader.settingsChanged();
    QTRY_VERIFY(reloader.restartPending());
    reloader.idleChanged(); // still busy
    QCOMPARE(restarts.count(), 0);
    idle = true;
    reloader.idleChanged(); // the turn completed
    QCOMPARE(restarts.count(), 1);
    QVERIFY(!reloader.restartPending());

    // A pending restart is dropped when the process exits on its own.
    reloader.launched(QStringList{QStringLiteral("old")});
    idle = false;
    reloader.settingsChanged();
    QTRY_VERIFY(reloader.restartPending());
    reloader.stopped();
    idle = true;
    reloader.idleChanged();
    QCOMPARE(restarts.count(), 1);
}

void CodexBrowserToolsTest::reloaderFollowsFileAndInProcessNotifications() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("browser-tools.json"));
    QVERIFY(saveSettings(Settings{}, path));
    CodexLaunchReloader reloader([&path] { return QStringList{loadSettings(path).allowedOrigins}; },
                                 [] { return true; }, path, 20);
    QSignalSpy restarts(&reloader, &CodexLaunchReloader::restartRequested);
    reloader.launched(QStringList{QStringLiteral("*")});
    Settings narrowed;
    narrowed.allowedOrigins = QStringLiteral("loopback");
    QVERIFY(saveSettings(narrowed, path)); // atomic replace, seen through the watcher
    QTRY_COMPARE_WITH_TIMEOUT(restarts.count(), 1, 5000);

    reloader.launched(QStringList{QStringLiteral("loopback")});
    CodexLaunchReloader other([] { return QStringList{QStringLiteral("x")}; }, [] { return true; },
                              QString(), 10);
    QSignalSpy otherRestarts(&other, &CodexLaunchReloader::restartRequested);
    other.launched(QStringList{QStringLiteral("y")});
    notifySettingsChanged();
    QTRY_COMPARE(otherRestarts.count(), 1);
}

void CodexBrowserToolsTest::autoApprovesOnlyInjectedBrowserToolApprovals() {
    using namespace ketplus::codex_requests;
    QJsonObject tagged{{"serverName", "ketplus_browser"},
                       {"mode", "form"},
                       {"_meta", QJsonObject{{"codex_approval_kind", "mcp_tool_call"}}},
                       {"requestedSchema", QJsonObject{{"properties", QJsonObject{}}}}};
    QJsonObject untagged = tagged;
    untagged.remove("_meta");
    QVERIFY(isMcpToolApproval(tagged));
    QVERIFY(!isMcpToolApproval(untagged)); // field-less forms are not approvals on their own
    QJsonObject url = tagged;
    url.insert("mode", "url");
    QVERIFY(!isMcpToolApproval(url));

    // Browser tools disabled: codexArguments() injects nothing, so a user-configured
    // `ketplus_browser` server from ~/.codex/config.toml must not be auto-approved.
    Settings disabled;
    disabled.enabled = false;
    const QStringList none = codexArguments(disabled, QDir::tempPath(), QDir::tempPath());
    QVERIFY(none.isEmpty());
    QVERIFY(!launchedWithBrowserTools(none));
    QVERIFY(!isInjectedBrowserToolApproval(tagged, none));

    const QStringList injected{QStringLiteral("-c"),
                               QStringLiteral("mcp_servers.ketplus_browser.command=\"node\"")};
    QVERIFY(launchedWithBrowserTools(injected));
    QVERIFY(isInjectedBrowserToolApproval(tagged, injected));
    QVERIFY(!isInjectedBrowserToolApproval(untagged, injected));
    QJsonObject other = tagged;
    other.insert("serverName", "docs");
    QVERIFY(!isInjectedBrowserToolApproval(other, injected));
    // Another server whose name merely starts with ours is not ours.
    QVERIFY(!launchedWithBrowserTools(
        {QStringLiteral("-c"), QStringLiteral("mcp_servers.ketplus_browser_x.command=\"x\"")}));

    QCOMPARE(declinedResult(mcpElicitationMethod), (QJsonObject{{"action", "decline"}}));
    QCOMPARE(declinedResult("item/permissions/requestApproval"),
             (QJsonObject{{"permissions", QJsonObject{}}}));
    QCOMPARE(declinedResult("item/commandExecution/requestApproval"),
             (QJsonObject{{"decision", "decline"}}));
}

QTEST_GUILESS_MAIN(CodexBrowserToolsTest)
#include "CodexBrowserToolsTest.moc"
