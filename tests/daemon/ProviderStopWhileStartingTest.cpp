#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace ketplus::daemon;

namespace {
// The fake provider answers a version probe and otherwise reads until its input closes, so a
// handshake sent to it stays unanswered until the adapter stops it.
int fixture(const QStringList& args) {
    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly, QFile::DontCloseHandle))
        return 1;
    if (args.contains("--version")) {
        output.write(QFileInfo(args.first()).fileName() == "fake-codex" ? "codex-cli 0.154.0\n"
                                                                        : "2026.09.18\n");
        output.flush();
        return 0;
    }
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly, QFile::DontCloseHandle))
        return 1;
    while (!input.readLine().isEmpty() || !input.atEnd()) {
    }
    return 0;
}

std::unique_ptr<ProviderAdapter> adapterFor(const QString& provider, const QString& executable) {
    if (provider == "codex")
        return std::make_unique<CodexAdapter>(executable);
    if (provider == "claude")
        return std::make_unique<ClaudeAdapter>(executable);
    return std::make_unique<CursorAdapter>(executable);
}
} // namespace

// Stopping an adapter whose process is still starting must not report a provider stop. The
// wait in stop() delivers the process start; initializing it then, and failing that handshake,
// told the owner the provider had crashed. During teardown that report reached a SessionHost
// whose other adapters were already gone.
class ProviderStopWhileStartingTest final : public QObject {
    Q_OBJECT

  private slots:
    void stopWhileStarting_data() {
        QTest::addColumn<QString>("provider");
        QTest::addColumn<bool>("destroy");
        for (const QString provider : {"codex", "claude", "cursor"}) {
            QTest::newRow(qPrintable(provider + "-stop")) << provider << false;
            QTest::newRow(qPrintable(provider + "-destroy")) << provider << true;
        }
    }

    void stopWhileStarting() {
#ifdef Q_OS_WIN
        QSKIP("The fixture uses POSIX executable symlink launchers.");
#else
        QFETCH(QString, provider);
        QFETCH(bool, destroy);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath("fake-" + provider);
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), executable));
        std::unique_ptr<ProviderAdapter> adapter = adapterFor(provider, executable);
        QObject observer;
        int stops = 0;
        connect(adapter.get(), &ProviderAdapter::stopped, &observer, [&stops] { ++stops; });
        QString callError;
        bool answered = false;
        adapter->call("fixture/trigger", {}, [&](const QJsonObject&, const QString& error) {
            answered = true;
            callError = error;
        });
        // No event loop runs between the call and the stop: the process is still starting.
        if (destroy)
            adapter.reset();
        else
            adapter->stop();
        QTest::qWait(100);
        QCOMPARE(stops, 0);
        if (!destroy) {
            // A deliberate stop fails the queued call; a destroyed adapter drops it.
            QVERIFY(answered);
            QVERIFY(!callError.isEmpty());
            QVERIFY(!adapter->isReady());
        }
#endif
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (QFileInfo(QString::fromLocal8Bit(argv[0])).fileName().startsWith("fake-"))
        return fixture(app.arguments());
    ProviderStopWhileStartingTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ProviderStopWhileStartingTest.moc"
