#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace ketplus::daemon;

namespace {
QByteArray badFrame(const QString& shape) {
    if (shape == "oversized") // One byte over the 16 MiB line limit, then a newline.
        return QByteArray("{\"secret-marker\":\"") + QByteArray(16 * 1024 * 1024, 'x') + "\"}\n";
    if (shape == "utf8")
        return QByteArray("{\"secret-marker\":\"") + char(0xc3) + "(\"}\n";
    if (shape == "binary")
        return QByteArray("{\"secret-marker\":\"") + char(0) + "\"}\n";
    if (shape == "control")
        return QByteArray("{\"secret-marker\":\"") + char(1) + "\"}\n";
    if (shape == "raw-tab")
        return "{\"secret-marker\":\"\t\"}\n";
    if (shape == "raw-cr")
        return "{\"secret-marker\":\"\r\"}\n";
    if (shape == "array")
        return "[\"secret-marker\"]\n";
    return "{\"secret-marker\":\n";
}

int fixture(const QString& executable, const QStringList& args) {
    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly, QFile::DontCloseHandle))
        return 1;
    if (args.contains("--version")) {
        output.write(executable.endsWith("fake-codex") ? "codex-cli 0.154.0\n" : "2026.09.18\n");
        output.flush();
        return 0;
    }
    QFile scenario(QFileInfo(executable).absolutePath() + "/scenario");
    if (!scenario.open(QIODevice::ReadOnly))
        return 2;
    const QString mode = QString::fromUtf8(scenario.readAll());
    const QString shape = mode.section(':', 1);
    const auto encode = [](const QJsonObject& object) {
        return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    };
    const auto reply = [&](const QJsonValue& id) {
        QByteArray bytes =
            encode({{"id", id}, {"result", QJsonObject{{"ok", true}, {"escaped", "\t\r\n\"\\"}}}});
        bytes.replace(",\"result\"", ",\t\"result\"");
        return bytes;
    };
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly, QFile::DontCloseHandle))
        return 3;
    QJsonValue held;
    while (true) {
        const QByteArray line = input.readLine();
        if (line.isEmpty())
            return 0;
        const QJsonObject call = QJsonDocument::fromJson(line).object();
        const auto id = call.value("id");
        const QString method = call.value("method").toString();
        if (method == "initialize") {
            // Coalesce an apparently valid suffix after the broken frame. It must
            // never initialize the rejected process generation.
            const QByteArray prefix =
                mode.startsWith("init:") ? badFrame(shape) : QByteArray("\n \r\n");
            output.write(prefix + reply(id));
        } else if (method == "fixture/hold") {
            held = id;
        } else if (method == "fixture/trigger") {
            const QByteArray prefix = mode.startsWith("call:") ? badFrame(shape) : QByteArray();
            output.write(prefix + reply(held) + reply(id) +
                         encode({{"id", "suffix"}, {"method", "fixture/forbidden-suffix"}}));
        } else if (method == "fixture/echo") {
            output.write(reply(id));
        }
        output.flush();
    }
}
} // namespace

class ProviderMalformedFrameTest final : public QObject {
    Q_OBJECT
  private slots:
    // An oversized line stops the process generation once: killing it must not publish a
    // second stop or count as a second crash.
    void oversizedFrameStopsOnce_data() {
        QTest::addColumn<QString>("provider");
        for (const QString& provider : {"claude", "cursor"})
            QTest::newRow(qPrintable(provider)) << provider;
    }
    void oversizedFrameStopsOnce() {
#ifdef Q_OS_WIN
        QSKIP("The fixture uses POSIX executable symlink launchers.");
#else
        QFETCH(QString, provider);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath("fake-" + provider);
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), executable));
        const auto setMode = [&](const QByteArray& value) {
            QFile file(directory.filePath("scenario"));
            return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                   file.write(value) == value.size();
        };
        QVERIFY(setMode("call:oversized"));
        std::unique_ptr<ProviderAdapter> adapter;
        if (provider == "claude")
            adapter = std::make_unique<ClaudeAdapter>(executable);
        else
            adapter = std::make_unique<CursorAdapter>(executable);
        QSignalSpy stops(adapter.get(), &ProviderAdapter::stopped);
        QString triggerError;
        adapter->call("fixture/trigger", {},
                      [&](const QJsonObject&, const QString& error) { triggerError = error; });
        QTRY_VERIFY_WITH_TIMEOUT(!triggerError.isEmpty(), 10000);
        QVERIFY(triggerError.contains("oversized message"));
        const auto processes =
            adapter->findChildren<QProcess*>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(!processes.isEmpty());
        for (QProcess* process : processes)
            QTRY_COMPARE_WITH_TIMEOUT(process->state(), QProcess::NotRunning, 10000);
        QTest::qWait(100);
        QCOMPARE(stops.size(), 1);
        QVERIFY(stops.first().first().toString().contains("oversized message"));
        // The next call starts a fresh process after one crash's backoff.
        QVERIFY(setMode("valid"));
        QString recoveryError = "pending";
        QElapsedTimer recovery;
        recovery.start();
        adapter->call("fixture/echo", {},
                      [&](const QJsonObject&, const QString& error) { recoveryError = error; });
        QTRY_VERIFY_WITH_TIMEOUT(recoveryError != "pending", 10000);
        QVERIFY2(recoveryError.isEmpty(), qPrintable(recoveryError));
        QVERIFY2(recovery.elapsed() < 1900, qPrintable(QString::number(recovery.elapsed())));
        QCOMPARE(stops.size(), 1);
        adapter->stop();
#endif
    }

    void rejectsBrokenGenerationAndRecovers_data() {
        QTest::addColumn<QString>("provider");
        QTest::addColumn<QString>("mode");
        for (const QString& provider : {"codex", "claude", "cursor"}) {
            QTest::newRow(qPrintable(provider + "-valid")) << provider << QString("valid");
            for (const QString& phase : {"init", "call"}) {
                for (const QString& shape :
                     {"utf8", "binary", "control", "raw-tab", "raw-cr", "array", "syntax"})
                    QTest::newRow(qPrintable(provider + '-' + phase + '-' + shape))
                        << provider << phase + ':' + shape;
            }
        }
    }
    void rejectsBrokenGenerationAndRecovers() {
#ifdef Q_OS_WIN
        QSKIP("The fixture uses POSIX executable symlink launchers.");
#else
        QFETCH(QString, provider);
        QFETCH(QString, mode);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath("fake-" + provider);
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), executable));
        const auto setMode = [&](const QByteArray& value) {
            QFile file(directory.filePath("scenario"));
            return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                   file.write(value) == value.size();
        };
        QVERIFY(setMode(mode.toUtf8()));
        std::unique_ptr<ProviderAdapter> adapter;
        if (provider == "codex")
            adapter = std::make_unique<CodexAdapter>(executable);
        else if (provider == "claude")
            adapter = std::make_unique<ClaudeAdapter>(executable);
        else
            adapter = std::make_unique<CursorAdapter>(executable);
        QSignalSpy stops(adapter.get(), &ProviderAdapter::stopped);
        QSignalSpy requests(adapter.get(), &ProviderAdapter::requestReceived);
        int held = 0, triggered = 0;
        QString heldError, triggerError;
        adapter->call("fixture/hold", {}, [&](const QJsonObject&, const QString& error) {
            ++held;
            heldError = error;
        });
        adapter->call("fixture/trigger", {}, [&](const QJsonObject&, const QString& error) {
            ++triggered;
            triggerError = error;
        });
        QTRY_COMPARE_WITH_TIMEOUT(held, 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(triggered, 1, 10000);
        if (mode == "valid") {
            QVERIFY(heldError.isEmpty());
            QVERIFY(triggerError.isEmpty());
            QVERIFY(adapter->isReady());
            QVERIFY(stops.isEmpty());
            QCOMPARE(requests.size(), 1);
            adapter->stop();
            return;
        }
        QVERIFY2(!heldError.isEmpty(), qPrintable(mode));
        QVERIFY2(!triggerError.isEmpty(), qPrintable(mode));
        QVERIFY(heldError.contains("malformed protocol message"));
        QVERIFY(triggerError.contains("malformed protocol message"));
        QVERIFY(!heldError.contains("secret-marker"));
        QVERIFY(!triggerError.contains("secret-marker"));
        QTRY_VERIFY_WITH_TIMEOUT(!stops.isEmpty(), 10000);
        QVERIFY(!adapter->isReady());
        QCOMPARE(stops.size(), 1);
        QVERIFY(requests.isEmpty());
        const auto processes =
            adapter->findChildren<QProcess*>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(!processes.isEmpty());
        for (QProcess* process : processes)
            QTRY_COMPARE_WITH_TIMEOUT(process->state(), QProcess::NotRunning, 10000);
        QVERIFY(setMode("valid"));
        int recovered = 0;
        QString recoveryError;
        QJsonObject recoveredResult;
        adapter->call("fixture/echo", {}, [&](const QJsonObject& result, const QString& error) {
            ++recovered;
            recoveryError = error;
            recoveredResult = result;
        });
        QTRY_COMPARE_WITH_TIMEOUT(recovered, 1, 10000);
        QVERIFY2(recoveryError.isEmpty(), qPrintable(recoveryError));
        QVERIFY(recoveredResult.value("ok").toBool());
        QVERIFY(adapter->isReady());
        QCOMPARE(held, 1);
        QCOMPARE(triggered, 1);
        adapter->stop();
#endif
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString executable = QString::fromLocal8Bit(argv[0]);
    if (QFileInfo(executable).fileName().startsWith("fake-"))
        return fixture(executable, app.arguments());
    ProviderMalformedFrameTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ProviderMalformedFrameTest.moc"
