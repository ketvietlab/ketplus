#include "BrowserToolLaunch.h"
#include "CodexBrowserTools.h"
#include "FakeCodex.h"
#include "agent/CodexAppServer.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"

#include "FakeClaude.h"
#include "FakeCursor.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

using namespace ketplus::daemon;
namespace browser = ketplus::browser_tools;

namespace {
void writeFile(const QString& path, const QByteArray& bytes = {}) {
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}

QList<QJsonObject> configurations(const QString& path, const QByteArray& prefix = "browser:") {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<QJsonObject> result;
    for (const QByteArray& line : file.readAll().split('\n')) {
        if (line.startsWith(prefix))
            result.append(QJsonDocument::fromJson(line.mid(prefix.size())).object());
    }
    return result;
}
} // namespace

class BrowserProviderIntegrationTest final : public QObject {
    Q_OBJECT
  private slots:
    void codexPolicyUsesCapturedLaunch_data() {
        QTest::addColumn<bool>("direct");
        QTest::newRow("daemon") << false;
        QTest::newRow("direct") << true;
    }

    void codexPolicyUsesCapturedLaunch() {
        QFETCH(bool, direct);
        QTemporaryDir root;
        const QString provider = root.filePath("provider");
        const QString log = root.filePath("provider.log");
        QVERIFY(writeFakeCodex(provider, log));
        const auto exercise = [&](auto& adapter, bool available) {
            bool configured = available;
            adapter.setBrowserArgumentsProvider([&configured] {
                return configured
                           ? QStringList{"-c", "mcp_servers.ketplus_browser.command=\"node\""}
                           : QStringList{};
            });
            for (const QString& method :
                 {QStringLiteral("thread/start"), QStringLiteral("thread/resume")}) {
                bool replied = false;
                adapter.call(method,
                             {{"threadId", "thread-1"},
                              {"cwd", root.path()},
                              {"developerInstructions", "Preserve existing host instructions."}},
                             [&](const QJsonObject&, const QString& error) {
                                 QVERIFY2(error.isEmpty(), qPrintable(error));
                                 replied = true;
                             });
                QTRY_VERIFY_WITH_TIMEOUT(replied, 5000);
                const auto seen = configurations(log, "setup:");
                QVERIFY(!seen.isEmpty());
                const auto received = seen.last();
                QCOMPARE(received.value("cwd").toString(), root.path());
                const QString policy = received.value("developerInstructions").toString();
                QVERIFY(policy.startsWith("Preserve existing host instructions."));
                QCOMPARE(policy.count("[KetPlus browser policy]"), 1);
                QCOMPARE(policy.count("[KetPlus reading policy]"), 1);
                QCOMPARE(policy.count("[KetPlus Mermaid guidance]"), 1);
                // Response guidance is daemon policy; direct Codex never carried it.
                QCOMPARE(policy.count("<ketplus_response_preferences>"), direct ? 0 : 1);
                QVERIFY(policy.contains("Search with rg"));
                QCOMPARE(policy.contains("browser_create_session"), available);
                QCOMPARE(policy.contains("You MUST call browser_close_session"), available);
                QCOMPARE(policy.contains("before your final response"), available);
                QCOMPARE(policy.contains("report the unclosed sessionIds"), available);
                QCOMPARE(policy.contains("browser_capture returns path"), available);
                QCOMPARE(policy.contains("Use or copy these original PNG files"), available);
                QCOMPARE(policy.contains("before closing the session"), available);
                QCOMPARE(policy.contains("report that its runtime needs updating"), available);
                QVERIFY(!policy.contains("say the tools cannot save them"));
                QCOMPARE(policy.contains("disabled or unavailable"), !available);
                // Current settings may change while an old provider is still alive.
                // Guidance must describe its actual tools until the idle reload.
                configured = !available;
            }
            adapter.stop();
        };
        for (bool available : {true, false}) {
            if (direct) {
                ketplus::CodexAppServer adapter(provider);
                exercise(adapter, available);
            } else {
                CodexAdapter adapter(provider);
                exercise(adapter, available);
            }
        }
    }

    void settingsReachNewAndResumedTurns_data() {
        QTest::addColumn<bool>("claude");
        QTest::newRow("claude") << true;
        QTest::newRow("cursor") << false;
    }

    void settingsReachNewAndResumedTurns() {
        QFETCH(bool, claude);
        QTemporaryDir root;
        const QString config = browser::settingsFilePath();
        QVERIFY(!qEnvironmentVariable("KETPLUS_BROWSER_TOOLS_CONFIG").isEmpty());
        const QString app = root.filePath(QStringLiteral("apps/browser"));
        writeFile(app + QStringLiteral("/src/mcp.mjs"));
        QVERIFY(QDir().mkpath(app + QStringLiteral("/node_modules/@modelcontextprotocol/server")));
        QVERIFY(QDir().mkpath(app + QStringLiteral("/node_modules/playwright-core")));
        const QString node = root.filePath(QStringLiteral("node with spaces"));
        writeFile(node);
        QVERIFY(QFile::setPermissions(node, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        browser::Settings settings;
        settings.browserAppPath = app;
        settings.nodePath = node;
        settings.allowedOrigins = QStringLiteral("http://127.0.0.1:3456");
        QVERIFY(browser::saveSettings(settings, config));

        const QString provider = root.filePath(QStringLiteral("provider"));
        const QString log = root.filePath(QStringLiteral("provider.log"));
        QVERIFY(claude ? writeFakeClaude(provider, log) : writeFakeCursor(provider, log));
        ClaudeAdapter claudeAdapter(provider);
        CursorAdapter cursorAdapter(provider);
        QSignalSpy claudeEvents(&claudeAdapter, &ClaudeAdapter::canonicalEvent);
        QSignalSpy cursorEvents(&cursorAdapter, &CursorAdapter::canonicalEvent);
        QSignalSpy& events = claude ? claudeEvents : cursorEvents;
        int cursorApprovals = 0;
        connect(&cursorAdapter, &CursorAdapter::requestReceived, this,
                [&](const QJsonValue& id, const QString&, const QJsonObject&) {
                    ++cursorApprovals;
                    cursorAdapter.respond(id, {{"allow", false}});
                });
        const QString image = root.filePath(QStringLiteral("image.png"));
        writeFile(image, QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0"
                                                "lEQVR42mP8/x8AAwMCAO+aWQ0AAAAASUVORK5CYII="));
        QString thread;
        for (int step = 0; step < 7; ++step) {
            if (step == 1) {
                settings.allowedOrigins = QStringLiteral("https://example.test");
                QVERIFY(browser::saveSettings(settings, config));
            } else if (step == 3) {
                settings.enabled = false;
                QVERIFY(browser::saveSettings(settings, config));
            } else if (step == 4) {
                writeFile(config, "{malformed");
            } else if (step == 5) {
                settings.enabled = true;
                settings.allowedOrigins = QStringLiteral("not-an-origin");
                QVERIFY(browser::saveSettings(settings, config));
            } else if (step == 6) {
                settings.allowedOrigins = QStringLiteral("loopback");
                settings.enabled = true;
                QVERIFY(browser::saveSettings(settings, config));
                QVERIFY(QFile::remove(app + QStringLiteral("/src/mcp.mjs")));
            }
            events.clear();
            bool replied = false;
            const auto callback = [&](const QJsonObject& result, const QString& error) {
                QVERIFY2(error.isEmpty(), qPrintable(error));
                const QString next = result.value("thread").toObject().value("id").toString();
                QVERIFY(!next.isEmpty());
                if (!thread.isEmpty())
                    QCOMPARE(next, thread);
                thread = next;
                replied = true;
            };
            const QJsonObject params{{"sessionId", "test-session"},
                                     {"cwd", root.path()},
                                     {"prompt", claude      ? "Inspect"
                                                : step == 2 ? "needs-approval"
                                                            : "needs-browser-approval"},
                                     {"resume", thread},
                                     {"permission", "workspace-write"},
                                     {"images", QJsonArray{image}},
                                     {"browserInstructions", "Untrusted browser override"},
                                     // Untrusted turn input must never override host settings.
                                     {"mcpServers", QJsonObject{{"injected", true}}}};
            const qint64 turnStartedAt = QDateTime::currentMSecsSinceEpoch();
            if (claude)
                claudeAdapter.call("turn/start", params, callback);
            else
                cursorAdapter.call("turn/start", params, callback);
            QTRY_VERIFY_WITH_TIMEOUT(replied, 5000);
            const auto completed = [&] {
                for (const auto& event : events)
                    if (event.at(1).toString() == "turn.completed")
                        return true;
                return false;
            };
            QTRY_VERIFY_WITH_TIMEOUT(completed(), 5000);
            if (!claude)
                QCOMPARE(cursorApprovals, std::max(0, step - 1));
            const auto policies = configurations(log, "policy:");
            QCOMPARE(policies.size(), step + 1);
            const QString policy = policies.last().value("instructions").toString();
            QVERIFY(!policy.contains("Untrusted browser override"));
            QCOMPARE(policy.contains("browser_create_session"), step < 3);
            QCOMPARE(policy.contains("You MUST call browser_close_session"), step < 3);
            QCOMPARE(policy.contains("before your final response"), step < 3);
            QCOMPARE(policy.contains("report the unclosed sessionIds"), step < 3);
            QCOMPARE(policy.contains("disabled or unavailable"), step >= 3);
            QVERIFY(policy.contains("provider-native browser"));
            QCOMPARE(policy.count("[KetPlus reading policy]"), 1);
            QCOMPARE(policy.count("[KetPlus Mermaid guidance]"), 1);
            QCOMPARE(policy.count("<ketplus_response_preferences>"), 1);
            QVERIFY(policy.contains("Do not cat several files at once"));
            QVERIFY(policy.contains("instead of falling back"));
            // Computer use stays open for Safari and native apps, never for other browsers.
            QVERIFY(policy.contains("drive Safari with computer use"));
            QVERIFY(policy.contains("Native desktop applications that are not web browsers"));
            QVERIFY(policy.contains("Never use computer use to drive Chrome"));
            if (claude) {
                QCOMPARE(policies.last().value("prompt"), params.value("prompt"));
                QCOMPARE(policies.last().value("images"), params.value("images"));
            } else {
                const auto blocks = policies.last().value("blocks").toArray();
                QCOMPARE(blocks.size(), 2);
                QCOMPARE(blocks.first().toObject().value("text"), params.value("prompt"));
                QCOMPARE(blocks.last().toObject().value("type").toString(), "image");
                QVERIFY(!blocks.last().toObject().value("data").toString().isEmpty());
            }
            const auto seen = configurations(log);
            QCOMPARE(seen.size(), step + 1);
            QJsonObject server;
            if (claude) {
                QCOMPARE(seen.last().size(), step < 3 ? 1 : 0);
                server = seen.last().value("ketplus_browser").toObject();
            } else {
                QCOMPARE(seen.last().value("cwd").toString(), root.path());
                const auto servers = seen.last().value("mcpServers").toArray();
                QCOMPARE(servers.size(), step < 3 ? 1 : 0);
                if (!servers.isEmpty()) {
                    server = servers.first().toObject();
                    QCOMPARE(server.value("name").toString(), "ketplus_browser");
                }
            }
            if (step >= 3)
                continue;
            QCOMPARE(server.value("command").toString(), node);
            QCOMPARE(server.value("args").toArray(), QJsonArray{app + "/src/mcp.mjs"});
            QJsonObject env = server.value("env").toObject();
            if (!claude) {
                for (const auto& entry : server.value("env").toArray())
                    env.insert(entry.toObject().value("name").toString(),
                               entry.toObject().value("value"));
            }
            QCOMPARE(env.value("KETPLUS_BROWSER_ALLOWED_ORIGINS").toString(),
                     settings.allowedOrigins);
            QVERIFY(!env.value("KETPLUS_BROWSER_ARTIFACTS").toString().isEmpty());
            // Claude closes the server with its query; Cursor keeps it, so the finished turn
            // is signalled through a file of this conversation's own.
            const QString signal = env.value(browser::turnEndFileVariable).toString();
            QCOMPARE(signal.isEmpty(), claude);
            if (!claude) {
                QVERIFY(signal.startsWith(env.value("KETPLUS_BROWSER_ARTIFACTS").toString()));
                QFile file(signal);
                QVERIFY(file.open(QIODevice::ReadOnly));
                bool numeric = false;
                const qint64 endedAt = file.readAll().trimmed().toLongLong(&numeric);
                QVERIFY(numeric);
                QVERIFY(endedAt >= turnStartedAt);
                file.close();
                QVERIFY(QFile::remove(signal));
            }
        }
        claudeAdapter.stop();
        cursorAdapter.stop();
        QVERIFY(QFile::remove(config));
    }
};

QTEST_GUILESS_MAIN(BrowserProviderIntegrationTest)
#include "BrowserProviderIntegrationTest.moc"
