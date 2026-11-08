#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {
constexpr qsizetype tailBytes = 1024 * 1024;
constexpr qsizetype chunkBytes = 64 * 1024;
constexpr int chunksPerFlood = 512;
constexpr int progressEvery = 64;

int fixture(const QString& executable, const QStringList& args) {
    QFile output, errors, input;
    if (!output.open(stdout, QIODevice::WriteOnly, QFile::DontCloseHandle) ||
        !errors.open(stderr, QIODevice::WriteOnly, QFile::DontCloseHandle) ||
        !input.open(stdin, QIODevice::ReadOnly, QFile::DontCloseHandle))
        return 1;
    const auto send = [&](const QJsonObject& object) {
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
        return output.write(bytes) == bytes.size() && output.flush();
    };
    if (args.contains("--version")) {
        output.write(executable.endsWith("fake-codex") ? "codex-cli 0.154.0\n" : "2026.09.18\n");
        return output.flush() ? 0 : 2;
    }
    while (true) {
        const auto line = input.readLine();
        if (line.isEmpty())
            return 0;
        const auto call = QJsonDocument::fromJson(line).object();
        const auto id = call.value("id");
        if (id.isUndefined())
            continue;
        const auto method = call.value("method").toString();
        if (method == "fixture/flood") {
            const char fill =
                call.value("params").toObject().value("round").toInt() == 1 ? 'a' : 'b';
            const QByteArray chunk(chunkBytes, fill);
            for (int i = 1; i <= chunksPerFlood; ++i) {
                if (errors.write(chunk) != chunk.size() || !errors.flush())
                    return 3;
                if (i % progressEvery == 0) {
                    const QString requestId = QStringLiteral("progress-%1").arg(i);
                    if (!send({{"id", requestId},
                               {"method", "fixture/progress"},
                               {"params", QJsonObject{{"chunks", i}}}}))
                        return 4;
                    // Require a stdout round trip before writing more stderr. A
                    // completion-only check would miss starvation during a flood.
                    const auto response = QJsonDocument::fromJson(input.readLine()).object();
                    if (response.value("id") != requestId || !response.value("result").isObject())
                        return 5;
                }
            }
        }
        if (!send({{"id", id}, {"result", QJsonObject{{"ok", true}}}}))
            return 6;
    }
}
} // namespace

namespace ketplus::daemon {
class ProviderStderrFloodTest final : public QObject {
    Q_OBJECT

    // Test-only friendship observes existing retention without adding a runtime
    // diagnostic API or copying the drain implementation into the fixture.
    static const QByteArray& tail(ProviderAdapter* adapter) {
        if (const auto* codex = qobject_cast<CodexAdapter*>(adapter))
            return codex->stderrBuffer_;
        if (const auto* claude = qobject_cast<ClaudeAdapter*>(adapter))
            return claude->stderrBuffer_;
        return qobject_cast<CursorAdapter*>(adapter)->stderrBuffer_;
    }

    static bool callSucceeds(ProviderAdapter* adapter, const QString& method,
                             const QJsonObject& params = {}) {
        struct Reply {
            int callbacks = 0;
            QString error;
            QJsonObject result;
        };
        const auto reply = std::make_shared<Reply>();
        adapter->call(method, params, [reply](const QJsonObject& value, const QString& error) {
            ++reply->callbacks;
            reply->result = value;
            reply->error = error;
        });
        return QTest::qWaitFor([&] { return reply->callbacks != 0; }, 30000) &&
               reply->callbacks == 1 && reply->error.isEmpty() &&
               reply->result.value("ok").toBool();
    }

  private slots:
    void drainsWhileServingStdout_data() {
        QTest::addColumn<QString>("provider");
        for (const QString& provider : {"codex", "claude", "cursor"})
            QTest::newRow(qPrintable(provider)) << provider;
    }
    void drainsWhileServingStdout() {
#ifdef Q_OS_WIN
        QSKIP("The fixture uses POSIX executable symlink launchers.");
#else
        QFETCH(QString, provider);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto executable = directory.filePath("fake-" + provider);
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), executable));
        std::unique_ptr<ProviderAdapter> adapter;
        if (provider == "codex")
            adapter = std::make_unique<CodexAdapter>(executable);
        else if (provider == "claude")
            adapter = std::make_unique<ClaudeAdapter>(executable);
        else
            adapter = std::make_unique<CursorAdapter>(executable);
        auto* process = adapter->findChild<QProcess*>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(process);
        QSignalSpy stops(adapter.get(), &ProviderAdapter::stopped);
        int stderrSignals = 0;
        bool bounded = true, drained = true;
        int progress = 0;
        // Destroy this receiver before captured locals, including on assertion failure.
        QObject observer;
        // The adapter connected its drain first; this observer runs afterward.
        connect(process, &QProcess::readyReadStandardError, &observer, [&] {
            ++stderrSignals;
            bounded &= tail(adapter.get()).size() <= tailBytes;
            const auto channel = process->readChannel();
            process->setReadChannel(QProcess::StandardError);
            drained &= process->bytesAvailable() == 0;
            process->setReadChannel(channel);
        });
        connect(adapter.get(), &ProviderAdapter::requestReceived, &observer,
                [&](const QJsonValue& id, const QString& method, const QJsonObject& params) {
                    if (method == "fixture/progress") {
                        ++progress;
                        QCOMPARE(params.value("chunks").toInt(),
                                 ((progress - 1) % (chunksPerFlood / progressEvery) + 1) *
                                     progressEvery);
                        adapter->respond(id, {});
                    }
                });
        QVERIFY(callSucceeds(adapter.get(), "fixture/echo"));
        QVERIFY(adapter->isReady());
        const auto pid = process->processId();
        QVERIFY(pid > 0);
        for (int round = 1; round <= 2; ++round) {
            QVERIFY(callSucceeds(adapter.get(), "fixture/flood", {{"round", round}}));
            QCOMPARE(progress, round * (chunksPerFlood / progressEvery));
            QVERIFY(stderrSignals > 0);
            QVERIFY2(drained, "QProcess retained unread stderr after the adapter drain slot");
            QVERIFY2(bounded, "adapter retained more than the 1-MiB stderr tail");
            QTRY_COMPARE_WITH_TIMEOUT(tail(adapter.get()).size(), tailBytes, 10000);
            QTRY_VERIFY_WITH_TIMEOUT(
                tail(adapter.get()) == QByteArray(tailBytes, round == 1 ? 'a' : 'b'), 10000);
            QVERIFY(callSucceeds(adapter.get(), "fixture/echo"));
            QCOMPARE(process->processId(), pid);
            QVERIFY(adapter->isReady());
            QVERIFY(stops.isEmpty());
        }
        adapter->stop();
#endif
    }
};
} // namespace ketplus::daemon

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString executable = QString::fromLocal8Bit(argv[0]);
    if (QFileInfo(executable).fileName().startsWith("fake-"))
        return fixture(executable, app.arguments());
    ketplus::daemon::ProviderStderrFloodTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ProviderStderrFloodTest.moc"
