#include "providers/CopilotCli.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon::copilot_cli;

namespace {
QProcessEnvironment windowsEnvironment() {
    QProcessEnvironment environment;
    environment.insert("SystemRoot", "C:/Windows");
    return environment;
}

struct DiscoveryFixture final {
    QSet<QString> runnable;
    QStringList checked;
    DiscoveryContext context;

    explicit DiscoveryFixture(Platform platform = Platform::Linux)
        : context{platform, {}, QStringLiteral("/home/test"), [this](const QString& path) {
                      checked.append(path);
                      return runnable.contains(
                          context.platform == Platform::Windows ? path.toLower() : path);
                  }} {}
};
} // namespace

class CopilotCliTest final : public QObject {
    Q_OBJECT

  private slots:
    void explicitOverrideWins() {
        DiscoveryFixture fixture;
        fixture.context.environment.insert("KETPLUS_COPILOT", "/environment/copilot");
        fixture.context.environment.insert("PATH", "/path");
        fixture.runnable = {"/explicit/copilot", "/environment/copilot", "/path/copilot"};
        const auto result = findExecutable("/explicit/copilot", fixture.context);
        QCOMPARE(result.executable, QStringLiteral("/explicit/copilot"));
        QVERIFY(result.error.isEmpty());
        QCOMPARE(fixture.checked, QStringList{"/explicit/copilot"});
    }

    void environmentOverrideWins() {
        DiscoveryFixture fixture;
        fixture.context.environment.insert("KETPLUS_COPILOT", "/environment/copilot");
        fixture.context.environment.insert("PATH", "/path");
        fixture.runnable = {"/environment/copilot", "/path/copilot"};
        QCOMPARE(findExecutable({}, fixture.context).executable,
                 QStringLiteral("/environment/copilot"));
        QCOMPARE(fixture.checked, QStringList{"/environment/copilot"});
    }

    void invalidOverrideDoesNotFallBack_data() {
        QTest::addColumn<QString>("explicitPath");
        QTest::addColumn<QString>("environmentPath");
        QTest::newRow("explicit-missing") << "/missing/copilot" << "/valid/copilot";
        QTest::newRow("environment-missing") << "" << "/missing/copilot";
        QTest::newRow("explicit-relative") << "./copilot" << "/valid/copilot";
        QTest::newRow("environment-relative") << "" << "copilot";
    }

    void invalidOverrideDoesNotFallBack() {
        QFETCH(QString, explicitPath);
        QFETCH(QString, environmentPath);
        DiscoveryFixture fixture;
        fixture.context.environment.insert("KETPLUS_COPILOT", environmentPath);
        fixture.context.environment.insert("PATH", "/valid");
        fixture.runnable = {"/valid/copilot", "copilot"};
        const auto result = findExecutable(explicitPath, fixture.context);
        QVERIFY(result.executable.isEmpty());
        QVERIFY(result.error.contains("override"));
        QVERIFY(result.error.contains("--copilot"));
        QVERIFY(!fixture.checked.contains("/valid/copilot"));
    }

    void pathPrecedesFallbacksAndSkipsUnrunnableFiles() {
        DiscoveryFixture fixture;
        fixture.context.environment.insert("PATH", "/first:/second:/third");
        fixture.runnable = {"/second/copilot", "/third/copilot", "/home/test/.local/bin/copilot"};
        QCOMPARE(findExecutable({}, fixture.context).executable, QStringLiteral("/second/copilot"));
        QCOMPARE(fixture.checked, (QStringList{"/first/copilot", "/second/copilot"}));
    }

    void fallbacks_data() {
        QTest::addColumn<int>("platform");
        QTest::addColumn<QStringList>("candidates");
        QTest::newRow("macOS") << int(Platform::MacOS)
                               << QStringList{"/home/test/.local/bin/copilot",
                                              "/opt/homebrew/bin/copilot",
                                              "/usr/local/bin/copilot"};
        QTest::newRow("linux") << int(Platform::Linux)
                               << QStringList{"/home/test/.local/bin/copilot",
                                              "/home/linuxbrew/.linuxbrew/bin/copilot",
                                              "/usr/local/bin/copilot", "/usr/bin/copilot"};
        QTest::newRow("windows") << int(Platform::Windows)
                                 << QStringList{
                                        "C:/Users/Test/Roaming/npm/copilot.cmd",
                                        "C:/Users/Test/Roaming/npm/copilot.ps1",
                                        "C:/Users/Test/Local/Microsoft/WinGet/Links/copilot.exe"};
    }

    void fallbacks() {
        QFETCH(int, platform);
        QFETCH(QStringList, candidates);
        for (qsizetype first = 0; first < candidates.size(); ++first) {
            DiscoveryFixture fixture(static_cast<Platform>(platform));
            fixture.context.environment.insert("AppData", "C:\\Users\\Test\\Roaming");
            fixture.context.environment.insert("LocalAppData", "C:\\Users\\Test\\Local");
            QCOMPARE(wellKnownCandidates(fixture.context), candidates);
            for (const auto& path : candidates.mid(first))
                fixture.runnable.insert(platform == int(Platform::Windows) ? path.toLower() : path);
            QCOMPARE(findExecutable({}, fixture.context).executable, candidates.at(first));
            QCOMPARE(fixture.checked, candidates.mid(0, first + 1));
        }
    }

    void ignoresRelativeAndEmptyPathEntries() {
        DiscoveryFixture fixture;
        fixture.context.environment.insert("PATH", ":.:tools:../bin:");
        fixture.runnable = {"copilot", "tools/copilot", "../bin/copilot"};
        const auto result = findExecutable({}, fixture.context);
        QVERIFY(result.executable.isEmpty());
        QVERIFY(result.error.contains("Install Copilot CLI"));
        QVERIFY(result.error.contains("https://docs.github.com/"));
        for (const auto& path : fixture.checked)
            QVERIFY(path.startsWith('/'));
    }

    void windowsPathUsesExtensionsAndCaseInsensitiveEnvironment() {
        DiscoveryFixture fixture(Platform::Windows);
        fixture.context.environment.insert("Path",
                                           ".;C:relative;\"C:\\Program Files\\Copilot\";D:\\Bin");
        fixture.context.environment.insert("PathExt", ".CMD;.EXE");
        fixture.runnable = {"c:/program files/copilot/copilot.cmd", "d:/bin/copilot.exe"};
        const auto result = findExecutable({}, fixture.context);
        QCOMPARE(result.executable, QStringLiteral("C:/Program Files/Copilot/copilot.cmd"));
        QCOMPARE(fixture.checked, QStringList{"C:/Program Files/Copilot/copilot.cmd"});
    }

    void windowsEnvironmentOverrideAndMissingDirectories() {
        DiscoveryFixture fixture(Platform::Windows);
        fixture.context.environment.insert("ketplus_copilot", "D:\\Tools\\copilot.CMD");
        fixture.runnable.insert("d:/tools/copilot.cmd");
        QCOMPARE(findExecutable({}, fixture.context).executable,
                 QStringLiteral("D:/Tools/copilot.CMD"));
        fixture.context.environment = {};
        QVERIFY(wellKnownCandidates(fixture.context).isEmpty());
        QVERIFY(!findExecutable({}, fixture.context).error.isEmpty());
    }

    void windowsUncOverrideStaysAbsoluteOnAnyHost() {
        DiscoveryFixture fixture(Platform::Windows);
        fixture.runnable.insert("//server/share/copilot.exe");
        QCOMPARE(findExecutable("\\\\server\\share\\copilot.exe", fixture.context).executable,
                 QStringLiteral("//server/share/copilot.exe"));
    }

    void missingFilesystemProbeFailsClosed() {
        DiscoveryFixture fixture;
        fixture.context.isRunnableFile = {};
        QVERIFY(findExecutable("/tools/copilot", fixture.context).executable.isEmpty());
    }

    void systemProbeChecksFileTypeAndPermissions() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        auto context = systemDiscoveryContext();
        QVERIFY(!context.isRunnableFile(temporary.path()));
        const auto filePath =
            temporary.filePath(context.platform == Platform::Windows ? "copilot.exe" : "copilot");
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
#if !defined(Q_OS_WIN)
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QVERIFY(!context.isRunnableFile(filePath));
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner));
#endif
        QCOMPARE(findExecutable(filePath, context).executable, filePath);
    }

    void nativeInvocationHasOnlyAcpStdioArguments() {
        for (const auto platform : {Platform::MacOS, Platform::Linux, Platform::Windows}) {
            const QString path =
                platform == Platform::Windows ? "C:/Tools/copilot.exe" : "/tools/copilot";
            const auto command = processInvocation(path, platform);
            QVERIFY(command.error.isEmpty());
            QCOMPARE(command.program, path);
            QCOMPARE(command.arguments, (QStringList{"--acp", "--stdio"}));
            QVERIFY(command.nativeArguments.isEmpty());
        }
    }

    void windowsCommandWrappersPreserveQuotedPath() {
        for (const auto& extension : {"CMD", "bat"}) {
            const auto command =
                processInvocation(QStringLiteral("C:/Space & bang! ^/copilot.%1").arg(extension),
                                  Platform::Windows, windowsEnvironment());
            QVERIFY(command.error.isEmpty());
            QCOMPARE(command.program, QStringLiteral("C:/Windows/System32/cmd.exe"));
            QCOMPARE(command.arguments, (QStringList{"/d", "/v:off", "/s", "/c"}));
            QCOMPARE(command.nativeArguments,
                     QStringLiteral("\"\"C:\\Space & bang! ^\\copilot.%1\" --acp --stdio\"")
                         .arg(extension));
        }
        const auto powershell =
            processInvocation("C:/Tools/copilot.ps1", Platform::Windows, windowsEnvironment());
        QCOMPARE(powershell.program,
                 QStringLiteral("C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"));
        QCOMPARE(powershell.arguments,
                 (QStringList{"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                              "-File", "C:\\Tools\\copilot.ps1", "--acp", "--stdio"}));
        QVERIFY(powershell.nativeArguments.isEmpty());
    }

    void invalidInvocationFailsWithoutAProgram() {
        for (const auto& path : {"", "copilot", "C:/percent%PATH%/copilot.cmd",
                                 "C:/quoted\"/copilot.cmd", "C:/newline\n/copilot.cmd"}) {
            const auto command = processInvocation(path, Platform::Windows, windowsEnvironment());
            QVERIFY(command.program.isEmpty());
            QVERIFY(!command.error.isEmpty());
        }
        QVERIFY(!processInvocation("C:/Tools/copilot.cmd", Platform::Windows).error.isEmpty());
    }

    void authenticationDiagnostics_data() {
        QTest::addColumn<QString>("message");
        QTest::addColumn<bool>("auth");
        QTest::newRow("canonical") << "Authentication required" << true;
        QTest::newRow("login-instruction")
            << "Error: Not authenticated. Please run 'copilot login'." << true;
        QTest::newRow("missing-credentials")
            << "No authentication information found. Run `copilot login` to authenticate." << true;
        QTest::newRow("empty") << "" << false;
        QTest::newRow("network") << "Authentication failed: connection timed out" << false;
        QTest::newRow("proxy") << "Authentication required for proxy" << false;
        QTest::newRow("model") << "Model not found. Please run copilot login." << false;
        QTest::newRow("forbidden-model") << "Model access denied (403)" << false;
        QTest::newRow("quota") << "Usage limit reached" << false;
        QTest::newRow("policy") << "Copilot is disabled by your organization" << false;
        QTest::newRow("command-denied") << "Permission denied" << false;
        QTest::newRow("incidental-text") << "The model said: Authentication required" << false;
    }

    void authenticationDiagnostics() {
        QFETCH(QString, message);
        QFETCH(bool, auth);
        const Failure failure{{{"code", -32000}, {"message", message}}, {}, {}};
        QCOMPARE(isAuthenticationFailure(failure), auth);
        QCOMPARE(!authenticationGuidance(failure).isEmpty(), auth);
        QCOMPARE(isAuthenticationFailure({{}, message, 1}), auth);
        QVERIFY(!isAuthenticationFailure({{}, message, 0}));
        QVERIFY(!isAuthenticationFailure({{}, message, {}}));
    }

    void nestedAcpDiagnosticAndGenericExitCodes() {
        QVERIFY(isAuthenticationFailure(
            {{{"code", -32000}, {"data", QJsonObject{{"details", "Authentication required"}}}},
             {},
             {}}));
        QVERIFY(!isAuthenticationFailure({{{"code", -32000}}, {}, 1}));
        QVERIFY(!isAuthenticationFailure({{}, {}, 401}));
        QVERIFY(!isAuthenticationFailure({{}, "network error", 403}));
        const auto guidance = authenticationGuidance(
            {{{"message", "Authentication required"}}, "sensitive provider diagnostic", 1});
        QVERIFY(guidance.contains("copilot login"));
        QVERIFY(guidance.contains("machine running the KetPlus daemon"));
        QVERIFY(!guidance.contains("sensitive provider diagnostic"));
    }

    void authenticationTailDoesNotTurnATruncatedLineIntoADiagnostic() {
        const QString text = QString(9000, 'x') + "Authentication required";
        QVERIFY(!isAuthenticationFailure({{}, text, 1}));
        QVERIFY(isAuthenticationFailure({{}, text + "\nAuthentication required", 1}));
    }

    void nativeWindowsWrapperRunsWithLiteralPath() {
#if defined(Q_OS_WIN)
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVERIFY(QDir(temporary.path()).mkdir("Space & bang! ^"));
        const auto path = temporary.filePath("Space & bang! ^/copilot.cmd");
        QFile script(path);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("@echo off\r\necho %1 %2\r\n");
        script.close();
        const auto command =
            processInvocation(path, Platform::Windows, QProcessEnvironment::systemEnvironment());
        QProcess process;
        process.setProgram(command.program);
        process.setArguments(command.arguments);
        process.setNativeArguments(command.nativeArguments);
        process.start();
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);
        QCOMPARE(process.readAllStandardOutput().trimmed(), QByteArray("--acp --stdio"));
#else
        QSKIP("Native cmd.exe execution requires Windows; all platform command shapes run above.");
#endif
    }

    void daemonRecognizesCopilotOptionWithoutStartingAProvider() {
        QProcess process;
        process.start(QStringLiteral(KETPLUS_TEST_DAEMON),
                      {"--copilot", "/not-a-real-provider/copilot", "--build-info"});
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(process.readAllStandardOutput().contains("product_version"));
    }
};

QTEST_GUILESS_MAIN(CopilotCliTest)
#include "CopilotCliTest.moc"
