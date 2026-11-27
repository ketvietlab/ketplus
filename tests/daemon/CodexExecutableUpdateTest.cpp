#include "ScriptFixture.h"
#include "providers/CodexAdapter.h"

#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <optional>

namespace ketplus::daemon {
namespace {
// A Codex app-server whose model catalog follows its version, as the real one's does.
bool writeFixture(const QString& path, const QByteArray& model) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QByteArray script = R"PY(#!/usr/bin/env python3
import json,signal,sys
MODEL='@MODEL@'
if '--version' in sys.argv:
    print('codex-cli 0.170.0');sys.exit(0)
signal.signal(signal.SIGTERM,lambda *a:sys.exit(0))
def emit(value):print(json.dumps(value),flush=True)
for line in sys.stdin:
    msg=json.loads(line);method=msg.get('method');p=msg.get('params',{});id=msg.get('id')
    if method=='model/list':emit(dict(id=id,result=dict(data=[dict(model=MODEL)],nextCursor=None)))
    elif method=='turn/start':
        emit(dict(id=id,result=dict(turn=dict(id='turn-1'))))
        emit(dict(method='turn/started',params=dict(threadId=p['threadId'],turn=dict(id='turn-1'))))
    elif method=='turn/interrupt':
        emit(dict(id=id,result={}))
        emit(dict(method='turn/completed',params=dict(threadId=p['threadId'],turn=dict(id='turn-1',status='interrupted'))))
    elif id is not None:emit(dict(id=id,result={}))
)PY";
    file.write(script.replace("@MODEL@", model));
    return finalizeScriptFixture(file);
}

std::optional<QString> listModel(CodexAdapter& adapter) {
    std::optional<QString> model;
    adapter.call(QStringLiteral("model/list"), {},
                 [&model](const QJsonObject& result, const QString& error) {
                     model = error.isEmpty() ? result.value(QStringLiteral("data"))
                                                   .toArray()
                                                   .first()
                                                   .toObject()
                                                   .value(QStringLiteral("model"))
                                                   .toString()
                                             : QStringLiteral("error: ") + error;
                 });
    if (!QTest::qWaitFor([&model] { return model.has_value(); }, 10000))
        return std::nullopt;
    return model;
}

void callAndWait(CodexAdapter& adapter, const QString& method, const QJsonObject& params) {
    bool done = false;
    adapter.call(method, params, [&done](const QJsonObject&, const QString&) { done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
}
} // namespace

class CodexExecutableUpdateTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
#if defined(Q_OS_WIN)
        // The fixture launcher is a fixed copy there; rewriting the script beside it does not
        // change the executable the fingerprint reads.
        QSKIP("An in-place script rewrite does not model a CLI update on Windows.");
#endif
    }

    void idleProviderMovesToTheUpdatedCliBeforeListingModels() {
        QTemporaryDir folder;
        const QString path = folder.filePath(fixtureName(QStringLiteral("codex")));
        QVERIFY(writeFixture(path, "gpt-old"));
        CodexAdapter adapter(path);
        QSignalSpy reloaded(&adapter, &CodexAdapter::reloaded);

        QCOMPARE(listModel(adapter), std::optional<QString>(QStringLiteral("gpt-old")));
        QCOMPARE(listModel(adapter), std::optional<QString>(QStringLiteral("gpt-old")));
        QCOMPARE(reloaded.count(), 0); // An unchanged CLI keeps its process.

        QVERIFY(writeFixture(path, "gpt-new-release"));
        QCOMPARE(listModel(adapter), std::optional<QString>(QStringLiteral("gpt-new-release")));
        QCOMPARE(reloaded.count(), 1);
        QCOMPARE(reloaded.first().first().toString(), QStringLiteral("executable_updated"));
        adapter.stop();
    }

    void activeTurnKeepsTheRunningCliUntilItEnds() {
        QTemporaryDir folder;
        const QString path = folder.filePath(fixtureName(QStringLiteral("codex")));
        QVERIFY(writeFixture(path, "gpt-old"));
        CodexAdapter adapter(path);
        QSignalSpy reloaded(&adapter, &CodexAdapter::reloaded);
        const QJsonObject thread{{QStringLiteral("threadId"), QStringLiteral("thread-1")}};

        callAndWait(adapter, QStringLiteral("turn/start"), thread);
        QTRY_VERIFY_WITH_TIMEOUT(!adapter.isIdle(), 10000);
        QVERIFY(writeFixture(path, "gpt-new-release"));
        QCOMPARE(listModel(adapter), std::optional<QString>(QStringLiteral("gpt-old")));
        QCOMPARE(reloaded.count(), 0);

        callAndWait(adapter, QStringLiteral("turn/interrupt"), thread);
        QTRY_VERIFY_WITH_TIMEOUT(adapter.isIdle(), 10000);
        QCOMPARE(listModel(adapter), std::optional<QString>(QStringLiteral("gpt-new-release")));
        QCOMPARE(reloaded.count(), 1);
        adapter.stop();
    }
};

} // namespace ketplus::daemon

QTEST_GUILESS_MAIN(ketplus::daemon::CodexExecutableUpdateTest)
#include "CodexExecutableUpdateTest.moc"
