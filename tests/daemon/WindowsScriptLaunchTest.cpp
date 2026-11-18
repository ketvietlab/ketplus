#include "providers/WindowsScriptLaunch.h"

#include <QtTest>

using namespace ketplus::daemon;

namespace {
const QString root = QStringLiteral("C:/Windows");
}

class WindowsScriptLaunchTest final : public QObject {
    Q_OBJECT
  private slots:
    void otherHostsLaunchTheFileDirectly() {
        const auto launch = scriptLaunch(QStringLiteral("/usr/bin/codex"),
                                         {QStringLiteral("app-server")}, false, {});
        QCOMPARE(launch.program, QStringLiteral("/usr/bin/codex"));
        QCOMPARE(launch.arguments, QStringList{QStringLiteral("app-server")});
        QVERIFY(launch.nativeArguments.isEmpty());
        QVERIFY(launch.error.isEmpty());
    }

    void executablesAreNotWrapped() {
        const auto launch = scriptLaunch(QStringLiteral("C:/tools/codex.exe"),
                                         {QStringLiteral("--version")}, true, root);
        QCOMPARE(launch.program, QStringLiteral("C:/tools/codex.exe"));
        QVERIFY(launch.nativeArguments.isEmpty());
    }

    void cmdShimGoesThroughCmdWithNativeArguments() {
        const auto launch =
            scriptLaunch(QStringLiteral("C:/Program Files/nodejs/agent.cmd"),
                         {QStringLiteral("app-server"), QStringLiteral("a b")}, true, root);
        QVERIFY2(launch.error.isEmpty(), qPrintable(launch.error));
        QCOMPARE(launch.program, QStringLiteral("C:/Windows/System32/cmd.exe"));
        QCOMPARE(launch.arguments, (QStringList{QStringLiteral("/d"), QStringLiteral("/v:off"),
                                                QStringLiteral("/s"), QStringLiteral("/c")}));
        QCOMPARE(launch.nativeArguments,
                 QStringLiteral("\"\"C:\\Program Files\\nodejs\\agent.cmd\" app-server \"a b\"\""));
    }

    static void writeFile(const QString& path, const QByteArray& bytes = {}) {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    }

    void npmCodexShimRunsNodeOnItsEntryPointWithoutCmd() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString package = dir.filePath(QStringLiteral("node_modules/@openai/codex"));
        writeFile(dir.filePath(QStringLiteral("codex.cmd")));
        writeFile(dir.filePath(QStringLiteral("node.exe")));
        writeFile(package + QStringLiteral("/package.json"),
                  R"({"name":"@openai/codex","bin":{"codex":"bin/codex.js"}})");
        writeFile(package + QStringLiteral("/bin/codex.js"));
        const auto launch =
            scriptLaunch(dir.filePath(QStringLiteral("codex.cmd")),
                         {QStringLiteral("-c"), QStringLiteral("a=\"b\"")}, true, root);
        QVERIFY2(launch.error.isEmpty(), qPrintable(launch.error));
        QCOMPARE(launch.program, dir.filePath(QStringLiteral("node.exe")));
        QVERIFY(launch.nativeArguments.isEmpty());
        QCOMPARE(launch.arguments.size(), 3);
        QVERIFY(launch.arguments.at(0).endsWith(QStringLiteral("codex.js")));
        QCOMPARE(launch.arguments.mid(1),
                 (QStringList{QStringLiteral("-c"), QStringLiteral("a=\"b\"")}));
    }

    void npmCodexShimWithoutNodeOrScriptIsRefusedNotRunThroughCmd() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFile(dir.filePath(QStringLiteral("codex.cmd")));
        const auto launch = scriptLaunch(dir.filePath(QStringLiteral("codex.cmd")),
                                         {QStringLiteral("x")}, true, root);
        QVERIFY(!launch.error.isEmpty());
        QVERIFY(launch.program.isEmpty());
        QVERIFY(launch.nativeArguments.isEmpty());
    }

    void cmdShimRefusesArgumentsItCannotQuote_data() {
        QTest::addColumn<QString>("argument");
        QTest::newRow("quote") << QStringLiteral("x=\"y\"");
        QTest::newRow("percent") << QStringLiteral("%PATH%");
        QTest::newRow("newline") << QStringLiteral("a\nb");
    }

    void cmdShimRefusesArgumentsItCannotQuote() {
        QFETCH(QString, argument);
        const auto launch = scriptLaunch(QStringLiteral("C:/n/agent.cmd"), {argument}, true, root);
        QVERIFY(!launch.error.isEmpty());
        QVERIFY(launch.program.isEmpty());
    }

    void cmdShimRefusesAPathItCannotQuote() {
        const auto launch = scriptLaunch(QStringLiteral("C:/100%/agent.cmd"), {}, true, root);
        QVERIFY(!launch.error.isEmpty());
    }

    void relativeSystemRootIsRefused() {
        const auto launch =
            scriptLaunch(QStringLiteral("C:/n/agent.cmd"), {}, true, QStringLiteral("rel"));
        QVERIFY(!launch.error.isEmpty());
    }

    void powershellScriptsAreWrapped() {
        const auto launch =
            scriptLaunch(QStringLiteral("C:/n/agent.ps1"), {QStringLiteral("acp")}, true, root);
        QCOMPARE(launch.program,
                 QStringLiteral("C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"));
        QVERIFY(launch.arguments.contains(QStringLiteral("-File")));
        QCOMPARE(launch.arguments.last(), QStringLiteral("acp"));
    }
};

QTEST_GUILESS_MAIN(WindowsScriptLaunchTest)
#include "WindowsScriptLaunchTest.moc"
