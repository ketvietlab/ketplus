#include "providers/ProviderResponseId.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <limits>
#include <memory>

using namespace ketplus::daemon;

namespace {
QJsonValue alteredId(const QJsonValue& id, QString mode) {
    mode.remove("oversized-");
    if (mode == "string")
        return QString::number(id.toInteger());
    if (mode == "leading-zero")
        return "00" + QString::number(id.toInteger());
    if (mode == "true")
        return true;
    if (mode == "fraction")
        return id.toDouble() + 0.25;
    if (mode == "null")
        return QJsonValue::Null;
    if (mode == "negative")
        return -1;
    if (mode == "zero")
        return 0;
    if (mode == "array")
        return QJsonArray{id};
    if (mode == "object")
        return QJsonObject{{"id", id}};
    if (mode == "other")
        return id.toDouble() + 1000;
    return id.toDouble(); // Integral JSON numbers retain the same request identity.
}

int serveFixture(const QString& executable) {
    QFile scenario(QFileInfo(executable).dir().filePath("scenario"));
    if (!scenario.open(QIODevice::ReadOnly))
        return 11;
    const QString mode = QString::fromUtf8(scenario.readAll());
    QFile input, output;
    if (!input.open(stdin, QIODevice::ReadOnly, QFile::DontCloseHandle) ||
        !output.open(stdout, QIODevice::WriteOnly, QFile::DontCloseHandle))
        return 12;
    const auto send = [&](QJsonObject value) {
        const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
        return output.write(bytes) == bytes.size() && output.flush();
    };
    QJsonValue initializeId;
    const QJsonObject initialized{
        {"protocolVersion", 1}, {"agentCapabilities", QJsonObject{}}, {"userAgent", "fixture"}};
    while (true) {
        const QByteArray line = input.readLine();
        if (line.isEmpty())
            return 0;
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        const auto id = request.value("id");
        const auto method = request.value("method").toString();
        if (method == "initialize") {
            initializeId = id;
            QJsonObject response{{"id", alteredId(id, mode)}, {"result", initialized}};
            if (mode.startsWith("oversized-"))
                response.insert("padding", QString(16 * 1024 * 1024 + 1, 'x'));
            if (!send(response) || !send({{"id", "fixture-barrier"},
                                          {"method", "fixture/barrier"},
                                          {"params", QJsonObject{}}}))
                return 13;
        } else if (id == QJsonValue("fixture-barrier") && method.isEmpty()) {
            if (!send({{"id", initializeId}, {"result", initialized}}))
                return 14;
        } else if (method == "fixture/echo") {
            if (!send({{"id", id}, {"result", QJsonObject{{"ok", true}}}}))
                return 15;
        }
    }
}
} // namespace

class ProviderResponseIdTest final : public QObject {
    Q_OBJECT
  private slots:
    void numericIdentityKeepsIntegerBounds() {
        const qint64 maximum = std::numeric_limits<qint64>::max();
        QCOMPARE(providerResponseId(QJsonValue(qint64(1))), std::optional<quint64>(1));
        QCOMPARE(providerResponseId(QJsonValue(1.0)), std::optional<quint64>(1));
        QCOMPARE(providerResponseId(QJsonValue(maximum)),
                 std::optional<quint64>(static_cast<quint64>(maximum)));
        QVERIFY(!providerResponseId(QJsonValue(static_cast<double>(maximum))));
        QVERIFY(!providerResponseId(QJsonValue(1.25)));
        QVERIFY(!providerResponseId(QJsonValue(-0.0)));
        QVERIFY(!providerResponseId(QJsonValue(QStringLiteral("1"))));
        QVERIFY(!providerResponseId(QJsonValue(true)));
    }
    void malformedIdsCannotTakePendingCalls_data() {
        QTest::addColumn<QString>("provider");
        QTest::addColumn<QString>("mode");
        for (const QString& provider : {"codex", "claude", "cursor"})
            for (const QString& mode : {"string", "leading-zero", "true", "fraction", "null",
                                        "negative", "zero", "array", "object", "other", "valid"})
                QTest::newRow(qPrintable(provider + '-' + mode)) << provider << mode;
        for (const QString& mode : {"oversized-string", "oversized-true", "oversized-fraction"})
            QTest::newRow(qPrintable("codex-" + mode)) << QString("codex") << mode;
    }
    void malformedIdsCannotTakePendingCalls() {
#ifdef Q_OS_WIN
        QSKIP("The local multi-provider fixture uses POSIX executable symlink launchers.");
#else
        QFETCH(QString, provider);
        QFETCH(QString, mode);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath("fake-" + provider);
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), executable));
        QFile scenario(directory.filePath("scenario"));
        QVERIFY(scenario.open(QIODevice::WriteOnly));
        QCOMPARE(scenario.write(mode.toUtf8()), mode.toUtf8().size());
        scenario.close();
        int callbacks = 0;
        QJsonObject result;
        QString error;
        std::unique_ptr<ProviderAdapter> adapter;
        if (provider == "codex")
            adapter = std::make_unique<CodexAdapter>(executable);
        else if (provider == "claude")
            adapter = std::make_unique<ClaudeAdapter>(executable);
        else
            adapter = std::make_unique<CursorAdapter>(executable);
        QSignalSpy stops(adapter.get(), &ProviderAdapter::stopped);
        QSignalSpy requests(adapter.get(), &ProviderAdapter::requestReceived);
        adapter->call("fixture/echo", {}, [&](const QJsonObject& value, const QString& failure) {
            ++callbacks;
            result = value;
            error = failure;
        });
        QTRY_VERIFY_WITH_TIMEOUT(!requests.isEmpty() || !stops.isEmpty(), 10000);
        QVERIFY2(stops.isEmpty(),
                 stops.isEmpty() ? "" : qPrintable(stops.first().first().toString()));
        QCOMPARE(requests.first().at(1).toString(), QString("fixture/barrier"));
        QCOMPARE(adapter->isReady(), mode == "valid");
        if (mode != "valid")
            QCOMPARE(callbacks, 0);
        // The barrier follows the malformed response on the same stdout stream.
        // Only now release the correctly typed response to the pending initialize call.
        adapter->respond(requests.first().first().value<QJsonValue>(), {});
        QTRY_VERIFY_WITH_TIMEOUT(adapter->isReady() && callbacks == 1, 10000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(result.value("ok"), QJsonValue(true));
        QVERIFY(stops.isEmpty());
        adapter->stop();
        QCOMPARE(callbacks, 1);
#endif
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const QString executable = args.first();
    const QString name = QFileInfo(executable).fileName();
    if (name.startsWith("fake-")) {
        if (args.contains("--version")) {
            QFile output;
            if (!output.open(stdout, QIODevice::WriteOnly, QFile::DontCloseHandle))
                return 16;
            output.write(name.contains("cursor") ? "2026.09.18-test\n" : "codex-cli 0.154.0\n");
            return output.flush() ? 0 : 17;
        }
        return serveFixture(executable);
    }
    ProviderResponseIdTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "ProviderResponseIdTest.moc"
