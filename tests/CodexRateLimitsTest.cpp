#include "agent/CodexRateLimits.h"
#include "agent/CodexAppServer.h"
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QtTest>

using namespace ketplus;
class CodexRateLimitsTest : public QObject {
    Q_OBJECT
    static QJsonObject payload(const QString& id = "codex", int duration = 10080) {
        return {{"rateLimits", QJsonObject{{"limitId", id}, {"secondary", QJsonObject{
            {"usedPercent", 50}, {"windowDurationMins", duration},
            {"resetsAt", QDateTime::currentSecsSinceEpoch() + 3600}}}}}};
    }
private slots:
    void readsThroughExistingServer() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile script(dir.filePath("codex"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/env python3
import json, sys, time
reads = 0
for line in sys.stdin:
    msg = json.loads(line)
    if msg.get('method') == 'initialize':
        print(json.dumps({'id': msg['id'], 'result': {}}), flush=True)
    elif msg.get('method') == 'account/rateLimits/read':
        reads += 1
        if reads == 1:
            result = {'rateLimits': {'limitId': 'codex', 'secondary': {'usedPercent': 42, 'windowDurationMins': 10080, 'resetsAt': int(time.time()) + 3600}}}
            print(json.dumps({'id': msg['id'], 'result': result}), flush=True)
        else:
            print(json.dumps({'id': msg['id'], 'error': {'code': -1, 'message': 'unavailable'}}), flush=True)
)PY");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        CodexAppServer server(script.fileName());
        CodexRateLimits limits(&server);
        limits.refresh();
        QVERIFY(limits.weeklyLimit().value("loading").toBool());
        QTRY_VERIFY(limits.weeklyLimit().value("available").toBool());
        QCOMPARE(limits.weeklyLimit().value("usedPercent").toInt(), 42);
        limits.refresh();
        QTRY_VERIFY(!limits.weeklyLimit().value("loading").toBool());
        QVERIFY(!limits.weeklyLimit().value("available").toBool());
        server.stop();
    }
    void parsesWeeklyWindow() {
        const auto result = CodexRateLimits::parseWeeklyLimit(payload());
        QVERIFY(result.value("available").toBool());
        QCOMPARE(result.value("usedPercent").toInt(), 50);
        QVERIFY(!CodexRateLimits::parseWeeklyLimit(payload("codex", 300)).value("available").toBool());
        QVERIFY(!CodexRateLimits::parseWeeklyLimit(payload("other")).value("available").toBool());
        QVERIFY(!CodexRateLimits::parseWeeklyLimit({}).value("available").toBool());
        auto bucket = payload().value("rateLimits").toObject();
        bucket.insert("primary", bucket.take("secondary"));
        QVERIFY(CodexRateLimits::parseWeeklyLimit({{"rateLimitsByLimitId", QJsonObject{{"codex", bucket}}}}).value("available").toBool());
        auto window = bucket.value("primary").toObject();
        window.insert("resetsAt", 1);
        bucket.insert("primary", window);
        QVERIFY(!CodexRateLimits::parseWeeklyLimit({{"rateLimits", bucket}}).value("available").toBool());
        window.insert("resetsAt", QJsonValue::Null);
        window.insert("usedPercent", QJsonValue::Null);
        bucket.insert("primary", window);
        QVERIFY(!CodexRateLimits::parseWeeklyLimit({{"rateLimits", bucket}}).value("available").toBool());
    }
    void notificationsAndAccountChanges() {
        CodexAppServer server("/nonexistent/ketplus-test-codex");
        CodexRateLimits limits(&server);
        QSignalSpy changed(&limits, &CodexRateLimits::weeklyLimitChanged);
        server.notificationReceived("account/rateLimits/updated", payload());
        QCOMPARE(limits.weeklyLimit().value("usedPercent").toInt(), 50);
        server.notificationReceived("account/rateLimits/updated", payload("other"));
        QCOMPARE(changed.count(), 1);
        server.notificationReceived("account/updated", {{"authMode", "apikey"}});
        QVERIFY(!limits.weeklyLimit().value("available").toBool());
        server.notificationReceived("account/rateLimits/updated", payload());
        QVERIFY(!limits.weeklyLimit().value("available").toBool());
        server.notificationReceived("account/updated", {{"authMode", "chatgpt"}});
        server.notificationReceived("account/rateLimits/updated", payload());
        QVERIFY(limits.weeklyLimit().value("available").toBool());
        server.serverStopped("offline");
        QVERIFY(!limits.weeklyLimit().value("available").toBool());
    }
    void unavailableExecutable() {
        CodexAppServer server("/nonexistent/ketplus-test-codex");
        CodexRateLimits limits(&server);
        limits.refresh();
        QTRY_VERIFY(!limits.weeklyLimit().value("loading").toBool());
        QVERIFY(!limits.weeklyLimit().value("available").toBool());
    }
};
QTEST_GUILESS_MAIN(CodexRateLimitsTest)
#include "CodexRateLimitsTest.moc"
