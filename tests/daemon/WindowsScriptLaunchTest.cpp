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
            scriptLaunch(QStringLiteral("C:/Program Files/nodejs/codex.cmd"),
                         {QStringLiteral("app-server"), QStringLiteral("a b")}, true, root);
        QVERIFY2(launch.error.isEmpty(), qPrintable(launch.error));
        QCOMPARE(launch.program, QStringLiteral("C:/Windows/System32/cmd.exe"));
        QCOMPARE(launch.arguments, (QStringList{QStringLiteral("/d"), QStringLiteral("/v:off"),
                                                QStringLiteral("/s"), QStringLiteral("/c")}));
        QCOMPARE(launch.nativeArguments,
                 QStringLiteral("\"\"C:\\Program Files\\nodejs\\codex.cmd\" app-server \"a b\"\""));
    }

    void npmCodexShimRunsNodeOnItsEntryPointWithoutCmd() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("node_modules/@openai/codex/bin")));
        for (const QString& name : {QStringLiteral("codex.cmd"), QStringLiteral("node.exe"),
                                    QStringLiteral("node_modules/@openai/codex/bin/codex.js")}) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
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

    void cmdShimRefusesArgumentsItCannotQuote_data() {
        QTest::addColumn<QString>("argument");
        QTest::newRow("quote") << QStringLiteral("x=\"y\"");
        QTest::newRow("percent") << QStringLiteral("%PATH%");
        QTest::newRow("newline") << QStringLiteral("a\nb");
    }

    void cmdShimRefusesArgumentsItCannotQuote() {
        QFETCH(QString, argument);
        const auto launch = scriptLaunch(QStringLiteral("C:/n/codex.cmd"), {argument}, true, root);
        QVERIFY(!launch.error.isEmpty());
        QVERIFY(launch.program.isEmpty());
    }

    void cmdShimRefusesAPathItCannotQuote() {
        const auto launch = scriptLaunch(QStringLiteral("C:/100%/codex.cmd"), {}, true, root);
        QVERIFY(!launch.error.isEmpty());
    }

    void relativeSystemRootIsRefused() {
        const auto launch =
            scriptLaunch(QStringLiteral("C:/n/codex.cmd"), {}, true, QStringLiteral("rel"));
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
