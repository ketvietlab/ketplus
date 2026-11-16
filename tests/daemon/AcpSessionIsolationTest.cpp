#include "providers/acp/AcpAdapter.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

using namespace ketplus::daemon;
namespace {
class FixtureAdapter final : public AcpAdapter {
  public:
    explicit FixtureAdapter(QString path)
        : AcpAdapter("Isolation fixture"), path_(std::move(path)) {}
    QString name() const override { return "fixture"; }
    QString executable() const override { return path_; }
    QString missingMessage() const override { return "Fixture missing"; }
    bool isAvailable() const override { return true; }
    bool supports(const QString&) const override { return true; }
    void runtimeStatus(std::function<void(const compat::Runtime&)> done) override { done({}); }
    void configurationCatalog(Callback done) override { done({}, {}); }

  protected:
    AcpAdapter* createSessionWorker() override { return new FixtureAdapter(path_); }
    void configureProcess() override { process_->setProgram(path_); }
    int requestTimeoutMs(const QString& method) const override {
        return method == "initialize" || method == "session/load" ? 500 : 0;
    }

  private:
    QString path_;
};

bool writeFixture(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(R"PY(#!/usr/bin/env python3
import json, os, signal, sys, uuid
root = os.path.dirname(os.path.realpath(__file__))
log_path = root + '/events.jsonl'
sid = None
epoch = ''
history = []
pending = None
def note(event, **fields):
    with open(log_path, 'a') as log:
        log.write(json.dumps(dict(event=event, pid=os.getpid(), sid=sid, epoch=epoch, **fields))+'\n')
def reply(id, result):
    print(json.dumps(dict(jsonrpc='2.0', id=id, result=result)), flush=True)
def error(id):
    print(json.dumps(dict(jsonrpc='2.0', id=id, error=dict(code=-32602, message='Fixture load refused'))), flush=True)
def state(): return root + '/' + sid + '.json'
def save():
    with open(state(), 'w') as file: json.dump(history, file)
note('started')
def end(*args):
    note('exit')
    sys.exit(0)
signal.signal(signal.SIGTERM, end)
for line in sys.stdin:
    msg=json.loads(line); method=msg.get('method'); params=msg.get('params', {})
    if method == 'initialize':
        reply(msg['id'], dict(protocolVersion=1, agentCapabilities=dict(loadSession=True)))
    elif method in ('session/new', 'session/load'):
        servers=params.get('mcpServers', [])
        epoch=next((e['value'] for s in servers for e in s.get('env', []) if e['name']=='FIXTURE_EPOCH'), '')
        if method == 'session/load':
            sid=params['sessionId']
            note('load')
            if sid == 'timeout': continue
            if not os.path.exists(state()): error(msg['id']); continue
            with open(state()) as file: history=json.load(file)
            reply(msg['id'], {})
        else:
            sid='fixture-'+uuid.uuid4().hex
            save(); note('new'); reply(msg['id'], dict(sessionId=sid))
    elif method == 'session/prompt':
        text=params['prompt'][-1]['text'];history.append(text);save()
        note('prompt', text=text, count=len(history))
        if text == 'crash': os._exit(7)
        if text == 'hold': pending=msg['id'];continue
        if text == 'ask':
            print(json.dumps(dict(jsonrpc='2.0', id='collision', method='fixture/request',
                                  params=dict(sessionId=sid))), flush=True)
        reply(msg['id'], dict(stopReason='end_turn'))
    elif method == 'session/cancel':
        if pending is not None: reply(pending, dict(stopReason='cancelled'));pending=None
    elif method == 'session/set_mode': reply(msg['id'], {})
    elif method is None and msg.get('id') == 'collision':
        note('answer', result=msg.get('result'))
)PY");
    file.close();
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                           QFileDevice::ExeOwner);
}

QJsonObject start(const QString& session, const QString& cwd, const QString& epoch,
                  const QString& prompt = "complete", const QString& resume = {}) {
    return {{"sessionId", session},
            {"cwd", cwd},
            {"managedTransportEpoch", epoch},
            {"resume", resume},
            {"prompt", prompt},
            {"permission", "workspace-write"},
            {"agentMcpServer", QJsonObject{{"name", "ketplus_agents"},
                                           {"command", "fixture-only"},
                                           {"args", QJsonArray{}},
                                           {"env", QJsonArray{QJsonObject{{"name", "FIXTURE_EPOCH"},
                                                                          {"value", epoch}}}}}}};
}
QList<QJsonObject> events(const QString& directory) {
    QFile file(directory + "/events.jsonl");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<QJsonObject> result;
    while (!file.atEnd())
        result.append(QJsonDocument::fromJson(file.readLine()).object());
    return result;
}
bool hasHistory(const QString& directory, const QString& epoch, int count) {
    const auto log = events(directory);
    return std::any_of(log.begin(), log.end(), [&](const QJsonObject& e) {
        return e.value("epoch") == epoch && e.value("event") == "prompt" &&
               e.value("count") == count;
    });
}
} // namespace

class AcpSessionIsolationTest final : public QObject {
    Q_OBJECT
  private slots:
    void twoSessionsKeepHistoryAndIndependentProcesses() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("fixture.py");
        QVERIFY(writeFixture(path));
        FixtureAdapter adapter(path);
        QSignalSpy eventSpy(&adapter, &ProviderAdapter::canonicalEvent);
        QString threadA;
        bool completed = false;
        adapter.call("turn/start", start("A", directory.path(), "a1"),
                     [&](const QJsonObject& result, const QString& error) {
                         QVERIFY2(error.isEmpty(), qPrintable(error));
                         QCOMPARE(result.value("managedTransportEpoch"), "a1");
                         threadA = result.value("thread").toObject().value("id").toString();
                         completed = true;
                     });
        QTRY_VERIFY(completed);
        QTRY_VERIFY(std::any_of(eventSpy.begin(), eventSpy.end(), [](const QList<QVariant>& args) {
            return args.at(1) == "turn.completed";
        }));
        completed = false;
        adapter.call("turn/start", start("B", directory.path(), "b1", "hold"),
                     [&](const QJsonObject&, const QString& error) {
                         QVERIFY2(error.isEmpty(), qPrintable(error));
                         completed = true;
                     });
        QTRY_VERIFY(completed);
        completed = false;
        adapter.call("turn/start", start("A", directory.path(), "a2", "complete", threadA),
                     [&](const QJsonObject& result, const QString& error) {
                         QVERIFY2(error.isEmpty(), qPrintable(error));
                         QCOMPARE(result.value("thread").toObject().value("id"), threadA);
                         QCOMPARE(result.value("managedTransportEpoch"), "a2");
                         completed = true;
                     });
        QTRY_VERIFY(completed);
        QTRY_VERIFY(hasHistory(directory.path(), "a2", 2));
        const auto log = events(directory.path());
        int a1Exit = -1, a2Start = -1;
        qint64 a1Pid = 0, a2Pid = 0, bPid = 0;
        for (int i = 0; i < log.size(); ++i) {
            const auto e = log.at(i);
            if (e.value("epoch") == "a1" && e.value("event") == "exit")
                a1Exit = i;
            if (e.value("epoch") == "a1" && e.value("event") == "prompt")
                a1Pid = e.value("pid").toInteger();
            if (e.value("epoch") == "a2" && e.value("event") == "load") {
                a2Start = i;
                a2Pid = e.value("pid").toInteger();
            }
            if (e.value("epoch") == "b1") {
                bPid = e.value("pid").toInteger();
                QVERIFY(e.value("event") != "exit");
            }
        }
        QVERIFY(a1Exit >= 0 && a2Start > a1Exit);
        QVERIFY(a1Pid > 0 && a2Pid > 0 && bPid > 0);
        QVERIFY(a1Pid != a2Pid && a2Pid != bPid);
        QVERIFY(!adapter.releaseSessionTransport("B"));
        QTRY_VERIFY(adapter.releaseSessionTransport("A"));
        QVERIFY(!adapter.ownsIsolatedSession("A"));
        QVERIFY(adapter.ownsIsolatedSession("B"));
        bool stopped = false;
        adapter.call("turn/interrupt", {{"sessionId", "B"}},
                     [&](const QJsonObject&, const QString&) { stopped = true; });
        QTRY_VERIFY(stopped);
        adapter.stop();
    }

    void failedLoadNeverCreatesNewSession_data() {
        QTest::addColumn<QString>("resume");
        QTest::newRow("refused") << "missing";
        QTest::newRow("timeout") << "timeout";
    }
    void failedLoadNeverCreatesNewSession() {
        QFETCH(QString, resume);
        QTemporaryDir directory;
        const auto path = directory.filePath("fixture.py");
        QVERIFY(writeFixture(path));
        FixtureAdapter adapter(path);
        bool completed = false;
        QString failure;
        adapter.call("turn/start", start("A", directory.path(), "a1", "complete", resume),
                     [&](const QJsonObject& result, const QString& error) {
                         QVERIFY(result.isEmpty());
                         failure = error;
                         completed = true;
                     });
        QTRY_VERIFY(completed);
        QVERIFY2(failure.contains("mutation_refresh_unavailable"), qPrintable(failure));
        for (const auto& e : events(directory.path())) {
            QVERIFY(e.value("event") != "new");
            QVERIFY(e.value("event") != "prompt");
        }
        adapter.stop();
    }

    void workerCrashDoesNotStopAnotherSession() {
        QTemporaryDir directory;
        const auto path = directory.filePath("fixture.py");
        QVERIFY(writeFixture(path));
        FixtureAdapter adapter(path);
        QSignalSpy stoppedSpy(&adapter, &ProviderAdapter::stopped);
        QSignalSpy eventSpy(&adapter, &ProviderAdapter::canonicalEvent);
        bool ready = false;
        adapter.call("turn/start", start("B", directory.path(), "b1", "hold"),
                     [&](const QJsonObject&, const QString& error) {
                         QVERIFY2(error.isEmpty(), qPrintable(error));
                         ready = true;
                     });
        QTRY_VERIFY(ready);
        ready = false;
        adapter.call("turn/start", start("A", directory.path(), "a1", "crash"),
                     [&](const QJsonObject&, const QString& error) {
                         QVERIFY2(error.isEmpty(), qPrintable(error));
                         ready = true;
                     });
        QTRY_VERIFY(ready);
        QTRY_VERIFY(std::any_of(eventSpy.begin(), eventSpy.end(), [](const QList<QVariant>& args) {
            return args.at(0) == "A" && args.at(1) == "turn.completed";
        }));
        QCOMPARE(stoppedSpy.size(), 0);
        for (const auto& args : eventSpy)
            QVERIFY(args.at(0) != "B" || args.at(1) != "turn.completed");
        for (const auto& e : events(directory.path()))
            QVERIFY(e.value("epoch") != "b1" || e.value("event") != "exit");
        bool interrupted = false;
        adapter.call("turn/interrupt", {{"sessionId", "B"}},
                     [&](const QJsonObject&, const QString& error) {
                         QVERIFY2(error.isEmpty(), qPrintable(error));
                         interrupted = true;
                     });
        QTRY_VERIFY(interrupted);
        adapter.stop();
    }

    void staleReplyCannotReachReplacementWithReusedWireId() {
        QTemporaryDir directory;
        const auto path = directory.filePath("fixture.py");
        QVERIFY(writeFixture(path));
        FixtureAdapter adapter(path);
        QSignalSpy requests(&adapter, &ProviderAdapter::requestReceived);
        QSignalSpy notifications(&adapter, &ProviderAdapter::canonicalEvent);
        QString history;
        for (const auto& epoch : {QString("a1"), QString("a2")}) {
            bool ready = false;
            adapter.call("turn/start", start("A", directory.path(), epoch, "ask", history),
                         [&](const QJsonObject& result, const QString& error) {
                             QVERIFY2(error.isEmpty(), qPrintable(error));
                             history = result.value("thread").toObject().value("id").toString();
                             ready = true;
                         });
            QTRY_VERIFY(ready);
            const auto count = epoch == "a1" ? 1 : 2;
            QTRY_COMPARE(requests.size(), count);
            QTRY_COMPARE(std::count_if(notifications.begin(), notifications.end(),
                                       [](const QList<QVariant>& event) {
                                           return event.at(1) == "turn.completed";
                                       }),
                         count);
        }
        const auto oldId = requests.at(0).at(0).value<QJsonValue>();
        const auto newId = requests.at(1).at(0).value<QJsonValue>();
        QVERIFY(oldId != newId);
        adapter.respond(oldId, {{"outcome", QJsonObject{{"optionId", "stale"}}}});
        adapter.respond(newId, {{"outcome", QJsonObject{{"optionId", "current"}}}});
        QTRY_VERIFY([&] {
            int count = 0;
            for (const auto& event : events(directory.path())) {
                if (event.value("event") != "answer")
                    continue;
                ++count;
                if (event.value("epoch") != "a2" ||
                    event.value("result").toObject().value("outcome").toObject().value(
                        "optionId") != "current")
                    return false;
            }
            return count == 1;
        }());
        adapter.stop();
    }
};
QTEST_GUILESS_MAIN(AcpSessionIsolationTest)
#include "AcpSessionIsolationTest.moc"
