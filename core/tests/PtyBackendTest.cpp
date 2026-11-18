// Backend-level checks for PtyProcess that hold on every platform: ConPTY on Windows, forkpty
// elsewhere. The shell is the platform default (SHELL is pinned to a known one for determinism).
#include "terminal/PtyProcess.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ketplus {
namespace {

#if defined(Q_OS_WIN)
constexpr auto kNewline = "\r\n";
#else
constexpr auto kNewline = "\n";
#endif

class PtyBackendTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        // KETPLUS_TEST_SHELL runs the same checks against another shell, e.g. PowerShell.
        if (const QByteArray shell = qgetenv("KETPLUS_TEST_SHELL"); !shell.isEmpty()) {
            qputenv("SHELL", shell);
            return;
        }
#if defined(Q_OS_WIN)
        qputenv("SHELL", qEnvironmentVariable("COMSPEC").toLocal8Bit());
#else
        qputenv("SHELL", "/bin/sh");
#endif
    }

    void echoesOutputAndReportsExitCode() {
        PtyProcess process;
        QByteArray output;
        connect(&process, &PtyProcess::outputReceived, this,
                [&output](const QByteArray& bytes) { output.append(bytes); });
        QSignalSpy exitSpy(&process, &PtyProcess::exited);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QVERIFY(process.start(directory.path(), 24, 80));
        QVERIFY(process.isRunning());
        QVERIFY(!process.shellPath().isEmpty());
        process.send(QByteArray("echo ketplus-pty-ok") + kNewline + "exit 7" + kNewline);

        QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, 15000);
        QCOMPARE(exitSpy.takeFirst().at(0).toInt(), 7);
        QVERIFY(!process.isRunning());
        QVERIFY2(output.contains("ketplus-pty-ok"), output.constData());
    }

    void startsInRequestedDirectory() {
        PtyProcess process;
        QByteArray output;
        connect(&process, &PtyProcess::outputReceived, this,
                [&output](const QByteArray& bytes) { output.append(bytes); });
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString marker = QFileInfo(directory.path()).fileName();

        QVERIFY(process.start(directory.path(), 24, 80));
#if defined(Q_OS_WIN)
        process.send(QByteArray("cd") + kNewline);
#else
        process.send(QByteArray("pwd") + kNewline);
#endif
        QTRY_VERIFY_WITH_TIMEOUT(output.contains(marker.toLocal8Bit()), 15000);
        process.stop();
    }

    void resizeIsReportedToTheShell() {
#if defined(Q_OS_WIN)
        constexpr auto query = "mode con";
#else
        constexpr auto query = "stty size";
#endif
        PtyProcess process;
        QByteArray output;
        connect(&process, &PtyProcess::outputReceived, this,
                [&output](const QByteArray& bytes) { output.append(bytes); });
        QVERIFY(process.start(QDir::tempPath(), 24, 80));
        process.resizeTerminal(31, 97);
        // A cold shell can take seconds to start, so ask again until it answers.
        const auto answered = [&output] {
#if defined(Q_OS_WIN)
            return QRegularExpression(QStringLiteral(R"(Lines:\s*31\s+Columns:\s*97)"))
                .match(QString::fromLocal8Bit(output))
                .hasMatch();
#else
            return output.contains("31 97");
#endif
        };
        QElapsedTimer waited;
        waited.start();
        while (!answered() && waited.elapsed() < 20000) {
            process.send(QByteArray(query) + kNewline);
            QTest::qWait(1000);
        }
        QVERIFY2(answered(), output.constData());
        process.stop();
    }

    void stopEndsTheChildAndIsIdempotent() {
        PtyProcess process;
        QSignalSpy exitSpy(&process, &PtyProcess::exited);
        QVERIFY(process.start(QDir::tempPath(), 24, 80));
        QVERIFY(process.isRunning());
        process.stop();
        QVERIFY(!process.isRunning());
        process.stop();
        process.send("ignored");
        QTest::qWait(300);
        QCOMPARE(exitSpy.count(), 0);
    }

#if defined(Q_OS_WIN)
    void controlCInterruptsTheForegroundCommand() {
        const QString shellName =
            QFileInfo(qEnvironmentVariable("SHELL")).completeBaseName().toLower();
        const bool powerShell =
            shellName == QLatin1String("powershell") || shellName == QLatin1String("pwsh");
        PtyProcess process;
        QByteArray output;
        connect(&process, &PtyProcess::outputReceived, this,
                [&output](const QByteArray& bytes) { output.append(bytes); });
        QVERIFY(process.start(QDir::tempPath(), 24, 80));
        // The marker is assembled by the shell, so the echoed command line cannot match it.
        process.send(QByteArray(powerShell ? "$a='KP_'" : "set A=KP_") + kNewline);
        process.send(powerShell ? QByteArray("echo \"${a}READY\"; Start-Sleep 60")
                                : QByteArray("echo %A%READY& ping -n 60 127.0.0.1 >nul"));
        process.send(kNewline);
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("KP_READY"), 30000);
        QTest::qWait(1500);
        output.clear();
        process.send(QByteArray(1, '\x03'));
        QTest::qWait(500);
        process.send(powerShell ? QByteArray("echo \"${a}AFTER\"") : QByteArray("echo %A%AFTER"));
        process.send(kNewline);
        QTRY_VERIFY2_WITH_TIMEOUT(output.contains("KP_AFTER"), output.constData(), 15000);
        process.stop();
    }

    void stopEndsDescendantProcesses() {
        PtyProcess process;
        QByteArray output;
        connect(&process, &PtyProcess::outputReceived, this,
                [&output](const QByteArray& bytes) { output.append(bytes); });
        QVERIFY(process.start(QDir::tempPath(), 24, 80));
        // A detached grandchild of the shell; it must still die with the terminal.
        const QString shellName =
            QFileInfo(qEnvironmentVariable("SHELL")).completeBaseName().toLower();
        const bool powerShell =
            shellName == QLatin1String("powershell") || shellName == QLatin1String("pwsh");
        const QByteArray script =
            "$p = Start-Process ping -ArgumentList '-t','127.0.0.1' -PassThru -WindowStyle "
            "Hidden; Write-Output ('KP_CHILD=' + $p.Id + '=')";
        // cmd.exe needs a PowerShell child to run it; PowerShell runs it directly.
        process.send((powerShell ? script : "powershell -NoProfile -Command \"" + script + "\"") +
                     kNewline);
        static const QRegularExpression pidLine(QStringLiteral("KP_CHILD=(\\d+)="));
        QTRY_VERIFY_WITH_TIMEOUT(pidLine.match(QString::fromLocal8Bit(output)).hasMatch(), 30000);
        const DWORD pid = pidLine.match(QString::fromLocal8Bit(output)).captured(1).toULong();
        HANDLE child = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
        QVERIFY(child != nullptr);
        QCOMPARE(::WaitForSingleObject(child, 0), static_cast<DWORD>(WAIT_TIMEOUT));
        process.stop();
        const DWORD waited = ::WaitForSingleObject(child, 10000);
        ::CloseHandle(child);
        QCOMPARE(waited, static_cast<DWORD>(WAIT_OBJECT_0));
    }
#endif

    void rejectsMissingWorkingDirectory() {
        PtyProcess process;
        QSignalSpy errorSpy(&process, &PtyProcess::errorOccurred);
        QVERIFY(
            !process.start(QDir::tempPath() + QStringLiteral("/ketplus-no-such-dir-xyz"), 24, 80));
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(!process.isRunning());
    }
};

} // namespace
} // namespace ketplus

QTEST_MAIN(ketplus::PtyBackendTest)
#include "PtyBackendTest.moc"
