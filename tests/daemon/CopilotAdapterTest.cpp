#include "providers/CopilotAdapter.h"
#include "FakeCopilot.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/ProviderErrorCode.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include <QDir>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;
namespace {
QJsonObject params(const QString& text = "run") {
    return {{"sessionId", "test-session"},
            {"cwd", QDir::tempPath()},
            {"prompt", text},
            {"permission", "workspace-write"}};
}
QJsonArray logEntries(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QJsonArray entries;
    for (const auto& line : file.readAll().split('\n')) {
        const auto entry = QJsonDocument::fromJson(line);
        if (entry.isObject())
            entries.append(entry.object());
    }
    return entries;
}
QString errors(const QSignalSpy& events) {
    QString text;
    for (const auto& event : events)
        if (event[1].toString() == "runtime.error")
            text += event[2].toJsonObject()["message"].toString();
    return text;
}
bool completed(const QSignalSpy& events) {
    for (const auto& event : events)
        if (event[1].toString() == "turn.completed")
            return true;
    return false;
}
QJsonObject request(const QString& type, const QJsonObject& payload) {
    return {{"protocol_version", protocol::currentVersion},
            {"message_id", type},
            {"message_type", type},
            {"sent_at", protocol::timestamp()},
            {"payload", payload}};
}
} // namespace

class CopilotAdapterTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
#ifdef Q_OS_WIN
        QSKIP("POSIX executable fixtures; CopilotCliTest covers Windows launch/discovery on every "
              "host");
#endif
    }
    void validatesExplicitPathAndEnvironmentPrecedence() {
        QTemporaryDir dir;
        const auto previous = qgetenv("KETPLUS_COPILOT");
        const auto restore = qScopeGuard([&] {
            if (previous.isNull())
                qunsetenv("KETPLUS_COPILOT");
            else
                qputenv("KETPLUS_COPILOT", previous);
        });
        const QString file = dir.filePath("copilot");
        QVERIFY(writeFakeCopilot(file, dir.filePath("log")));
        qputenv("KETPLUS_COPILOT", file.toUtf8());
        CopilotAdapter discovered;
        QCOMPARE(discovered.executable(), file);
        CopilotAdapter invalid(dir.filePath("not-installed"));
        QVERIFY(!invalid.isAvailable());
        QVERIFY(invalid.missingMessage().contains("--copilot"));
        QVERIFY(invalid.missingMessage().contains("install-copilot-cli"));
        CopilotAdapter explicitCli(file);
        QCOMPARE(explicitCli.executable(), file);
    }
    void streamsAndCancels() {
        QTemporaryDir dir;
        const QString path = dir.filePath("copilot"), log = dir.filePath("log");
        QVERIFY(writeFakeCopilot(path, log));
        CopilotAdapter adapter(path);
        QCOMPARE(adapter.name(), "copilot");
        QSignalSpy events(&adapter, &ProviderAdapter::canonicalEvent);
        QString error;
        bool accepted = false;
        adapter.call("turn/start", params(),
                     [&](const QJsonObject& result, const QString& failure) {
                         error = failure;
                         accepted = !result["thread"].toObject()["id"].toString().isEmpty();
                     });
        QTRY_VERIFY(completed(events));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(accepted);
        QString text;
        for (const auto& event : events)
            if (event[1].toString() == "assistant.delta")
                text += event[2].toJsonObject()["delta"].toString();
        QCOMPARE(text, "hello world");
        const auto records = logEntries(log);
        QCOMPARE(records.first().toObject()["argv"].toArray(), QJsonArray({"--acp", "--stdio"}));
        QCOMPARE(records[1].toObject()["method"].toString(), "initialize");
        events.clear();
        accepted = false;
        adapter.call("turn/start", params("hold"), [&](const QJsonObject&, const QString& failure) {
            error = failure;
            accepted = true;
        });
        QTRY_VERIFY(accepted);
        adapter.call(
            "turn/interrupt", {{"sessionId", "test-session"}},
            [](const QJsonObject&, const QString& failure) { QVERIFY(failure.isEmpty()); });
        QTRY_VERIFY(completed(events));
        QCOMPARE(events.last()[2].toJsonObject()["status"].toString(), "interrupted");
    }
    void authenticationFailures_data() {
        QTest::addColumn<QString>("mode");
        for (const auto* mode :
             {"init-auth", "init-nested-auth", "new-auth", "prompt-auth", "exit-auth"})
            QTest::newRow(mode) << QString::fromUtf8(mode);
    }
    void authenticationFailures() {
        QFETCH(QString, mode);
        QTemporaryDir dir;
        const QString path = dir.filePath("copilot");
        QVERIFY(writeFakeCopilot(path, dir.filePath("log"), mode));
        CopilotAdapter adapter(path);
        QSignalSpy events(&adapter, &ProviderAdapter::canonicalEvent);
        QString failure;
        bool settled = false;
        adapter.call("turn/start", params(), [&](const QJsonObject&, const QString& error) {
            failure = error;
            settled = true;
        });
        QTRY_VERIFY(settled);
        if (failure.isEmpty())
            QTRY_VERIFY(completed(events));
        const auto explanation = failure + errors(events);
        QVERIFY2(explanation.contains("copilot login"), qPrintable(explanation));
        QVERIFY(!explanation.contains("jsonrpc"));
        QCOMPARE(providerErrorCode("copilot", {}, explanation), "auth_required");
        QTRY_VERIFY(!adapter.isReady());
        QTRY_COMPARE(adapter.findChild<QProcess*>()->state(), QProcess::NotRunning);
    }
    void retryStartsFreshProcess() {
        QTemporaryDir dir;
        const QString path = dir.filePath("copilot"), log = dir.filePath("log");
        QVERIFY(writeFakeCopilot(path, log, "auth-once"));
        CopilotAdapter adapter(path);
        QString failure;
        adapter.call("turn/start", params(),
                     [&](const QJsonObject&, const QString& error) { failure = error; });
        QTRY_VERIFY(failure.contains("copilot login"));
        QSignalSpy events(&adapter, &ProviderAdapter::canonicalEvent);
        adapter.call("turn/start", params(),
                     [&](const QJsonObject&, const QString& error) { failure = error; });
        QTRY_VERIFY(completed(events));
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QSet<qint64> pids;
        for (const auto& entry : logEntries(log)) {
            const auto pid = entry.toObject()["pid"].toInteger();
            if (pid)
                pids.insert(pid);
        }
        QCOMPARE(pids.size(), 2);
    }
    void preservesUnrelatedFailures_data() {
        QTest::addColumn<QString>("mode");
        for (const auto* mode : {"permission", "network", "usage", "secret"})
            QTest::newRow(mode) << QString::fromUtf8(mode);
    }
    void preservesUnrelatedFailures() {
        QFETCH(QString, mode);
        QTemporaryDir dir;
        const QString path = dir.filePath("copilot");
        QVERIFY(writeFakeCopilot(path, dir.filePath("log"), mode));
        CopilotAdapter adapter(path);
        QSignalSpy events(&adapter, &ProviderAdapter::canonicalEvent);
        adapter.call("turn/start", params(),
                     [](const QJsonObject&, const QString& error) { QVERIFY(error.isEmpty()); });
        QTRY_VERIFY(completed(events));
        const auto explanation = errors(events);
        QVERIFY(!explanation.isEmpty());
        QVERIFY(!explanation.contains("copilot login"));
        QVERIFY(!explanation.contains("ghp_fixtureSecret123"));
        QVERIFY(!explanation.contains("fixture-bearer"));
    }
    void doesNotAssumeResumeCapability() {
        QTemporaryDir dir;
        const QString path = dir.filePath("copilot"), log = dir.filePath("log");
        QVERIFY(writeFakeCopilot(path, log, "no-load"));
        CopilotAdapter adapter(path);
        QVERIFY(!adapter.supports(QString(compat::capability::threadResume)));
        auto input = params();
        input.insert("resume", "old-session");
        QSignalSpy events(&adapter, &ProviderAdapter::canonicalEvent);
        adapter.call("turn/start", input,
                     [](const QJsonObject&, const QString& error) { QVERIFY(error.isEmpty()); });
        QTRY_VERIFY(completed(events));
        QVERIFY(!adapter.supports(QString(compat::capability::threadResume)));
        for (const auto& entry : logEntries(log))
            QVERIFY(entry.toObject()["method"] != "session/load");
    }
    void rejectsUnsupportedProtocolVersion() {
        QTemporaryDir dir;
        const QString path = dir.filePath("copilot");
        QVERIFY(writeFakeCopilot(path, dir.filePath("log"), "unsupported"));
        CopilotAdapter adapter(path);
        QString failure;
        adapter.call("turn/start", params(),
                     [&](const QJsonObject&, const QString& error) { failure = error; });
        QTRY_VERIFY(!failure.isEmpty());
        QVERIFY2(failure.contains("ACP protocol version 1"), qPrintable(failure));
        QVERIFY(!failure.contains("copilot login"));
        QTRY_COMPARE(adapter.findChild<QProcess*>()->state(), QProcess::NotRunning);
    }

    void startupDeadlineSettlesOnce() {
        QTemporaryDir dir;
        const QString path = dir.filePath("copilot");
        QVERIFY(writeFakeCopilot(path, dir.filePath("log"), "hang"));
        CopilotAdapter adapter(path);
        QSignalSpy stopped(&adapter, &ProviderAdapter::stopped);
        int settled = 0;
        QString failure;
        adapter.call("turn/start", params(), [&](const QJsonObject&, const QString& error) {
            ++settled;
            failure = error;
        });
        QTRY_COMPARE_WITH_TIMEOUT(settled, 1, 35000);
        QVERIFY2(failure.contains("timed out"), qPrintable(failure));
        QTRY_COMPARE(adapter.findChild<QProcess*>()->state(), QProcess::NotRunning);
        QCOMPARE(stopped.size(), 1);
        QCOMPARE(settled, 1);
    }
    void hostRoutesCopilotTurns() {
        QTemporaryDir dir;
        const QString path = dir.filePath("copilot");
        QVERIFY(writeFakeCopilot(path, dir.filePath("log")));
        LocalJournal journal(dir.filePath("journal.sqlite"));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(dir.filePath("missing-codex"));
        CopilotAdapter copilot(path);
        bool queried = false;
        QJsonObject runtime;
        QString runtimeError;
        SessionHost host(&journal, &codex, QList<ProviderAdapter*>{&copilot});
        host.handleProviderQuery(request("provider.runtime_status", {}),
                                 [&](const QJsonObject& value, const QString& error) {
                                     runtime = value;
                                     runtimeError = error;
                                     queried = true;
                                 });
        QTRY_VERIFY(queried);
        QVERIFY2(runtimeError.isEmpty(), qPrintable(runtimeError));
        bool listed = false;
        for (const auto& value : runtime["providers"].toArray())
            if (value.toObject()["provider"] == "copilot")
                listed = true;
        QVERIFY(listed);
        const auto result = host.handleCommand(
            request("session.start_turn",
                    {{"worktree_path", dir.path()},
                     {"prompt", "hold"},
                     {"options",
                      QJsonObject{{"provider", "copilot"}, {"permission", "workspace-write"}}}}),
            "desktop-test");
        const auto id = result["session_id"].toString();
        QVERIFY2(!id.isEmpty(), QJsonDocument(result).toJson().constData());
        const auto started = [&] {
            for (const auto& value : journal.eventsAfter(id, 1, 0))
                if (value.toObject()["event_type"] == "turn.started")
                    return true;
            return false;
        };
        QTRY_VERIFY(started());
        auto interrupt = request("session.interrupt", {});
        interrupt.insert("session_id", id);
        interrupt.insert("session_generation", 1);
        const auto interrupted = host.handleCommand(interrupt, "desktop-test");
        QVERIFY2(interrupted["ok"].toBool(), QJsonDocument(interrupted).toJson().constData());
        const auto finished = [&] {
            for (const auto& value : journal.eventsAfter(id, 1, 0))
                if (value.toObject()["event_type"] == "turn.completed")
                    return true;
            return false;
        };
        QTRY_VERIFY(finished());
        bool streamed = false;
        for (const auto& value : journal.eventsAfter(id, 1, 0))
            if (value.toObject()["event_type"] == "assistant.delta")
                streamed = true;
        QVERIFY(streamed);
    }
};
QTEST_GUILESS_MAIN(CopilotAdapterTest)
#include "CopilotAdapterTest.moc"
