#include "BrowserToolLaunch.h"
#include "FakeCopilotTools.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "providers/CopilotAdapter.h"
#include "runtime/ApprovalBroker.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include <QDateTime>
#include <QDir>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
using namespace ketplus::daemon;
namespace browser = ketplus::browser_tools;
namespace {
QJsonArray records(const QString& path, const QString& method = {}) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QJsonArray result;
    for (const auto& line : file.readAll().split('\n')) {
        const auto doc = QJsonDocument::fromJson(line);
        if (doc.isObject() && (method.isEmpty() || doc.object()["method"] == method))
            result.append(doc.object());
    }
    return result;
}
QJsonArray replies(const QString& path) {
    QJsonArray result;
    for (const auto& value : records(path))
        if (value.toObject()["id"].toInt() == 77 && !value.toObject().contains("method"))
            result.append(value.toObject()["result"].toObject()["outcome"]);
    return result;
}
QJsonObject turn(const QString& prompt = "ask") {
    return {{"sessionId", "tools-session"},
            {"cwd", QDir::tempPath()},
            {"prompt", prompt},
            {"permission", "workspace-write"}};
}
bool ended(const QSignalSpy& events) {
    for (const auto& event : events)
        if (event[1].toString() == "turn.completed")
            return true;
    return false;
}
void start(CopilotAdapter& adapter, const QJsonObject& params) {
    adapter.call("turn/start", params, [](const QJsonObject&, const QString& error) {
        QVERIFY2(error.isEmpty(), qPrintable(error));
    });
}
QJsonObject command(const QString& type, const QJsonObject& payload, const QString& id = {}) {
    QJsonObject result{{"protocol_version", protocol::currentVersion},
                       {"message_id", type},
                       {"message_type", type},
                       {"sent_at", protocol::timestamp()},
                       {"payload", payload}};
    if (!id.isEmpty()) {
        result.insert("session_id", id);
        result.insert("session_generation", 1);
    }
    return result;
}
void writeFile(const QString& path) {
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
}
} // namespace
class CopilotToolsTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
#ifdef Q_OS_WIN
        QSKIP("POSIX fake ACP process; no real provider launched");
#endif
    }
    void permissionOptions_data() {
        QTest::addColumn<QString>("prompt");
        QTest::addColumn<bool>("allow");
        QTest::addColumn<QString>("expected");
        QTest::newRow("allow-once") << "ask" << true << "allow-custom";
        QTest::newRow("reject-once") << "ask" << false << "deny-custom";
        QTest::newRow("never-promote-always") << "always-only" << true << "";
        QTest::newRow("missing-options-cancels") << "no-options" << true << "";
        QTest::newRow("unknown-session-cancels") << "foreign" << true << "";
    }
    void permissionOptions() {
        QFETCH(QString, prompt);
        QFETCH(bool, allow);
        QFETCH(QString, expected);
        QTemporaryDir root;
        const QString log = root.filePath("log"), cli = root.filePath("copilot");
        QVERIFY(writeFakeCopilotTools(cli, log));
        CopilotAdapter adapter(cli);
        QSignalSpy events(&adapter, &ProviderAdapter::canonicalEvent);
        QSignalSpy requests(&adapter, &ProviderAdapter::requestReceived);
        connect(&adapter, &ProviderAdapter::requestReceived, &adapter,
                [&](const QJsonValue& id, const QString&, const QJsonObject&) {
                    adapter.respond(id, {{"allow", allow}});
                });
        start(adapter, turn(prompt));
        QTRY_VERIFY(ended(events));
        QTRY_COMPARE(replies(log).size(), 1);
        const auto outcome = replies(log).first().toObject();
        QCOMPARE(outcome["outcome"].toString(), expected.isEmpty() ? "cancelled" : "selected");
        QCOMPARE(outcome["optionId"].toString(), expected);
        QCOMPARE(requests.size(), (prompt == "no-options" || prompt == "foreign") ? 0 : 1);
        if (allow && !expected.isEmpty()) {
            bool image = false;
            for (const auto& event : events)
                if (event[1].toString() == "item.completed") {
                    const auto content = event[2]
                                             .toJsonObject()["item"]
                                             .toObject()["result"]
                                             .toObject()["content"]
                                             .toArray();
                    if (!content.isEmpty())
                        image |= content.first().toObject()["mimeType"] == "image/png";
                }
            QVERIFY(image);
        }
    }
    void cancellationAndStaleAnswer_data() {
        QTest::addColumn<bool>("restart");
        QTest::newRow("interrupt") << false;
        QTest::newRow("restart") << true;
    }
    void cancellationAndStaleAnswer() {
        QFETCH(bool, restart);
        QTemporaryDir root;
        const auto cli = root.filePath("copilot"), log = root.filePath("log");
        QVERIFY(writeFakeCopilotTools(cli, log));
        CopilotAdapter adapter(cli);
        QSignalSpy requests(&adapter, &ProviderAdapter::requestReceived);
        QSignalSpy events(&adapter, &ProviderAdapter::canonicalEvent);
        start(adapter, turn());
        QTRY_COMPARE(requests.size(), 1);
        const auto stale = requests.first()[0].toJsonValue();
        if (restart)
            adapter.stop();
        else
            adapter.call("turn/interrupt", {{"sessionId", "tools-session"}},
                         [](const QJsonObject&, const QString&) {});
        QTRY_VERIFY(ended(events));
        if (!restart) {
            QTRY_COMPARE(replies(log).size(), 1);
            QCOMPARE(replies(log).first().toObject()["outcome"], "cancelled");
        }
        events.clear();
        start(adapter, turn());
        QTRY_COMPARE(requests.size(), 2);
        const auto current = requests.last()[0].toJsonValue();
        QVERIFY(stale != current);
        const int before = replies(log).size();
        adapter.respond(stale, {{"allow", true}});
        // The current request stays pending; only its new opaque ID may answer wire id 77.
        adapter.respond(current, {{"allow", false}});
        QTRY_VERIFY(ended(events));
        QTRY_COMPARE(replies(log).size(), before + 1);
        QCOMPARE(replies(log).last().toObject()["optionId"], "deny-custom");
    }
    void brokerExpiryIsCancelled() {
        QTemporaryDir root;
        const auto cli = root.filePath("copilot"), log = root.filePath("log");
        QVERIFY(writeFakeCopilotTools(cli, log));
        CopilotAdapter adapter(cli);
        ApprovalBroker broker;
        connect(&adapter, &ProviderAdapter::requestReceived, &broker,
                [&](const QJsonValue& id, const QString&, const QJsonObject&) {
                    (void)broker.request("tools-session", 1, id, "approval",
                                         {{"request_type", "copilot_tool"}}, 10);
                });
        connect(&broker, &ApprovalBroker::providerResolutionReady, &adapter,
                &CopilotAdapter::respond);
        start(adapter, turn());
        QTRY_COMPARE(replies(log).size(), 1);
        QCOMPARE(replies(log).first().toObject()["outcome"], "cancelled");
    }
    void browserConfigurationAndCleanup_data() {
        QTest::addColumn<QString>("ending");
        for (const auto* ending : {"complete", "fail", "interrupt", "stop", "setup-fail"})
            QTest::newRow(ending) << QString(ending);
    }
    void browserConfigurationAndCleanup() {
        QFETCH(QString, ending);
        QTemporaryDir root;
        const auto cli = root.filePath("copilot"), log = root.filePath("log");
        const auto app = root.filePath("browser"), node = root.filePath("node");
        writeFile(app + "/src/mcp.mjs");
        writeFile(node);
        QVERIFY(QFile::setPermissions(node, QFile::ReadOwner | QFile::ExeOwner));
        QVERIFY(QDir().mkpath(app + "/node_modules/@modelcontextprotocol/server"));
        QVERIFY(QDir().mkpath(app + "/node_modules/playwright-core"));
        browser::Settings settings;
        settings.browserAppPath = app;
        settings.nodePath = node;
        settings.allowedOrigins = "https://example.test";
        QVERIFY(browser::saveSettings(settings, browser::settingsFilePath()));
        QString signal;
        const auto cleanup = qScopeGuard([&] {
            if (!signal.isEmpty())
                QFile::remove(signal);
        });
        QVERIFY(writeFakeCopilotTools(cli, log));
        CopilotAdapter adapter(cli);
        QSignalSpy requests(&adapter, &ProviderAdapter::requestReceived);
        QSignalSpy events(&adapter, &ProviderAdapter::canonicalEvent);
        const QByteArray png =
            QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/"
                                   "x8AAwMCAO+aWQ0AAAAASUVORK5CYII=");
        const QString imagePath = root.filePath("user.png");
        QFile image(imagePath);
        QVERIFY(image.open(QIODevice::WriteOnly));
        QCOMPARE(image.write(png), png.size());
        image.close();
        auto input = turn();
        input.insert("images", QJsonArray{imagePath});
        input.insert("permission", "danger-full-access");
        input.insert("mcpServers", QJsonArray{QJsonObject{{"name", "untrusted"}}});
        input.insert("browserInstructions", "untrusted override");
        bool setupFailed = false;
        if (ending == "setup-fail") {
            writeFile(log + ".newfail");
            adapter.call("turn/start", input, [&](const QJsonObject&, const QString& error) {
                setupFailed = error.contains("MCP setup failed");
            });
            QTRY_VERIFY(setupFailed);
            QCOMPARE(requests.size(), 0);
        } else {
            start(adapter, input);
            QTRY_COMPARE(requests.size(), 1);
        }
        // Cursor-shaped identity AND a familiar title still ask even with host injection.
        QCOMPARE(replies(log).size(), 0);
        const auto setup = records(log, "session/new").first().toObject()["params"].toObject();
        QCOMPARE(setup["cwd"], input["cwd"]);
        const auto servers = setup["mcpServers"].toArray();
        QVERIFY(!servers.isEmpty());
        const auto server = servers.first().toObject();
        QCOMPARE(server["name"], "ketplus_browser");
        QCOMPARE(server["command"], node);
        for (const auto& env : server["env"].toArray())
            if (env.toObject()["name"] == "KETPLUS_BROWSER_TURN_END_FILE")
                signal = env.toObject()["value"].toString();
        QVERIFY(!signal.isEmpty());
        if (ending == "setup-fail") {
            QTRY_VERIFY(QFileInfo::exists(signal));
            return;
        }
        QFile::remove(signal);
        const auto prompt = records(log, "session/prompt")
                                .first()
                                .toObject()["params"]
                                .toObject()["prompt"]
                                .toArray();
        QVERIFY(prompt.first().toObject()["text"].toString().contains("browser_create_session"));
        QVERIFY(!prompt.first().toObject()["text"].toString().contains("untrusted override"));
        QCOMPARE(prompt[1].toObject()["text"], "ask");
        QCOMPARE(prompt.last().toObject()["type"], "image");
        QCOMPARE(prompt.last().toObject()["data"].toString(), QString::fromLatin1(png.toBase64()));
        if (ending == "stop")
            adapter.stop();
        else if (ending == "interrupt")
            adapter.call("turn/interrupt", {{"sessionId", "tools-session"}},
                         [](const QJsonObject&, const QString&) {});
        else
            adapter.respond(requests.first()[0].toJsonValue(), {{"allow", true}});
        QTRY_VERIFY(QFileInfo::exists(signal));
        if (ending == "fail") {
            events.clear();
            QVERIFY(QFile::remove(signal));
            start(adapter, turn("fail"));
            QTRY_VERIFY(ended(events));
            QTRY_VERIFY(QFileInfo::exists(signal));
        }
        if (ending != "stop") {
            for (int policy = 0; policy < 4; ++policy) {
                events.clear();
                settings.enabled = policy != 0;
                settings.allowedOrigins = policy == 2 ? "not-an-origin" : "https://example.test";
                settings.browserAppPath = policy == 3 ? root.filePath("missing-runtime") : app;
                QVERIFY(browser::saveSettings(settings, browser::settingsFilePath()));
                if (policy == 1) {
                    QFile config(browser::settingsFilePath());
                    QVERIFY(config.open(QIODevice::WriteOnly));
                    config.write("{malformed");
                }
                start(adapter, turn("complete"));
                QTRY_VERIFY(ended(events));
                const auto load =
                    records(log, "session/load").last().toObject()["params"].toObject();
                for (const auto& value : load["mcpServers"].toArray())
                    QVERIFY(value.toObject()["name"] != "ketplus_browser");
                const auto unavailable = records(log, "session/prompt")
                                             .last()
                                             .toObject()["params"]
                                             .toObject()["prompt"]
                                             .toArray();
                QVERIFY(unavailable.first().toObject()["text"].toString().contains(
                    "disabled or unavailable"));
            }
        }
    }

    void hostApprovalCardAndResolution() {
        QTemporaryDir root;
        const auto cli = root.filePath("copilot"), log = root.filePath("log");
        QVERIFY(writeFakeCopilotTools(cli, log));
        LocalJournal journal(root.filePath("journal.sqlite"));
        QVERIFY(journal.isOpen());
        CodexAdapter codex(root.filePath("missing"));
        CopilotAdapter adapter(cli);
        SessionHost host(&journal, &codex, QList<ProviderAdapter*>{&adapter});
        const auto result = host.handleCommand(
            command("session.start_turn", {{"worktree_path", root.path()},
                                           {"prompt", "ask"},
                                           {"options", QJsonObject{{"provider", "copilot"}}}}),
            "desktop-test");
        const QString id = result["session_id"].toString();
        QVERIFY(!id.isEmpty());
        const auto cards = [&] {
            QJsonArray found;
            for (const auto& event : journal.eventsAfter(id, 1, 0))
                if (event.toObject()["event_type"] == "approval.requested")
                    found.append(event.toObject()["payload"]);
            return found;
        };
        QTRY_COMPARE(cards().size(), 1);
        const auto card = cards().first().toObject();
        QVERIFY(card["reason"].toString().contains("GitHub Copilot"));
        const auto resolved =
            host.handleCommand(command("approval.resolve",
                                       {{"approval_id", card["approval_id"]},
                                        {"resolution", QJsonObject{{"decision", "accept"}}}},
                                       id),
                               "desktop-test");
        QVERIFY(resolved["ok"].toBool());
        QTRY_COMPARE(replies(log).size(), 1);
        QCOMPARE(replies(log).first().toObject()["optionId"], "allow-custom");
        const auto screenshot = [&] {
            for (const auto& event : journal.eventsAfter(id, 1, 0)) {
                const auto row = event.toObject();
                if (row["event_type"] != "item.completed")
                    continue;
                const auto content = row["payload"]
                                         .toObject()["item"]
                                         .toObject()["result"]
                                         .toObject()["content"]
                                         .toArray();
                for (const auto& block : content)
                    if (block.toObject()["media_ref"].toString().startsWith("sha256:"))
                        return block.toObject();
            }
            return QJsonObject{};
        };
        QTRY_VERIFY(!screenshot().isEmpty());
        QCOMPARE(screenshot()["mime"], "image/png");
        QVERIFY(!screenshot().contains("data"));
        const QString hash = screenshot()["media_ref"].toString().mid(7);
        QVERIFY(QFileInfo::exists(root.filePath("media/" + hash + ".png")));
    }
};
QTEST_GUILESS_MAIN(CopilotToolsTest)
#include "CopilotToolsTest.moc"
