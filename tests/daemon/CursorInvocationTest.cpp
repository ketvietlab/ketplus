// How the Cursor CLI is started. Runs on every host, including Windows, where the rest of the
// Cursor suite is disabled because its provider fixtures are shell scripts.
#include "providers/CursorAdapter.h"

#include <QDir>
#include <QtTest>

using namespace ketplus::daemon;

class CursorInvocationTest final : public QObject {
    Q_OBJECT
  private slots:
    void aModelIdThatCouldReachAShellIsRefused_data() {
        QTest::addColumn<QString>("model");
        QTest::newRow("quote") << QStringLiteral("x\" & calc");
        QTest::newRow("percent") << QStringLiteral("%COMSPEC%");
        QTest::newRow("space") << QStringLiteral("two words");
        QTest::newRow("newline") << QStringLiteral("a\nb");
        QTest::newRow("ampersand") << QStringLiteral("a&b");
        QTest::newRow("leading-dash") << QStringLiteral("-c");
    }

    void aModelIdThatCouldReachAShellIsRefused() {
        QFETCH(QString, model);
        const auto invocation =
            CursorAdapter::processInvocation(QStringLiteral("/opt/cursor/agent"), model);
        QVERIFY(!invocation.error.isEmpty());
        QVERIFY(invocation.program.isEmpty());
    }

    void aPlainModelIdIsPassedAsAnArgument() {
        const auto invocation = CursorAdapter::processInvocation(
            QDir::tempPath() + QStringLiteral("/agent.exe"), QStringLiteral("composer-2.5"));
        QVERIFY2(invocation.error.isEmpty(), qPrintable(invocation.error));
        QCOMPARE(invocation.arguments,
                 (QStringList{QStringLiteral("--model"), QStringLiteral("composer-2.5"),
                              QStringLiteral("acp")}));
        QVERIFY(invocation.nativeArguments.isEmpty());
    }

#if defined(Q_OS_WIN)
    void aCmdWrapperRunsThroughCmdExeWithNativeArguments() {
        const QString system = QDir::cleanPath(qEnvironmentVariable("SystemRoot")) + "/System32";
        const auto cmd = CursorAdapter::processInvocation(
            QStringLiteral("C:/Users/dev/AppData/Local/cursor-agent/agent.cmd"));
        QVERIFY2(cmd.error.isEmpty(), qPrintable(cmd.error));
        QCOMPARE(cmd.program, system + "/cmd.exe");
        QCOMPARE(cmd.arguments, (QStringList{QStringLiteral("/d"), QStringLiteral("/v:off"),
                                             QStringLiteral("/s"), QStringLiteral("/c")}));
        QCOMPARE(
            cmd.nativeArguments,
            QStringLiteral("\"\"C:\\Users\\dev\\AppData\\Local\\cursor-agent\\agent.cmd\" acp\""));
    }

    void aCmdWrapperWithAPathCmdExeWouldExpandIsRefusedNotRun() {
        const auto cmd = CursorAdapter::processInvocation(QStringLiteral("C:/100%/agent.cmd"));
        QVERIFY(!cmd.error.isEmpty());
        QVERIFY(cmd.program.isEmpty()); // not the .cmd itself, which Windows would run via cmd
    }

    void aPowerShellScriptRunsThroughPowerShell() {
        const auto ps1 = CursorAdapter::processInvocation(
            QStringLiteral("C:/Users/dev/AppData/Local/cursor-agent/agent.ps1"));
        QVERIFY(ps1.program.endsWith(QStringLiteral("/WindowsPowerShell/v1.0/powershell.exe")));
        QVERIFY(ps1.arguments.contains(QStringLiteral("-File")));
        QCOMPARE(ps1.arguments.last(), QStringLiteral("acp"));
    }
#endif
};

QTEST_GUILESS_MAIN(CursorInvocationTest)
#include "CursorInvocationTest.moc"
