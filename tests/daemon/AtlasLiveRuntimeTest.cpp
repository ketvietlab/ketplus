#include "atlas/AtlasLiveRuntime.h"
#include <QFile>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
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
