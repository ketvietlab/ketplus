#include "ComputerToolLaunch.h"
#include "FakeClaude.h"
#include "FakeCursor.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CursorAdapter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;
namespace computer = ketplus::computer_tools;

namespace {
QList<QJsonObject> logged(const QString& path, const QByteArray& prefix) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<QJsonObject> result;
    for (const QByteArray& line : file.readAll().split('\n'))
        if (line.startsWith(prefix))
            result.append(QJsonDocument::fromJson(line.mid(prefix.size())).object());
    return result;
}

QJsonObject computerServer(const QJsonObject& configuration, bool claude) {
    if (claude)
        return configuration.value(QLatin1String(computer::serverName)).toObject();
    for (const auto& value : configuration.value("mcpServers").toArray())
        if (value.toObject().value("name").toString() == QLatin1String(computer::serverName))
            return value.toObject();
    return {};
}

QString envValue(const QJsonObject& server, const QString& name, bool claude) {
    if (claude)
        return server.value("env").toObject().value(name).toString();
    for (const auto& entry : server.value("env").toArray())
        if (entry.toObject().value("name").toString() == name)
            return entry.toObject().value("value").toString();
    return {};
}
} // namespace

// ADR 0030: settings decide whether each provider gets the per-conversation computer front;
// turn input cannot add or replace it. Uses fake providers only (no quota).
class ComputerProviderIntegrationTest final : public QObject {
    Q_OBJECT

  private slots:
    void frontFollowsSettingsForEachProvider_data() {
        QTest::addColumn<bool>("claude");
        QTest::newRow("claude") << true;
        QTest::newRow("cursor") << false;
    }

    void frontFollowsSettingsForEachProvider() {
        QFETCH(bool, claude);
        QTemporaryDir root;
        QVERIFY(!qEnvironmentVariable("KETPLUS_COMPUTER_TOOLS_CONFIG").isEmpty());
        QVERIFY(!qEnvironmentVariable("KETPLUS_COMPUTER_DIR").isEmpty());
        const QString app = root.filePath(QStringLiteral("KetPlus Computer.app"));
        const QString front = app + QStringLiteral("/") + QLatin1String(computer::frontExecutable);
        QVERIFY(QDir().mkpath(QFileInfo(front).absolutePath()));
        QFile script(front);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\n");
        script.close();
        QVERIFY(QFile::setPermissions(front, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        qputenv("KETPLUS_COMPUTER_APP", app.toUtf8());

        const QString provider = root.filePath(QStringLiteral("provider"));
        const QString log = root.filePath(QStringLiteral("provider.log"));
        QVERIFY(claude ? writeFakeClaude(provider, log) : writeFakeCursor(provider, log));
        ClaudeAdapter claudeAdapter(provider);
        CursorAdapter cursorAdapter(provider);
        QSignalSpy claudeEvents(&claudeAdapter, &ClaudeAdapter::canonicalEvent);
        QSignalSpy cursorEvents(&cursorAdapter, &CursorAdapter::canonicalEvent);
        QSignalSpy& events = claude ? claudeEvents : cursorEvents;

        computer::Settings settings;
        QString thread;
        for (const bool enabled : {false, true}) {
            settings.enabled = enabled;
            QVERIFY(computer::saveSettings(settings));
            events.clear();
            bool replied = false;
            const QJsonObject params{
                {"sessionId", "computer-session"},
                {"cwd", root.path()},
                {"prompt", "Inspect"},
                {"resume", claude ? thread : QString()},
                {"permission", "workspace-write"},
                {"mcpServers",
                 QJsonObject{{"ketplus_computer", QJsonObject{{"command", "/evil"}}}}}};
            const auto callback = [&](const QJsonObject& result, const QString& error) {
                QVERIFY2(error.isEmpty(), qPrintable(error));
                thread = result.value("thread").toObject().value("id").toString();
                replied = true;
            };
            if (claude)
                claudeAdapter.call("turn/start", params, callback);
            else
                cursorAdapter.call("turn/start", params, callback);
            QTRY_VERIFY_WITH_TIMEOUT(replied, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(std::any_of(events.cbegin(), events.cend(),
                                                 [](const QList<QVariant>& event) {
                                                     return event.at(1).toString() ==
                                                            "turn.completed";
                                                 }),
                                     5000);

            const QJsonObject server = computerServer(logged(log, "browser:").last(), claude);
            const QString policy = logged(log, "policy:").last().value("instructions").toString();
            QCOMPARE(policy.count("[KetPlus computer policy]"), 1);
            if (!enabled) {
                QVERIFY(server.isEmpty());
                QVERIFY(policy.contains("off or unavailable"));
                continue;
            }
            QCOMPARE(server.value("command").toString(), QFileInfo(front).absoluteFilePath());
            QCOMPARE(envValue(server, "KETPLUS_COMPUTER_CONVERSATION", claude), "computer-session");
            QCOMPARE(envValue(server, "KETPLUS_COMPUTER_TOKEN", claude),
                     computer::mintToken(computer::ensureHostKey(), "computer-session"));
            QVERIFY(policy.contains("supplied the ketplus_computer MCP server"));
        }
        claudeAdapter.stop();
        cursorAdapter.stop();
        qunsetenv("KETPLUS_COMPUTER_APP");
    }
};

QTEST_GUILESS_MAIN(ComputerProviderIntegrationTest)
#include "ComputerProviderIntegrationTest.moc"
