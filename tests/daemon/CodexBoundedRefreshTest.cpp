#include "agents/AgentToolLaunch.h"
#include "providers/CodexAdapter.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
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
    return !executable ||
           QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                           QFileDevice::ExeOwner);
}
QString quoted(const QString& text) {
    const auto json = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}
bool reserve(const QString& path, const QString& model) {
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(0))
        return false;
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly))
        return false;
    auto ledger = QJsonDocument::fromJson(input.readAll()).object();
    input.close();
    auto codex = ledger.value("codex").toObject();
    if (ledger.value("decision") != "D-3" || ledger.value("maximum_per_provider").toInt() != 1 ||
        ledger.value("owner_conversation").toInt() !=
            qEnvironmentVariableIntValue("KETPLUS_BOUNDED_TEST_OWNER") ||
        qEnvironmentVariable("KETPLUS_BOUNDED_TEST_HEAD").isEmpty() ||
        ledger.value("recall_allowed").toBool(true) || ledger.value("retry_allowed").toBool(true) ||
        codex.value("sent_or_uncertain").toInt(-1) != 0)
        return false;
    // Reserve before delivery. Errors and timeouts never refund or retry it.
    codex.insert("sent_or_uncertain", 1);
    codex.insert("model", model);
    codex.insert("head", qEnvironmentVariable("KETPLUS_BOUNDED_TEST_HEAD"));
    codex.insert("auth_route", "inherited-provider-bootstrap");
    codex.insert("status", "reserved_before_delivery");
    ledger.insert("codex", codex);
    QSaveFile output(path);
    output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return output.open(QIODevice::WriteOnly) && output.write(QJsonDocument(ledger).toJson()) > 0 &&
           output.commit();
}
QJsonObject call(CodexAdapter& adapter, const QString& method, const QJsonObject& params) {
    auto result = std::make_shared<QJsonObject>();
    auto done = std::make_shared<bool>(false);
    adapter.call(method, params, [result, done](const QJsonObject& value, const QString& error) {
        *result = error.isEmpty() ? value : QJsonObject{{"probe_error", error}};
        *done = true;
    });
    QElapsedTimer timer;
    timer.start();
    while (!*done && timer.elapsed() < 30000)
        QTest::qWait(10);
    return *done ? *result : QJsonObject{{"probe_error", "Request timed out; do not retry."}};
}
QList<QJsonObject> events(const QString& path) {
    QFile file(path);
    QList<QJsonObject> result;
    if (file.open(QIODevice::ReadOnly))
        while (!file.atEnd())
            result.append(QJsonDocument::fromJson(file.readLine()).object());
    return result;
}
bool alive(qint64 pid) {
#ifdef Q_OS_UNIX
    return pid > 0 && ::kill(pid, 0) == 0;
#else
    Q_UNUSED(pid)
    return false;
#endif
}
bool exited(const QString& log) {
    for (const auto& row : events(log))
        if ((row.value("event") == "mcp" || row.value("event") == "provider") &&
            alive(row.value("pid").toInteger()))
            return false;
    return true;
}
struct Cleanup {
    CodexAdapter& adapter;
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
QJsonObject inventory(CodexAdapter& adapter, const QString& id) {
    const auto result = call(adapter, "mcpServerStatus/list", {{"threadId", id}, {"limit", 100}});
    for (const auto& row : result.value("data").toArray())
        if (row.toObject().value("name") == "ketplus_agents")
            return row.toObject().value("tools").toObject();
    return {};
}
bool completedAnswer(const QJsonObject& result, const QString& marker) {
    for (const auto& value : result.value("thread").toObject().value("turns").toArray()) {
        const auto turn = value.toObject();
        if (turn.value("status") != "completed")
            continue;
        for (const auto& item : turn.value("items").toArray())
            if (item.toObject().value("type") == "agentMessage" &&
                item.toObject().value("text").toString().contains(marker))
                return true;
    }
    return false;
}
} // namespace

class CodexBoundedRefreshTest final : public QObject {
    Q_OBJECT
  private slots:
    void completedSyntheticTurnSurvivesOwnedTransportReplacement() {
        if (qEnvironmentVariable("KETPLUS_CODEX_BOUNDED_REFRESH") != "D-3")
            QSKIP("Requires explicit quota authorization and an unconsumed external ledger.");
#ifndef Q_OS_UNIX
        QSKIP("This bounded owned-process observer requires Unix.");
#endif
        const auto binary = qEnvironmentVariable("KETPLUS_CODEX_TEST_BINARY");
        const auto ledger = qEnvironmentVariable("KETPLUS_BOUNDED_TEST_LEDGER");
        QVERIFY(QFileInfo(binary).isExecutable());
        QVERIFY(QFileInfo(ledger).isFile());
        const QString model = "gpt-6-luna";
        const auto python = QStandardPaths::findExecutable("python3");
        QVERIFY(!python.isEmpty());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto root = QFileInfo(directory.path()).canonicalFilePath();
        const auto work = root + "/work";
        const auto log = root + "/processes.jsonl";
        QVERIFY(QDir().mkpath(work));
        const auto fixture = QString::fromUtf8(KETPLUS_CODEX_LIVE_FIXTURE);
        const auto configPath = root + "/launch.json";
        QVERIFY(writeFile(
            configPath,
            QJsonDocument(QJsonObject{{"executable", binary}, {"work", work}, {"log", log}})
                .toJson()));
        const auto wrapper = root + "/codex-bounded";
        QVERIFY(writeFile(
            wrapper,
            QString(
                "#!%1\nimport os,sys\nos.execv(%2,[%2,%3,'--launch-config',%4,*sys.argv[1:]])\n")
                .arg(python, quoted(python), quoted(fixture), quoted(configPath))
                .toUtf8(),
            true));
        CodexAdapter adapter(wrapper);
        adapter.setBrowserArgumentsProvider([] { return QStringList{}; });
        Cleanup cleanup{adapter, log};
        connect(&adapter, &ProviderAdapter::requestReceived, &adapter,
                [&](const QJsonValue& id, const QString&, const QJsonObject&) {
                    adapter.respondError(id, "denied",
                                         "Text-only bounded probe denies tool requests.");
                });
        bool completed = false, failed = false;
        connect(&adapter, &CodexAdapter::notificationReceived, &adapter,
                [&](const QString& method, const QJsonObject& data) {
                    if (method == "turn/completed") {
                        completed = true;
                        failed = data.value("turn").toObject().value("status") != "completed";
                    }
                    if (method == "error")
                        failed = true;
                });
        auto setup = [&](const QString& epoch, const QString& id = QString{}) {
            auto config = agent_tools::codexConfig({python, "synthetic-socket", "synthetic-read",
                                                    "synthetic-mutation-" + epoch, epoch});
            config.insert("mcp_servers.ketplus_agents.args",
                          QJsonArray{fixture, "--worker-log", log});
            config.insert("features.shell_tool", false);
            config.insert("features.multi_agent", false);
            config.insert("web_search", "disabled");
            return QJsonObject{{"managedSessionId", "t3-bounded-owner"},
                               {"managedTransportEpoch", epoch},
                               {"threadId", id},
                               {"cwd", work},
                               {"model", model},
                               {"approvalPolicy", "never"},
                               {"sandbox", "read-only"},
                               {"config", config}};
        };
        const auto catalog = call(adapter, "model/list", {{"limit", 100}});
        bool available = false;
        for (const auto& row : catalog.value("data").toArray())
            available |= row.toObject().value("model") == model;
        QVERIFY2(available, "Cheap explicit model unavailable; stop before consuming quota.");
        adapter.stop();
        QTRY_VERIFY(exited(log));
        QVERIFY(QFile::remove(log));
        const auto first = call(adapter, "thread/start", setup("one"));
        QVERIFY2(!first.contains("probe_error"), qPrintable(first.value("probe_error").toString()));
        QCOMPARE(first.value("managedTransportEpoch"), "one");
        QCOMPARE(first.value("model"), model);
        const auto id = first.value("thread").toObject().value("id").toString();
        QVERIFY(!id.isEmpty());
        QVERIFY(inventory(adapter, id).contains("t3_epoch_one"));
        QVERIFY2(reserve(ledger, model), "No unused D-3 allowance; no prompt sent.");
        const QString marker = "T3_BOUNDED_CODEX_ALPHA";
        const auto turn =
            call(adapter, "turn/start",
                 {{"threadId", id},
                  {"model", model},
                  {"effort", "low"},
                  {"input", QJsonArray{QJsonObject{
                                {"type", "text"},
                                {"text", "Reply exactly " + marker + ". Do not use tools."}}}}});
        QVERIFY2(!turn.contains("probe_error"), qPrintable(turn.value("probe_error").toString()));
        QTRY_VERIFY_WITH_TIMEOUT(completed, 120000);
        QVERIFY(!failed);
        auto sameSetup = setup("one", id);
        sameSetup.remove("managedSessionId");
        sameSetup.remove("managedTransportEpoch");
        const auto same = call(adapter, "thread/resume", sameSetup);
        QCOMPARE(same.value("thread").toObject().value("id"), id);
        const auto before =
            call(adapter, "thread/read", {{"threadId", id}, {"includeTurns", true}});
        QVERIFY(completedAnswer(before, marker));
        const auto second = call(adapter, "thread/resume", setup("two", id));
        QVERIFY2(!second.contains("probe_error"),
                 qPrintable(second.value("probe_error").toString()));
        QCOMPARE(second.value("thread").toObject().value("id"), id);
        QCOMPARE(second.value("managedTransportEpoch"), "two");
        const auto tools = inventory(adapter, id);
        QVERIFY(tools.contains("t3_epoch_two"));
        QVERIFY(!tools.contains("t3_epoch_one"));
        const auto after = call(adapter, "thread/read", {{"threadId", id}, {"includeTurns", true}});
        QVERIFY(completedAnswer(after, marker));
        QList<QJsonObject> providers;
        for (const auto& row : events(log)) {
            if (row.value("event") == "provider")
                providers.append(row);
            QVERIFY(row.value("event") != "unexpected_tool");
        }
        QCOMPARE(providers.size(), 2);
        QCOMPARE(providers.last().value("prior_mcp_alive").toInt(), 0);
        QVERIFY(!alive(providers.first().value("pid").toInteger()));
        QVERIFY(adapter.releaseSessionTransport("t3-bounded-owner"));
        adapter.stop();
        QTRY_VERIFY(exited(log));
        qInfo("D-3 codex: one gpt-6-luna/low turn; completed history survives same-ID refresh; "
              "MCP epoch one -> two, prior processes exited, no recall or tool call.");
    }
};
QTEST_GUILESS_MAIN(CodexBoundedRefreshTest)
#include "CodexBoundedRefreshTest.moc"
