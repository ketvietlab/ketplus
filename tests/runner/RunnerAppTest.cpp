#include "RunnerAppTest.h"

#include "commands/Commands.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QTest>
#include <QTimeZone>

namespace runner_app_test {

using ketplus::runner::app::Environment;

TestHost::TestHost()
    : clock(QDate(2026, 11, 18), QTime(9, 0), QTimeZone::UTC),
      relay(std::make_unique<FakeRelay>([this] { return clock; })) {}

Environment TestHost::environment() {
    Environment environment;
    environment.now = [this] { return clock; };
    environment.sleep = [this](const qint64 milliseconds) {
        sleeps.append(milliseconds);
        clock = clock.addMSecs(milliseconds);
        if (onSleep)
            onSleep(milliseconds);
        if (sleeps.size() >= stopAfterSleeps)
            stop = true;
    };
    environment.stopRequested = [this] { return stop; };
    environment.random = [this] { return randomValue; };
    environment.out = [this](const QString& line) { out.append(line); };
    environment.err = [this](const QString& line) { err.append(line); };
    environment.readStdin = [this] { return stdinData; };
    environment.variable = [](const char*) { return QString(); };
    environment.platform = platform;
    environment.hostName = hostName;
    environment.relayUrl = relay->url();
    environment.runnerVersion = QStringLiteral("0.1.0-test");
    environment.requestTimeoutMs = 5'000;
    return environment;
}

int TestHost::run(QStringList arguments) {
    arguments << QStringLiteral("--state-dir") << stateDir();
    return ketplus::runner::app::runCli(arguments, environment());
}

QStringList TestHost::stateFiles() const {
    QStringList files;
    QDirIterator it(stateDir(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext())
        files.append(it.next());
    files.sort();
    return files;
}

QByteArray TestHost::stateBytes() const {
    QByteArray all;
    for (const QString& path : stateFiles()) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            all += file.readAll();
    }
    return all;
}

QString TestHost::writeFile(const QString& name, const QByteArray& content) const {
    const QString path = directory.path() + QLatin1Char('/') + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
        qFatal("cannot write a test file");
    return path;
}

QString TestHost::registerRunner(const QStringList& extra) {
    const QByteArray token = relay->createToken(QStringLiteral("desktop-1"));
    const int code =
        run(QStringList{QStringLiteral("register"), QString::fromLatin1(token)} + extra);
    if (code != 0)
        qFatal("register failed: %s", qPrintable(output()));
    return relay->lastRunnerId;
}

} // namespace runner_app_test

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    int failures = 0;
    {
        runner_app_test::RunnerAppStateTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_app_test::RunnerAppPairingTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_app_test::RunnerAppRunTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_app_test::RunnerAppDeliveryFilesTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    {
        runner_app_test::RunnerAppJobCallsTest test;
        failures += QTest::qExec(&test, argc, argv);
    }
    return failures == 0 ? 0 : 1;
}
