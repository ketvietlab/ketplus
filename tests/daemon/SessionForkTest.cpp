#include "protocol/DaemonProtocol.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/compat/ProviderCapabilities.h"
#include "providers/compat/ProviderCompatibility.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include "FakeClaude.h"
#include "FakeCodex.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <optional>

using namespace ketplus::daemon;

namespace {

QJsonObject request(const QString& type, const QString& id, const QJsonObject& payload = {},
                    const QString& sessionId = {}, const qint64 generation = -1) {
    QJsonObject result{{"protocol_version", protocol::currentVersion},
                       {"message_id", id},
                       {"message_type", type},
                       {"sent_at", protocol::timestamp()},
                       {"payload", payload}};
    if (!sessionId.isEmpty())
        result.insert("session_id", sessionId);
    if (generation >= 0)
        result.insert("session_generation", generation);
    return result;
}

QJsonObject startTurn(SessionHost& host, const QString& id, const QString& worktree,
                      const QString& provider, const QString& prompt,
                      const QString& sessionId = {}) {
    return host.handleCommand(
        request(QStringLiteral("session.start_turn"), id,
                {{QStringLiteral("worktree_path"), worktree},
                 {QStringLiteral("prompt"), prompt},
                 {QStringLiteral("options"),
                  QJsonObject{{QStringLiteral("provider"), provider},
                              {QStringLiteral("permission"), QStringLiteral("workspace-write")}}}},
                sessionId, sessionId.isEmpty() ? -1 : 1),
        QStringLiteral("desktop-test"));
}

QJsonObject fork(SessionHost& host, const QString& id, const QString& sessionId,
                 const QJsonObject& payload = {}) {
    std::optional<QJsonObject> answer;
    host.forkSession(request(QStringLiteral("session.fork"), id, payload, sessionId, 1),
                     [&answer](const QJsonObject& result) { answer = result; });
    if (!QTest::qWaitFor([&answer] { return answer.has_value(); }, 5000))
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error_code"), "timeout"}};
    return *answer;
}

QStringList eventTypes(LocalJournal& journal, const QString& sessionId) {
    QStringList types;
    for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0))
        types.append(value.toObject().value(QStringLiteral("event_type")).toString());
    return types;
}

bool turnCompleted(LocalJournal& journal, const QString& sessionId, const int count = 1) {
    return eventTypes(journal, sessionId).count(QStringLiteral("turn.completed")) >= count;
}

QByteArray logText(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QString text(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

StoredSession storedSession(LocalJournal& journal, const QString& sessionId) {
    for (const StoredSession& session : journal.sessions())
        if (session.id == sessionId)
            return session;
    return {};
}

} // namespace

class SessionForkTest final : public QObject {
    Q_OBJECT

  private slots:
    void onlyCodexAndClaudeCanFork() {
        QCOMPARE(compat::capabilityForMethod(u"thread/fork"), QStringLiteral("threadFork"));
        QVERIFY(compat::policy(QStringLiteral("codex"))
                    .baselineSupports(compat::capability::threadFork));
        QVERIFY(compat::policy(QStringLiteral("claude"))
                    .baselineSupports(compat::capability::threadFork));
        QVERIFY(!compat::policy(QStringLiteral("cursor"))
                     .baselineSupports(compat::capability::threadFork));
        QVERIFY(protocol::features(1).contains(QStringLiteral("session.fork")));
        QVERIFY(!protocol::features(0).contains(QStringLiteral("session.fork")));
    }

    void forksClaudeConversationWithItsTranscript() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString claudePath = directory.filePath(QStringLiteral("fake-claude"));
        const QString logPath = directory.filePath(QStringLiteral("claude.log"));
        QVERIFY(writeFakeClaude(claudePath, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        const QString codexPath = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(codexPath, directory.filePath(QStringLiteral("codex.log"))));
        CodexAdapter codex(codexPath);
        ClaudeAdapter claude(claudePath);
        SessionHost host(&journal, &codex, &claude);

        const QJsonObject started = startTurn(host, QStringLiteral("start"), directory.path(),
                                              QStringLiteral("claude"), QStringLiteral("hello"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(), qPrintable(text(started)));
        const QString sourceId = started.value(QStringLiteral("session_id")).toString();
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(journal, sourceId), 5000);
        const QStringList sourceEvents = eventTypes(journal, sourceId);
        const StoredSession source = storedSession(journal, sourceId);
        QCOMPARE(source.threadId, QStringLiteral("claude-thread-1"));

        const QJsonObject forked = fork(host, QStringLiteral("fork-1"), sourceId,
                                        {{QStringLiteral("fork_session_id"), "fork-session"}});
        QVERIFY2(forked.value(QStringLiteral("ok")).toBool(), qPrintable(text(forked)));
        QCOMPARE(forked.value(QStringLiteral("session_id")).toString(),
                 QStringLiteral("fork-session"));
        QCOMPARE(forked.value(QStringLiteral("adapter")).toString(), QStringLiteral("claude"));
        QCOMPARE(forked.value(QStringLiteral("worktree_path")).toString(), source.worktreePath);
        QVERIFY(logText(logPath).contains(
            QStringLiteral("fork:{\"cwd\": \"%1\", \"threadId\": \"claude-thread-1\"}")
                .arg(source.worktreePath)
                .toUtf8()));

        // The fork replays its source's transcript at the same cursors, then records the fork.
        const QStringList forkEvents = eventTypes(journal, QStringLiteral("fork-session"));
        QCOMPARE(forkEvents.mid(0, sourceEvents.size()), sourceEvents);
        QCOMPARE(forkEvents.size(), sourceEvents.size() + 1);
        QCOMPARE(forkEvents.last(), QStringLiteral("session.forked"));
        const QJsonObject marker = journal.eventsAfter(QStringLiteral("fork-session"), 1, 0)
                                       .last()
                                       .toObject()
                                       .value(QStringLiteral("payload"))
                                       .toObject();
        QCOMPARE(marker.value(QStringLiteral("from_session_id")).toString(), sourceId);
        QCOMPARE(marker.value(QStringLiteral("through_cursor")).toInteger(), source.lastCursor);
        const StoredSession stored = storedSession(journal, QStringLiteral("fork-session"));
        QCOMPARE(stored.threadId, QStringLiteral("fork-of-claude-thread-1"));
        QCOMPARE(stored.currentTurnSequence, source.currentTurnSequence);
        QCOMPARE(stored.lastCursor, source.lastCursor + 1);
        QCOMPARE(eventTypes(journal, sourceId), sourceEvents);

        // A retried request returns the first answer instead of forking again.
        QCOMPARE(fork(host, QStringLiteral("fork-1"), sourceId,
                      {{QStringLiteral("fork_session_id"), "fork-session"}}),
                 forked);
        QCOMPARE(logText(logPath).count("fork:"), 1);

        // The next turn in the fork continues the forked SDK session, not the source's.
        const QJsonObject next =
            startTurn(host, QStringLiteral("next"), directory.path(), QStringLiteral("claude"),
                      QStringLiteral("continue"), QStringLiteral("fork-session"));
        QVERIFY2(next.value(QStringLiteral("ok")).toBool(), qPrintable(text(next)));
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(journal, QStringLiteral("fork-session"), 2), 5000);
        QCOMPARE(storedSession(journal, QStringLiteral("fork-session")).threadId,
                 QStringLiteral("fork-of-claude-thread-1"));
        QCOMPARE(storedSession(journal, sourceId).threadId, QStringLiteral("claude-thread-1"));
    }

    void forksCodexConversationAndResumesTheCopy() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString codexPath = directory.filePath(QStringLiteral("fake-codex"));
        const QString logPath = directory.filePath(QStringLiteral("codex.log"));
        QVERIFY(writeFakeCodex(codexPath, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(codexPath);
        SessionHost host(&journal, &codex);

        const QJsonObject started = startTurn(host, QStringLiteral("start"), directory.path(),
                                              QStringLiteral("codex"), QStringLiteral("hello"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(), qPrintable(text(started)));
        const QString sourceId = started.value(QStringLiteral("session_id")).toString();
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(journal, sourceId), 5000);

        const QJsonObject forked = fork(host, QStringLiteral("fork-1"), sourceId);
        QVERIFY2(forked.value(QStringLiteral("ok")).toBool(), qPrintable(text(forked)));
        const QString forkId = forked.value(QStringLiteral("session_id")).toString();
        QVERIFY(!forkId.isEmpty());
        QVERIFY(forkId != sourceId);
        QVERIFY(logText(logPath).contains("fork:{\"cwd\": "));
        QVERIFY(logText(logPath).contains(
            "\"ephemeral\": false, \"excludeTurns\": true, \"threadId\": \"thread-1\"}"));
        QCOMPARE(storedSession(journal, forkId).threadId, QStringLiteral("fork-of-thread-1"));

        const QJsonObject next = startTurn(host, QStringLiteral("next"), directory.path(),
                                           QStringLiteral("codex"), QStringLiteral("go"), forkId);
        QVERIFY2(next.value(QStringLiteral("ok")).toBool(), qPrintable(text(next)));
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(journal, forkId, 2), 5000);
        QVERIFY(logText(logPath).contains("turn:fork-of-thread-1"));
    }

    void forksRunningSession_data() {
        QTest::addColumn<QString>("provider");
        QTest::newRow("claude") << QStringLiteral("claude");
        QTest::newRow("codex") << QStringLiteral("codex");
    }

    void forksRunningSession() {
        QFETCH(QString, provider);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString claudePath = directory.filePath(QStringLiteral("fake-claude"));
        const QString logPath = directory.filePath(QStringLiteral("claude.log"));
        QVERIFY(writeFakeClaude(claudePath, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        const QString codexPath = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(codexPath, directory.filePath(QStringLiteral("codex.log"))));
        CodexAdapter codex(codexPath);
        ClaudeAdapter claude(claudePath);
        SessionHost host(&journal, &codex, &claude);

        QCOMPARE(fork(host, QStringLiteral("unknown"), QStringLiteral("nope"))
                     .value(QStringLiteral("error_code"))
                     .toString(),
                 QStringLiteral("session_not_found"));

        const QJsonObject started = startTurn(host, QStringLiteral("start"), directory.path(),
                                              provider, QStringLiteral("hello"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(), qPrintable(text(started)));
        const QString sourceId = started.value(QStringLiteral("session_id")).toString();
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(journal, sourceId), 5000);
        const QJsonObject active =
            startTurn(host, QStringLiteral("source-active"), directory.path(), provider,
                      QStringLiteral("hang"), sourceId);
        QVERIFY2(active.value(QStringLiteral("ok")).toBool(), qPrintable(text(active)));
        QTRY_COMPARE_WITH_TIMEOUT(
            eventTypes(journal, sourceId).count(QStringLiteral("turn.started")), 2, 5000);
        QString unrelatedId;
        StoredSession unrelatedBefore;
        if (provider == QStringLiteral("codex")) {
            const auto unrelated = startTurn(host, QStringLiteral("unrelated-active"),
                                             directory.path(), provider, QStringLiteral("hang"));
            QVERIFY(unrelated.value(QStringLiteral("ok")).toBool());
            unrelatedId = unrelated.value(QStringLiteral("session_id")).toString();
            QTRY_VERIFY(eventTypes(journal, unrelatedId).contains(QStringLiteral("turn.started")));
            unrelatedBefore = storedSession(journal, unrelatedId);
        }
        QSignalSpy stopped(&codex, &CodexAdapter::stopped);
        const StoredSession before = storedSession(journal, sourceId);
        const QStringList sourceEvents = eventTypes(journal, sourceId);
        const QJsonObject result = fork(host, QStringLiteral("live-fork"), sourceId);
        QVERIFY2(result.value(QStringLiteral("ok")).toBool(), qPrintable(text(result)));
        const QString forkId = result.value(QStringLiteral("session_id")).toString();
        QCOMPARE(storedSession(journal, sourceId).state, before.state);
        QCOMPARE(storedSession(journal, sourceId).turnId, before.turnId);
        QCOMPARE(eventTypes(journal, sourceId), sourceEvents);
        QCOMPARE(storedSession(journal, forkId).state, QStringLiteral("ready"));
        QVERIFY(storedSession(journal, forkId).turnId.isEmpty());
        QVERIFY(storedSession(journal, forkId).threadId != before.threadId);
        QVERIFY(turnCompleted(journal, forkId));
        QCOMPARE(fork(host, QStringLiteral("live-fork"), sourceId), result);
        const QJsonObject next = startTurn(host, QStringLiteral("fork-next"), directory.path(),
                                           provider, QStringLiteral("continue"), forkId);
        QVERIFY2(next.value(QStringLiteral("ok")).toBool(), qPrintable(text(next)));
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(journal, forkId, 3), 5000);
        QVERIFY(!turnCompleted(journal, sourceId, 2));
        QCOMPARE(stopped.count(), 0);
        if (!unrelatedId.isEmpty()) {
            QCOMPARE(storedSession(journal, unrelatedId).state, unrelatedBefore.state);
            QCOMPARE(storedSession(journal, unrelatedId).turnId, unrelatedBefore.turnId);
            QVERIFY(!turnCompleted(journal, unrelatedId));
        }
        QCOMPARE(storedSession(journal, sourceId).state, before.state);
    }
};

QTEST_GUILESS_MAIN(SessionForkTest)
#include "SessionForkTest.moc"
