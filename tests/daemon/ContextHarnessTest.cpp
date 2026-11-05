#include "FakeClaude.h"
#include "FakeCodex.h"
#include "protocol/DaemonProtocol.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "runtime/CompactWindowPolicy.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include <QFile>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace ketplus::daemon;

namespace {
QJsonObject command(const QString& type, const QString& sessionId, const QJsonObject& payload) {
    return {{"protocol_version", protocol::currentVersion},
            {"message_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"message_type", type},
            {"session_id", sessionId},
            {"session_generation", 1},
            {"sent_at", protocol::timestamp()},
            {"payload", payload}};
}

QJsonObject start(SessionHost& host, const QString& worktree, const QString& id,
                  const QJsonObject& options = {},
                  const QString& prompt = QStringLiteral("hello")) {
    return host.handleCommand(
        command("session.start_turn", id,
                {{"worktree_path", worktree}, {"prompt", prompt}, {"options", options}}),
        QStringLiteral("context-test"));
}

int completions(LocalJournal& journal, const QString& id) {
    int count = 0;
    for (const QJsonValue& event : journal.eventsAfter(id, 1, 0))
        count += event.toObject().value("event_type").toString() == "turn.completed";
    return count;
}

QList<QJsonObject> parameters(const QString& path, const QByteArray& prefix = "setup:") {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<QJsonObject> result;
    for (const QByteArray& line : file.readAll().split('\n'))
        if (line.startsWith(prefix))
            result.append(QJsonDocument::fromJson(line.mid(prefix.size())).object());
    return result;
}

QJsonObject effective(SessionHost& host, const QString& id) {
    return host.handleCommand(command("session.configuration.read", id, {}), "context-test")
        .value("effective")
        .toObject();
}
} // namespace

class ContextHarnessTest final : public QObject {
    Q_OBJECT
  private slots:
    void settingsDuringAttachStayOutOfTheSubmittedTurn() {
        QTemporaryDir root;
        const auto executable = root.filePath("codex");
        const auto log = root.filePath("codex.log");
        QVERIFY(writeFakeCodex(executable, log));
        LocalJournal journal(root.filePath("journal.sqlite"));
        CodexAdapter codex(executable);
        SessionHost host(&journal, &codex);
        QVERIFY(start(host, root.path(), "s",
                      {{"model", "model-a"}, {"effort", "low"}, {"compactWindow", 272'000}})
                    .value("ok")
                    .toBool());
        const auto changed = host.handleCommand(
            command("session.configuration.apply", "s",
                    {{"expected_revision", journal.sessions().first().configurationRevision},
                     {"desktop_selection", true},
                     {"model", "model-b"},
                     {"effort", "high"},
                     {"compactWindow", 500'000}}),
            "context-test");
        QVERIFY(changed.value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(completions(journal, "s"), 1, 5000);
        const auto first = parameters(log).first();
        QCOMPARE(first.value("model").toString(), QString("model-a"));
        QCOMPARE(
            first.value("config").toObject().value("model_auto_compact_token_limit").toInteger(),
            272'000);
        QVERIFY(start(host, root.path(), "s").value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(completions(journal, "s"), 2, 5000);
        QCOMPARE(parameters(log).size(), 2);
        QCOMPARE(parameters(log)
                     .last()
                     .value("config")
                     .toObject()
                     .value("model_auto_compact_token_limit")
                     .toInteger(),
                 500'000);
    }

    void rejectsInvalidWindowsBeforeCreatingSessions_data() {
        QTest::addColumn<QJsonValue>("window");
        QTest::newRow("zero") << QJsonValue(0);
        QTest::newRow("negative") << QJsonValue(-1);
        QTest::newRow("below-minimum") << QJsonValue(99'999);
        QTest::newRow("above-maximum") << QJsonValue(1'000'001);
        QTest::newRow("fractional") << QJsonValue(272'000.5);
        QTest::newRow("string") << QJsonValue("272000");
        QTest::newRow("boolean") << QJsonValue(true);
        QTest::newRow("array") << QJsonValue(QJsonArray{272'000});
        QTest::newRow("object") << QJsonValue(QJsonObject{{"tokens", 272'000}});
    }

    void rejectsInvalidWindowsBeforeCreatingSessions() {
        QFETCH(QJsonValue, window);
        QTemporaryDir root;
        LocalJournal journal(root.filePath("journal.sqlite3"));
        CodexAdapter codex(QStringLiteral("/nonexistent/context-test-codex"));
        SessionHost host(&journal, &codex);
        const auto result = start(host, root.path(), "s", {{"compactWindow", window}});
        QCOMPARE(result.value("error_code").toString(), QStringLiteral("invalid_compact_window"));
        QVERIFY(journal.sessions().isEmpty());
    }

    void inheritanceBoundsAndBusyTools() {
        qint64 resolved = 0;
        for (const qint64 tokens : {100'000, 272'000, 1'000'000}) {
            QVERIFY(
                context_harness::resolveCompactWindow(tokens, "codex", 0, {}, resolved).isEmpty());
            QCOMPARE(resolved, tokens);
            QVERIFY(
                context_harness::resolveCompactWindow(tokens, "claude", 0, {}, resolved).isEmpty());
        }
        const QJsonArray tasks{QJsonObject{{"status", "running"}}};
        QVERIFY(context_harness::resolveCompactWindow(QJsonValue::Undefined, "claude", 272'000,
                                                      tasks, resolved)
                    .isEmpty());
        QCOMPARE(resolved, 272'000);
        QVERIFY(context_harness::resolveCompactWindow(500'000, "claude", 272'000, tasks, resolved)
                    .isEmpty());
        QCOMPARE(resolved, 500'000);
        QVERIFY(
            context_harness::resolveCompactWindow(QJsonValue::Null, "cursor", 272'000, {}, resolved)
                .isEmpty());
        QCOMPARE(resolved, 0);
        QCOMPARE(context_harness::resolveCompactWindow(272'000, "cursor", 0, {}, resolved)
                     .value("error_code")
                     .toString(),
                 QStringLiteral("unsupported_compact_window"));
        QVERIFY(protocol::features(1).contains(QStringLiteral("session.compact_window/1")));
        QVERIFY(!protocol::features(0).contains(QStringLiteral("session.compact_window/1")));
    }

    void migratesExistingJournalsWithoutChangingModels() {
        QTemporaryDir root;
        const QString path = root.filePath("journal.sqlite3");
        {
            LocalJournal journal(path);
            StoredSession stored;
            stored.id = "s";
            stored.worktreePath = root.path();
            stored.adapter = "codex";
            stored.state = "ready";
            stored.model = "test-model";
            stored.compactWindow = 272'000;
            QVERIFY(journal.upsertSession(stored));
            QCOMPARE(journal.sessions().first().compactWindow, 272'000);
        }
        {
            const QString connection = QUuid::createUuid().toString();
            {
                auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
                database.setDatabaseName(path);
                QVERIFY(database.open());
                QSqlQuery query(database);
                QVERIFY(query.exec("ALTER TABLE sessions DROP COLUMN compact_window"));
            }
            QSqlDatabase::removeDatabase(connection);
        }
        LocalJournal restored(path);
        QVERIFY2(restored.isOpen(), qPrintable(restored.errorString()));
        QCOMPARE(restored.sessions().first().compactWindow, 0);
        QCOMPARE(restored.sessions().first().model, QStringLiteral("test-model"));
    }

    void codexStartResumeForkAndReset() {
        QTemporaryDir root;
        const QString provider = root.filePath("codex");
        const QString log = root.filePath("codex.log");
        const QString path = root.filePath("journal.sqlite3");
        QVERIFY(writeFakeCodex(provider, log));
        const QJsonObject config{{"model_auto_compact_token_limit", 272'000},
                                 {"model_auto_compact_token_limit_scope", "total"}};
        {
            LocalJournal journal(path);
            CodexAdapter codex(provider);
            SessionHost host(&journal, &codex);
            QVERIFY(
                start(host, root.path(), "s", {{"compactWindow", 272'000}}).value("ok").toBool());
            QTRY_COMPARE_WITH_TIMEOUT(completions(journal, "s"), 1, 5000);
            QCOMPARE(parameters(log).last().value("config").toObject(), config);
            QCOMPARE(effective(host, "s").value("compactWindow").toInteger(), 272'000);
            QVERIFY(start(host, root.path(), "s").value("ok").toBool());
            QTRY_COMPARE_WITH_TIMEOUT(completions(journal, "s"), 2, 5000);
            QCOMPARE(parameters(log).size(), 1); // The same policy keeps the loaded thread.
        }
        LocalJournal journal(path);
        CodexAdapter codex(provider);
        SessionHost host(&journal, &codex);
        QVERIFY(start(host, root.path(), "s").value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(completions(journal, "s"), 3, 5000);
        const auto resumed = parameters(log).last();
        QCOMPARE(resumed.value("threadId").toString(), QStringLiteral("thread-1"));
        QCOMPARE(resumed.value("config").toObject(), config);
        QVERIFY(resumed.value("excludeTurns").toBool());
        bool forked = false;
        host.forkSession(command("session.fork", "s", {{"fork_session_id", "fork"}}),
                         [&](const QJsonObject& result) {
                             QVERIFY2(result.value("ok").toBool(),
                                      qPrintable(result.value("error_message").toString()));
                             forked = true;
                         });
        QTRY_VERIFY_WITH_TIMEOUT(forked, 5000);
        QCOMPARE(parameters(log, "fork:").last().value("config").toObject(), config);
        QCOMPARE(effective(host, "fork").value("compactWindow").toInteger(), 272'000);
        QVERIFY(start(host, root.path(), "fork").value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(completions(journal, "fork"), 4, 5000);
        QCOMPARE(parameters(log).last().value("config").toObject(), config);
        QVERIFY(start(host, root.path(), "s", {{"compactWindow", QJsonValue::Null}})
                    .value("ok")
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(completions(journal, "s"), 4, 5000);
        QVERIFY(!parameters(log).last().contains("config"));
        QVERIFY(effective(host, "s").value("compactWindow").isNull());
        QCOMPARE(journal.sessions().last().compactWindow, 0);
    }

    void configurationChangesApplyToLoadedCodexAtTheNextTurn() {
        QTemporaryDir root;
        const QString provider = root.filePath("codex");
        const QString log = root.filePath("codex.log");
        QVERIFY(writeFakeCodex(provider, log));
        LocalJournal journal(root.filePath("journal.sqlite3"));
        CodexAdapter codex(provider);
        SessionHost host(&journal, &codex);
        QVERIFY(start(host, root.path(), "s", {{"model", "test-model"}}).value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(completions(journal, "s"), 1, 5000);
        const auto changed = host.handleCommand(command("session.configuration.apply", "s",
                                                        {{"expected_revision", 2},
                                                         {"desktop_selection", true},
                                                         {"compactWindow", 500'000}}),
                                                "context-test");
        QVERIFY2(changed.value("ok").toBool(),
                 qPrintable(changed.value("error_message").toString()));
        QCOMPARE(changed.value("effective").toObject().value("model").toString(),
                 QStringLiteral("test-model"));
        QVERIFY(start(host, root.path(), "s").value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(completions(journal, "s"), 2, 5000);
        QCOMPARE(parameters(log).size(), 2);
        QCOMPARE(parameters(log)
                     .last()
                     .value("config")
                     .toObject()
                     .value("model_auto_compact_token_limit")
                     .toInteger(),
                 500'000);
        QVERIFY(start(host, root.path(), "s", {}, "hang").value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(journal.sessions().first().state, QStringLiteral("running"),
                                  5000);
        const auto changedWhileRunning =
            host.handleCommand(command("session.configuration.apply", "s",
                                       {{"expected_revision", 3},
                                        {"desktop_selection", true},
                                        {"compactWindow", 272'000}}),
                               "context-test");
        QVERIFY(changedWhileRunning.value("ok").toBool());
        QCOMPARE(journal.sessions().first().compactWindow, 272'000);
        QCOMPARE(journal.sessions().first().state, QStringLiteral("running"));
        QCOMPARE(parameters(log)
                     .last()
                     .value("config")
                     .toObject()
                     .value("model_auto_compact_token_limit")
                     .toInteger(),
                 500'000);
    }

    void claudeReceivesInheritedWindowsOnEveryTurn() {
        QTemporaryDir root;
        const QString provider = root.filePath("claude");
        const QString log = root.filePath("claude.log");
        QVERIFY(writeFakeClaude(provider, log));
        LocalJournal journal(root.filePath("journal.sqlite3"));
        CodexAdapter codex(QStringLiteral("/nonexistent/context-test-codex"));
        ClaudeAdapter claude(provider);
        SessionHost host(&journal, &codex, &claude);
        QVERIFY(start(host, root.path(), "s", {{"provider", "claude"}, {"compactWindow", 272'000}})
                    .value("ok")
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(completions(journal, "s"), 1, 5000);
        QCOMPARE(parameters(log).last().value("compactWindow").toInteger(), 272'000);
        QVERIFY(start(host, root.path(), "s").value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(completions(journal, "s"), 2, 5000);
        QCOMPARE(parameters(log).last().value("compactWindow").toInteger(), 272'000);
        QVERIFY(!parameters(log).last().value("resume").toString().isEmpty());
        QVERIFY(start(host, root.path(), "s", {{"compactWindow", QJsonValue::Null}})
                    .value("ok")
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(completions(journal, "s"), 3, 5000);
        QVERIFY(!parameters(log).last().contains("compactWindow"));
    }
};

QTEST_GUILESS_MAIN(ContextHarnessTest)
#include "ContextHarnessTest.moc"
