#include "ComputerToolLaunch.h"
#include "ComputerAgentInstructions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

namespace computer = ketplus::computer_tools;

namespace {
QString makeAgentApp(const QString& root) {
    const QString app = QDir(root).filePath(QLatin1String(computer::agentAppName));
    const QString front = QDir(app).filePath(QLatin1String(computer::frontExecutable));
    if (!QDir().mkpath(QFileInfo(front).absolutePath()))
        return {};
    QFile file(front);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write("#!/bin/sh\n");
    file.close();
    QFile::setPermissions(front,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return app;
}
} // namespace

class ComputerToolLaunchTest final : public QObject {
    Q_OBJECT

  private slots:
    void tokenMatchesTheAgentVector() {
        // Same vector as apps/computer/Tests/ComputerProtocolTests/PolicyTests.swift.
        QByteArray key(32, Qt::Uninitialized);
        for (int i = 0; i < 32; ++i)
            key[i] = char(i);
        QCOMPARE(
            computer::mintToken(key, QStringLiteral("conv-123")),
            QStringLiteral("87a6f0b9a2457797da83b42f05dc619965e1681453effbb6d607042db46b341d"));
        QCOMPARE(computer::fileStem(QStringLiteral("conv-123")),
                 QStringLiteral("1723376043e8a9d0090025f147440b99"));
    }

    void settingsDefaultOffAndKeepUnknownKeys() {
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("nested/computer-tools.json"));
        QVERIFY(!computer::loadSettings(path).enabled);
        QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("nested"))));
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly));
        seed.write(R"({"future": 7})");
        seed.close();
        computer::Settings settings;
        settings.enabled = true;
        settings.deniedApps = {QStringLiteral("com.example.Bank")};
        QVERIFY(computer::saveSettings(settings, path));
        const auto loaded = computer::loadSettings(path);
        QVERIFY(loaded.enabled);
        QCOMPARE(loaded.deniedApps, QStringList{QStringLiteral("com.example.Bank")});
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value("future").toInt(), 7);
        QCOMPARE(QFileInfo(path).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
                 QFileDevice::Permissions{});
    }

    void hostKeyIsPrivateAndStable() {
        QTemporaryDir root;
        const QString directory = root.filePath(QStringLiteral("computer"));
        const QByteArray key = computer::ensureHostKey(directory);
        QCOMPARE(key.size(), 32);
        QCOMPARE(computer::ensureHostKey(directory), key);
        const auto keyPermissions =
            QFileInfo(directory + QStringLiteral("/host.key")).permissions();
        QCOMPARE(keyPermissions & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
                 QFileDevice::Permissions{});
        QCOMPARE(QFileInfo(directory).permissions() &
                     (QFileDevice::ExeGroup | QFileDevice::ExeOther),
                 QFileDevice::Permissions{});
    }

    void launchNeedsEnabledSettingsAndTheApp() {
        QTemporaryDir root;
        const QString directory = root.filePath(QStringLiteral("computer"));
        computer::Settings settings;
        const QString helpers = root.filePath(QStringLiteral("Helpers"));
        QVERIFY(QDir().mkpath(helpers));
        QVERIFY(!computer::launchForConversation(settings, "c1", helpers, directory));
        settings.enabled = true;
        QVERIFY(!computer::launchForConversation(settings, "c1", helpers, directory));
        QVERIFY(!makeAgentApp(helpers).isEmpty());
        QVERIFY(!computer::launchForConversation(settings, QString(), helpers, directory));
        const auto launch = computer::launchForConversation(settings, "c1", helpers, directory);
        QVERIFY(launch);
        QCOMPARE(launch->command, computer::findFrontExecutable(helpers));
        QCOMPARE(launch->env.value("KETPLUS_COMPUTER_CONVERSATION"), QStringLiteral("c1"));
        QCOMPARE(launch->env.value("KETPLUS_COMPUTER_TOKEN"),
                 computer::mintToken(computer::ensureHostKey(directory), "c1"));
        // The desktop app (Contents/MacOS) finds the same nested app through ../Helpers.
        const QString macos = root.filePath(QStringLiteral("MacOS"));
        QVERIFY(QDir().mkpath(macos));
        QCOMPARE(computer::findFrontExecutable(macos), launch->command);
    }

    void serversMergeWithoutReplacingOthers() {
        computer::Launch launch;
        launch.command = QStringLiteral("/app/ketplus-computer-mcp");
        launch.env.insert(QStringLiteral("KETPLUS_COMPUTER_TOKEN"), QStringLiteral("t"));
        const QJsonObject browser{{"ketplus_browser", QJsonObject{{"command", "node"}}}};
        QCOMPARE(computer::withClaudeMcpServer(browser, std::nullopt), browser);
        const QJsonObject claude = computer::withClaudeMcpServer(browser, launch);
        QVERIFY(claude.contains("ketplus_browser"));
        QCOMPARE(claude.value("ketplus_computer").toObject().value("command").toString(),
                 launch.command);
        QCOMPARE(claude.value("ketplus_computer")
                     .toObject()
                     .value("env")
                     .toObject()
                     .value("KETPLUS_COMPUTER_TOKEN"),
                 QJsonValue("t"));
        const QJsonArray acp = computer::withAcpMcpServer(
            QJsonArray{QJsonObject{{"name", "ketplus_browser"}}}, launch);
        QCOMPARE(acp.size(), 2);
        QCOMPARE(acp.last().toObject().value("name").toString(), "ketplus_computer");
        QCOMPARE(acp.last()
                     .toObject()
                     .value("env")
                     .toArray()
                     .first()
                     .toObject()
                     .value("name")
                     .toString(),
                 "KETPLUS_COMPUTER_TOKEN");
    }

    void remoteTurnMarkerFollowsTheClient() {
        QTemporaryDir root;
        const QString directory = root.filePath(QStringLiteral("computer"));
        const QString marker =
            directory + QStringLiteral("/remote-turns/") + computer::fileStem(QStringLiteral("c1"));
        QVERIFY(computer::setRemoteTurn("c1", false, directory));
        QVERIFY(!QFileInfo::exists(marker));
        QVERIFY(computer::setRemoteTurn("c1", true, directory));
        QVERIFY(QFileInfo::exists(marker));
        QVERIFY(computer::setRemoteTurn("c1", false, directory));
        QVERIFY(!QFileInfo::exists(marker));
        QVERIFY(!computer::setRemoteTurn(QString(), true, directory));
    }

    void instructionsDescribeAvailability() {
        const QString on = computer::agentInstructions(true);
        const QString off = computer::agentInstructions(false);
        QVERIFY(on.startsWith("[KetPlus computer policy]"));
        QVERIFY(on.contains("request_access"));
        QVERIFY(!on.contains("off or unavailable"));
        QVERIFY(off.contains("off or unavailable"));
        for (const QString& text : {on, off})
            QVERIFY(text.contains("no provider-native computer use"));
    }
};

QTEST_GUILESS_MAIN(ComputerToolLaunchTest)
#include "ComputerToolLaunchTest.moc"
