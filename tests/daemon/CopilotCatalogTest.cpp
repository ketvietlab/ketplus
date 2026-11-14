#include "CodexBrowserTools.h"
#include "FakeCopilotModels.h"
#include "providers/CodexAdapter.h"
#include "providers/CopilotAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include <QDir>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
using namespace ketplus::daemon;
namespace {
QJsonArray records(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QJsonArray result;
    for (const auto& line : file.readAll().split('\n')) {
        const auto document = QJsonDocument::fromJson(line);
        if (document.isObject())
            result.append(document.object());
    }
    return result;
}
int count(const QJsonArray& log, const QString& method) {
    int result = 0;
    for (const auto& record : log)
        result += record.toObject().value("method") == method;
    return result;
}
QJsonObject turn(const QString& model) {
    return {{"sessionId", "ketplus-fixture"},
            {"cwd", QDir::tempPath()},
            {"model", model},
            {"prompt", "fixture"},
            {"permission", "workspace-write"}};
}
} // namespace
class CopilotCatalogTest final : public QObject {
    Q_OBJECT
  private slots:
    void parserAndBounds() {
        auto source = copilotModelFixture("config");
        auto snapshot = copilot_catalog::parse(source);
        QCOMPARE(snapshot.models.size(), 2);
        QCOMPARE(snapshot.configId, "vendor-model");
        QCOMPARE(snapshot.current, "model-a");
        QVERIFY(!snapshot.legacy);
        QCOMPARE(snapshot.models.first().toObject().value("provider"), "copilot");
        QVERIFY(!snapshot.models.first().toObject().value("permissions").toArray().isEmpty());
        QVERIFY(snapshot.models.first().toObject().value("efforts").toArray().isEmpty());
        auto config = source["configOptions"].toArray().first().toObject();
        auto options = config["options"].toArray();
        auto first = options.first().toObject();
        first["name"] = QString(5000, 'n');
        first["description"] = QString(5000, 'd');
        options[0] = first;
        options.append(first); // duplicate removed
        options.append(QJsonObject{{"value", QString(257, 'x')}, {"name", "Rejected"}});
        config["options"] = QJsonArray{
            QJsonObject{{"group", "recommended"}, {"name", "Recommended"}, {"options", options}}};
        source["configOptions"] = QJsonArray{config};
        snapshot = copilot_catalog::parse(source);
        QCOMPARE(snapshot.models.size(), 2);
        QCOMPARE(snapshot.models.first().toObject()["label"].toString().size(), 256);
        QCOMPARE(snapshot.models.first().toObject()["description"].toString().size(), 2000);
        QVERIFY(copilot_catalog::parse(copilotModelFixture("too-many")).models.isEmpty());
        config["currentValue"] = "unknown";
        source["configOptions"] = QJsonArray{config};
        QVERIFY(copilot_catalog::parse(source).models.isEmpty());
        const auto legacy = copilot_catalog::parse(copilotModelFixture("legacy"));
        QVERIFY(legacy.legacy);
        QCOMPARE(legacy.models.size(), 2);
        const auto select = copilot_catalog::select(legacy, "session", "model-b");
        QCOMPARE(select.method, "session/set_model");
        QCOMPARE(select.params["modelId"], "model-b");
    }
    void serializedCatalogFitsDaemonFrame() {
        auto source = copilotModelFixture("config");
        auto config = source["configOptions"].toArray().first().toObject();
        QJsonArray options;
        for (int i = 0; i < 256; ++i)
            options.append(
                QJsonObject{{"value", QStringLiteral("model-%1").arg(i)}, {"name", "Model"}});
        config["currentValue"] = "model-0";
        config["options"] = options;
        source["configOptions"] = QJsonArray{config};
        const auto maximumCount = copilot_catalog::parse(source);
        QCOMPARE(maximumCount.models.size(), 256);
        QVERIFY(QJsonDocument(maximumCount.models).toJson(QJsonDocument::Compact).size() <=
                copilot_catalog::maximumCatalogBytes);
        options = {};
        for (int i = 0; i < 40; ++i)
            options.append(QJsonObject{{"value", QStringLiteral("model-%1").arg(i)},
                                       {"name", QString(256, QChar(0x0800))},
                                       {"description", QString(2000, QChar(0x0800))}});
        config["options"] = options;
        source["configOptions"] = QJsonArray{config};
        QVERIFY(QJsonDocument(source).toJson(QJsonDocument::Compact).size() <
                copilot_catalog::maximumOutputBytes);
        QVERIFY(copilot_catalog::parse(source).models.isEmpty());
    }
    void probe_data() {
        QTest::addColumn<QString>("shape");
        QTest::addColumn<bool>("accepted");
        for (const auto& shape :
             {"config", "legacy", "missing", "error", "too-many", "flood-stdout", "flood-stderr",
              "timeout", "request-permission", "stdout-boundary", "stderr-boundary",
              "stdout-overflow", "stderr-overflow", "malformed", "wrong-id", "unsupported-version"})
            QTest::newRow(shape) << QString(shape)
                                 << (QString(shape) == "config" || QString(shape) == "legacy" ||
                                     QString(shape).endsWith("boundary"));
    }
    void probe() {
        QFETCH(QString, shape);
        QFETCH(bool, accepted);
        QTemporaryDir dir;
        const QString log = dir.filePath("log");
        qputenv("KETPLUS_TEST_COPILOT_MODEL_SHAPE", shape.toUtf8());
        qputenv("KETPLUS_TEST_COPILOT_MODEL_LOG", log.toUtf8());
        CopilotCatalog probe(nullptr, shape == "timeout" ? 500 : 2000);
        int callbacks = 0;
        QJsonArray catalog;
        for (int i = 0; i < 3; ++i)
            probe.get(QCoreApplication::applicationFilePath(),
                      [&](const QJsonObject& result, const QString& error) {
                          QVERIFY(error.isEmpty());
                          catalog = result["data"].toArray();
                          ++callbacks;
                      });
        QTRY_COMPARE_WITH_TIMEOUT(callbacks, 3, 5000);
        QCOMPARE(catalog, accepted ? copilot_catalog::parse(copilotModelFixture(shape)).models
                                   : copilot_catalog::fallback());
        const auto logEntries = records(log);
        QCOMPARE(count(logEntries, "session/prompt"), 0);
        int launches = 0;
        for (const auto& value : logEntries) {
            QVERIFY(!value.toObject().contains("floodCompleted"));
            if (value.toObject()["launched"].toBool()) {
                ++launches;
                QCOMPARE(value.toObject()["argv"].toArray(), QJsonArray({"--acp", "--stdio"}));
            }
            if (value.toObject()["method"] == "session/new")
                QVERIFY(value.toObject()["params"].toObject()["mcpServers"].toArray().isEmpty());
        }
        if (shape == "timeout")
            QVERIFY(launches <= 1); // A startup deadline may expire before the child runs main().
        else
            QCOMPARE(launches, 1);
        probe.get(QCoreApplication::applicationFilePath(),
                  [&](const QJsonObject&, const QString&) { ++callbacks; });
        QCOMPARE(callbacks, 4);
        QCOMPARE(probe.findChild<QProcess*>()->state(), QProcess::NotRunning);
    }
    void selection_data() {
        QTest::addColumn<QString>("shape");
        QTest::addColumn<bool>("accepted");
        for (const auto& shape : {"config", "legacy", "empty-load", "no-load", "reject",
                                  "unconfirmed", "missing", "update"})
            QTest::newRow(shape) << QString(shape)
                                 << (QString(shape) != "reject" &&
                                     QString(shape) != "unconfirmed" &&
                                     QString(shape) != "missing");
    }
    void selection() {
        QFETCH(QString, shape);
        QFETCH(bool, accepted);
        QTemporaryDir dir;
        const QString log = dir.filePath("log");
        qputenv("KETPLUS_TEST_COPILOT_MODEL_SHAPE", shape.toUtf8());
        qputenv("KETPLUS_TEST_COPILOT_MODEL_LOG", log.toUtf8());
        CopilotAdapter adapter(QCoreApplication::applicationFilePath());
        QSignalSpy events(&adapter, &ProviderAdapter::canonicalEvent);
        int callbacks = 0;
        QString failure;
        adapter.call("turn/start", turn("model-b"), [&](const QJsonObject&, const QString& error) {
            failure = error;
            ++callbacks;
        });
        QTRY_COMPARE(callbacks, 1);
        QCOMPARE(failure.isEmpty(), accepted);
        if (!accepted) {
            QVERIFY2(failure.contains("model", Qt::CaseInsensitive), qPrintable(failure));
            QCOMPARE(count(records(log), "session/prompt"), 0);
            return;
        }
        const auto completed = [&] {
            for (const auto& event : events)
                if (event[1].toString() == "turn.completed")
                    return true;
            return false;
        };
        QTRY_VERIFY(completed());
        events.clear();
        const QString secondModel = shape == "update" ? "model-b" : "model-a";
        adapter.call("turn/start", turn(secondModel),
                     [&](const QJsonObject&, const QString& error) {
                         failure = error;
                         ++callbacks;
                     });
        QTRY_COMPARE(callbacks, 2);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QTRY_VERIFY(completed());
        QJsonArray used;
        for (const auto& value : records(log))
            if (value.toObject().contains("promptModel"))
                used.append(value.toObject()["promptModel"]);
        QCOMPARE(used, QJsonArray({"model-b", secondModel}));
        if (shape == "update")
            QCOMPARE(count(records(log), "session/set_config_option"), 2);
        if (shape == "legacy")
            QCOMPARE(count(records(log), "session/set_model"), 2);
        if (shape == "no-load")
            QCOMPARE(count(records(log), "session/new"), 2);
    }
    void catalogsReachProviderFacade() {
        qputenv("KETPLUS_TEST_COPILOT_MODEL_SHAPE", "config");
        QTemporaryDir dir;
        qputenv("KETPLUS_TEST_COPILOT_MODEL_LOG", dir.filePath("log").toUtf8());
        CopilotAdapter adapter(QCoreApplication::applicationFilePath());
        int callbacks = 0;
        QJsonArray models, config;
        adapter.call("model/list", {}, [&](const QJsonObject& result, const QString& error) {
            QVERIFY(error.isEmpty());
            models = result["data"].toArray();
            ++callbacks;
        });
        adapter.configurationCatalog([&](const QJsonObject& result, const QString& error) {
            QVERIFY(error.isEmpty());
            config = result["catalog"].toArray();
            ++callbacks;
        });
        QTRY_COMPARE(callbacks, 2);
        QCOMPARE(models, config);
        QCOMPARE(models.size(), 2);
        QVERIFY(!adapter.isReady()); // setup probe did not start the conversation process

        LocalJournal journal(dir.filePath("journal.sqlite"));
        QVERIFY(journal.isOpen());
        CodexAdapter codex(dir.filePath("missing-codex"));
        SessionHost host(&journal, &codex, QList<ProviderAdapter*>{&adapter});
        bool queried = false;
        host.handleProviderQuery({{"message_type", "provider.models"}, {"payload", QJsonObject{}}},
                                 [&](const QJsonObject& result, const QString& error) {
                                     QVERIFY2(error.isEmpty(), qPrintable(error));
                                     QCOMPARE(result["data"].toArray(), models);
                                     queried = true;
                                 });
        QTRY_VERIFY(queried);
    }
    void defaultAndUnknownSelection() {
        QTemporaryDir dir;
        const QString log = dir.filePath("log");
        qputenv("KETPLUS_TEST_COPILOT_MODEL_SHAPE", "missing");
        qputenv("KETPLUS_TEST_COPILOT_MODEL_LOG", log.toUtf8());
        CopilotAdapter adapter(QCoreApplication::applicationFilePath());
        int callbacks = 0;
        adapter.call("turn/start", turn("default"), [&](const QJsonObject&, const QString& error) {
            QVERIFY(error.isEmpty());
            ++callbacks;
        });
        QTRY_COMPARE(callbacks, 1);
        QTRY_COMPARE(count(records(log), "session/prompt"), 1);
        QCOMPARE(count(records(log), "session/set_model"), 0);
        QCOMPARE(count(records(log), "session/set_config_option"), 0);
        const auto snapshot = copilot_catalog::parse(copilotModelFixture("config"));
        QVERIFY(!copilot_catalog::select(snapshot, "id", "model-does-not-exist").error.isEmpty());
        QVERIFY(copilot_catalog::select(snapshot, "id", "model-a").method.isEmpty());
    }
    void cacheExpiresAndStopSettles() {
        QTemporaryDir dir;
        const QString log = dir.filePath("log");
        qputenv("KETPLUS_TEST_COPILOT_MODEL_SHAPE", "config");
        qputenv("KETPLUS_TEST_COPILOT_MODEL_LOG", log.toUtf8());
        CopilotCatalog probe(nullptr, 1000, 0);
        int callbacks = 0;
        const auto callback = [&](const QJsonObject&, const QString&) { ++callbacks; };
        probe.get(QCoreApplication::applicationFilePath(), callback);
        QTRY_COMPARE(callbacks, 1);
        probe.get(QCoreApplication::applicationFilePath(), callback);
        QTRY_COMPARE(callbacks, 2);
        QCOMPARE(count(records(log), "initialize"), 2);
        qputenv("KETPLUS_TEST_COPILOT_MODEL_SHAPE", "timeout");
        probe.get(QCoreApplication::applicationFilePath(), callback);
        QTRY_COMPARE(probe.findChild<QProcess*>()->state(), QProcess::Running);
        probe.stop();
        QTRY_COMPARE(callbacks, 3);
        QCOMPARE(probe.findChild<QProcess*>()->state(), QProcess::NotRunning);
    }
    void failedSelectionSignalsBrowserTurnEnd() {
        QTemporaryDir dir;
        const QString log = dir.filePath("log");
        qputenv("KETPLUS_TEST_COPILOT_MODEL_SHAPE", "reject");
        qputenv("KETPLUS_TEST_COPILOT_MODEL_LOG", log.toUtf8());
        const auto previous = qgetenv("KETPLUS_BROWSER_TOOLS_CONFIG");
        const auto restore =
            qScopeGuard([&] { qputenv("KETPLUS_BROWSER_TOOLS_CONFIG", previous); });
        const auto configPath = dir.filePath("browser.json");
        qputenv("KETPLUS_BROWSER_TOOLS_CONFIG", configPath.toUtf8());
        const QString app = dir.filePath("browser");
        QVERIFY(QDir().mkpath(app + "/src"));
        QVERIFY(QDir().mkpath(app + "/node_modules/@modelcontextprotocol/server"));
        QVERIFY(QDir().mkpath(app + "/node_modules/playwright-core"));
        QFile script(app + "/src/mcp.mjs");
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.close();
        ketplus::browser_tools::Settings settings;
        settings.browserAppPath = app;
        settings.nodePath = QCoreApplication::applicationFilePath();
        QVERIFY(ketplus::browser_tools::saveSettings(settings, configPath));
        CopilotAdapter adapter(QCoreApplication::applicationFilePath());
        int callbacks = 0;
        adapter.call("turn/start", turn("model-b"), [&](const QJsonObject&, const QString& error) {
            QVERIFY(!error.isEmpty());
            ++callbacks;
        });
        QTRY_COMPARE(callbacks, 1);
        QString signalFile;
        for (const auto& value : records(log)) {
            const auto object = value.toObject();
            if (object["method"] != "session/new")
                continue;
            for (const auto& server : object["params"].toObject()["mcpServers"].toArray())
                for (const auto& variable : server.toObject()["env"].toArray())
                    if (variable.toObject()["name"] == "KETPLUS_BROWSER_TURN_END_FILE")
                        signalFile = variable.toObject()["value"].toString();
        }
        QVERIFY(!signalFile.isEmpty());
        QVERIFY(QFile::exists(signalFile));
        QVERIFY(QFile::remove(signalFile));
        adapter.stop();
        QVERIFY(!QFile::exists(signalFile)); // failPending must not signal the failed model again.
        QCOMPARE(count(records(log), "session/prompt"), 0);
    }
};
int main(int argc, char** argv) {
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--acp"))
        return runFakeCopilotModels();
    CopilotCatalogTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "CopilotCatalogTest.moc"
