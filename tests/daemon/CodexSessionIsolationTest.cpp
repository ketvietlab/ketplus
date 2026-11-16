#include "providers/CodexAdapter.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

namespace ketplus::daemon {
namespace {
bool fixture(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(R"PY(#!/usr/bin/env python3
import json,os,signal,sys,uuid
if '--version' in sys.argv:
    print('codex-cli 0.154.0');sys.exit(0)
root=os.path.dirname(os.path.realpath(__file__))
thread='';epoch='';history=[];turn=''
def note(event,**fields):
    with open(root+'/events.jsonl','a') as log:
        log.write(json.dumps(dict(event=event,pid=os.getpid(),thread=thread,epoch=epoch,**fields))+'\n')
def emit(value):print(json.dumps(value),flush=True)
def reply(id,result):emit(dict(id=id,result=result))
def state():return root+'/'+thread+'.json'
def end(*args):note('exit');sys.exit(0)
signal.signal(signal.SIGTERM,end)
note('started')
for line in sys.stdin:
    msg=json.loads(line);method=msg.get('method');p=msg.get('params',{});id=msg.get('id')
    if method=='initialize':reply(id,{})
    elif method in ('thread/start','thread/resume'):
        epoch=p.get('config',{}).get('mcp_servers.ketplus_agents.env',{}).get('FIXTURE_EPOCH','')
        settings=dict(cwd=p.get('cwd'),model=p.get('model'),sandbox=p.get('sandbox'),approval=p.get('approvalPolicy'))
        if method=='thread/resume':
            thread=p['threadId'];note('resume',**settings)
            if thread=='timeout':continue
            if not os.path.exists(state()):
                emit(dict(id=id,error=dict(code=-32602,message='Fixture history missing')));continue
            with open(state()) as f:history=json.load(f)
        else:
            thread='fixture-'+uuid.uuid4().hex
            with open(state(),'w') as f:json.dump(history,f)
            note('new',**settings)
        reply(id,dict(thread=dict(id=thread)))
    elif method=='mcpServerStatus/list':
        tools={name:dict(name=name) for name in ('agents_whoami','background_task_start','background_task_stop')}
        reply(id,dict(data=[] if epoch=='no-mcp' else [dict(name='ketplus_agents',tools=tools)],nextCursor=None))
    elif method=='turn/start':
        text=p['input'][0]['text'];history.append(text)
        with open(state(),'w') as f:json.dump(history,f)
        turn='turn-'+str(len(history));note('turn',text=text,count=len(history),model=p.get('model'),effort=p.get('effort'))
        reply(id,dict(turn=dict(id=turn)))
        emit(dict(method='turn/started',params=dict(threadId=thread,turn=dict(id=turn))))
        if text=='crash':os._exit(7)
        if text=='ask':
            emit(dict(id='collision',method='item/commandExecution/requestApproval',
                      params=dict(threadId=thread,turnId=turn,command='fixture')))
        if text!='hold':
            emit(dict(method='turn/completed',params=dict(threadId=thread,turn=dict(id=turn,status='completed'))))
    elif method=='turn/interrupt':
        reply(id,{})
        emit(dict(method='turn/completed',params=dict(threadId=thread,turn=dict(id=turn,status='interrupted'))))
    elif method=='thread/read':reply(id,dict(thread=dict(id=thread)))
    elif method is None and id=='collision':note('approval',decision=msg.get('result',{}))
    elif id is not None:reply(id,{})
)PY");
    file.close();
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                           QFileDevice::ExeOwner);
}
QList<QJsonObject> events(const QString& folder) {
    QFile file(folder + "/events.jsonl");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<QJsonObject> result;
    while (!file.atEnd())
        result.append(QJsonDocument::fromJson(file.readLine()).object());
    return result;
}
QJsonObject setup(const QString& folder, const QString& session, const QString& epoch,
                  const QString& thread = {}) {
    return {{"managedSessionId", session},
            {"managedTransportEpoch", epoch},
            {"threadId", thread},
            {"cwd", folder},
            {"approvalPolicy", "never"},
            {"sandbox", "read-only"},
            {"model", "fixture-model"},
            {"config", QJsonObject{{"mcp_servers.ketplus_agents.command", "fixture-only"},
                                   {"mcp_servers.ketplus_agents.env",
                                    QJsonObject{{"FIXTURE_EPOCH", epoch}}}}}};
}
void start(CodexAdapter& adapter, const QString& thread, const QString& text) {
    adapter.call("turn/start",
                 {{"threadId", thread},
                  {"model", "fixture-model"},
                  {"effort", "high"},
                  {"input", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}},
                 [](const QJsonObject&, const QString& error) {
                     QVERIFY2(error.isEmpty(), qPrintable(error));
                 });
}
} // namespace

class CodexSessionIsolationTest final : public QObject {
    Q_OBJECT
  private slots:
    void twoSessionsKeepHistoryWhileOnlyOneProcessRefreshes() {
        QTemporaryDir folder;
        const auto path = folder.filePath("fake-codex");
        QVERIFY(fixture(path));
        CodexAdapter adapter(path);
        QStringList browserArgs{"-c", "fixture.browser=one"};
        adapter.setBrowserArgumentsProvider([&] { return browserArgs; });
        QString a, b;
        adapter.call("thread/start", setup(folder.path(), "A", "a1"),
                     [&](const QJsonObject& result, const QString& error) {
                         QVERIFY2(error.isEmpty(), qPrintable(error));
                         QCOMPARE(result.value("managedTransportEpoch"), "a1");
                         a = result.value("thread").toObject().value("id").toString();
                     });
        QTRY_VERIFY(!a.isEmpty());
        QCOMPARE(adapter.launchedBrowserArguments(a), browserArgs);
        QSignalSpy notifications(&adapter, &CodexAdapter::notificationReceived);
        start(adapter, a, "complete");
        QTRY_VERIFY(
            std::any_of(notifications.begin(), notifications.end(),
                        [](const QList<QVariant>& e) { return e.at(0) == "turn/completed"; }));
        browserArgs.last() = "fixture.browser=two";
        adapter.call("thread/start", setup(folder.path(), "B", "b1"),
                     [&](const QJsonObject& result, const QString& error) {
                         QVERIFY2(error.isEmpty(), qPrintable(error));
                         b = result.value("thread").toObject().value("id").toString();
                     });
        QTRY_VERIFY(!b.isEmpty());
        QCOMPARE(adapter.launchedBrowserArguments(b), browserArgs);
        QCOMPARE(adapter.launchedBrowserArguments(a).last(), "fixture.browser=one");
        start(adapter, b, "hold");
        QTRY_VERIFY(!adapter.sessionWorkers_.value("B").adapter->isIdle());
        bool resumed = false;
        adapter.call("thread/resume", setup(folder.path(), "A", "a2", a),
                     [&](const QJsonObject& result, const QString& error) {
                         QVERIFY2(error.isEmpty(), qPrintable(error));
                         QCOMPARE(result.value("thread").toObject().value("id"), a);
                         QCOMPARE(result.value("managedTransportEpoch"), "a2");
                         resumed = true;
                     });
        QTRY_VERIFY(resumed);
        start(adapter, a, "again");
        QTRY_VERIFY([&] {
            const auto log = events(folder.path());
            return std::any_of(log.begin(), log.end(), [](const QJsonObject& e) {
                return e.value("epoch") == "a2" && e.value("count") == 2;
            });
        }());
        const auto log = events(folder.path());
        int exited = -1, loaded = -1;
        for (int i = 0; i < log.size(); ++i) {
            const auto e = log.at(i);
            if (e.value("epoch") == "a1" && e.value("event") == "exit")
                exited = i;
            if (e.value("epoch") == "a2" && e.value("event") == "resume")
                loaded = i;
            if (e.value("epoch") == "a2" && e.value("event") == "resume") {
                QCOMPARE(e.value("cwd"), folder.path());
                QCOMPARE(e.value("model"), "fixture-model");
                QCOMPARE(e.value("sandbox"), "read-only");
                QCOMPARE(e.value("approval"), "never");
            }
            if (e.value("epoch") == "a2" && e.value("event") == "turn") {
                QCOMPARE(e.value("model"), "fixture-model");
                QCOMPARE(e.value("effort"), "high");
            }
            QVERIFY(e.value("epoch") != "b1" || e.value("event") != "exit");
        }
        QVERIFY(exited >= 0 && loaded > exited);
        QTRY_VERIFY(adapter.sessionWorkers_.value("A").adapter->isIdle());
        QVERIFY(!adapter.releaseSessionTransport("B"));
        QVERIFY(adapter.releaseSessionTransport("A"));
        QVERIFY(!adapter.ownsIsolatedThread(a));
        QVERIFY(adapter.ownsIsolatedThread(b));
        QVERIFY(!adapter.sessionWorkers_.value("B").adapter->isIdle());
        adapter.stop();
    }

    void missingHistoryOrMcpNeverAcknowledgesAnEpoch_data() {
        QTest::addColumn<QString>("thread");
        QTest::addColumn<QString>("epoch");
        QTest::newRow("missing-history") << "missing" << "fresh";
        QTest::newRow("load-timeout") << "timeout" << "fresh";
        QTest::newRow("mcp-timeout") << "" << "no-mcp";
    }

    void oldApprovalCannotAnswerReplacementOrAnotherSession() {
        QTemporaryDir folder;
        const auto path = folder.filePath("fake-codex");
        QVERIFY(fixture(path));
        CodexAdapter adapter(path);
        adapter.setBrowserArgumentsProvider([] { return QStringList{}; });
        auto open = [&](const QString& session, const QString& epoch, QString& thread, bool& done) {
            adapter.call(thread.isEmpty() ? "thread/start" : "thread/resume",
                         setup(folder.path(), session, epoch, thread),
                         [&](const QJsonObject& result, const QString& error) {
                             QVERIFY2(error.isEmpty(), qPrintable(error));
                             thread = result.value("thread").toObject().value("id").toString();
                             done = true;
                         });
        };
        QString a, b;
        bool ready = false;
        open("A", "a1", a, ready);
        QTRY_VERIFY(ready);
        QSignalSpy requests(&adapter, &ProviderAdapter::requestReceived);
        start(adapter, a, "ask");
        QTRY_COMPARE(requests.size(), 1);
        const auto oldId = requests.at(0).at(0).value<QJsonValue>();
        QTRY_VERIFY(adapter.sessionWorkers_.value("A").adapter->isIdle());
        ready = false;
        open("A", "a2", a, ready);
        QTRY_VERIFY(ready);
        start(adapter, a, "ask");
        QTRY_COMPARE(requests.size(), 2);
        ready = false;
        open("B", "b1", b, ready);
        QTRY_VERIFY(ready);
        start(adapter, b, "ask");
        QTRY_COMPARE(requests.size(), 3);
        const auto aId = requests.at(1).at(0).value<QJsonValue>();
        const auto bId = requests.at(2).at(0).value<QJsonValue>();
        QVERIFY(oldId != aId && aId != bId && oldId != bId);
        QVERIFY(!adapter.ownsIsolatedRequest(oldId.toString()));
        adapter.respond(oldId, {{"decision", "stale"}});
        adapter.respond(aId, {{"decision", "A"}});
        adapter.respond(bId, {{"decision", "B"}});
        QTRY_VERIFY([&] {
            int approvals = 0;
            for (const auto& event : events(folder.path())) {
                if (event.value("event") != "approval")
                    continue;
                ++approvals;
                const auto decision = event.value("decision").toObject().value("decision");
                if (!(event.value("epoch") == "a2"
                          ? decision == "A"
                          : event.value("epoch") == "b1" && decision == "B"))
                    return false;
            }
            return approvals == 2;
        }());
        adapter.stop();
    }

    void crashedWorkerDoesNotStopAnotherConversation() {
        QTemporaryDir folder;
        const auto path = folder.filePath("fake-codex");
        QVERIFY(fixture(path));
        CodexAdapter adapter(path);
        adapter.setBrowserArgumentsProvider([] { return QStringList{}; });
        QString a, b;
        for (const auto& session : {QString("A"), QString("B")}) {
            bool ready = false;
            adapter.call("thread/start", setup(folder.path(), session, session),
                         [&](const QJsonObject& result, const QString& error) {
                             QVERIFY2(error.isEmpty(), qPrintable(error));
                             (session == "A" ? a : b) =
                                 result.value("thread").toObject().value("id").toString();
                             ready = true;
                         });
            QTRY_VERIFY(ready);
        }
        QSignalSpy stopped(&adapter, &ProviderAdapter::stopped);
        QSignalSpy notifications(&adapter, &CodexAdapter::notificationReceived);
        start(adapter, b, "hold");
        start(adapter, a, "crash");
        QTRY_VERIFY(std::any_of(
            notifications.begin(), notifications.end(), [&](const QList<QVariant>& event) {
                return event.at(0) == "error" && event.at(1).toJsonObject().value("threadId") == a;
            }));
        QCOMPARE(stopped.size(), 0);
        QVERIFY(adapter.sessionWorkers_.value("B").adapter->isReady());
        QVERIFY(!adapter.sessionWorkers_.value("B").adapter->isIdle());
        for (const auto& event : events(folder.path()))
            QVERIFY(event.value("epoch") != "B" || event.value("event") != "exit");
        adapter.stop();
    }
    void missingHistoryOrMcpNeverAcknowledgesAnEpoch() {
        QFETCH(QString, thread);
        QFETCH(QString, epoch);
        QTemporaryDir folder;
        const auto path = folder.filePath("fake-codex");
        QVERIFY(fixture(path));
        CodexAdapter adapter(path);
        adapter.originRefreshTimeoutMs_ = 1000;
        adapter.setBrowserArgumentsProvider([] { return QStringList{}; });
        bool done = false;
        adapter.call(thread.isEmpty() ? "thread/start" : "thread/resume",
                     setup(folder.path(), "A", epoch, thread),
                     [&](const QJsonObject& result, const QString& error) {
                         QVERIFY(result.isEmpty());
                         QVERIFY2(error.contains("mutation_refresh_unavailable"),
                                  qPrintable(error));
                         done = true;
                     });
        QTRY_VERIFY(done);
        for (const auto& e : events(folder.path())) {
            QVERIFY(thread.isEmpty() || e.value("event") != "new");
            QVERIFY(e.value("event") != "turn");
        }
        adapter.stop();
    }
};
} // namespace ketplus::daemon
QTEST_GUILESS_MAIN(ketplus::daemon::CodexSessionIsolationTest)
#include "CodexSessionIsolationTest.moc"
