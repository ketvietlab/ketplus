#include "atlas/AtlasLiveRuntime.h"
#include <QFile>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QWebSocketServer>
#include <QtTest>
using namespace ketplus;
using namespace ketplus::daemon;
namespace {
bool write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray get(quint16 port) {
    QNetworkAccessManager network;
    auto* reply = network.get(QNetworkRequest(QUrl(QString("http://127.0.0.1:%1/").arg(port))));
    QSignalSpy finished(reply, &QNetworkReply::finished);
    if (!reply->isFinished() && !finished.wait(5000))
        return {};
    const auto bytes = reply->readAll();
    delete reply;
    return bytes;
}
bool listening(quint16 port) {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    return socket.waitForConnected(100);
}
} // namespace
class AtlasLiveRuntimeTest final : public QObject {
    Q_OBJECT
  private slots:
    void syntheticReadiness_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("accepted");
        for (const auto* mode : {"wrong-port", "longer-port", "embedded", "unterminated",
                                 "truncated-prefix", "on-stop"})
            QTest::newRow(mode) << QString::fromLatin1(mode) << false;
        for (const auto* mode : {"valid-lf", "valid-crlf", "valid-slash", "valid-split",
                                 "valid-after-log", "valid-exit", "wrong-then-right"})
            QTest::newRow(mode) << QString::fromLatin1(mode) << true;
    }
    void syntheticReadiness() {
#ifndef Q_OS_UNIX
        QSKIP("The synthetic executable fixture uses a Unix shebang.");
#else
        QFETCH(QString, mode);
        QFETCH(bool, accepted);
        const auto previousCli = qgetenv("KETPLUS_KETATLAS_CLI");
        const auto previousMode = qgetenv("KETPLUS_TEST_ATLAS_READY_MODE");
        const auto previousMarker = qgetenv("KETPLUS_TEST_ATLAS_READY_MARKER");
        const auto restore = qScopeGuard([&] {
            if (previousCli.isNull())
                qunsetenv("KETPLUS_KETATLAS_CLI");
            else
                qputenv("KETPLUS_KETATLAS_CLI", previousCli);
            if (previousMode.isNull())
                qunsetenv("KETPLUS_TEST_ATLAS_READY_MODE");
            else
                qputenv("KETPLUS_TEST_ATLAS_READY_MODE", previousMode);
            if (previousMarker.isNull())
                qunsetenv("KETPLUS_TEST_ATLAS_READY_MARKER");
            else
                qputenv("KETPLUS_TEST_ATLAS_READY_MARKER", previousMarker);
        });
        const QString cli = QFileInfo(QString::fromUtf8(__FILE__))
                                .dir()
                                .filePath("../fixtures/fake-atlas-daemon-readiness.py");
        QVERIFY(QFileInfo(cli).isExecutable());
        qputenv("KETPLUS_KETATLAS_CLI", QFile::encodeName(cli));
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString marker = root.filePath("process-ready-to-stop");
        qputenv("KETPLUS_TEST_ATLAS_READY_MARKER", QFile::encodeName(marker));
        AtlasDescriptor atlas;
        atlas.path = root.filePath("atlas.json");
        atlas.directory = root.path();
        atlas.workspaceRoot = root.path();
        QVERIFY(write(atlas.path, "{}"));
        QWebSocketServer relay("synthetic-runtime", QWebSocketServer::NonSecureMode);
        QVERIFY(relay.listen(QHostAddress::LocalHost, 0));
        int relayConnections = 0;
        connect(&relay, &QWebSocketServer::newConnection, &relay, [&] {
            ++relayConnections;
            auto* peer = relay.nextPendingConnection();
            peer->setParent(&relay);
            peer->sendTextMessage("{\"type\":\"ready\",\"version\":1}");
        });
        // Rejected output can be repaired by starting a fresh owned process.
        const QStringList attempts = accepted ? QStringList{mode} : QStringList{mode, "valid-lf"};
        for (const auto& attempt : attempts) {
            qputenv("KETPLUS_TEST_ATLAS_READY_MODE", attempt.toUtf8());
            QPointer<AtlasLiveRuntime> runtime = new AtlasLiveRuntime;
            const auto cleanup = qScopeGuard([&] {
                if (runtime) {
                    runtime->stop();
                    delete runtime.data();
                }
            });
            QList<QJsonObject> callbacks;
            runtime->start(
                atlas, QUrl(QString("ws://127.0.0.1:%1/v2/atlas/tunnel").arg(relay.serverPort())),
                "synthetic-only", 7, [&](const QJsonObject& result) { callbacks.append(result); });
            if (attempt == "on-stop") {
                QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 5000);
                runtime->stop();
                QTest::qWait(100);
                QVERIFY(callbacks.isEmpty());
                QVERIFY(runtime && !runtime->reusable());
                QCOMPARE(relayConnections, 0);
                continue;
            }
            QTRY_VERIFY_WITH_TIMEOUT(!callbacks.isEmpty(), 5000);
            QCOMPARE(callbacks.size(), 1);
            const bool expected = accepted || attempt == "valid-lf";
            QCOMPARE(callbacks.front().value("ok").toBool(), expected);
            if (expected) {
                if (attempt == "valid-exit") {
                    QTRY_VERIFY_WITH_TIMEOUT(!runtime, 5000);
                    QCOMPARE(callbacks.size(), 1);
                } else {
                    QVERIFY(runtime && runtime->reusable());
                }
                QCOMPARE(callbacks.front().value("context_revision").toInteger(), 7);
                const auto origins = callbacks.front().value("origins").toArray();
                QCOMPARE(origins.size(), 1);
                QVERIFY(origins.at(0).toObject().value("port").toInt() > 0);
            } else {
                QCOMPARE(callbacks.front().value("error_code").toString(), "atlas_process_exited");
                QVERIFY(!runtime || !runtime->reusable());
            }
        }
#endif
    }
    void realCliOwnsViewerAndRendererAndReattachesWithoutChangingSource() {
        if (qEnvironmentVariable("KETPLUS_KETATLAS_CLI").isEmpty())
            QSKIP("Set an isolated KetAtlas 0.5.2 CLI to run the real-process test.");
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto directory = root.filePath("live.ketatlas");
        QVERIFY(QDir().mkpath(directory));
        QVERIFY(write(
            directory + "/atlas.json",
            R"({"version":1,"title":"Isolated live fixture","screens":[{"id":"one","title":"Live","url":"/"}],"flows":[{"id":"main","title":"Main","nodes":[{"id":"one","screen":"one"}],"edges":[]}]})"));
        QVERIFY(write(
            directory + "/atlas.renderer.json",
            R"({"version":1,"framework":"node-http-test","command":["node","renderer.mjs","{port}"],"readyPath":"/","screenBasePath":"/"})"));
        QVERIFY(write(
            directory + "/renderer.mjs",
            R"(import{createServer}from'node:http';import{readFileSync}from'node:fs';const server=createServer((q,r)=>{r.setHeader('content-type','text/html');r.end('<!doctype html><title>Live fixture</title>'+readFileSync('state.txt','utf8'))});server.listen(Number(process.argv[2]),'127.0.0.1');process.on('SIGTERM',()=>server.close(()=>process.exit(0)));)"));
        QVERIFY(write(directory + "/state.txt", "first-live-state"));
        QWebSocketServer relay("runtime-test", QWebSocketServer::NonSecureMode);
        QVERIFY(relay.listen(QHostAddress::LocalHost, 0));
        connect(&relay, &QWebSocketServer::newConnection, this, [&] {
            auto* peer = relay.nextPendingConnection();
            peer->setParent(&relay);
            peer->sendTextMessage("{\"type\":\"ready\",\"version\":1}");
        });
        AtlasDescriptor atlas;
        atlas.path = directory + "/atlas.json";
        atlas.directory = directory;
        atlas.workspaceRoot = root.path();
        atlas.title = "Isolated live fixture";
        QJsonObject first;
        auto* runtime = new AtlasLiveRuntime(this);
        const QUrl url(QString("ws://127.0.0.1:%1/v2/atlas/tunnel").arg(relay.serverPort()));
        runtime->start(atlas, url, "test-only", 1,
                       [&](const QJsonObject& value) { first = value; });
        QTRY_VERIFY_WITH_TIMEOUT(!first.isEmpty(), 30000);
        QVERIFY2(first.value("ok").toBool(), qPrintable(first.value("error_code").toString()));
        const auto origins = first.value("origins").toArray();
        QCOMPARE(origins.size(), 2);
        const auto viewer = quint16(origins[0].toObject().value("port").toInt());
        const auto renderer = quint16(origins[1].toObject().value("port").toInt());
        QVERIFY(get(viewer).contains("KetAtlas"));
        QVERIFY(get(renderer).contains("first-live-state"));
        QVERIFY(write(directory + "/state.txt", "second-live-state"));
        QVERIFY(get(renderer).contains("second-live-state"));
        QJsonObject second;
        runtime->attach(url, "test-only", [&](const QJsonObject& value) { second = value; });
        QVERIFY(second.value("ok").toBool());
        QCOMPARE(second.value("origins"), first.value("origins"));
        QCOMPARE(second.value("context_revision").toInteger(), 1);
        QVERIFY(second.value("channel") != first.value("channel"));
        QVERIFY(second.value("key") != first.value("key"));
        runtime->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!listening(viewer) && !listening(renderer), 5000);
        QVERIFY(relay.isListening());
        delete runtime;
    }
};
QTEST_GUILESS_MAIN(AtlasLiveRuntimeTest)
#include "AtlasLiveRuntimeTest.moc"
