#include "providers/acp/AcpAdapter.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

using namespace ketplus::daemon;

namespace {
// Deliberately has no provider-specific model, discovery or error overrides.
class FixtureAdapter final : public AcpAdapter {
  public:
    explicit FixtureAdapter(QString path) : AcpAdapter("Fixture"), path_(std::move(path)) {}
    QString name() const override { return "fixture"; }
    QString executable() const override { return path_; }
    QString missingMessage() const override { return "Fixture missing"; }
    bool isAvailable() const override { return !path_.isEmpty(); }
    bool supports(const QString&) const override { return true; }
    void runtimeStatus(std::function<void(const compat::Runtime&)> done) override { done({}); }
    void configurationCatalog(Callback done) override { done({}, {}); }
    int backoff() const { return restartDelayMs_; }

  protected:
    void configureProcess() override {
        process_->setProgram(path_);
        process_->setArguments({});
    }

  private:
    QString path_;
};

bool writeFixture(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(R"PY(#!/usr/bin/env python3
import json, sys
calls = []
pending = None

def emit(value):
    print(json.dumps(value), flush=True)

def reply(id, result):
    emit({'jsonrpc':'2.0', 'id':id, 'result':result})

def update(update_kind, **fields):
    emit({'jsonrpc':'2.0', 'method':'session/update', 'params':{
        'sessionId':'acp-session', 'update':{'sessionUpdate':update_kind, **fields}}})

for line in sys.stdin:
    msg = json.loads(line)
    method = msg.get('method')
    params = msg.get('params', {})
    calls.append(msg)
    if method == 'initialize':
        reply(msg['id'], {'protocolVersion':1})
    elif method == 'session/new':
        reply(msg['id'], {'sessionId':'acp-session'})
    elif method == 'session/load':
        if params['sessionId'] == 'missing':
            emit({'jsonrpc':'2.0','id':msg['id'],'error':{'code':-1,'message':'No session'}})
        else:
            reply(msg['id'], {})
    elif method == 'session/set_mode':
        reply(msg['id'], {})
    elif method == 'fixture/log':
        reply(msg['id'], {'calls':calls})
    elif method == 'fixture/malformed':
        print('not-json', flush=True)
    elif method == 'fixture/hang':
        pass
    elif method == 'session/prompt':
        pending = msg['id']
        text = params['prompt'][-1].get('text', '')
        if text == 'hold':
            continue
        update('agent_message_chunk', content={'type':'text','text':'hello'})
        update('tool_call', toolCallId='tool', kind='execute', status='in_progress', rawInput={'command':'echo hi'})
        update('tool_call_update', toolCallId='tool', status='completed', content=[{'type':'content','content':{'type':'text','text':'hi'}}])
        update('usage_update', used=12, size=100)
        emit({'jsonrpc':'2.0', 'id':'permission', 'method':'session/request_permission',
              'params':{'sessionId':'acp-session','toolCall':{'title':'Run','rawInput':{'command':'echo hi'}},
                        'options':[{'optionId':'yes-custom','kind':'allow_once'},{'optionId':'no-custom','kind':'reject_once'}]}})
    elif method == 'session/cancel':
        reply(pending, {'stopReason':'cancelled'})
        pending = None
    elif method is None and msg.get('id') == 'permission':
        outcome = msg.get('result', {}).get('outcome', {})
        update('agent_message_chunk', content={'type':'text','text':outcome.get('optionId', 'cancelled')})
        reply(pending, {'stopReason':'end_turn'})
        pending = None
)PY");
    file.close();
    return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

QJsonObject turnParams(const QString& prompt = "run") {
    return {{"sessionId", "host-session"},
            {"cwd", QDir::tempPath()},
            {"prompt", prompt},
            {"permission", "workspace-write"},
            {"agentMcpServer", QJsonObject{{"name", "ketplus_agents"},
                                           {"command", "/fixture/mcp"},
                                           {"args", QJsonArray{}},
                                           {"env", QJsonArray{}}}}};
}

QJsonObject call(FixtureAdapter& adapter, const QString& method, const QJsonObject& params = {}) {
    bool done = false;
    QJsonObject result;
    QString error;
    adapter.call(method, params, [&](const QJsonObject& value, const QString& failure) {
        result = value;
        error = failure;
        done = true;
    });
    if (!QTest::qWaitFor([&] { return done; }, 5000)) {
        QTest::qFail("Fixture call did not settle", __FILE__, __LINE__);
        adapter.stop();
        return {};
    }
    if (!error.isEmpty())
        QTest::qFail(qPrintable(error), __FILE__, __LINE__);
    return result;
}

bool containsEvent(const QSignalSpy& events, const QString& type) {
    return std::any_of(events.begin(), events.end(),
                       [&](const QList<QVariant>& event) { return event[1].toString() == type; });
}
} // namespace

class AcpAdapterTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
#ifdef Q_OS_WIN
        QSKIP("POSIX executable fixture");
#endif
    }
    void normalizesTranscriptAndPermission_data() {
        QTest::addColumn<bool>("allow");
        QTest::newRow("accept") << true;
        QTest::newRow("decline") << false;
    }
    void normalizesTranscriptAndPermission() {
        QFETCH(bool, allow);
        QTemporaryDir dir;
        QVERIFY(writeFixture(dir.filePath("acp")));
        FixtureAdapter adapter(dir.filePath("acp"));
        QSignalSpy events(&adapter, &ProviderAdapter::canonicalEvent);
        QSignalSpy requests(&adapter, &ProviderAdapter::requestReceived);
        const auto result = call(adapter, "turn/start", turnParams());
        QCOMPARE(result["thread"].toObject()["id"].toString(), "acp-session");
        QTRY_COMPARE(requests.size(), 1);
        QCOMPARE(requests[0][0].toString(), "permission");
        QCOMPARE(requests[0][1].toString(), "permission.request");
        QCOMPARE(requests[0][2].toJsonObject()["sessionId"].toString(), "host-session");
        adapter.respond(requests[0][0].toJsonValue(), {{"allow", allow}});
        QTRY_VERIFY(containsEvent(events, "turn.completed"));
        QVERIFY(containsEvent(events, "item.started"));
        QVERIFY(containsEvent(events, "usage.context"));
        bool final = false, tool = false;
        for (const auto& event : events) {
            const auto payload = event[2].toJsonObject();
            if (event[1] == "item.completed" && payload["item_id"] == "tool") {
                tool = true;
                QCOMPARE(payload["item"].toObject()["command"].toString(), "echo hi");
            }
            if (event[1] == "item.completed" && payload["item_type"] == "agentMessage") {
                final = true;
                QCOMPARE(payload["text"].toString(), allow ? "helloyes-custom" : "hellono-custom");
            }
        }
        QVERIFY(final);
        QVERIFY(tool);
        const auto calls = call(adapter, "fixture/log")["calls"].toArray();
        QCOMPARE(calls[0].toObject()["method"].toString(), "initialize");
        const auto session = calls[1].toObject()["params"].toObject();
        QCOMPARE(session["mcpServers"].toArray().last().toObject()["name"].toString(),
                 "ketplus_agents");
        QVERIFY(!session["cwd"].toString().isEmpty());
    }
    void loadsOrFallsBackAndCancels_data() {
        QTest::addColumn<QString>("resume");
        QTest::newRow("empty-load-response") << "acp-session";
        QTest::newRow("load-fallback") << "missing";
    }
    void loadsOrFallsBackAndCancels() {
        QFETCH(QString, resume);
        QTemporaryDir dir;
        QVERIFY(writeFixture(dir.filePath("acp")));
        FixtureAdapter adapter(dir.filePath("acp"));
        QSignalSpy events(&adapter, &ProviderAdapter::canonicalEvent);
        auto params = turnParams("hold");
        params.insert("resume", resume);
        QCOMPARE(call(adapter, "turn/start", params)["thread"].toObject()["id"].toString(),
                 "acp-session");
        call(adapter, "turn/interrupt", {{"sessionId", "host-session"}});
        QTRY_VERIFY(containsEvent(events, "turn.completed"));
        QCOMPARE(events.last()[2].toJsonObject()["status"].toString(), "interrupted");
        const auto calls = call(adapter, "fixture/log")["calls"].toArray();
        QStringList methods;
        for (const auto& value : calls)
            methods.append(value.toObject()["method"].toString());
        QVERIFY(methods.contains("session/load"));
        QCOMPARE(methods.contains("session/new"), resume == "missing");
        QVERIFY(methods.contains("session/cancel"));
    }
    void stopsPendingCallsAndRestarts() {
        QTemporaryDir dir;
        QVERIFY(writeFixture(dir.filePath("acp")));
        FixtureAdapter adapter(dir.filePath("acp"));
        call(adapter, "fixture/log");
        int settled = 0;
        adapter.call("fixture/hang", {}, [&](const QJsonObject&, const QString& error) {
            ++settled;
            QVERIFY(error.contains("Fixture ACP process was stopped"));
        });
        adapter.stop();
        QCOMPARE(settled, 1);
        QVERIFY(!adapter.isReady());
        const auto log = call(adapter, "fixture/log")["calls"].toArray();
        QCOMPARE(log.first().toObject()["method"].toString(), "initialize");
        QCOMPARE(settled, 1);
    }
    void quarantinesMalformedOutput() {
        QTemporaryDir dir;
        QVERIFY(writeFixture(dir.filePath("acp")));
        FixtureAdapter adapter(dir.filePath("acp"));
        call(adapter, "fixture/log");
        QSignalSpy stops(&adapter, &ProviderAdapter::stopped);
        int settled = 0;
        adapter.call("fixture/malformed", {}, [&](const QJsonObject&, const QString& error) {
            ++settled;
            QVERIFY(error.contains("malformed protocol message"));
        });
        QTRY_COMPARE(stops.size(), 1);
        QCOMPARE(settled, 1);
        QCOMPARE(adapter.backoff(), 1000);
        QVERIFY(!adapter.isReady());
        call(adapter, "fixture/log");
        QVERIFY(adapter.isReady());
        QCOMPARE(stops.size(), 1);
    }
};
QTEST_GUILESS_MAIN(AcpAdapterTest)
#include "AcpAdapterTest.moc"
