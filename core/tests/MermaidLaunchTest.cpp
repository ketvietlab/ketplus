#include "preview/MermaidLaunch.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

namespace ketplus {
namespace {

bool touch(const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("x") == 1;
}

class MermaidLaunchTest final : public QObject {
    Q_OBJECT

  private slots:
    void quotesSpacesAndCmdOperators() {
        bool ok = false;
        const QString line =
            cmdCommandLine(QStringLiteral("C:\\Users\\John Smith\\npm\\mmdc.cmd"),
                           {QStringLiteral("-i"), QStringLiteral("C:\\a b\\x&y^z(1).mmd")}, &ok);
        QVERIFY(ok);
        QCOMPARE(line, QStringLiteral("\"C:\\Users\\John Smith\\npm\\mmdc.cmd\" \"-i\" "
                                      "\"C:\\a b\\x&y^z(1).mmd\""));
    }

    void refusesPercentQuoteAndLineBreak_data() {
        QTest::addColumn<QString>("value");
        QTest::newRow("percent") << QStringLiteral("C:\\a\\%PATH%\\x");
        QTest::newRow("quote") << QStringLiteral("C:\\a\"b");
        QTest::newRow("newline") << QStringLiteral("C:\\a\nb");
        QTest::newRow("carriage return") << QStringLiteral("C:\\a\rb");
    }

    void refusesPercentQuoteAndLineBreak() {
        QFETCH(QString, value);
        bool ok = true;
        QVERIFY(cmdCommandLine(QStringLiteral("C:\\mmdc.cmd"), {value}, &ok).isEmpty());
        QVERIFY(!ok);
        ok = true;
        QVERIFY(cmdCommandLine(value, {}, &ok).isEmpty());
        QVERIFY(!ok);
    }

    void usesCmdWithOuterQuotesAndAutoRunOff() {
        const auto launch = mermaidWindowsLaunch(QStringLiteral("C:\\Windows\\System32\\cmd.exe"),
                                                 QStringLiteral("C:\\no node\\mmdc.cmd"),
                                                 {QStringLiteral("-i"), QStringLiteral("a b")}, {});
        QVERIFY(launch.error.isEmpty());
        QCOMPARE(launch.program, QStringLiteral("C:\\Windows\\System32\\cmd.exe"));
        QVERIFY(launch.arguments.isEmpty());
        QCOMPARE(launch.nativeArguments,
                 QStringLiteral("/D /V:OFF /S /C \"\"C:\\no node\\mmdc.cmd\" \"-i\" \"a b\"\""));
    }

    void turnsForwardSlashesOfTheProgramIntoBackslashes() {
        bool ok = false;
        const QString line = cmdCommandLine(QStringLiteral("C:/Users/John Smith/npm/mmdc.cmd"),
                                            {QStringLiteral("C:/a b/x.mmd")}, &ok);
        QVERIFY(ok);
        // Arguments are left as given: they are not programs, and mmdc takes either form.
        QCOMPARE(line, QStringLiteral("\"C:\\Users\\John Smith\\npm\\mmdc.cmd\" \"C:/a b/x.mmd\""));
    }

    void ignoresNodeWrappersThatAreNotExecutables() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDir root(directory.path());
        const QString shim = root.filePath(QStringLiteral("mmdc.cmd"));
        QVERIFY(touch(shim));
        QVERIFY(touch(
            root.filePath(QStringLiteral("node_modules/@mermaid-js/mermaid-cli/src/cli.js"))));
        const QString wrapper = root.filePath(QStringLiteral("node.cmd"));
        const QString batch = root.filePath(QStringLiteral("node.bat"));
        QVERIFY(touch(wrapper));
        QVERIFY(touch(batch));
        const auto launch =
            mermaidWindowsLaunch(QStringLiteral("cmd.exe"), shim, {}, {wrapper, batch});
        QCOMPARE(launch.program, QStringLiteral("cmd.exe"));
        QVERIFY(!launch.nativeArguments.isEmpty());
    }

    void reportsPathsCmdCannotTakeLiterally() {
        const auto launch = mermaidWindowsLaunch(QStringLiteral("cmd.exe"),
                                                 QStringLiteral("C:\\100%\\mmdc.cmd"), {}, {});
        QVERIFY(launch.program.isEmpty());
        QVERIFY(!launch.error.isEmpty());
    }

    void runsNodeDirectlyWhenThePackageIsNextToTheShim() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDir root(directory.path());
        const QString shim = root.filePath(QStringLiteral("with space/mmdc.cmd"));
        const QString script = root.filePath(
            QStringLiteral("with space/node_modules/@mermaid-js/mermaid-cli/src/cli.js"));
        const QString node = root.filePath(QStringLiteral("with space/node.exe"));
        QVERIFY(touch(shim));
        QVERIFY(touch(script));
        QVERIFY(touch(node));
        // A path cmd could not take literally is fine: no cmd.exe is involved.
        const auto launch = mermaidWindowsLaunch(
            QStringLiteral("cmd.exe"), shim, {QStringLiteral("-i"), QStringLiteral("100%.mmd")},
            {root.filePath(QStringLiteral("missing/node.exe")), node});
        QVERIFY(launch.error.isEmpty());
        QCOMPARE(launch.program, node);
        QCOMPARE(launch.arguments,
                 (QStringList{script, QStringLiteral("-i"), QStringLiteral("100%.mmd")}));
        QVERIFY(launch.nativeArguments.isEmpty());
    }

    void fallsBackToCmdWithoutNode() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDir root(directory.path());
        const QString shim = root.filePath(QStringLiteral("mmdc.cmd"));
        QVERIFY(touch(shim));
        QVERIFY(touch(
            root.filePath(QStringLiteral("node_modules/@mermaid-js/mermaid-cli/src/cli.js"))));
        const auto launch = mermaidWindowsLaunch(QStringLiteral("cmd.exe"), shim, {}, {});
        QCOMPARE(launch.program, QStringLiteral("cmd.exe"));
        QVERIFY(!launch.nativeArguments.isEmpty());
    }

#if defined(Q_OS_WIN)
    // The user's real npm install, if there is one: node runs mermaid-cli directly (no cmd.exe).
    void runsARealMermaidCliThroughNodeDirectly() {
        const QString shim =
            QDir(qEnvironmentVariable("APPDATA")).filePath(QStringLiteral("npm/mmdc.cmd"));
        const QString node = QStandardPaths::findExecutable(QStringLiteral("node.exe"));
        if (!QFileInfo::exists(shim) || node.isEmpty())
            QSKIP("mermaid-cli and node.exe are not installed here.");
        const auto launch = mermaidWindowsLaunch(QStringLiteral("cmd.exe"), shim,
                                                 {QStringLiteral("--version")}, {node});
        QVERIFY2(launch.error.isEmpty(), qPrintable(launch.error));
        QCOMPARE(QFileInfo(launch.program).fileName().toLower(), QStringLiteral("node.exe"));
        QVERIFY(launch.nativeArguments.isEmpty());
        QProcess process;
        process.start(launch.program, launch.arguments);
        QVERIFY(process.waitForFinished(30000));
        QCOMPARE(process.exitCode(), 0);
        QVERIFY2(QRegularExpression(QStringLiteral("^\\d+\\.\\d+"))
                     .match(QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed())
                     .hasMatch(),
                 "mmdc --version did not print a version");
    }

    // End to end on Windows: a shim in a folder with spaces, run through cmd.exe, receives each
    // argument intact, including & ^ ( ) and spaces.
    void cmdRunsAShimWhosePathAndArgumentsHaveSpaces() {
        QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/ketplus mermaid XXXXXX"));
        QVERIFY(directory.isValid());
        const QDir root(directory.path());
        QVERIFY(QDir().mkpath(root.filePath(QStringLiteral("npm bin"))));
        const QString shim =
            QDir::toNativeSeparators(root.filePath(QStringLiteral("npm bin/mmdc.cmd")));
        QFile script(shim);
        QVERIFY(script.open(QIODevice::WriteOnly));
        // %1 keeps its quotes, so `echo "a&b"` is safe; the output lands beside the shim.
        script.write(
            "@echo off\r\necho %1> \"%~dp0first.txt\"\r\necho %2> \"%~dp0second.txt\"\r\n");
        script.close();

        const QString awkward =
            QDir::toNativeSeparators(root.filePath(QStringLiteral("a b/x&y^z(1).mmd")));
        const auto launch =
            mermaidWindowsLaunch(qEnvironmentVariable("COMSPEC", QStringLiteral("cmd.exe")), shim,
                                 {QStringLiteral("-i"), awkward}, {});
        QVERIFY2(launch.error.isEmpty(), qPrintable(launch.error));
        QProcess process;
        process.setNativeArguments(launch.nativeArguments);
        process.start(launch.program, launch.arguments);
        QVERIFY(process.waitForFinished(15000));
        QCOMPARE(process.exitCode(), 0);

        const auto read = [&root](const char* name) {
            QFile file(root.filePath(QStringLiteral("npm bin/%1").arg(QLatin1String(name))));
            return file.open(QIODevice::ReadOnly) ? QString::fromLocal8Bit(file.readAll()).trimmed()
                                                  : QString();
        };
        QCOMPARE(read("first.txt"), QStringLiteral("\"-i\""));
        QCOMPARE(read("second.txt"), QLatin1Char('"') + awkward + QLatin1Char('"'));
    }
#endif
};

} // namespace
} // namespace ketplus

QTEST_GUILESS_MAIN(ketplus::MermaidLaunchTest)
#include "MermaidLaunchTest.moc"
