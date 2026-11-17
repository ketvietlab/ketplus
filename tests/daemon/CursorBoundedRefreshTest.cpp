#include "agents/AgentToolLaunch.h"
#include "providers/CursorAdapter.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <memory>
#ifdef Q_OS_UNIX
#include <csignal>
#endif

using namespace ketplus::daemon;
namespace {
bool writeFile(const QString& path, const QByteArray& contents, bool executable = false) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
        return false;
    file.close();
    auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (executable)
        permissions |= QFileDevice::ExeOwner;
    return QFile::setPermissions(path, permissions);
}
QString quoted(const QString& text) {
    const auto json = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}
QList<QJsonObject> events(const QString& path) {
    QFile file(path);
    QList<QJsonObject> result;
    if (file.open(QIODevice::ReadOnly))
        while (!file.atEnd()) {
            const auto row = QJsonDocument::fromJson(file.readLine()).object();
            if (!row.isEmpty())
                result.append(row);
        }
    return result;
}
bool exited(const QString& log) {
    for (const auto& row : events(log))
        for (const auto* key : {"pid", "child_pid"}) {
            const auto pid = row.value(key).toInteger();
#ifdef Q_OS_UNIX
            if (pid > 0 && ::kill(pid, 0) == 0)
                return false;
#else
            Q_UNUSED(pid)
#endif
        }
    return true;
}
struct Cleanup {
    CursorAdapter& adapter;
    QString log;
    ~Cleanup() {
        adapter.stop();
        QElapsedTimer timer;
        timer.start();
        while (!exited(log) && timer.elapsed() < 3000)
            QTest::qWait(20);
        if (!exited(log))
            qCritical("Owned bounded-probe process cleanup was not confirmed.");
        const auto evidence = qEnvironmentVariable("KETPLUS_BOUNDED_TEST_EVENTS");
        if (!evidence.isEmpty() && !QFile::copy(log, evidence))
            qWarning("Could not preserve the process-only probe evidence.");
    }
};
QJsonObject call(CursorAdapter& adapter, const QJsonObject& params) {
    auto result = std::make_shared<QJsonObject>();
    auto done = std::make_shared<bool>(false);
    adapter.call(
        "turn/start", params, [result, done](const QJsonObject& value, const QString& error) {
            *result = error.isEmpty()
                          ? value
                          : QJsonObject{{"probe_error", "Provider setup/load failed; inspect "
                                                        "bounded observer codes."}};
            *done = true;
        });
    QElapsedTimer timer;
    timer.start();
    while (!*done && timer.elapsed() < 30000)
        QTest::qWait(10);
    return *done ? *result : QJsonObject{{"probe_error", "Request timed out; do not retry."}};
}
} // namespace

class CursorBoundedRefreshTest final : public QObject {
    Q_OBJECT
  private slots:
    void realHistoryAndMcpReplacementWithoutRecall() {
        const auto mode = qEnvironmentVariable("KETPLUS_CURSOR_BOUNDED_REFRESH");
        if (mode != "D-3" && mode != "D-7" && mode != "control")
            QSKIP("Requires explicit quota authorization or a no-model control opt-in.");
#ifndef Q_OS_UNIX
        QSKIP("This bounded owned-process observer requires Unix.");
#endif
        const auto binary = qEnvironmentVariable("KETPLUS_CURSOR_TEST_BINARY");
        QVERIFY(QFileInfo(binary).isExecutable());
        const auto python = QStandardPaths::findExecutable("python3");
        QVERIFY(!python.isEmpty());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto root = QFileInfo(directory.path()).canonicalFilePath();
        const auto proposal = qEnvironmentVariable("KETPLUS_CURSOR_APPROVED_PROPOSAL");
        const bool attested = qEnvironmentVariable("KETPLUS_CURSOR_ATTESTED_PROBE") == "1";
        // Attested text-only history validation still issues no mutation grant.
        QVERIFY(proposal.isEmpty() || mode == "control" || attested);
        QVERIFY(!attested || !proposal.isEmpty());
        QJsonObject approvedSession;
        if (!proposal.isEmpty()) {
            QFile file(proposal + "/session-new.json");
            QVERIFY(file.open(QIODevice::ReadOnly));
            approvedSession = QJsonDocument::fromJson(file.readAll()).object();
            QVERIFY(!approvedSession.value("mcpServers").toArray().isEmpty());
        }
        const auto work =
            proposal.isEmpty() ? root + "/work" : approvedSession.value("cwd").toString();
        QString ownHistory;
        if (qEnvironmentVariable("KETPLUS_CURSOR_READ_OWN_HISTORY") == "1") {
            // An official read of only this probe's ID; never a model retry or
            // a scan of personal history. Control mode intercepts every prompt.
            QVERIFY(attested && mode == "control");
            QFile ledger(qEnvironmentVariable("KETPLUS_BOUNDED_TEST_LEDGER"));
            QVERIFY(ledger.open(QIODevice::ReadOnly));
            const auto budget = QJsonDocument::fromJson(ledger.readAll()).object();
            const int owner = qEnvironmentVariableIntValue("KETPLUS_BOUNDED_TEST_OWNER");
            QVERIFY(owner > 0);
            QCOMPARE(budget.value("owner_conversation").toInt(), owner);
            QCOMPARE(budget.value("decision").toString(), "D-3");
            const auto cursor = budget.value("cursor").toObject();
            QCOMPARE(cursor.value("sent_or_uncertain").toInt(), 1);
            QCOMPARE(cursor.value("synthetic_cwd").toString(), work);
            ownHistory = cursor.value("session_id").toString();
            QVERIFY(!ownHistory.isEmpty());
        }
        const auto log = root + "/processes.jsonl";
        QVERIFY(QDir().mkpath(work));
        const auto config = root + "/launch.json";
        QVERIFY(writeFile(
            config,
            QJsonDocument(
                QJsonObject{
                    {"executable", binary},
                    {"work", work},
                    {"log", log},
                    {"mode", mode},
                    {"approval_log", proposal.isEmpty() ? QString{} : proposal + "/events.jsonl"},
                    {"attestation", attested},
                    {"approved_session", approvedSession},
                    {"observer_epochs", QJsonObject{{"one", QUuid::createUuid().toString()},
                                                    {"two", QUuid::createUuid().toString()}}},
                    {"ledger", qEnvironmentVariable("KETPLUS_BOUNDED_TEST_LEDGER")},
                    {"head", qEnvironmentVariable("KETPLUS_BOUNDED_TEST_HEAD")},
                    {"owner", qEnvironmentVariableIntValue("KETPLUS_BOUNDED_TEST_OWNER")}})
                .toJson()));
        const auto wrapper = root + "/cursor-bounded";
        const auto fixture = QString::fromUtf8(KETPLUS_CURSOR_LIVE_FIXTURE);
        QVERIFY(writeFile(wrapper,
                          QString("#!%1\nimport os,sys\nos.execv(%2,[%2,%3,%4,*sys.argv[1:]])\n")
                              .arg(python, quoted(python), quoted(fixture), quoted(config))
                              .toUtf8(),
                          true));
        CursorAdapter adapter(wrapper);
        Cleanup cleanup{adapter, log};
        int nativeRequests = 0;
        connect(&adapter, &ProviderAdapter::requestReceived, &adapter,
                [&](const QJsonValue& id, const QString&, const QJsonObject&) {
                    ++nativeRequests;
                    adapter.respond(id, {{"outcome", QJsonObject{{"outcome", "cancelled"}}}});
                });
        int completed = 0;
        bool failed = false;
        connect(&adapter, &ProviderAdapter::canonicalEvent, &adapter,
                [&](const QString&, const QString& type, const QJsonObject& payload) {
                    if (type == "turn.completed") {
                        ++completed;
                        failed |= payload.value("status") == "failed";
                    }
                    if (type == "runtime.error")
                        failed = true;
                });
        auto setup = [&](const QString& epoch, const QString& resume = QString{}) {
            auto server = agent_tools::acpServer({python, "synthetic-socket", "synthetic-read",
                                                  "synthetic-mutation-" + epoch, epoch});
            server.insert("args", QJsonArray{QString::fromUtf8(KETPLUS_CODEX_LIVE_FIXTURE),
                                             "--worker-log", log});
            if (!proposal.isEmpty())
                server = approvedSession.value("mcpServers").toArray().first().toObject();
            return QJsonObject{
                {"sessionId", "t3-bounded-owner"},
                {"managedTransportEpoch", epoch},
                {"resume", resume},
                {"cwd", work},
                {"model", "claude-haiku-5-5-low"},
                {"permission", "read-only"},
                {"agentMcpServer", server},
                {"prompt", "Reply exactly T3_BOUNDED_CURSOR_ALPHA. Do not use tools."}};
        };
        const auto first = call(adapter, setup("one", ownHistory));
        QVERIFY2(!first.contains("probe_error"), qPrintable(first.value("probe_error").toString()));
        QCOMPARE(first.value("managedTransportEpoch"), "one");
        const auto id = first.value("thread").toObject().value("id").toString();
        QVERIFY(!id.isEmpty());
        if (!ownHistory.isEmpty())
            QCOMPARE(id, ownHistory);
        QTRY_VERIFY_WITH_TIMEOUT(completed > 0 || failed, 120000);
        QVERIFY2(!failed, "Provider/control failed; no automatic retry is permitted.");
        QCOMPARE(nativeRequests, 0);
        auto observed = events(log);
        int forwarded = 0;
        bool ready = false;
        for (const auto& row : observed) {
            forwarded += row.value("event") == "model_request_forwarded";
            if (row.value("event") == "prompt_barrier")
                ready = row.value("mcp_ready").toBool() && row.value("cheap_model_ack").toBool();
        }
        QVERIFY2(ready, "Actual MCP/model control failed before any model delivery.");
        if (mode == "control") {
            QCOMPARE(forwarded, 0);
            adapter.stop();
            QTRY_VERIFY(exited(log));
            if (!ownHistory.isEmpty()) {
                bool marker = false;
                for (const auto& row : events(log))
                    marker |= row.value("event") == "history_marker";
                qInfo("Owned synthetic same-ID load ACK; assistant marker replay: %s; zero "
                      "native prompts.",
                      marker ? "present" : "absent");
            }
            qInfo("Cursor actual adapter: transport/model readiness control; zero model "
                  "turns.");
            return;
        }
        QCOMPARE(forwarded, 1);
        const auto second = call(adapter, setup("two", id));
        QVERIFY2(!second.contains("probe_error"),
                 qPrintable(second.value("probe_error").toString()));
        QCOMPARE(second.value("managedTransportEpoch"), "two");
        QCOMPARE(second.value("thread").toObject().value("id"), id);
        QTRY_VERIFY_WITH_TIMEOUT(completed > 1 || failed, 20000);
        QVERIFY(!failed);
        QCOMPARE(nativeRequests, 0);
        bool history = false, refreshReady = false, oldGone = false;
        forwarded = 0;
        for (const auto& row : events(log)) {
            QVERIFY(row.value("event") != "provider_tool_call");
            if (row.value("event") == "injected_attestation_final")
                QVERIFY(!row.value("forbidden").toBool());
            forwarded += row.value("event") == "model_request_forwarded";
            history |= row.value("event") == "history_marker" &&
                       row.value("kind") == "agent_message_chunk";
            refreshReady |= row.value("event") == "tools_list" && row.value("epoch") == "two";
            if (attested)
                refreshReady |= row.value("event") == "injected_attestation" &&
                                row.value("epoch") == "two" && row.value("ready").toBool();
            oldGone |= row.value("event") == "provider" && row.value("epoch") == "two" &&
                       row.value("prior_mcp_alive").toInt() == 0;
        }
        QCOMPARE(forwarded, 1);
        QVERIFY(history);
        QVERIFY(refreshReady);
        QVERIFY(oldGone);
        QVERIFY(adapter.releaseSessionTransport("t3-bounded-owner"));
        adapter.stop();
        QTRY_VERIFY(exited(log));
        qInfo("%s Cursor: one explicit Haiku low turn; same-ID replay before any recall; "
              "new MCP transport attested and old processes exited. Inspect injected inventory "
              "evidence separately; no production mutation is enabled by this probe.",
              qPrintable(mode));
    }
};
QTEST_GUILESS_MAIN(CursorBoundedRefreshTest)
#include "CursorBoundedRefreshTest.moc"
