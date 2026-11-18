#include "RunnerAppTest.h"

#include "RunnerRules.h"
#include "run/Backoff.h"
#include "state/RunnerState.h"
#include "state/StateDirectory.h"

#include <QFile>
#include <QTest>

#include <sys/stat.h>

using namespace ketplus::runner::app;

namespace runner_app_test {
namespace {

mode_t modeOf(const QString& path) {
    struct stat status{};
    if (::stat(QFile::encodeName(path).constData(), &status) != 0)
        return 0;
    return status.st_mode & 0777;
}

} // namespace

void RunnerAppStateTest::createsAPrivateDirectory() {
    TestHost host;
    host.registerRunner();
    QCOMPARE(modeOf(host.stateDir()), mode_t(0700));
    QCOMPARE(modeOf(host.stateDir() + "/pins"), mode_t(0700));
    for (const QString& file : host.stateFiles())
        QCOMPARE(modeOf(file), mode_t(0600));
    for (const char* name : {"/runner.key", "/credential.json", "/config.json", "/lock"})
        QVERIFY2(QFile::exists(host.stateDir() + name), name);
    QCOMPARE(QFile(host.stateDir() + "/runner.key").size(), 32);
}

void RunnerAppStateTest::refusesADirectoryOthersCanReach() {
    for (const mode_t mode : {0750, 0705, 0701, 0770}) {
        TestHost host;
        QVERIFY(QDir().mkpath(host.stateDir()));
        ::chmod(QFile::encodeName(host.stateDir()).constData(), mode);
        QCOMPARE(host.run({"status"}), 3);
        QVERIFY(host.output().contains("accessible to other users"));
        QCOMPARE(modeOf(host.stateDir()), mode); // left as it was
    }
}

void RunnerAppStateTest::refusesADirectoryOfAnotherUser() {
    TestHost host;
    QString error;
    QVERIFY(!StateDirectory::open(host.stateDir(), &error, ::geteuid() + 1));
    QVERIFY(error.contains("belongs to another user"));
    QVERIFY(StateDirectory::open(host.stateDir(), &error).has_value());
}

void RunnerAppStateTest::refusesASecondProcess() {
    TestHost host;
    QString error;
    std::optional<StateDirectory> first = StateDirectory::open(host.stateDir(), &error);
    QVERIFY(first);
    QCOMPARE(host.run({"status"}), 3);
    QVERIFY(host.output().contains("Another ketplus-runner is using"));
    first.reset(); // the lock goes with the process
    QCOMPARE(host.run({"status"}), 0);
}

void RunnerAppStateTest::writesAtomically() {
    TestHost host;
    QString error;
    const std::optional<StateDirectory> directory = StateDirectory::open(host.stateDir(), &error);
    QVERIFY(directory);
    QVERIFY(directory->write("config.json", "first", &error));
    QVERIFY(directory->write("config.json", "second", &error));
    QCOMPARE(directory->read("config.json").data, QByteArray("second"));
    QCOMPARE(modeOf(host.stateDir() + "/config.json"), mode_t(0600));
    // Only the target and the lock: no temporary file is left behind.
    QCOMPARE(host.stateFiles(),
             QStringList({host.stateDir() + "/config.json", host.stateDir() + "/lock"}));
    // A temporary file of an interrupted write is never mistaken for a pin.
    QVERIFY(directory->write("pins/.desktop-1.json.0.tmp", "partial", &error));
    const RunnerState state(*directory);
    const std::optional<QList<ketplus::runner::pairing::RunnerPin>> pins = state.loadPins(&error);
    QVERIFY(pins && pins->isEmpty());
}

void RunnerAppStateTest::reportsCorruptFilesWithoutOverwriting_data() {
    QTest::addColumn<QString>("file");
    QTest::addColumn<QByteArray>("content");
    QTest::newRow("credential not JSON") << "credential.json" << QByteArray("{oops");
    QTest::newRow("credential other version")
        << "credential.json" << QByteArray(R"({"version":2})");
    QTest::newRow("credential short secret")
        << "credential.json"
        << QByteArray(R"({"version":1,"runnerId":"r","workspaceId":"w","keyVersion":1,)"
                      R"("refreshCredential":"short"})");
    QTest::newRow("config bad label")
        << "config.json" << QByteArray(R"({"version":1,"name":"n","labels":["Bad"]})");
    QTest::newRow("key too short") << "runner.key" << QByteArray(31, 'k');
    QTest::newRow("pin not JSON") << "pins/desktop-1.json" << QByteArray("[]");
    QTest::newRow("pin of another machine") << "pins/desktop-1.json" << QByteArray();
}

void RunnerAppStateTest::reportsCorruptFilesWithoutOverwriting() {
    QFETCH(QString, file);
    QFETCH(QByteArray, content);
    TestHost host;
    host.registerRunner();
    QByteArray corrupt = content;
    if (corrupt.isEmpty()) { // a valid pin stored under the wrong machine's name
        QFile pin(host.stateDir() + "/pins/desktop-1.json");
        QVERIFY(pin.open(QIODevice::ReadOnly));
        corrupt = pin.readAll().replace("\"desktop-1\"", "\"desktop-9\"");
    }
    {
        QFile target(host.stateDir() + "/" + file);
        QVERIFY(target.open(QIODevice::WriteOnly | QIODevice::Truncate));
        target.write(corrupt);
    }
    host.err.clear();
    QCOMPARE(host.run({"status"}), 3);
    QVERIFY2(host.output().contains(file), qPrintable(host.output()));
    QCOMPARE(host.run({"run"}), 3);
    QFile target(host.stateDir() + "/" + file);
    QVERIFY(target.open(QIODevice::ReadOnly));
    QCOMPARE(target.readAll(), corrupt);
}

void RunnerAppStateTest::refusesAFileOthersCanRead() {
    TestHost host;
    host.registerRunner();
    ::chmod(QFile::encodeName(host.stateDir() + "/credential.json").constData(), 0644);
    QCOMPARE(host.run({"status"}), 3);
    QVERIFY(host.output().contains("private file"));
}

void RunnerAppStateTest::resolvesTheDirectory() {
    Environment environment;
    QHash<QString, QString> variables;
    environment.variable = [&variables](const char* name) {
        return variables.value(QString::fromLatin1(name));
    };
    variables.insert("HOME", "/home/u");
    QCOMPARE(StateDirectory::resolve({}, environment), "/home/u/.local/state/ketplus-runner");
    variables.insert("XDG_STATE_HOME", "relative/ignored");
    QCOMPARE(StateDirectory::resolve({}, environment), "/home/u/.local/state/ketplus-runner");
    variables.insert("XDG_STATE_HOME", "/xdg");
    QCOMPARE(StateDirectory::resolve({}, environment), "/xdg/ketplus-runner");
    variables.insert("KETPLUS_RUNNER_STATE_DIR", "/env/state");
    QCOMPARE(StateDirectory::resolve({}, environment), "/env/state");
    QCOMPARE(StateDirectory::resolve("/option/state", environment), "/option/state");
}

void RunnerAppStateTest::checksNamesAndLabels() {
    QCOMPARE(normalizedName("  node one \t"), std::optional<QString>("node one"));
    QCOMPARE(normalizedName(QString(64, QChar(0x00e9))),
             std::optional<QString>(QString(64, QChar(0x00e9))));
    QVERIFY(!normalizedName(QString(65, 'a')));
    QVERIFY(!normalizedName("   "));
    QVERIFY(!normalizedName("a\x01"
                            "b"));
    QVERIFY(!normalizedName(QString("a") + QChar(0x7f)));
    // 64 code points of a character outside the BMP: 128 UTF-16 units.
    QString emoji;
    for (int i = 0; i < 64; ++i)
        emoji += QString::fromUcs4(U"\U0001F600", 1);
    QVERIFY(normalizedName(emoji));
    QCOMPARE(defaultName(QString(70, 'h')), QString(64, 'h'));

    QVERIFY(labelsError({"gpu", "linux.x86_64", "a-b_c", "0"}).isEmpty());
    QStringList many;
    for (int i = 0; i < 32; ++i)
        many.append(QString::number(i));
    QVERIFY(labelsError(many).isEmpty());
    QVERIFY(!labelsError(many + QStringList{"x"}).isEmpty());
    QVERIFY(labelsError({QString(64, 'a')}).isEmpty());
    for (const QString& bad : {QString(65, 'a'), QString("Gpu"), QString("-a"), QString(".a"),
                               QString(), QString("os:linux"), QString("a b")})
        QVERIFY2(!labelsError({bad}).isEmpty(), qPrintable(bad));
    QVERIFY(!labelsError({"a", "a"}).isEmpty());
}

void RunnerAppStateTest::backoffDoublesWithJitterUpToTheCap() {
    double random = 0.0;
    Backoff backoff([&random] { return random; });
    QList<qint64> delays;
    for (int i = 0; i < 8; ++i)
        delays.append(backoff.next());
    QCOMPARE(delays, (QList<qint64>{1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000}));
    backoff.reset();
    random = 0.999999;
    QCOMPARE(backoff.next(), 1499);
    QCOMPARE(backoff.next(), 2999);
    for (int i = 0; i < 4; ++i)
        static_cast<void>(backoff.next());
    QCOMPARE(backoff.next(), 60000); // 64 s with jitter is still capped
    random = 0.5;
    backoff.reset();
    QCOMPARE(backoff.next(), 1250);
}

} // namespace runner_app_test
