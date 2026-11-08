#include "providers/compat/ProviderVersionProbe.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <optional>

using namespace ketplus::daemon::compat;

namespace {
Policy fixturePolicy() {
    Policy result;
    result.provider = "codex";
    result.name = "Codex fixture";
    result.minimum = parseVersion("0.142.1");
    result.newestTested = parseVersion("0.150.0");
    result.tested = {"0.150.0"};
    return result;
}

int emitVersion(const QString& mode, const QString& marker) {
    QFile output;
    QFile errors;
    if (!output.open(stdout, QIODevice::WriteOnly, QFile::DontCloseHandle) ||
        !errors.open(stderr, QIODevice::WriteOnly, QFile::DontCloseHandle))
        return 11;
    if (mode == "empty")
        return 0;
    if (mode == "timeout") {
        QThread::msleep(ProviderVersionProbe::timeoutMs + 2000);
        return 0;
    }
    if (mode == "stdin-eof") {
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly, QFile::DontCloseHandle) ||
            !input.readAll().isEmpty())
            return 12;
    }
    const QByteArray banner = mode == "older" ? "codex-cli 0.120.0\n" : "codex-cli 0.150.0\n";
    if (output.write(banner) != banner.size())
        return 13;
    if (mode == "exact" || mode == "oversized") {
        const QByteArray padding((mode == "exact" ? 4096 : 4097) - banner.size(), ' ');
        if (output.write(padding) != padding.size())
            return 14;
    }
    if (mode == "stderr-overflow") {
        if (!output.flush() || errors.write(QByteArray(4096, 'x')) != 4096 || !errors.flush())
            return 15;
    }
    if (mode == "flood") {
        const QByteArray chunk(64 * 1024, 'x');
        for (int i = 0; i < 1024; ++i)
            if (output.write(chunk) != chunk.size())
                return 16;
        if (!output.flush())
            return 17;
        QFile done(marker);
        if (!done.open(QIODevice::WriteOnly) || done.write("completed") != 9)
            return 18;
    }
    if (!output.flush())
        return 19;
    return mode == "nonzero" ? 7 : 0;
}
} // namespace

class ProviderVersionProbeTest final : public QObject {
    Q_OBJECT
  private slots:
    void acceptsOnlySuccessfulBoundedOutput_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("accepted");
        for (const QString& mode : {"valid", "exact", "stdin-eof", "oversized", "stderr-overflow",
                                    "flood", "nonzero", "empty", "timeout"})
            QTest::newRow(qPrintable(mode))
                << mode << (mode == "valid" || mode == "exact" || mode == "stdin-eof");
    }
    void acceptsOnlySuccessfulBoundedOutput() {
        QFETCH(QString, mode);
        QFETCH(bool, accepted);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString marker = directory.filePath("flood-completed");
        const QString executable = QCoreApplication::applicationFilePath();
        int invocations = 0;
        ProviderVersionProbe probe(fixturePolicy(), [&](const QString& path) {
            ++invocations;
            return ProviderVersionProbe::Invocation{path, {"--probe-fixture", mode, marker}};
        });
        std::optional<Runtime> result;
        int callbacks = 0;
        probe.check(executable, [&](const Runtime& value) {
            ++callbacks;
            result = value;
        });
        QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), ProviderVersionProbe::timeoutMs + 3000);
        QCOMPARE(callbacks, 1);
        QCOMPARE(result->executable, executable);
        QCOMPARE(result->status, accepted ? SupportStatus::Supported : SupportStatus::Unknown);
        QCOMPARE(probe.cached(executable).has_value(), accepted);
        if (mode == "flood")
            QVERIFY(!QFile::exists(marker));
        mode = "valid";
        result.reset();
        probe.check(executable, [&](const Runtime& value) {
            ++callbacks;
            result = value;
        });
        QTRY_VERIFY(result.has_value());
        QCOMPARE(result->status, SupportStatus::Supported);
        QCOMPARE(result->version, QString("0.150.0"));
        QCOMPARE(callbacks, 2);
        QCOMPARE(invocations, accepted ? 1 : 2);
    }
    void concurrentExecutablesKeepTheirOwnResults() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString firstPath = directory.filePath("first-cli");
        const QString secondPath = directory.filePath("second-cli");
        for (const QString& path : {firstPath, secondPath}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("fixture"), 7);
        }
        int invocations = 0;
        ProviderVersionProbe probe(fixturePolicy(), [&](const QString& path) {
            ++invocations;
            return ProviderVersionProbe::Invocation{
                QCoreApplication::applicationFilePath(),
                {"--probe-fixture", path == firstPath ? "valid" : "older", "unused"}};
        });
        std::optional<Runtime> first, second, joined;
        probe.check(firstPath, [&](const Runtime& value) { first = value; });
        probe.check(secondPath, [&](const Runtime& value) { second = value; });
        probe.check(secondPath, [&](const Runtime& value) { joined = value; });
        QTRY_VERIFY(first && second && joined);
        QCOMPARE(first->executable, firstPath);
        QCOMPARE(first->status, SupportStatus::Supported);
        QCOMPARE(second->executable, secondPath);
        QCOMPARE(second->status, SupportStatus::Outdated);
        QCOMPARE(second->version, QString("0.120.0"));
        QCOMPARE(joined->executable, secondPath);
        QCOMPARE(joined->version, second->version);
        QCOMPARE(invocations, 2);
    }
    void replacementDuringProbeGetsASeparateResult() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("changing-cli");
        const auto replace = [&](const QByteArray& value) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                   file.write(value) == value.size();
        };
        QVERIFY(replace("valid"));
        int invocations = 0;
        ProviderVersionProbe probe(fixturePolicy(), [&](const QString& executable) {
            ++invocations;
            QFile file(executable);
            const bool current = file.open(QIODevice::ReadOnly) && file.readAll() == "valid";
            return ProviderVersionProbe::Invocation{
                QCoreApplication::applicationFilePath(),
                {"--probe-fixture", current ? "valid" : "older", "unused"}};
        });
        std::optional<Runtime> first, updated;
        probe.check(path, [&](const Runtime& value) { first = value; });
        QVERIFY(replace("replaced-version"));
        probe.check(path, [&](const Runtime& value) { updated = value; });
        QTRY_VERIFY(first && updated);
        QCOMPARE(first->version, QString("0.150.0"));
        QCOMPARE(updated->version, QString("0.120.0"));
        QCOMPARE(invocations, 2);
        const auto cached = probe.cached(path);
        QVERIFY(cached);
        QCOMPARE(cached->version, updated->version);
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe-fixture")
        return emitVersion(args[2], args[3]);
    ProviderVersionProbeTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "ProviderVersionProbeTest.moc"
