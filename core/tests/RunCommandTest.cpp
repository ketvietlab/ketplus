#include "terminal/RunCommand.h"
#include "terminal/PtyProcess.h"
#include "terminal/TerminalTab.h"
#include "terminal/TerminalView.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace ketplus {
namespace {

QProcessEnvironment environmentWith(const QStringList& names) {
    QProcessEnvironment environment;
    for (const QString& name : names)
        environment.insert(name, QStringLiteral("value"));
    return environment;
}

class RunCommandTest final : public QObject {
    Q_OBJECT

  private slots:
    void detectsTheShellByName_data() {
        QTest::addColumn<QString>("path");
        QTest::addColumn<int>("kind");
        const auto row = [](const char* name, const char* path, ShellKind kind) {
            QTest::newRow(name) << QString::fromLatin1(path) << static_cast<int>(kind);
        };
        row("cmd", "C:\\Windows\\System32\\cmd.exe", ShellKind::Cmd);
        row("cmd upper case", "C:\\WINDOWS\\SYSTEM32\\CMD.EXE", ShellKind::Cmd);
        row("powershell", "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
            ShellKind::PowerShell);
        row("pwsh", "C:\\Program Files\\PowerShell\\7\\pwsh.exe", ShellKind::PowerShell);
        row("pwsh without suffix", "/usr/local/bin/pwsh", ShellKind::PowerShell);
        row("zsh", "/bin/zsh", ShellKind::Posix);
        row("bash.exe", "C:\\Program Files\\Git\\bin\\bash.exe", ShellKind::Posix);
        row("pwsh.cmd wrapper", "C:\\tools\\pwsh.cmd", ShellKind::PowerShell);
        row("empty", "", ShellKind::Posix);
    }

    void detectsTheShellByName() {
        QFETCH(QString, path);
        QFETCH(int, kind);
        QCOMPARE(static_cast<int>(shellKindFor(path)), kind);
    }

    void posixKeepsSingleQuotesAndTheInterpreter() {
        const auto executable = runCommandFor(ShellKind::Posix, QStringLiteral("/bin/zsh"),
                                              QStringLiteral("/tmp/it's here/run me.sh"), true);
        QCOMPARE(executable.line, QStringLiteral("'/tmp/it'\\''s here/run me.sh'"));
        const auto script = runCommandFor(ShellKind::Posix, QStringLiteral("/bin/zsh"),
                                          QStringLiteral("/tmp/a b/x.sh"), false);
        QCOMPARE(script.line, QStringLiteral("'/bin/zsh' '/tmp/a b/x.sh'"));
        const auto noShell =
            runCommandFor(ShellKind::Posix, {}, QStringLiteral("/tmp/x.sh"), false);
        QCOMPARE(noShell.line, QStringLiteral("'/bin/sh' '/tmp/x.sh'"));
    }

    void powerShellQuotesEverythingLiterally() {
        const auto command =
            runCommandFor(ShellKind::PowerShell, {},
                          QStringLiteral("C:\\a b\\it's & ^ % \" $x `t (1).ps1"), false);
        QVERIFY(command.error.isEmpty());
        QCOMPARE(command.line, QStringLiteral("& 'C:\\a b\\it''s & ^ % \" $x `t (1).ps1'"));
    }

    void powerShellDoublesTypographicQuotes() {
        const QString path = QStringLiteral("C:\\a\u2018b\u2019c\u201Ad\u201Be.ps1");
        const auto command = runCommandFor(ShellKind::PowerShell, {}, path, false);
        QCOMPARE(
            command.line,
            QStringLiteral("& 'C:\\a\u2018\u2018b\u2019\u2019c\u201A\u201Ad\u201B\u201Be.ps1'"));
    }

    void cmdQuotesSpacesAndOperators() {
        const auto command =
            runCommandFor(ShellKind::Cmd, {}, QStringLiteral("C:\\a b\\x & y ^ (1) it's.cmd"), true,
                          environmentWith({}));
        QVERIFY(command.error.isEmpty());
        QCOMPARE(command.line, QStringLiteral("\"C:\\a b\\x & y ^ (1) it's.cmd\""));
    }

    void cmdKeepsAPercentThatExpandsToNothing() {
        const auto lone = runCommandFor(ShellKind::Cmd, {}, QStringLiteral("C:\\100%\\x.cmd"), true,
                                        environmentWith({}));
        QVERIFY(lone.error.isEmpty());
        const auto unknown = runCommandFor(ShellKind::Cmd, {}, QStringLiteral("C:\\%nodef%\\x.cmd"),
                                           true, environmentWith({QStringLiteral("PATH")}));
        QVERIFY(unknown.error.isEmpty());
        QCOMPARE(unknown.line, QStringLiteral("\"C:\\%nodef%\\x.cmd\""));
    }

    void cmdRefusesWhatItWouldExpand_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("variable") << QStringLiteral("C:\\%PATH%\\x.cmd");
        QTest::newRow("variable, other case") << QStringLiteral("C:\\%path%\\x.cmd");
        QTest::newRow("substring form") << QStringLiteral("C:\\%PATH:~0,2%\\x.cmd");
        QTest::newRow("replace form") << QStringLiteral("C:\\%PATH:a=b%\\x.cmd");
        QTest::newRow("dynamic") << QStringLiteral("C:\\%CD%\\x.cmd");
        QTest::newRow("dynamic random") << QStringLiteral("C:\\%random%\\x.cmd");
        QTest::newRow("second pair") << QStringLiteral("C:\\100%\\%PATH%.cmd");
        QTest::newRow("double quote") << QStringLiteral("C:\\a\"b.cmd");
        QTest::newRow("line break") << QStringLiteral("C:\\a\nb.cmd");
    }

    void cmdRefusesWhatItWouldExpand() {
        QFETCH(QString, path);
        const auto command = runCommandFor(ShellKind::Cmd, {}, path, true,
                                           environmentWith({QStringLiteral("PATH")}));
        QVERIFY(command.line.isEmpty());
        QVERIFY(!command.error.isEmpty());
    }

    void windowsShellsRefuseLineBreaksAndControls() {
        for (const auto kind : {ShellKind::PowerShell, ShellKind::Cmd}) {
            for (const QString& path : {QStringLiteral("C:\\a\rb"), QStringLiteral("C:\\a\nb"),
                                        QStringLiteral("C:\\a\tb")}) {
                const auto command = runCommandFor(kind, {}, path, true);
                QVERIFY(command.line.isEmpty());
                QVERIFY(!command.error.isEmpty());
            }
        }
    }

    void posixStillTypesWhateverTheNameHolds() {
        // As before this change: single quotes around the name, control characters included.
        const auto command = runCommandFor(ShellKind::Posix, {}, QStringLiteral("/tmp/a\tb"), true);
        QVERIFY(command.error.isEmpty());
        QCOMPARE(command.line, QStringLiteral("'/tmp/a\tb'"));
    }

    void cmdKnowsTheVariablesTheTerminalSetsForItsShell() {
        for (const char* name : {"TERM_PROGRAM", "term", "COLORTERM", "TERM_PROGRAM_VERSION"}) {
            const auto command = runCommandFor(ShellKind::Cmd, {},
                                               QStringLiteral("C:\\%") + QLatin1String(name) +
                                                   QStringLiteral("%\\x.cmd"),
                                               true, environmentWith({}));
            QVERIFY2(command.line.isEmpty(), name);
        }
    }

#if defined(Q_OS_WIN)
    void windowsShellsSeeNativeSeparators() {
        const auto powerShell =
            runCommandFor(ShellKind::PowerShell, {}, QStringLiteral("C:/a b/x.ps1"), true);
        QCOMPARE(powerShell.line, QStringLiteral("& 'C:\\a b\\x.ps1'"));
        const auto cmd = runCommandFor(ShellKind::Cmd, {}, QStringLiteral("C:/a b/x.cmd"), true,
                                       environmentWith({}));
        QCOMPARE(cmd.line, QStringLiteral("\"C:\\a b\\x.cmd\""));
    }
#endif

#if defined(Q_OS_WIN)
    // End to end in the real shell: a script whose path has spaces and & ^ % ' ( ) runs and
    // leaves its marker beside itself.
    void runsAnAwkwardPathInTheShell_data() {
        QTest::addColumn<QString>("shell");
        QTest::newRow("cmd") << qEnvironmentVariable("COMSPEC");
        QTest::newRow("powershell")
            << qEnvironmentVariable("SystemRoot") +
                   QStringLiteral("\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
        // PowerShell with a prompt of its own, which the terminal cannot recognise.
        QTest::newRow("powershell custom prompt") << QStringLiteral("<custom prompt>");
    }

    void runsAnAwkwardPathInTheShell() {
        QFETCH(QString, shell);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        if (shell == QLatin1String("<custom prompt>")) {
            // pwsh.cmd is read as a PowerShell by its name; it starts the real one with a
            // prompt function that never says "PS".
            const QString launcher = directory.filePath(QStringLiteral("pwsh.cmd"));
            QFile file(launcher);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("@\"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" "
                       "-NoLogo -NoExit -Command \"function prompt { 'custom> ' }\"\r\n");
            file.close();
            shell = QDir::toNativeSeparators(launcher);
        }
        const QString folder = QStringLiteral("run it & ^ %nodef% it's (1)");
        QVERIFY(QDir(directory.path()).mkpath(folder));
        const QString script = QDir::toNativeSeparators(
            QDir(directory.path()).filePath(folder + QStringLiteral("/x & y.cmd")));
        QFile file(script);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("@echo off\r\necho done> \"%~dp0marker.txt\"\r\n");
        file.close();

        qputenv("SHELL", shell.toLocal8Bit());
        // The terminal tab itself: it types the command when the shell starts.
        {
            TerminalTab tab(directory.path(), script);
            const QString tabMarker =
                QDir(directory.path()).filePath(folder + QStringLiteral("/marker.txt"));
            QFile::remove(tabMarker);
            tab.resize(800, 400);
            tab.show();
            tab.start();
            QTRY_VERIFY2_WITH_TIMEOUT(QFileInfo::exists(tabMarker),
                                      qPrintable(tab.view()->visibleText()), 20000);
            tab.stop();
            // Hidden while its session is alive: the view reports focus loss to the session.
            tab.hide();
            QFile::remove(tabMarker);
        }
        PtyProcess process;
        QSignalSpy exitSpy(&process, &PtyProcess::exited);
        QVERIFY(process.start(directory.path(), 24, 80));
        const auto command =
            runCommandFor(shellKindFor(process.shellPath()), process.shellPath(), script, true);
        QVERIFY2(command.error.isEmpty(), qPrintable(command.error));
        QTest::qWait(2500); // Let the shell reach its prompt, as the terminal tab does.
        process.send(command.line.toUtf8() + "\r");
        const QString marker =
            QDir(directory.path()).filePath(folder + QStringLiteral("/marker.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 20000);
        process.send("exit\r");
        QTRY_VERIFY_WITH_TIMEOUT(!exitSpy.isEmpty(), 15000);
    }
#endif
};

} // namespace
} // namespace ketplus

QTEST_MAIN(ketplus::RunCommandTest)
#include "RunCommandTest.moc"
