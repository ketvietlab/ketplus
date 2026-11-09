#include "CodexLaunchReloader.h"
#include "ipc/LocalIpcServer.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "providers/ProviderLineReader.h"
#include "runtime/SessionHost.h"

#include "storage/LocalJournal.h"
#include <QScopeGuard>

#include "FakeCodex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <algorithm>
#include <memory>
#include <utility>

using namespace ketplus::daemon;

namespace {

QJsonObject request(const QString& type, const QString& id, const QJsonObject& payload = {},
                    const QString& sessionId = {}, const qint64 generation = -1) {
    QJsonObject result{{"protocol_version", protocol::currentVersion},
                       {"message_id", id},
                       {"message_type", type},
                       {"sent_at", protocol::timestamp()},
                       {"payload", payload}};
    if (!sessionId.isEmpty()) {
        result.insert("session_id", sessionId);
    }
    if (generation >= 0) {
        result.insert("session_generation", generation);
    }
    return result;
}

class TestClient final : public QObject {
  public:
    explicit TestClient(QObject* parent = nullptr) : QObject(parent) {
        connect(&socket, &QLocalSocket::readyRead, this, [this] { readFrames(); });
    }

    void connectTo(const QString& name) { socket.connectToServer(name); }

    void send(const QJsonObject& value) {
        socket.write(protocol::encodeFrame(value));
        socket.flush();
    }

    [[nodiscard]] bool hasResponse(const QString& correlationId) const {
        for (const QJsonObject& message : messages) {
            if (message.value("correlation_id").toString() == correlationId) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] QJsonObject takeResponse(const QString& correlationId) {
        for (qsizetype index = 0; index < messages.size(); ++index) {
            if (messages.at(index).value("correlation_id").toString() == correlationId) {
                return messages.takeAt(index);
            }
        }
        return {};
    }

    [[nodiscard]] QJsonObject findEvent(const QString& type) const {
        for (auto it = messages.crbegin(); it != messages.crend(); ++it) {
            if (it->value("message_type").toString() == "session.event" &&
                it->value("payload").toObject().value("event_type").toString() == type) {
                return *it;
            }
        }
        return {};
    }

    void clear() { messages.clear(); }

    QLocalSocket socket;
    QList<QJsonObject> messages;

  private:
    void readFrames() {
        buffer_.append(socket.readAll());
        while (true) {
            if (expected_ == 0) {
                if (buffer_.size() < 4) {
                    return;
                }
                expected_ = (static_cast<quint32>(static_cast<uchar>(buffer_[0])) << 24U) |
                            (static_cast<quint32>(static_cast<uchar>(buffer_[1])) << 16U) |
                            (static_cast<quint32>(static_cast<uchar>(buffer_[2])) << 8U) |
                            static_cast<quint32>(static_cast<uchar>(buffer_[3]));
                buffer_.remove(0, 4);
            }
            if (buffer_.size() < static_cast<qsizetype>(expected_)) {
                return;
            }
            const QByteArray payload = buffer_.left(static_cast<qsizetype>(expected_));
            buffer_.remove(0, static_cast<qsizetype>(expected_));
            expected_ = 0;
            const QJsonDocument document = QJsonDocument::fromJson(payload);
            if (document.isObject()) {
                messages.append(document.object());
            }
        }
    }

    QByteArray buffer_;
    quint32 expected_{0};
};

void authenticate(TestClient& client, const QString& endpoint, const QByteArray& token,
                  const QString& id) {
    client.connectTo(endpoint);
    QTRY_COMPARE_WITH_TIMEOUT(client.socket.state(), QLocalSocket::ConnectedState, 1000);
    client.send(request("daemon.hello", id,
                        {{"token", QString::fromUtf8(token)}, {"client_id", "desktop-test"}}));
    QTRY_VERIFY_WITH_TIMEOUT(client.hasResponse(id), 1000);
    QVERIFY(client.takeResponse(id).value("payload").toObject().value("ok").toBool());
}

} // namespace

class DaemonLifecycleTest final : public QObject {
    Q_OBJECT

  private slots:
    void repairsHandoffTitlesOnRestore() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY(journal.isOpen());
        StoredSession stored;
        stored.id = QStringLiteral("long-handoff");
        stored.worktreePath = directory.path();
        stored.adapter = QStringLiteral("codex");
        stored.state = QStringLiteral("ready");
        stored.title = QStringLiteral(
            "# Long handoff\nPrevious context.\nUser request: Fix the conversation title\n"
            "<ketplus_response_preferences>Do not use this as a title");
        QVERIFY(journal.upsertSession(stored));
        CodexAdapter adapter(QStringLiteral("/missing-codex-for-title-repair"));
        SessionHost host(&journal, &adapter);
        const auto listed = host.handleCommand(
            request(QStringLiteral("session.list"), QStringLiteral("list-repaired")),
            QStringLiteral("desktop-test"));
        QCOMPARE(listed.value(QStringLiteral("sessions"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("title"))
                     .toString(),
                 QStringLiteral("Fix the conversation title"));
        QCOMPARE(journal.sessions().first().title, QStringLiteral("Fix the conversation title"));
    }

    void deletesOnlyIdleSessionAndItsJournalProjection() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY(journal.isOpen());
        StoredSession idle;
        idle.id = QStringLiteral("idle");
        idle.worktreePath = directory.path();
        idle.adapter = QStringLiteral("codex");
        idle.threadId = QStringLiteral("provider-idle");
        idle.state = QStringLiteral("ready");
        StoredSession busy;
        busy.id = QStringLiteral("busy");
        busy.worktreePath = directory.path();
        busy.adapter = QStringLiteral("codex");
        busy.threadId = QStringLiteral("provider-busy");
        busy.state = QStringLiteral("waiting_for_input");
        QVERIFY(journal.upsertSession(idle));
        QVERIFY(journal.upsertSession(busy));
        QString eventError;
        QVERIFY(journal.appendEvent(idle.id, idle.generation, QStringLiteral("assistant.delta"),
                                    {{QStringLiteral("delta"), QStringLiteral("private")}},
                                    &eventError) > 0);
        CodexAdapter adapter(QStringLiteral("/missing-codex-for-idle-delete"));
        SessionHost host(&journal, &adapter);

        const QJsonObject rejected =
            host.handleCommand(request(QStringLiteral("session.delete"),
                                       QStringLiteral("delete-busy"), {}, busy.id, busy.generation),
                               QStringLiteral("desktop-test"));
        QCOMPARE(rejected.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("session_busy"));

        const QJsonObject deleted =
            host.handleCommand(request(QStringLiteral("session.delete"),
                                       QStringLiteral("delete-idle"), {}, idle.id, idle.generation),
                               QStringLiteral("desktop-test"));
        QVERIFY2(deleted.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(deleted).toJson(QJsonDocument::Compact)));
        QVERIFY(deleted.value(QStringLiteral("applied")).toBool());
        QCOMPARE(deleted.value(QStringLiteral("lifecycle_revision")).toInteger(), 1);
        QCOMPARE(deleted.value(QStringLiteral("operation_id")).toString(),
                 QStringLiteral("delete-idle"));
        QCOMPARE(journal.eventsAfter(idle.id, idle.generation, 0).size(), 0);
        const auto sessions = journal.sessions();
        QCOMPARE(sessions.size(), 1);
        QCOMPARE(sessions.first().id, busy.id);

        const QJsonObject replay = host.handleCommand(
            request(QStringLiteral("session.delete"), QStringLiteral("delete-idle-retry"),
                    {{QStringLiteral("delete_operation_id"), QStringLiteral("delete-idle")}},
                    idle.id, idle.generation),
            QStringLiteral("desktop-test"));
        QVERIFY(replay.value(QStringLiteral("ok")).toBool());
        QVERIFY(replay.value(QStringLiteral("applied")).toBool());

        const QJsonObject aba = host.handleCommand(request(QStringLiteral("session.delete"),
                                                           QStringLiteral("delete-idle-aba"), {},
                                                           idle.id, idle.generation),
                                                   QStringLiteral("desktop-test"));
        QCOMPARE(aba.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("lifecycle_revision_conflict"));
        QVERIFY(!journal.upsertSession(idle));
    }

    void deletionTombstoneSurvivesRestartAndFencesABA() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("journal.sqlite3"));
        StoredSession session;
        session.id = QStringLiteral("restart-delete");
        session.worktreePath = directory.path();
        session.adapter = QStringLiteral("codex");
        session.threadId = QStringLiteral("provider-restart-delete");
        session.state = QStringLiteral("ready");
        {
            LocalJournal journal(path);
            QVERIFY(journal.upsertSession(session));
            CodexAdapter adapter(QStringLiteral("/missing-codex-for-restart-delete"));
            SessionHost host(&journal, &adapter);
            const QJsonObject deleted = host.handleCommand(
                request(QStringLiteral("session.delete"), QStringLiteral("delete-before-crash"),
                        {{QStringLiteral("delete_operation_id"), QStringLiteral("operation-1")},
                         {QStringLiteral("expected_lifecycle_revision"), 4}},
                        session.id, session.generation),
                QStringLiteral("cloud-control-v2"));
            QVERIFY(deleted.value(QStringLiteral("ok")).toBool());
            QCOMPARE(deleted.value(QStringLiteral("lifecycle_revision")).toInteger(), 6);
        }
        {
            LocalJournal journal(path);
            QVERIFY(journal.isOpen());
            CodexAdapter adapter(QStringLiteral("/missing-codex-for-restart-delete"));
            SessionHost host(&journal, &adapter);
            const QJsonObject replayed = host.handleCommand(
                request(QStringLiteral("session.delete"), QStringLiteral("delete-after-crash"),
                        {{QStringLiteral("delete_operation_id"), QStringLiteral("operation-1")},
                         {QStringLiteral("expected_lifecycle_revision"), 4}},
                        session.id, session.generation),
                QStringLiteral("cloud-control-v2"));
            QVERIFY(replayed.value(QStringLiteral("ok")).toBool());
            QVERIFY(replayed.value(QStringLiteral("applied")).toBool());
            QCOMPARE(replayed.value(QStringLiteral("operation_id")).toString(),
                     QStringLiteral("operation-1"));
            QVERIFY(!journal.upsertSession(session));
        }
    }

    void userMessageEchoesClientMessageId() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        CodexAdapter adapter(QStringLiteral("/missing-codex-for-client-message-id"));
        SessionHost host(&journal, &adapter);
        // Pending provider callbacks capture the host; drain them before it is destroyed, as
        // DaemonService does, instead of relying on reverse local destruction order.
        const auto stopProvider = qScopeGuard([&adapter] { adapter.stop(); });
        const auto startAndReadUserMessage = [&](const QString& messageId,
                                                 const QString& clientMessageId) {
            const QJsonObject started = host.handleCommand(
                request(QStringLiteral("session.start_turn"), messageId,
                        {{QStringLiteral("worktree_path"), directory.path()},
                         {QStringLiteral("prompt"), QStringLiteral("wrapped provider prompt")},
                         {QStringLiteral("display_text"), QStringLiteral("typed prompt")},
                         {QStringLiteral("client_message_id"), clientMessageId}}),
                QStringLiteral("desktop-test"));
            if (!started.value(QStringLiteral("ok")).toBool())
                return QJsonObject{};
            const QJsonArray events = journal.eventsAfter(
                started.value(QStringLiteral("session_id")).toString(),
                started.value(QStringLiteral("session_generation")).toInteger(), 0);
            for (const QJsonValue& value : events) {
                const QJsonObject event = value.toObject();
                if (event.value(QStringLiteral("event_type")).toString() ==
                    QStringLiteral("user.message"))
                    return event.value(QStringLiteral("payload")).toObject();
            }
            return QJsonObject{};
        };
        const QJsonObject echoed =
            startAndReadUserMessage(QStringLiteral("start-client-id"), QStringLiteral("local-1"));
        QCOMPARE(echoed.value(QStringLiteral("text")).toString(), QStringLiteral("typed prompt"));
        QCOMPARE(echoed.value(QStringLiteral("client_message_id")).toString(),
                 QStringLiteral("local-1"));

        QTemporaryDir other;
        QVERIFY(other.isValid());
        const QJsonObject oversized = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-long-client-id"),
                    {{QStringLiteral("worktree_path"), other.path()},
                     {QStringLiteral("prompt"), QStringLiteral("prompt")},
                     {QStringLiteral("client_message_id"), QString(129, QLatin1Char('x'))}}),
            QStringLiteral("desktop-test"));
        QVERIFY(oversized.value(QStringLiteral("ok")).toBool());
        for (const QJsonValue& value : journal.eventsAfter(
                 oversized.value(QStringLiteral("session_id")).toString(),
                 oversized.value(QStringLiteral("session_generation")).toInteger(), 0)) {
            QVERIFY(!value.toObject()
                         .value(QStringLiteral("payload"))
                         .toObject()
                         .contains(QStringLiteral("client_message_id")));
        }
    }

    void restartInterruptsTurnsWithoutProvider() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("journal.sqlite3"));
        const auto stored = [&directory](const QString& id, const QString& state) {
            StoredSession session;
            session.id = id;
            session.worktreePath = directory.path();
            session.adapter = QStringLiteral("codex");
            session.threadId = QStringLiteral("provider-%1").arg(id);
            session.turnId = QStringLiteral("turn-%1").arg(id);
            session.state = state;
            return session;
        };
        LocalJournal journal(path);
        QVERIFY(
            journal.upsertSession(stored(QStringLiteral("running"), QStringLiteral("running"))));
        QVERIFY(journal.upsertSession(
            stored(QStringLiteral("recovering"), QStringLiteral("recovering"))));
        CodexAdapter adapter(QStringLiteral("/missing-codex-for-restart-interrupt"));
        SessionHost host(&journal, &adapter);
        int checked = 0;
        for (const StoredSession& session : journal.sessions()) {
            QCOMPARE(session.state, QStringLiteral("interrupted"));
            QVERIFY(session.turnId.isEmpty());
            ++checked;
        }
        QCOMPARE(checked, 2);
    }

    void renamesSessionBeforeProviderThreadIsReady() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        const QString providerLog = directory.filePath(QStringLiteral("provider.log"));
        QVERIFY(writeFakeCodex(executable, providerLog));

        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);

        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-rename"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"),
                      QStringLiteral("# Long handoff\nPrevious context.\n"
                                     "User request: Fix the conversation title\n"
                                     "<ketplus_response_preferences>Do not use this as a title")}}),
            QStringLiteral("desktop-test"));
        QVERIFY(started.value(QStringLiteral("ok")).toBool());
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        QVERIFY(!sessionId.isEmpty());
        const QJsonObject initial = host.handleCommand(
            request(QStringLiteral("session.list"), QStringLiteral("list-initial")),
            QStringLiteral("desktop-test"));
        QCOMPARE(initial.value(QStringLiteral("sessions"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("title"))
                     .toString(),
                 QStringLiteral("Fix the conversation title"));

        const QJsonObject renamed = host.handleCommand(
            request(QStringLiteral("session.rename"), QStringLiteral("rename-session"),
                    {{QStringLiteral("title"), QStringLiteral("Persistent Codex title")}},
                    sessionId, 1),
            QStringLiteral("desktop-test"));
        QVERIFY2(renamed.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(renamed).toJson(QJsonDocument::Compact)));

        const QJsonObject listed = host.handleCommand(
            request(QStringLiteral("session.list"), QStringLiteral("list-renamed")),
            QStringLiteral("desktop-test"));
        QCOMPARE(listed.value(QStringLiteral("sessions"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("title"))
                     .toString(),
                 QStringLiteral("Persistent Codex title"));

        bool foundRenameEvent = false;
        for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
            const QJsonObject event = value.toObject();
            if (event.value(QStringLiteral("event_type")).toString() ==
                QStringLiteral("session.renamed")) {
                foundRenameEvent = true;
                QCOMPARE(event.value(QStringLiteral("payload"))
                             .toObject()
                             .value(QStringLiteral("title"))
                             .toString(),
                         QStringLiteral("Persistent Codex title"));
            }
        }
        QVERIFY(foundRenameEvent);

        const auto providerNameWasUpdated = [&providerLog] {
            QFile log(providerLog);
            return log.open(QIODevice::ReadOnly) &&
                   log.readAll().contains("name:thread-1:Persistent Codex title");
        };
        QTRY_VERIFY_WITH_TIMEOUT(providerNameWasUpdated(), 3000);

        emit adapter.notificationReceived(
            QStringLiteral("thread/name/updated"),
            {{QStringLiteral("threadId"), QStringLiteral("thread-1")},
             {QStringLiteral("threadName"), QStringLiteral("Provider-side title")}});
        const QJsonObject providerRenamed = host.handleCommand(
            request(QStringLiteral("session.list"), QStringLiteral("list-provider-renamed")),
            QStringLiteral("desktop-test"));
        QCOMPARE(providerRenamed.value(QStringLiteral("sessions"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("title"))
                     .toString(),
                 QStringLiteral("Provider-side title"));
        emit adapter.notificationReceived(
            QStringLiteral("thread/name/updated"),
            {{QStringLiteral("threadId"), QStringLiteral("thread-1")},
             {QStringLiteral("threadName"),
              QStringLiteral("# Handoff\nUser request: Fix the diff gutter\n"
                             "<ketplus_response_preferences>More instructions")}});
        const auto shortProviderTitle = host.handleCommand(
            request(QStringLiteral("session.list"), QStringLiteral("list-short-provider-title")),
            QStringLiteral("desktop-test"));
        QCOMPARE(shortProviderTitle.value(QStringLiteral("sessions"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("title"))
                     .toString(),
                 QStringLiteral("Fix the diff gutter"));

        // SessionHost does not own the adapter. Drain queued provider callbacks
        // while the host is still alive, matching DaemonService teardown order.
        adapter.stop();
    }

    void preservesStreamAcrossJournalFailure_data() {
        QTest::addColumn<QString>("boundary");
        for (const auto& value : {"timer", "item", "event", "command", "cap", "delete",
                                  "subscriber-delete", "shutdown", "oversized"})
            QTest::newRow(value) << QString(value);
    }
    void preservesStreamAcrossJournalFailure() {
        QFETCH(QString, boundary);
        QTemporaryDir dir;
        const auto executable = dir.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, dir.filePath("provider.log")));
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        CodexAdapter adapter(executable);
        auto owner = std::make_unique<SessionHost>(&journal, &adapter);
        auto& host = *owner;
        const auto stopProvider = qScopeGuard([&] { adapter.stop(); });
        const auto started =
            host.handleCommand(request("session.start_turn", "fault-stream",
                                       {{"worktree_path", dir.path()}, {"prompt", "seed"}}),
                               "desktop-test");
        QVERIFY(started.value("ok").toBool());
        const auto id = started.value("session_id").toString();
        const auto completed = [&] {
            for (const auto& value : journal.eventsAfter(id, 1, 0))
                if (value.toObject().value("event_type") == "turn.completed")
                    return true;
            return false;
        };
        QTRY_VERIFY(completed());
        const auto baseline = journal.sessionHead(id).cursor;
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        QVERIFY(q.exec("CREATE TRIGGER fail_stream BEFORE INSERT ON events WHEN "
                       "NEW.event_type='assistant.delta' BEGIN SELECT "
                       "RAISE(ABORT,'synthetic_stream_failure'); END"));
        const auto clearFault = qScopeGuard([&] {
            QSqlQuery clean(db);
            clean.exec("DROP TRIGGER IF EXISTS fail_stream");
        });
        const auto delta = [&](const QString& item, const QString& text) {
            emit adapter.notificationReceived(
                "item/agentMessage/delta",
                {{"threadId", "thread-1"}, {"itemId", item}, {"delta", text}});
        };
        delta("fault-answer", "prefix-");
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression("Unable to journal assistant\\.delta.*synthetic_stream_failure.*"));
        if (boundary == "timer" || boundary == "command" || boundary == "cap" ||
            boundary == "delete")
            QTest::qWait(250);
        else if (boundary == "item")
            delta("fault-second", "tail");
        else
            emit adapter.notificationReceived(
                "item/started",
                {{"threadId", "thread-1"},
                 {"item",
                  QJsonObject{{"id", "marker"}, {"type", "mcpToolCall"}, {"tool", "read_file"}}}});
        if (boundary == "oversized") {
            QTest::ignoreMessage(
                QtWarningMsg,
                QRegularExpression("Unable to retain journal event .*recovery limit reached"));
            emit adapter.notificationReceived(
                "item/completed",
                {{"threadId", "thread-1"},
                 {"item", QJsonObject{{"id", "too-large-command"},
                                      {"type", "commandExecution"},
                                      {"aggregatedOutput", QString(300 * 1024, 'x')}}}});
            emit adapter.notificationReceived("item/started",
                                              {{"threadId", "thread-1"},
                                               {"item", QJsonObject{{"id", "after-rejection"},
                                                                    {"type", "mcpToolCall"},
                                                                    {"tool", "read_file"}}}});
        }
        if (boundary == "command") {
            const auto rejected = host.handleCommand(
                request("session.start_turn", "must-not-send",
                        {{"worktree_path", dir.path()}, {"prompt", "must-not-send"}}, id, 1),
                "desktop-test");
            QVERIFY(!rejected.value("ok").toBool());
            QCOMPARE(rejected.value("error_code").toString(), QString("journal_error"));
        }
        if (boundary == "cap") {
            QTest::ignoreMessage(
                QtWarningMsg,
                QRegularExpression("Unable to retain journal event .*recovery limit reached"));
            for (int n = 1; n <= 129; ++n)
                delta(QString("queued-%1").arg(n), "tail");
        }
        bool deleted = false;
        if (boundary == "subscriber-delete") {
            connect(&host, &SessionHost::eventReady, &host, [&](const QJsonObject& event) {
                if (event.value("session_id").toString() == id &&
                    event.value("event_type") == "assistant.delta" &&
                    event.value("payload").toObject().value("item_id") == "fault-answer") {
                    const auto result = host.handleCommand(
                        request("session.delete", "delete-on-retry", {}, id, 1), "desktop-test");
                    deleted = result.value("ok").toBool();
                }
            });
        }
        // A later event must not overtake the failed prefix.
        QCOMPARE(journal.eventsAfter(id, 1, baseline).size(), 0);
        if (boundary == "delete") {
            const auto result = host.handleCommand(
                request("session.delete", "delete-backlog", {}, id, 1), "desktop-test");
            QVERIFY(result.value("ok").toBool());
        }
        QVERIFY(q.exec("DROP TRIGGER fail_stream"));
        if (boundary == "delete" || boundary == "subscriber-delete") {
            if (boundary == "subscriber-delete")
                QTRY_VERIFY_WITH_TIMEOUT(deleted, 3000);
            QTest::qWait(1200);
            QCOMPARE(journal.sessionHead(id).generation, qint64(0));
            QVERIFY(journal.eventsAfter(id, 1, 0).isEmpty());
            const auto fresh =
                host.handleCommand(request("session.start_turn", "after-delete",
                                           {{"worktree_path", dir.path()}, {"prompt", "fresh"}}),
                                   "desktop-test");
            QVERIFY(fresh.value("ok").toBool());
            return;
        }
        if (boundary == "shutdown")
            owner.reset();
        if (boundary == "timer")
            delta("fault-answer", "tail");
        const int expected = boundary == "oversized"                          ? 3
                             : boundary == "cap"                              ? 128
                             : (boundary == "timer" || boundary == "command") ? 1
                                                                              : 2;
        QTRY_COMPARE_WITH_TIMEOUT(journal.eventsAfter(id, 1, baseline).size(), expected, 10000);
        const auto events = journal.eventsAfter(id, 1, baseline);
        QCOMPARE(events.at(0).toObject().value("event_type").toString(),
                 QString("assistant.delta"));
        const auto first = events.at(0).toObject().value("payload").toObject();
        QCOMPARE(first.value("item_id").toString(), QString("fault-answer"));
        QCOMPARE(first.value("delta").toString(),
                 boundary == "timer" ? QString("prefix-tail") : QString("prefix-"));
        if (boundary == "item")
            QCOMPARE(events.at(1).toObject().value("payload").toObject().value("delta").toString(),
                     QString("tail"));
        if (boundary == "event" || boundary == "shutdown")
            QCOMPARE(events.at(1).toObject().value("event_type").toString(),
                     QString("item.started"));
        if (boundary == "cap")
            QCOMPARE(
                events.last().toObject().value("payload").toObject().value("item_id").toString(),
                QString("queued-127"));
        if (boundary == "command")
            for (const auto& event : events)
                QVERIFY(!QJsonDocument(event.toObject())
                             .toJson(QJsonDocument::Compact)
                             .contains("must-not-send"));
        LocalJournal reopened(path);
        QVERIFY(reopened.isOpen());
        QCOMPARE(reopened.eventsAfter(id, 1, baseline), events);
    }

    void boundsJournalRecoveryAcrossSessions_data() {
        QTest::addColumn<QString>("budget");
        for (const auto& value :
             {"global-events", "delete-releases", "session-bytes", "global-bytes"})
            QTest::newRow(value) << QString(value);
    }
    void boundsJournalRecoveryAcrossSessions() {
        QFETCH(QString, budget);
        QTemporaryDir dir;
        const auto executable = dir.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, dir.filePath("provider.log")));
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto stopProvider = qScopeGuard([&] { adapter.stop(); });
        const bool bytes = budget.endsWith("bytes");
        const int count = budget == "session-bytes" ? 1 : bytes ? 3 : 5;
        QStringList ids;
        QList<qint64> baselines;
        for (int n = 0; n < count; ++n) {
            const auto started =
                host.handleCommand(request("session.start_turn", QString("seed-%1").arg(n),
                                           {{"worktree_path", dir.path()}, {"prompt", "seed"}}),
                                   "desktop-test");
            QVERIFY(started.value("ok").toBool());
            const auto id = started.value("session_id").toString();
            const auto completed = [&] {
                for (const auto& value : journal.eventsAfter(id, 1, 0))
                    if (value.toObject().value("event_type") == "turn.completed")
                        return true;
                return false;
            };
            QTRY_VERIFY(completed());
            ids.append(id);
            baselines.append(journal.sessionHead(id).cursor);
        }
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        QVERIFY(q.exec("CREATE TRIGGER fail_stream BEFORE INSERT ON events WHEN "
                       "NEW.event_type='assistant.delta' BEGIN SELECT "
                       "RAISE(ABORT,'synthetic_stream_failure'); END"));
        const auto clearFault = qScopeGuard([&] {
            QSqlQuery clean(db);
            clean.exec("DROP TRIGGER IF EXISTS fail_stream");
        });
        const QString large = bytes ? QString(19 * 1024 * 1024 / 2, 'x') : QString();
        for (int n = 0; n < count; ++n) {
            if (budget == "delete-releases" && n == 4) {
                const auto deleted = host.handleCommand(
                    request("session.delete", "free-budget", {}, ids[0], 1), "desktop-test");
                QVERIFY(deleted.value("ok").toBool());
            }
            const auto thread = QString("thread-%1").arg(n + 1);
            const auto delta = [&](int item) {
                emit adapter.notificationReceived("item/agentMessage/delta",
                                                  {{"threadId", thread},
                                                   {"itemId", QString("buffer-%1").arg(item)},
                                                   {"delta", "prefix"}});
            };
            QTest::ignoreMessage(
                QtWarningMsg,
                QRegularExpression(
                    "Unable to journal assistant\\.delta.*synthetic_stream_failure.*"));
            const bool full = budget == "session-bytes" || (budget == "global-bytes" && n == 2) ||
                              (budget == "global-events" && n == 4);
            if (full)
                QTest::ignoreMessage(
                    QtWarningMsg,
                    QRegularExpression("Unable to retain journal event .*recovery limit reached"));
            delta(0);
            if (bytes) {
                const int largeCount = budget == "session-bytes" ? 3 : 2;
                for (int item = 0; item < largeCount; ++item)
                    emit adapter.notificationReceived(
                        "item/completed",
                        {{"threadId", thread},
                         {"item", QJsonObject{{"id", QString("large-%1").arg(item)},
                                              {"type", "agentMessage"},
                                              {"text", large}}}});
            } else {
                for (int item = 1; item < 128; ++item)
                    delta(item);
            }
        }
        QVERIFY(q.exec("DROP TRIGGER fail_stream"));
        const auto rowCount = [&](const QString& sql, const QString& id) -> qint64 {
            QSqlQuery read(db);
            if (!read.prepare(sql))
                return -1;
            read.addBindValue(id);
            if (!read.exec() || !read.next())
                return -1;
            return read.value(0).toLongLong();
        };
        for (int n = 0; n < count; ++n) {
            if (budget == "delete-releases" && n == 0) {
                QCOMPARE(journal.sessionHead(ids[n]).generation, qint64(0));
                continue;
            }
            if (bytes) {
                // Count metadata only; do not repeatedly hydrate the 19 MiB bodies.
                const int expected = budget == "global-bytes" && n == 2 ? 0 : 2;
                QTRY_COMPARE_WITH_TIMEOUT(
                    rowCount("SELECT count(*) FROM large_event_payloads WHERE session_id=?",
                             ids[n]),
                    expected, 15000);
                QTRY_VERIFY_WITH_TIMEOUT(journal.sessionHead(ids[n]).cursor > baselines[n], 15000);
                QCOMPARE(
                    rowCount("SELECT count(*) FROM events WHERE event_type='assistant.delta' AND "
                             "json_extract(payload,'$.item_id')='buffer-0' AND session_id=?",
                             ids[n]),
                    qint64(1));
            } else {
                const int expected = budget == "global-events" && n == 4 ? 1 : 128;
                QTRY_COMPARE_WITH_TIMEOUT(journal.eventsAfter(ids[n], 1, baselines[n]).size(),
                                          expected, 10000);
            }
        }
        QTest::qWait(1200);
        if (budget == "delete-releases")
            QVERIFY(journal.eventsAfter(ids[0], 1, 0).isEmpty());
        if (bytes)
            QCOMPARE(rowCount("SELECT count(*) FROM large_event_payloads WHERE session_id=?",
                              ids.last()),
                     budget == "global-bytes" ? qint64(0) : qint64(2));
    }

    void retriesStreamAfterBusyWriter() {
        QTemporaryDir dir;
        const auto executable = dir.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, dir.filePath("provider.log")));
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto stopProvider = qScopeGuard([&] { adapter.stop(); });
        const auto started =
            host.handleCommand(request("session.start_turn", "busy-seed",
                                       {{"worktree_path", dir.path()}, {"prompt", "seed"}}),
                               "desktop-test");
        QVERIFY(started.value("ok").toBool());
        const auto id = started.value("session_id").toString();
        const auto completed = [&] {
            for (const auto& value : journal.eventsAfter(id, 1, 0))
                if (value.toObject().value("event_type") == "turn.completed")
                    return true;
            return false;
        };
        QTRY_VERIFY(completed());
        const auto baseline = journal.sessionHead(id).cursor;
        const auto connection = QString("busy-stream-%1").arg(id);
        const auto removeConnection =
            qScopeGuard([&] { QSqlDatabase::removeDatabase(connection); });
        auto writer = QSqlDatabase::addDatabase("QSQLITE", connection);
        writer.setDatabaseName(path);
        QVERIFY(writer.open());
        QSqlQuery lock(writer);
        QVERIFY(lock.exec("BEGIN IMMEDIATE"));
        const auto unlock = qScopeGuard([&] { writer.rollback(); });
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression("Unable to journal assistant\\.delta.*locked.*"));
        QElapsedTimer elapsed;
        elapsed.start();
        for (const auto& item : {"before-lock", "after-lock"})
            emit adapter.notificationReceived(
                "item/agentMessage/delta",
                {{"threadId", "thread-1"}, {"itemId", item}, {"delta", "kept"}});
        QVERIFY2(elapsed.elapsed() < 500,
                 "A busy writer must not block the daemon for the SQLite timeout.");
        QCOMPARE(journal.eventsAfter(id, 1, baseline).size(), 0);
        QVERIFY(writer.rollback());
        QTRY_COMPARE_WITH_TIMEOUT(journal.eventsAfter(id, 1, baseline).size(), 2, 4000);
        QCOMPARE(journal.eventsAfter(id, 1, baseline)
                     .first()
                     .toObject()
                     .value("payload")
                     .toObject()
                     .value("item_id")
                     .toString(),
                 QString("before-lock"));
    }

    void coalescesStreamedDeltasPerItem() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(executable, directory.filePath(QStringLiteral("provider.log"))));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        auto host = std::make_unique<SessionHost>(&journal, &adapter);
        const auto stopProvider = qScopeGuard([&adapter] { adapter.stop(); });

        const QJsonObject started = host->handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-deltas"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("Stream")}}),
            QStringLiteral("desktop-test"));
        QVERIFY(started.value(QStringLiteral("ok")).toBool());
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        const auto hasEvent = [&journal, sessionId](const QString& type) {
            for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0))
                if (value.toObject().value(QStringLiteral("event_type")).toString() == type)
                    return true;
            return false;
        };
        QTRY_VERIFY(hasEvent(QStringLiteral("turn.completed")));
        const QJsonArray before = journal.eventsAfter(sessionId, 1, 0);
        const qint64 baseline =
            before.last().toObject().value(QStringLiteral("cursor")).toInteger();

        const auto delta = [&adapter](const QString& method, const QString& item,
                                      const QString& text) {
            emit adapter.notificationReceived(
                method, {{QStringLiteral("threadId"), QStringLiteral("thread-1")},
                         {QStringLiteral("itemId"), item},
                         {QStringLiteral("delta"), text}});
        };
        QString expected;
        for (int word = 0; word < 40; ++word) {
            expected += QStringLiteral("w%1 ").arg(word);
            delta(QStringLiteral("item/agentMessage/delta"), QStringLiteral("answer"),
                  QStringLiteral("w%1 ").arg(word));
        }
        // Held back: one provider chunk is not an event of its own.
        QCOMPARE(journal.eventsAfter(sessionId, 1, baseline).size(), 0);
        // Another item, then another kind, each journal what came before them in order.
        delta(QStringLiteral("item/commandExecution/outputDelta"), QStringLiteral("command"),
              QStringLiteral("out1\n"));
        delta(QStringLiteral("item/commandExecution/outputDelta"), QStringLiteral("command"),
              QStringLiteral("out2\n"));
        delta(QStringLiteral("item/agentMessage/delta"), QStringLiteral("later"),
              QStringLiteral("x"));
        delta(QStringLiteral("item/agentMessage/delta"), QStringLiteral("later"),
              QStringLiteral("y"));
        QTRY_COMPARE(journal.eventsAfter(sessionId, 1, baseline).size(), 3);
        const QJsonArray events = journal.eventsAfter(sessionId, 1, baseline);
        const auto payload = [&events](int index) {
            return events.at(index).toObject().value(QStringLiteral("payload")).toObject();
        };
        QCOMPARE(events.at(0).toObject().value(QStringLiteral("event_type")).toString(),
                 QStringLiteral("assistant.delta"));
        QCOMPARE(payload(0).value(QStringLiteral("item_id")).toString(), QStringLiteral("answer"));
        QCOMPARE(payload(0).value(QStringLiteral("delta")).toString(), expected);
        QCOMPARE(events.at(1).toObject().value(QStringLiteral("event_type")).toString(),
                 QStringLiteral("command.output_delta"));
        QCOMPARE(payload(1).value(QStringLiteral("delta")).toString(),
                 QStringLiteral("out1\nout2\n"));
        QCOMPARE(payload(2).value(QStringLiteral("item_id")).toString(), QStringLiteral("later"));
        QCOMPARE(payload(2).value(QStringLiteral("delta")).toString(), QStringLiteral("xy"));

        // Text still held when the daemon stops is journaled, not lost.
        delta(QStringLiteral("item/agentMessage/delta"), QStringLiteral("later"),
              QStringLiteral("z"));
        adapter.stop();
        host.reset();
        const QJsonArray after = journal.eventsAfter(sessionId, 1, baseline);
        QCOMPARE(after.size(), 4);
        QCOMPARE(after.at(3)
                     .toObject()
                     .value(QStringLiteral("payload"))
                     .toObject()
                     .value(QStringLiteral("delta"))
                     .toString(),
                 QStringLiteral("z"));
    }

    void journalsCodexContextWindowUsage() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        const QString providerLog = directory.filePath(QStringLiteral("provider.log"));
        QVERIFY(writeFakeCodex(executable, providerLog));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto stopProvider = qScopeGuard([&adapter] { adapter.stop(); });

        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-context"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("Measure context")}}),
            QStringLiteral("desktop-test"));
        QVERIFY(started.value(QStringLiteral("ok")).toBool());
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        QVERIFY(!sessionId.isEmpty());
        const auto turnFinished = [&journal, sessionId] {
            for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
                if (value.toObject().value(QStringLiteral("event_type")).toString() ==
                    QStringLiteral("turn.completed"))
                    return true;
            }
            return false;
        };
        QTRY_VERIFY(turnFinished());

        emit adapter.notificationReceived(
            QStringLiteral("thread/tokenUsage/updated"),
            {{QStringLiteral("threadId"), QStringLiteral("thread-1")},
             {QStringLiteral("tokenUsage"),
              QJsonObject{
                  {QStringLiteral("last"), QJsonObject{{QStringLiteral("totalTokens"), 5168}}},
                  {QStringLiteral("total"), QJsonObject{{QStringLiteral("totalTokens"), 90000}}},
                  {QStringLiteral("modelContextWindow"), 258400}}}});
        bool found = false;
        for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
            const QJsonObject event = value.toObject();
            if (event.value(QStringLiteral("event_type")).toString() !=
                QStringLiteral("usage.context"))
                continue;
            found = true;
            const QJsonObject usage = event.value(QStringLiteral("payload"))
                                          .toObject()
                                          .value(QStringLiteral("tokenUsage"))
                                          .toObject();
            QCOMPARE(usage.value(QStringLiteral("last"))
                         .toObject()
                         .value(QStringLiteral("totalTokens"))
                         .toInteger(),
                     5168);
            QCOMPARE(usage.value(QStringLiteral("modelContextWindow")).toInteger(), 258400);
        }
        QVERIFY(found);
    }

    void journalsCodexPlanLimitsIntoTheConversationsMidTurn() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(executable, directory.filePath(QStringLiteral("provider.log"))));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto stopProvider = qScopeGuard([&adapter] { adapter.stop(); });

        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-limits"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("Use the plan")}}),
            QStringLiteral("desktop-test"));
        QVERIFY(started.value(QStringLiteral("ok")).toBool());
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        const auto limitEvents = [&journal, sessionId] {
            QList<QJsonObject> found;
            for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
                const QJsonObject event = value.toObject();
                if (event.value(QStringLiteral("event_type")).toString() ==
                    QStringLiteral("usage.limits"))
                    found.append(event.value(QStringLiteral("payload")).toObject());
            }
            return found;
        };
        const auto turnFinished = [&journal, sessionId] {
            for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
                if (value.toObject().value(QStringLiteral("event_type")).toString() ==
                    QStringLiteral("turn.completed"))
                    return true;
            }
            return false;
        };
        QTRY_VERIFY(turnFinished());

        const QJsonObject window5h{{QStringLiteral("usedPercent"), 31},
                                   {QStringLiteral("windowDurationMins"), 300},
                                   {QStringLiteral("resetsAt"), 1790506800}};
        const QJsonObject window7d{{QStringLiteral("usedPercent"), 8.4},
                                   {QStringLiteral("windowDurationMins"), 10080},
                                   {QStringLiteral("resetsAt"), 1791072000}};
        const QJsonObject limits{
            {QStringLiteral("rateLimits"),
             QJsonObject{{QStringLiteral("limitId"), QStringLiteral("other")}}},
            {QStringLiteral("rateLimitsByLimitId"),
             QJsonObject{{QStringLiteral("codex"),
                          QJsonObject{{QStringLiteral("limitId"), QStringLiteral("codex")},
                                      {QStringLiteral("primary"), window5h},
                                      {QStringLiteral("secondary"), window7d}}}}}};

        // An idle conversation records nothing: its cursor moves only when it has work.
        emit adapter.notificationReceived(QStringLiteral("account/rateLimits/updated"), limits);
        QVERIFY(limitEvents().isEmpty());

        emit adapter.notificationReceived(
            QStringLiteral("turn/started"),
            {{QStringLiteral("threadId"), QStringLiteral("thread-1")},
             {QStringLiteral("turn"),
              QJsonObject{{QStringLiteral("id"), QStringLiteral("turn-2")}}}});
        emit adapter.notificationReceived(QStringLiteral("account/rateLimits/updated"), limits);
        const QList<QJsonObject> recorded = limitEvents();
        QCOMPARE(recorded.size(), 1);
        QCOMPARE(recorded.first().value(QStringLiteral("provider")).toString(),
                 QStringLiteral("codex"));
        const QJsonArray rows = recorded.first().value(QStringLiteral("limits")).toArray();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).toObject().value(QStringLiteral("label")).toString(),
                 QStringLiteral("Session · 5 hours"));
        QCOMPARE(rows.at(0).toObject().value(QStringLiteral("usedPercent")).toDouble(), 31.0);
        QCOMPARE(rows.at(0).toObject().value(QStringLiteral("resetsAt")).toInteger(), 1790506800);
        QCOMPARE(rows.at(1).toObject().value(QStringLiteral("id")).toString(),
                 QStringLiteral("seven_day"));
        QCOMPARE(rows.at(1).toObject().value(QStringLiteral("label")).toString(),
                 QStringLiteral("Weekly · all models"));
    }

    void userConfiguredBrowserServerIsNotAutoApproved() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        const QString providerLog = directory.filePath(QStringLiteral("provider.log"));
        QVERIFY(writeFakeCodex(executable, providerLog));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        // Browser tools disabled / unresolved: nothing injected, so a `ketplus_browser` server
        // can only come from the user's ~/.codex/config.toml.
        adapter.setBrowserArgumentsProvider([] { return QStringList{}; });
        SessionHost host(&journal, &adapter);
        const auto logText = [&providerLog] {
            QFile log(providerLog);
            return log.open(QIODevice::ReadOnly) ? log.readAll() : QByteArray();
        };
        const auto events = [&journal](const QString& id, const QString& type) {
            QJsonArray matches;
            for (const QJsonValue& value : journal.eventsAfter(id, 1, 0)) {
                if (value.toObject().value(QStringLiteral("event_type")).toString() == type)
                    matches.append(value);
            }
            return matches;
        };

        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("mcp-untrusted"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("mcp-untrusted")}}),
            QStringLiteral("desktop-test"));
        QVERIFY(started.value(QStringLiteral("ok")).toBool());
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();

        // An untagged field-less form is not a tool approval: declined and journaled.
        QTRY_VERIFY_WITH_TIMEOUT(logText().contains(R"(response:untagged:{"action": "decline"})"),
                                 3000);
        QTRY_COMPARE_WITH_TIMEOUT(events(sessionId, "provider.request_declined").size(), 1, 2000);
        QCOMPARE(
            events(sessionId, "provider.request_declined").first()["payload"]["reason"].toString(),
            "unsupported_elicitation");
        // The tagged ketplus_browser approval waits for a card instead of being auto-accepted.
        QTRY_COMPARE_WITH_TIMEOUT(events(sessionId, "approval.requested").size(), 1, 2000);
        QVERIFY(!logText().contains("response:browser:"));
        const QJsonObject card =
            events(sessionId, "approval.requested").first()["payload"].toObject();
        QCOMPARE(card.value(QStringLiteral("server_name")).toString(), "ketplus_browser");
        const QJsonObject resolved = host.handleCommand(
            request(QStringLiteral("approval.resolve"), QStringLiteral("resolve-untrusted"),
                    {{QStringLiteral("approval_id"), card.value("approval_id")},
                     {QStringLiteral("resolution"),
                      QJsonObject{{QStringLiteral("decision"), QStringLiteral("decline")}}}},
                    sessionId, 1),
            QStringLiteral("desktop-test"));
        QVERIFY(resolved.value(QStringLiteral("ok")).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(logText().contains(R"(response:browser:{"action": "decline"})"),
                                 2000);
        QTRY_COMPARE_WITH_TIMEOUT(events(sessionId, "turn.completed").size(), 1, 2000);
        adapter.stop();
    }

    void answersMcpApprovalsWithElicitationShapes() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        const QString providerLog = directory.filePath(QStringLiteral("provider.log"));
        QVERIFY(writeFakeCodex(executable, providerLog));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        // Browser tools resolved: the launch carries KetPlus' own ketplus_browser server.
        adapter.setBrowserArgumentsProvider([] {
            return QStringList{QStringLiteral("-c"),
                               QStringLiteral("mcp_servers.ketplus_browser.command=\"node\"")};
        });
        SessionHost host(&journal, &adapter);
        const auto logText = [&providerLog] {
            QFile log(providerLog);
            return log.open(QIODevice::ReadOnly) ? log.readAll() : QByteArray();
        };
        const auto events = [&journal](const QString& id, const QString& type) {
            QJsonArray matches;
            for (const QJsonValue& value : journal.eventsAfter(id, 1, 0)) {
                if (value.toObject().value(QStringLiteral("event_type")).toString() == type)
                    matches.append(value);
            }
            return matches;
        };

        for (const QString decision : {QStringLiteral("accept"), QStringLiteral("decline")}) {
            QFile::remove(providerLog);
            const QJsonObject started = host.handleCommand(
                request(QStringLiteral("session.start_turn"), QStringLiteral("mcp-") + decision,
                        {{QStringLiteral("worktree_path"), directory.path()},
                         {QStringLiteral("prompt"), QStringLiteral("mcp-requests")}}),
                QStringLiteral("desktop-test"));
            QVERIFY(started.value(QStringLiteral("ok")).toBool());
            const QString sessionId = started.value(QStringLiteral("session_id")).toString();

            // ketplus_browser is approved without a card; unsupported requests are declined
            // in their own response shape and journaled as a notice.
            QTRY_VERIFY_WITH_TIMEOUT(
                logText().contains(R"(response:browser:{"action": "accept", "content": {}})"),
                3000);
            QTRY_VERIFY_WITH_TIMEOUT(logText().contains(R"(response:unknown:{"permissions": {}})"),
                                     2000);
            QTRY_VERIFY_WITH_TIMEOUT(logText().contains(R"(response:url:{"action": "decline"})"),
                                     2000);
            QTRY_COMPARE_WITH_TIMEOUT(events(sessionId, "approval.requested").size(), 1, 2000);
            const QJsonObject card = events(sessionId, "approval.requested")
                                         .first()
                                         .toObject()
                                         .value(QStringLiteral("payload"))
                                         .toObject();
            QCOMPARE(card.value(QStringLiteral("request_type")).toString(), "mcp_tool_call");
            QCOMPARE(card.value(QStringLiteral("server_name")).toString(), "docs");
            const QJsonArray declined = events(sessionId, "provider.request_declined");
            QCOMPARE(declined.size(), 2);
            QCOMPARE(declined.at(0)["payload"]["method"].toString(),
                     "item/permissions/requestApproval");
            QCOMPARE(declined.at(1)["payload"]["reason"].toString(), "unsupported_elicitation");
            QCOMPARE(declined.at(1)["payload"]["server_name"].toString(), "docs");

            const QJsonObject resolved = host.handleCommand(
                request(QStringLiteral("approval.resolve"), QStringLiteral("resolve-") + decision,
                        {{QStringLiteral("approval_id"), card.value("approval_id")},
                         {QStringLiteral("resolution"),
                          QJsonObject{{QStringLiteral("decision"), decision}}}},
                        sessionId, 1),
                QStringLiteral("desktop-test"));
            QVERIFY2(resolved.value(QStringLiteral("ok")).toBool(),
                     qPrintable(QJsonDocument(resolved).toJson(QJsonDocument::Compact)));
            const QByteArray expected =
                decision == QStringLiteral("accept")
                    ? R"(response:other:{"action": "accept", "content": {}})"
                    : R"(response:other:{"action": "decline"})";
            QTRY_VERIFY_WITH_TIMEOUT(logText().contains(expected), 2000);
            // The journal keeps the decision, so a restored transcript says how it ended.
            QCOMPARE(events(sessionId, "approval.resolved")
                         .first()["payload"]["answers"]["decision"]
                         .toString(),
                     decision);
            QTRY_COMPARE_WITH_TIMEOUT(events(sessionId, "turn.completed").size(), 1, 2000);
            QCOMPARE(events(sessionId, "runtime.error").size(), 0);
        }
        adapter.stop();
    }

    void closesBrowserSessionsWhenATurnThatUsedThemEnds() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        const QString providerLog = directory.filePath(QStringLiteral("provider.log"));
        QVERIFY(writeFakeCodex(executable, providerLog));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        adapter.setBrowserArgumentsProvider([] {
            return QStringList{QStringLiteral("-c"),
                               QStringLiteral("mcp_servers.ketplus_browser.command=\"node\"")};
        });
        SessionHost host(&journal, &adapter);
        const auto toolCalls = [&providerLog] {
            QFile log(providerLog);
            QList<QJsonObject> calls;
            if (!log.open(QIODevice::ReadOnly))
                return calls;
            for (const QByteArray& line : log.readAll().split('\n'))
                if (line.startsWith("toolcall:"))
                    calls.append(QJsonDocument::fromJson(line.mid(9)).object());
            return calls;
        };
        const auto runTurn = [&](const QString& prompt) {
            const QJsonObject started = host.handleCommand(
                request(QStringLiteral("session.start_turn"), QStringLiteral("turn-") + prompt,
                        {{QStringLiteral("worktree_path"), directory.path()},
                         {QStringLiteral("prompt"), prompt}}),
                QStringLiteral("desktop-test"));
            QVERIFY(started.value(QStringLiteral("ok")).toBool());
            const QString sessionId = started.value(QStringLiteral("session_id")).toString();
            QTRY_VERIFY_WITH_TIMEOUT(
                [&] {
                    for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0))
                        if (value["event_type"].toString() == QStringLiteral("turn.completed"))
                            return true;
                    return false;
                }(),
                3000);
        };

        // A turn that called the browser closes what it left open, on its own thread.
        runTurn(QStringLiteral("browser-tool"));
        QTRY_COMPARE_WITH_TIMEOUT(toolCalls().size(), 1, 2000);
        const QJsonObject call = toolCalls().first();
        QCOMPARE(call.value(QStringLiteral("server")).toString(), "ketplus_browser");
        QCOMPARE(call.value(QStringLiteral("tool")).toString(), "browser_close_all_sessions");
        QVERIFY(call.value(QStringLiteral("threadId")).toString().startsWith("thread-"));

        // Turns that never touched the browser do not start its server just to close nothing.
        runTurn(QStringLiteral("docs-tool"));
        runTurn(QStringLiteral("plain"));
        QTest::qWait(200);
        QCOMPARE(toolCalls().size(), 1);
        adapter.stop();
    }

    void journalsMcpImagesAsMediaRefsWithoutBase64() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        const QString providerLog = directory.filePath(QStringLiteral("provider.log"));
        QVERIFY(writeFakeCodex(executable, providerLog));

        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-media"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("Capture the page")}}),
            QStringLiteral("desktop-test"));
        QVERIFY(started.value(QStringLiteral("ok")).toBool());
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        const auto threadReady = [&journal, &sessionId] {
            for (const StoredSession& session : journal.sessions()) {
                if (session.id == sessionId && !session.threadId.isEmpty())
                    return true;
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(threadReady(), 3000);
        QString threadId;
        for (const StoredSession& session : journal.sessions()) {
            if (session.id == sessionId)
                threadId = session.threadId;
        }

        QByteArray png("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR\0\0\0\x05\0\0\0\x07\x08\x06\0\0\0", 29);
        png.append(QByteArray(64, 'p'));
        const QString base64 = QString::fromLatin1(png.toBase64());
        emit adapter.notificationReceived(
            QStringLiteral("item/completed"),
            {{QStringLiteral("threadId"), threadId},
             {QStringLiteral("item"),
              QJsonObject{{"id", "capture-1"},
                          {"type", "mcpToolCall"},
                          {"server", "ketplus_browser"},
                          {"tool", "browser_capture"},
                          {"status", "completed"},
                          {"result", QJsonObject{{"content", QJsonArray{QJsonObject{
                                                                 {"type", "image"},
                                                                 {"data", base64},
                                                                 {"mimeType", "image/png"}}}}}}}}});

        QJsonObject journaled;
        for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
            const QJsonObject event = value.toObject();
            if (event.value(QStringLiteral("event_type")).toString() == "item.completed" &&
                event.value(QStringLiteral("payload")).toObject().value("item_id") == "capture-1")
                journaled = event.value(QStringLiteral("payload")).toObject();
        }
        QVERIFY(!journaled.isEmpty());
        const QByteArray raw = QJsonDocument(journal.eventsAfter(sessionId, 1, 0)).toJson();
        QVERIFY(!raw.contains(base64.left(24).toLatin1()));
        const QJsonObject block = journaled.value("item")
                                      .toObject()
                                      .value("result")
                                      .toObject()
                                      .value("content")
                                      .toArray()
                                      .first()
                                      .toObject();
        QVERIFY(!block.contains("data"));
        QCOMPARE(block.value("width").toInt(), 5);
        QCOMPARE(block.value("height").toInt(), 7);
        const QString hex = block.value("media_ref").toString().mid(7);
        const QString file = directory.filePath(QStringLiteral("media/%1.png").arg(hex));
        QVERIFY(QFileInfo::exists(file));
        QCOMPARE(QFile::permissions(file) & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
                 QFileDevice::Permissions{});
        adapter.stop();
    }

    void viewedImagesRemainReadableByTheirSessionAfterSourceCleanup() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("browser capture.png");
        QImage image(6, 4, QImage::Format_RGB32);
        image.fill(Qt::blue);
        QVERIFY(image.save(path, "PNG"));
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(journal.isOpen());
        StoredSession session;
        session.id = "image-owner";
        session.worktreePath = directory.path();
        session.adapter = "codex";
        session.threadId = "image-thread";
        session.state = "ready";
        QVERIFY(journal.upsertSession(session));
        StoredSession other = session;
        other.id = "other-session";
        other.threadId = "other-thread";
        QVERIFY(journal.upsertSession(other));
        CodexAdapter adapter("/nonexistent/ketplus-image-test-codex");
        SessionHost host(&journal, &adapter);
        emit adapter.notificationReceived(
            "item/completed",
            {{"threadId", session.threadId},
             {"item", QJsonObject{{"id", "viewed-image"}, {"type", "imageView"}, {"path", path}}}});
        QVERIFY(QFile::remove(path));
        QString reference;
        for (const auto& event : journal.eventsAfter(session.id, 1, 0)) {
            const auto content = event.toObject()
                                     .value("payload")
                                     .toObject()
                                     .value("item")
                                     .toObject()
                                     .value("result")
                                     .toObject()
                                     .value("content")
                                     .toArray();
            if (!content.isEmpty())
                reference = content.first().toObject().value("media_ref").toString();
        }
        QVERIFY(reference.startsWith("sha256:"));
        const QJsonObject payload{{"media_ref", reference}, {"thumbnail", true}};
        const auto preview = host.handleCommand(
            request("session.media.read", "read-viewed", payload, session.id, 1), "phone");
        QVERIFY2(preview.value("ok").toBool(), qPrintable(QJsonDocument(preview).toJson()));
        QCOMPARE(preview.value("mime").toString(), "image/jpeg");
        const auto refused = host.handleCommand(
            request("session.media.read", "read-other", payload, other.id, 1), "phone");
        QCOMPARE(refused.value("error_code").toString(), "media_unavailable");
    }

    void journalsMessageImagesAsMediaRefsThePhoneCanRead() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(executable, directory.filePath(QStringLiteral("provider.log"))));
        QImage image(6, 4, QImage::Format_RGB32);
        image.fill(Qt::red);
        const QString shot = directory.filePath(QStringLiteral("shot.png"));
        QVERIFY(image.save(shot, "PNG"));

        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-image"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("What is this?")},
                     {QStringLiteral("input"),
                      QJsonArray{QJsonObject{{"type", "text"}, {"text", "What is this?"}},
                                 QJsonObject{{"type", "localImage"}, {"path", shot}}}}}),
            QStringLiteral("desktop-test"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(started).toJson(QJsonDocument::Compact)));
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();

        QJsonArray images;
        for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
            const QJsonObject event = value.toObject();
            if (event.value(QStringLiteral("event_type")).toString() == "user.message")
                images =
                    event.value(QStringLiteral("payload")).toObject().value("images").toArray();
        }
        QCOMPARE(images.size(), 1);
        const QJsonObject block = images.first().toObject();
        QCOMPARE(block.value("type").toString(), QStringLiteral("image"));
        QCOMPARE(block.value("mime").toString(), QStringLiteral("image/png"));
        QCOMPARE(block.value("name").toString(), QStringLiteral("shot.png"));
        QCOMPARE(block.value("width").toInt(), 6);
        QVERIFY(!block.contains("path"));
        const QString reference = block.value("media_ref").toString();
        QVERIFY(QFileInfo::exists(
            directory.filePath(QStringLiteral("media/%1.png").arg(reference.mid(7)))));

        // The phone reads it back through the journal check, and nothing unreferenced.
        const QJsonObject preview = host.handleCommand(
            request(QStringLiteral("session.media.read"), QStringLiteral("read-image"),
                    {{QStringLiteral("media_ref"), reference}, {QStringLiteral("thumbnail"), true}},
                    sessionId, 1),
            QStringLiteral("desktop-test"));
        QVERIFY2(preview.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(preview).toJson(QJsonDocument::Compact)));
        const QJsonObject missing = host.handleCommand(
            request(QStringLiteral("session.media.read"), QStringLiteral("read-other"),
                    {{QStringLiteral("media_ref"), QStringLiteral("sha256:") + QString(64, '0')}},
                    sessionId, 1),
            QStringLiteral("desktop-test"));
        QCOMPARE(missing.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("media_unavailable"));
        // The relay path makes the preview off the main thread and answers on it.
        QJsonObject deferred;
        QThread* answeredOn = nullptr;
        host.handleCommandDeferred(
            request(QStringLiteral("session.media.read"), QStringLiteral("read-deferred"),
                    {{QStringLiteral("media_ref"), reference}, {QStringLiteral("thumbnail"), true}},
                    sessionId, 1),
            QStringLiteral("cloud-control-v2"), [&](const QJsonObject& result) {
                deferred = result;
                answeredOn = QThread::currentThread();
            });
        QTRY_VERIFY(!deferred.isEmpty());
        QCOMPARE(answeredOn, QThread::currentThread());
        QCOMPARE(deferred, preview);
        adapter.stop();
    }

    void journalsEachChangedFileByItsWorktreePath() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(executable, directory.filePath(QStringLiteral("provider.log"))));
        const QString worktree = directory.filePath(QStringLiteral("app"));
        QVERIFY(QDir().mkpath(worktree));

        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-edit"),
                    {{QStringLiteral("worktree_path"), worktree},
                     {QStringLiteral("prompt"), QStringLiteral("Edit the files")}}),
            QStringLiteral("desktop-test"));
        QVERIFY(started.value(QStringLiteral("ok")).toBool());
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        StoredSession stored;
        const auto threadReady = [&] {
            for (const StoredSession& session : journal.sessions()) {
                if (session.id == sessionId && !session.threadId.isEmpty()) {
                    stored = session;
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(threadReady(), 3000);

        // Codex names each file by absolute path, whichever worktree it is in.
        const QString root = stored.worktreePath;
        emit adapter.notificationReceived(
            QStringLiteral("item/completed"),
            {{QStringLiteral("threadId"), stored.threadId},
             {QStringLiteral("item"),
              QJsonObject{
                  {"id", "edit-1"},
                  {"type", "fileChange"},
                  {"status", "completed"},
                  {"changes",
                   QJsonArray{QJsonObject{{"path", root + "/apps/ios/App.swift"}, {"diff", ""}},
                              QJsonObject{{"path", "docs/README.md"}, {"diff", ""}},
                              QJsonObject{{"path", root + "-other/KETJS.override"}, {"diff", ""}},
                              QJsonObject{{"path", root + "/../escape.txt"}, {"diff", ""}},
                              QJsonObject{{"path", "/etc/hosts"}, {"diff", ""}}}}}}});

        QJsonArray changes;
        for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
            const QJsonObject payload =
                value.toObject().value(QStringLiteral("payload")).toObject();
            if (payload.value("item_id") == "edit-1")
                changes = payload.value("item").toObject().value("changes").toArray();
        }
        QCOMPARE(changes.size(), 5);
        QCOMPARE(changes[0].toObject().value("worktreePath").toString(), "apps/ios/App.swift");
        QCOMPARE(changes[0].toObject().value("path").toString(), root + "/apps/ios/App.swift");
        QCOMPARE(changes[1].toObject().value("worktreePath").toString(), "docs/README.md");
        for (qsizetype index = 2; index < changes.size(); ++index) {
            const QJsonObject change = changes[index].toObject();
            QVERIFY2(!change.contains("worktreePath"), qPrintable(change.value("path").toString()));
            QCOMPARE(change.value("outsideWorktree").toBool(), true);
        }
        QVERIFY(!changes[0].toObject().contains("outsideWorktree"));
        adapter.stop();
    }

    void readsAWorktreeImageByPathForThePhone() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(executable, directory.filePath(QStringLiteral("provider.log"))));
        const QString worktree = directory.filePath(QStringLiteral("worktree"));
        QVERIFY(QDir().mkpath(worktree + QStringLiteral("/assets")));
        QImage wide(3000, 1000, QImage::Format_ARGB32);
        wide.fill(Qt::transparent);
        QVERIFY(wide.save(worktree + QStringLiteral("/assets/wide.png"), "PNG"));
        QImage outside(4, 4, QImage::Format_RGB32);
        outside.fill(Qt::blue);
        QVERIFY(outside.save(directory.filePath(QStringLiteral("outside.png")), "PNG"));
        QVERIFY(QFile::link(directory.filePath(QStringLiteral("outside.png")),
                            worktree + QStringLiteral("/linked.png")));
        QFile text(worktree + QStringLiteral("/notes.png"));
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.write("not an image");
        text.close();

        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start"),
                    {{QStringLiteral("worktree_path"), worktree},
                     {QStringLiteral("prompt"), QStringLiteral("Look")}}),
            QStringLiteral("desktop-test"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(started).toJson(QJsonDocument::Compact)));
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        int serial = 0;
        const auto read = [&](const QString& path) {
            return host.handleCommand(request(QStringLiteral("session.media.read"),
                                              QStringLiteral("read-path-%1").arg(++serial),
                                              {{QStringLiteral("path"), path}}, sessionId, 1),
                                      QStringLiteral("desktop-test"));
        };

        // Scaled into the preview bound, with the path echoed and no root disclosed.
        const QJsonObject preview = read(QStringLiteral("assets/wide.png"));
        QVERIFY2(preview.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(preview).toJson(QJsonDocument::Compact)));
        QCOMPARE(preview.value(QStringLiteral("path")).toString(),
                 QStringLiteral("assets/wide.png"));
        QCOMPARE(preview.value(QStringLiteral("mime")).toString(), QStringLiteral("image/jpeg"));
        QCOMPARE(preview.value(QStringLiteral("width")).toInt(), 1280);
        QVERIFY(!QJsonDocument(preview).toJson().contains(worktree.toUtf8()));
        const QImage decoded = QImage::fromData(
            QByteArray::fromBase64(preview.value(QStringLiteral("data")).toString().toLatin1(),
                                   QByteArray::Base64UrlEncoding));
        QVERIFY(!decoded.isNull());
        QVERIFY(qGray(decoded.pixel(10, 10)) > 240); // transparent reads as white, not black

        const auto code = [&](const QString& path) {
            return read(path).value(QStringLiteral("error_code")).toString();
        };
        QCOMPARE(code(QStringLiteral("linked.png")), QStringLiteral("symlink_unsupported"));
        QCOMPARE(code(QStringLiteral("../outside.png")), QStringLiteral("invalid_path"));
        QCOMPARE(code(QStringLiteral("/etc/hosts")), QStringLiteral("invalid_path"));
        QCOMPARE(code(QStringLiteral("assets/gone.png")), QStringLiteral("missing"));
        QCOMPARE(code(QStringLiteral("notes.png")), QStringLiteral("media_unavailable"));
        adapter.stop();
    }

    void survivesClientDisconnectAndReplaysWithoutDuplicates() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir().mkpath(directory.filePath("worktree-a")));
        QVERIFY(QDir().mkpath(directory.filePath("worktree-b")));
        const QString executable = directory.filePath("fake-codex");
        const QString providerLog = directory.filePath("provider.log");
        QVERIFY(writeFakeCodex(executable, providerLog));

        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const QByteArray token("local-test-token-with-at-least-32-bytes");
        const QString endpoint = directory.filePath("daemon.sock");
        LocalIpcServer server(&host, endpoint, token);
        QVERIFY2(server.listen(), qPrintable(server.errorString()));

        TestClient unauthenticated;
        unauthenticated.connectTo(endpoint);
        QTRY_COMPARE_WITH_TIMEOUT(unauthenticated.socket.state(), QLocalSocket::ConnectedState,
                                  1000);
        unauthenticated.send(request("session.list", "unauthenticated"));
        QTRY_VERIFY_WITH_TIMEOUT(unauthenticated.hasResponse("unauthenticated"), 1000);
        QCOMPARE(unauthenticated.takeResponse("unauthenticated")
                     .value("payload")
                     .toObject()
                     .value("error_code")
                     .toString(),
                 "authentication_required");

        TestClient first;
        authenticate(first, endpoint, token, "hello-1");
        const QJsonObject startA =
            request("session.start_turn", "start-a",
                    {{"worktree_path", directory.filePath("worktree-a")}, {"prompt", "continue"}});
        first.send(startA);
        QTRY_VERIFY_WITH_TIMEOUT(first.hasResponse("start-a"), 2000);
        const QJsonObject accepted = first.takeResponse("start-a").value("payload").toObject();
        QVERIFY2(accepted.value("ok").toBool(),
                 qPrintable(QJsonDocument(accepted).toJson(QJsonDocument::Compact)));
        const QString sessionA = accepted.value("session_id").toString();
        QCOMPARE(accepted.value("last_event_cursor").toInteger(), 3);

        first.socket.disconnectFromServer();
        QTRY_COMPARE_WITH_TIMEOUT(first.socket.state(), QLocalSocket::UnconnectedState, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(journal.eventsAfter(sessionA, 1, 0).size() >= 5, 3000);

        TestClient second;
        authenticate(second, endpoint, token, "hello-2");
        second.send(request("session.attach", "attach-a", {{"after_cursor", 3}}, sessionA, 1));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("attach-a"), 1000);
        const QJsonArray suffix =
            second.takeResponse("attach-a").value("payload").toObject().value("events").toArray();
        QCOMPARE(suffix.size(), 3);
        QCOMPARE(suffix.at(0).toObject().value("cursor").toInteger(), 4);
        QCOMPARE(suffix.at(1).toObject().value("cursor").toInteger(), 5);
        QCOMPARE(suffix.at(2).toObject().value("cursor").toInteger(), 6);
        QCOMPARE(suffix.at(2).toObject().value("event_type").toString(), "turn.completed");

        second.send(
            request("session.events_after", "replay-a", {{"after_cursor", 3}}, sessionA, 1));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("replay-a"), 1000);
        QCOMPARE(
            second.takeResponse("replay-a").value("payload").toObject().value("events").toArray(),
            suffix);
        second.send(request("client.ack_cursor", "ack-a", {{"cursor", 6}}, sessionA, 1));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("ack-a"), 1000);
        QVERIFY(second.takeResponse("ack-a").value("payload").toObject().value("ok").toBool());
        second.send(
            request("session.events_after", "expired-a", {{"after_cursor", 3}}, sessionA, 1));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("expired-a"), 1000);
        QCOMPARE(second.takeResponse("expired-a")
                     .value("payload")
                     .toObject()
                     .value("error_code")
                     .toString(),
                 "cursor_expired");

        second.send(startA);
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("start-a"), 1000);
        QCOMPARE(second.takeResponse("start-a").value("payload").toObject(), accepted);
        QFile log(providerLog);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QCOMPARE(log.readAll().count("turn:thread-1"), 1);
        log.close();

        second.send(request("session.interrupt", "stale", {}, sessionA, 0));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("stale"), 1000);
        QCOMPARE(
            second.takeResponse("stale").value("payload").toObject().value("error_code").toString(),
            "stale_generation");

        second.send(
            request("session.start_turn", "start-b",
                    {{"worktree_path", directory.filePath("worktree-b")}, {"prompt", "separate"}}));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("start-b"), 2000);
        const QString sessionB = second.takeResponse("start-b")
                                     .value("payload")
                                     .toObject()
                                     .value("session_id")
                                     .toString();
        QVERIFY(!sessionB.isEmpty());
        QVERIFY(sessionB != sessionA);
        QTRY_VERIFY_WITH_TIMEOUT(journal.eventsAfter(sessionB, 1, 0).size() >= 5, 3000);
        const QJsonArray eventsA = journal.eventsAfter(sessionA, 1, 0);
        for (const QJsonValue& event : eventsA) {
            QCOMPARE(event.toObject().value("session_id").toString(), sessionA);
        }

        second.clear();
        second.send(
            request("session.start_turn", "approval-turn", {{"prompt", "approve"}}, sessionA, 1));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("approval-turn"), 2000);
        QVERIFY(
            second.takeResponse("approval-turn").value("payload").toObject().value("ok").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!second.findEvent("approval.requested").isEmpty(), 2000);
        QCOMPARE(second.findEvent("approval.requested").value("turn_sequence").toInteger(), 2);
        const QString approvalId = second.findEvent("approval.requested")
                                       .value("payload")
                                       .toObject()
                                       .value("data")
                                       .toObject()
                                       .value("approval_id")
                                       .toString();
        QVERIFY(!approvalId.isEmpty());

        second.send(request(
            "approval.resolve", "approval-stale",
            {{"approval_id", approvalId}, {"resolution", QJsonObject{{"decision", "accept"}}}},
            sessionA, 0));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("approval-stale"), 1000);
        QCOMPARE(second.takeResponse("approval-stale")
                     .value("payload")
                     .toObject()
                     .value("error_code")
                     .toString(),
                 "stale_generation");

        second.send(request(
            "approval.resolve", "approval-accept",
            {{"approval_id", approvalId}, {"resolution", QJsonObject{{"decision", "accept"}}}},
            sessionA, 1));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("approval-accept"), 1000);
        QVERIFY(second.takeResponse("approval-accept")
                    .value("payload")
                    .toObject()
                    .value("ok")
                    .toBool());

        second.send(request(
            "approval.resolve", "approval-repeat",
            {{"approval_id", approvalId}, {"resolution", QJsonObject{{"decision", "decline"}}}},
            sessionA, 1));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("approval-repeat"), 1000);
        QCOMPARE(second.takeResponse("approval-repeat")
                     .value("payload")
                     .toObject()
                     .value("error_code")
                     .toString(),
                 "already_resolved");
        QTRY_VERIFY_WITH_TIMEOUT(
            journal.eventsAfter(sessionA, 1, 0).last().toObject().value("event_type") ==
                "turn.completed",
            2000);

        second.clear();
        second.send(
            request("session.start_turn", "question-turn", {{"prompt", "ask"}}, sessionA, 1));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("question-turn"), 2000);
        QVERIFY(
            second.takeResponse("question-turn").value("payload").toObject().value("ok").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!second.findEvent("approval.requested").isEmpty(), 2000);
        const QJsonObject questionData = second.findEvent("approval.requested")
                                             .value("payload")
                                             .toObject()
                                             .value("data")
                                             .toObject();
        QCOMPARE(questionData.value("kind").toString(), "input");
        const QString questionRequestId = questionData.value("request_id").toString();
        const QJsonObject question = questionData.value("questions").toArray().first().toObject();
        const QString optionId =
            question.value("options").toArray().first().toObject().value("id").toString();
        QVERIFY(!questionRequestId.isEmpty());
        QVERIFY(!optionId.isEmpty());

        second.send(request("approval.resolve", "question-wrong-kind",
                            {{"approval_id", questionRequestId},
                             {"resolution", QJsonObject{{"decision", "accept"}}}},
                            sessionA, 1));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("question-wrong-kind"), 1000);
        QCOMPARE(second.takeResponse("question-wrong-kind")
                     .value("payload")
                     .toObject()
                     .value("error_code")
                     .toString(),
                 "request_kind_conflict");

        const QJsonObject deferResolution{{"request_id", questionRequestId},
                                          {"revision", questionData.value("revision")},
                                          {"action", "defer"},
                                          {"answers", QJsonArray{}}};
        second.send(request("question.answer", "question-defer",
                            {{"request_id", questionRequestId}, {"resolution", deferResolution}},
                            sessionA, 1));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("question-defer"), 1000);
        QCOMPARE(second.takeResponse("question-defer")
                     .value("payload")
                     .toObject()
                     .value("result_code")
                     .toString(),
                 "deferred");
        QTRY_VERIFY_WITH_TIMEOUT(!second.findEvent("question.deferred").isEmpty(), 1000);

        second.send(request("question.answer", "question-defer-repeat",
                            {{"request_id", questionRequestId}, {"resolution", deferResolution}},
                            sessionA, 1));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("question-defer-repeat"), 1000);
        QCOMPARE(second.takeResponse("question-defer-repeat")
                     .value("payload")
                     .toObject()
                     .value("result_code")
                     .toString(),
                 "already_deferred");
        QCOMPARE(std::count_if(second.messages.cbegin(), second.messages.cend(),
                               [](const QJsonObject& message) {
                                   return message.value("message_type") == "session.event" &&
                                          message.value("payload").toObject().value("event_type") ==
                                              "question.deferred";
                               }),
                 1);

        const QJsonObject resolution{
            {"request_id", questionRequestId},
            {"revision", questionData.value("revision")},
            {"action", "answer"},
            {"answers", QJsonArray{QJsonObject{{"question_id", question.value("id")},
                                               {"question_revision", question.value("revision")},
                                               {"option_ids", QJsonArray{optionId}}}}}};
        second.send(request("question.answer", "question-accept",
                            {{"request_id", questionRequestId}, {"resolution", resolution}},
                            sessionA, 1));
        QTRY_VERIFY_WITH_TIMEOUT(second.hasResponse("question-accept"), 1000);
        QVERIFY(second.takeResponse("question-accept")
                    .value("payload")
                    .toObject()
                    .value("ok")
                    .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!second.findEvent("question.answered").isEmpty(), 1000);
        const QJsonObject answeredData = second.findEvent("question.answered")
                                             .value("payload")
                                             .toObject()
                                             .value("data")
                                             .toObject();
        QCOMPARE(answeredData.value("request_id").toString(), questionRequestId);
        QCOMPARE(answeredData.value("revision").toInt(), 1);
        QVERIFY(QJsonDocument(answeredData)
                    .toJson(QJsonDocument::Compact)
                    .contains("Current workspace"));
        QVERIFY(log.open(QIODevice::ReadOnly));
        const QByteArray providerResults = log.readAll();
        QVERIFY(providerResults.contains("approval:accept"));
        QVERIFY(providerResults.contains("Current workspace"));
    }

    void rejectsFrameSizeBeforeReadingPayload() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        CodexAdapter adapter("/nonexistent/ketplus-codex");
        SessionHost host(&journal, &adapter);
        const QString endpoint = directory.filePath("bounded.sock");
        LocalIpcServer server(&host, endpoint, "local-test-token-with-at-least-32-bytes");
        QVERIFY(server.listen());

        QLocalSocket socket;
        socket.connectToServer(endpoint);
        QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QLocalSocket::ConnectedState, 1000);
        const quint32 size = static_cast<quint32>(protocol::maximumFrameBytes + 1);
        QByteArray header(4, Qt::Uninitialized);
        header[0] = static_cast<char>((size >> 24U) & 0xffU);
        header[1] = static_cast<char>((size >> 16U) & 0xffU);
        header[2] = static_cast<char>((size >> 8U) & 0xffU);
        header[3] = static_cast<char>(size & 0xffU);
        socket.write(header);
        socket.flush();
        QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QLocalSocket::UnconnectedState, 1000);
    }

    void negotiatesProtocolMinorAtHello() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        CodexAdapter adapter("/nonexistent/ketplus-codex");
        SessionHost host(&journal, &adapter);
        const QString endpoint = directory.filePath("minor.sock");
        const QByteArray token("local-test-token-with-at-least-32-bytes");
        LocalIpcServer server(&host, endpoint, token);
        QVERIFY(server.listen());

        const auto hello = [&](const QString& id, const QJsonValue& minor) {
            TestClient client;
            client.connectTo(endpoint);
            [&] {
                QTRY_COMPARE_WITH_TIMEOUT(client.socket.state(), QLocalSocket::ConnectedState,
                                          1000);
            }();
            QJsonObject payload{{"token", QString::fromUtf8(token)}, {"client_id", id}};
            if (!minor.isUndefined())
                payload.insert("protocol_minor", minor);
            client.send(request("daemon.hello", id, payload));
            [&] { QTRY_VERIFY_WITH_TIMEOUT(client.hasResponse(id), 1000); }();
            return client.takeResponse(id).value("payload").toObject();
        };
        // A client from before negotiation gets minor 0 and no features.
        const QJsonObject legacy = hello("legacy", QJsonValue::Undefined);
        QVERIFY(legacy.value("ok").toBool());
        QCOMPARE(legacy.value("protocol_version").toInt(-1), 0);
        QCOMPARE(legacy.value("protocol_minor").toInt(-1), 0);
        QVERIFY(legacy.value("features").toArray().isEmpty());
        // A newer client is answered with the daemon's own minor.
        const QJsonObject newer = hello("newer", 99);
        QCOMPARE(newer.value("protocol_minor").toInt(-1), protocol::currentMinor);
        QVERIFY(newer.value("features").toArray().contains("provider.runtime_status"));
        QVERIFY(newer.value("features").toArray().contains("transcript.window/1"));
    }

    void closesConnectionsThatDoNotCompleteHello() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        CodexAdapter adapter("/nonexistent/ketplus-codex");
        SessionHost host(&journal, &adapter);
        const QString endpoint = directory.filePath("hello.sock");
        const QByteArray token("local-test-token-with-at-least-32-bytes");
        LocalIpcServer server(&host, endpoint, token);
        server.setHelloTimeout(200);
        QVERIFY(server.listen());

        QLocalSocket idle;
        idle.connectToServer(endpoint);
        QTRY_COMPARE_WITH_TIMEOUT(idle.state(), QLocalSocket::ConnectedState, 1000);
        TestClient authenticated;
        authenticate(authenticated, endpoint, token, "timeout-hello");
        QTRY_COMPARE_WITH_TIMEOUT(idle.state(), QLocalSocket::UnconnectedState, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(server.connectionCount(), 1, 1000);
        QTest::qWait(400);
        QCOMPARE(authenticated.socket.state(), QLocalSocket::ConnectedState);
    }

    void rejectsConnectionsBeyondLimit() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        CodexAdapter adapter("/nonexistent/ketplus-codex");
        SessionHost host(&journal, &adapter);
        const QString endpoint = directory.filePath("limit.sock");
        const QByteArray token("local-test-token-with-at-least-32-bytes");
        LocalIpcServer server(&host, endpoint, token);
        server.setMaximumConnections(2);
        QVERIFY(server.listen());

        TestClient first;
        authenticate(first, endpoint, token, "limit-1");
        TestClient second;
        authenticate(second, endpoint, token, "limit-2");
        QCOMPARE(server.connectionCount(), 2);

        TestClient extra;
        extra.connectTo(endpoint);
        QTRY_VERIFY_WITH_TIMEOUT(!extra.messages.isEmpty(), 2000);
        QCOMPARE(extra.messages.first().value("payload").toObject().value("error_code").toString(),
                 QStringLiteral("connection_limit"));
        QTRY_COMPARE_WITH_TIMEOUT(extra.socket.state(), QLocalSocket::UnconnectedState, 3000);
        QCOMPARE(server.connectionCount(), 2);

        // Freeing a slot admits a new client.
        second.socket.disconnectFromServer();
        QTRY_COMPARE_WITH_TIMEOUT(server.connectionCount(), 1, 2000);
        TestClient replacement;
        authenticate(replacement, endpoint, token, "limit-3");
    }

    void answersABurstOfLargeRequestsWithoutDisconnecting() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(journal.isOpen());
        StoredSession session;
        session.id = QStringLiteral("large");
        session.worktreePath = directory.path();
        session.adapter = QStringLiteral("codex");
        session.state = QStringLiteral("ready");
        QVERIFY(journal.upsertSession(session));
        // Each replay of this session is about 600 KiB, so four answers exceed the two-MiB
        // queue a client may leave unread.
        const QString padding(200 * 1024, QLatin1Char('x'));
        for (int index = 0; index < 3; ++index) {
            QString error;
            QVERIFY(journal.appendEvent(session.id, session.generation, "item.completed",
                                        {{"padding", padding}}, &error) > 0);
            QVERIFY2(error.isEmpty(), qPrintable(error));
        }
        CodexAdapter adapter("/nonexistent/ketplus-codex");
        SessionHost host(&journal, &adapter);
        const QString endpoint = directory.filePath("burst.sock");
        const QByteArray token("local-test-token-with-at-least-32-bytes");
        LocalIpcServer server(&host, endpoint, token);
        QVERIFY(server.listen());

        TestClient client;
        authenticate(client, endpoint, token, "burst-hello");
        // A reconnecting Desktop sends everything it queued in one write.
        constexpr int requests = 8;
        QByteArray burst;
        for (int index = 0; index < requests; ++index) {
            burst.append(protocol::encodeFrame(
                request("session.events_after", QStringLiteral("burst-%1").arg(index),
                        {{"after_cursor", 0}}, session.id, session.generation)));
        }
        client.socket.write(burst);
        client.socket.flush();

        const auto answered = [&client] {
            for (int index = 0; index < requests; ++index) {
                if (!client.hasResponse(QStringLiteral("burst-%1").arg(index)))
                    return false;
            }
            return true;
        };
        QTRY_VERIFY_WITH_TIMEOUT(
            answered() || client.socket.state() != QLocalSocket::ConnectedState, 5000);
        QCOMPARE(client.socket.state(), QLocalSocket::ConnectedState);
        QVERIFY(answered());
        for (int index = 0; index < requests; ++index) {
            const QJsonObject payload = client.takeResponse(QStringLiteral("burst-%1").arg(index))
                                            .value("payload")
                                            .toObject();
            QVERIFY(payload.value("ok").toBool());
            QCOMPARE(payload.value("events").toArray().size(), 3);
        }
    }

    void browserReloadWaitsForStartingAndFailedTurns() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(executable, directory.filePath(QStringLiteral("provider.log"))));
        CodexAdapter adapter(executable);
        QSignalSpy reloads(&adapter, &CodexAdapter::reloaded);
        QSignalSpy stops(&adapter, &CodexAdapter::stopped);
        bool threadReady = false;
        adapter.call(QStringLiteral("thread/start"), {},
                     [&threadReady](const QJsonObject&, const QString&) { threadReady = true; });
        QTRY_VERIFY_WITH_TIMEOUT(threadReady, 3000);
        const auto turn = [](const QString& text) {
            return QJsonObject{
                {QStringLiteral("threadId"), QStringLiteral("thread-1")},
                {QStringLiteral("input"),
                 QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                        {QStringLiteral("text"), text}}}}};
        };

        // A reload requested between the turn/start response and turn/started must wait.
        bool responded = false;
        bool idleAtResponse = true;
        adapter.call(QStringLiteral("turn/start"), turn(QStringLiteral("slow-start")),
                     [&](const QJsonObject&, const QString&) {
                         responded = true;
                         idleAtResponse = adapter.isIdle();
                         emit adapter.browserToolsReloader()->restartRequested();
                     });
        QTRY_VERIFY_WITH_TIMEOUT(responded, 3000);
        QVERIFY(!idleAtResponse);
        QCOMPARE(reloads.count(), 0);
        QVERIFY(adapter.isReady());
        QTRY_VERIFY_WITH_TIMEOUT(adapter.isIdle(), 3000);
        QVERIFY(adapter.isReady());

        // A turn that ends with a final error and no turn/completed does not block reloads.
        bool failedResponded = false;
        adapter.call(
            QStringLiteral("turn/start"), turn(QStringLiteral("error-final")),
            [&failedResponded](const QJsonObject&, const QString&) { failedResponded = true; });
        QTRY_VERIFY_WITH_TIMEOUT(failedResponded && adapter.isIdle(), 3000);
        emit adapter.browserToolsReloader()->restartRequested();
        QCOMPARE(reloads.count(), 1);
        QCOMPARE(reloads.first().first().toString(), QStringLiteral("browser_settings"));
        QCOMPARE(stops.count(), 0);
        QVERIFY(!adapter.isReady());
        adapter.stop();
    }

    void oversizedCodexMessageSkipsOnlyItself() {
        // The reader reports fields that follow the oversized payload, across split reads.
        ProviderLineReader reader(64, {QStringLiteral("method"), QStringLiteral("params.threadId"),
                                       QStringLiteral("params.turn.id")});
        QList<QByteArray> lines;
        QList<OversizedProviderLine> skipped;
        const QByteArray input =
            QByteArray("{\"method\":\"turn/completed\",\"params\":{\"turn\":{\"items\":"
                       "[{\"id\":\"no\"}],\"text\":\"") +
            QByteArray(200, 'x') +
            "\\\"\",\"id\":\"turn-1\"},\"threadId\":\"thread-1\"}}\n"
            "{\"method\":\"after\"}\n";
        for (qsizetype offset = 0; offset < input.size(); offset += 7)
            reader.append(
                input.mid(offset, 7), [&](const QByteArray& line) { lines.append(line); },
                [&](const OversizedProviderLine& line) { skipped.append(line); });
        QCOMPARE(skipped.size(), 1);
        QCOMPARE(skipped.first().bytes, input.indexOf('\n'));
        QCOMPARE(skipped.first().fields.value(QStringLiteral("method")).toString(),
                 QStringLiteral("turn/completed"));
        QCOMPARE(skipped.first().fields.value(QStringLiteral("params.threadId")).toString(),
                 QStringLiteral("thread-1"));
        QCOMPARE(skipped.first().fields.value(QStringLiteral("params.turn.id")).toString(),
                 QStringLiteral("turn-1"));
        QCOMPARE(lines, QList<QByteArray>{QByteArray("{\"method\":\"after\"}")});

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(executable, directory.filePath(QStringLiteral("provider.log"))));
        CodexAdapter adapter(executable);
        QSignalSpy stops(&adapter, &CodexAdapter::stopped);
        QSignalSpy skips(&adapter, &CodexAdapter::notificationSkipped);
        QSignalSpy notifications(&adapter, &CodexAdapter::notificationReceived);
        const auto turn = [](const QString& thread, const QString& text) {
            return QJsonObject{
                {QStringLiteral("threadId"), thread},
                {QStringLiteral("input"),
                 QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                        {QStringLiteral("text"), text}}}}};
        };
        adapter.call(QStringLiteral("turn/start"),
                     turn(QStringLiteral("thread-1"), QStringLiteral("oversized")),
                     [](const QJsonObject&, const QString&) {});
        QTRY_COMPARE_WITH_TIMEOUT(skips.count(), 2, 10000);
        QCOMPARE(skips.at(0).at(0).toString(), QStringLiteral("item/completed"));
        QCOMPARE(skips.at(0).at(1).toString(), QStringLiteral("thread-1"));
        QVERIFY(skips.at(0).at(2).value<qsizetype>() > 16 * 1024 * 1024);
        QCOMPARE(skips.at(1).at(0).toString(), QStringLiteral("turn/completed"));
        // The skipped turn/completed still ends the turn.
        QTRY_VERIFY_WITH_TIMEOUT(adapter.isIdle(), 3000);
        const auto completed = std::find_if(
            notifications.cbegin(), notifications.cend(), [](const QList<QVariant>& args) {
                return args.at(0).toString() == QStringLiteral("turn/completed");
            });
        QVERIFY(completed != notifications.cend());
        const QJsonObject params = completed->at(1).toJsonObject();
        QCOMPARE(params.value(QStringLiteral("threadId")).toString(), QStringLiteral("thread-1"));
        QCOMPARE(params.value(QStringLiteral("turn")).toObject().value(QStringLiteral("id")),
                 QJsonValue(QStringLiteral("turn-1")));

        // The same process keeps serving other threads.
        bool answered = false;
        adapter.call(
            QStringLiteral("turn/start"), turn(QStringLiteral("thread-2"), QStringLiteral("hello")),
            [&answered](const QJsonObject&, const QString& error) { answered = error.isEmpty(); });
        QTRY_VERIFY_WITH_TIMEOUT(answered && adapter.isIdle(), 3000);
        QCOMPARE(stops.count(), 0);
        QVERIFY(adapter.isReady());
        adapter.stop();
    }

    void browserReloadLeavesIdleSessionsQuiet() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        const QString providerLog = directory.filePath(QStringLiteral("provider.log"));
        QVERIFY(writeFakeCodex(executable, providerLog));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        QSignalSpy stops(&adapter, &CodexAdapter::stopped);

        QStringList ids;
        for (int index = 0; index < 3; ++index) {
            const QString worktree = directory.filePath(QStringLiteral("worktree-%1").arg(index));
            QVERIFY(QDir().mkpath(worktree));
            const QJsonObject started = host.handleCommand(
                request(QStringLiteral("session.start_turn"), QStringLiteral("start-%1").arg(index),
                        {{QStringLiteral("worktree_path"), worktree},
                         {QStringLiteral("prompt"), QStringLiteral("hello")}}),
                QStringLiteral("desktop-test"));
            QVERIFY2(started.value(QStringLiteral("ok")).toBool(),
                     qPrintable(QJsonDocument(started).toJson(QJsonDocument::Compact)));
            ids.append(started.value(QStringLiteral("session_id")).toString());
        }
        const auto count = [&journal](const QString& id, const QString& type) {
            int found = 0;
            for (const QJsonValue& value : journal.eventsAfter(id, 1, 0))
                found += value.toObject().value(QStringLiteral("event_type")).toString() == type;
            return found;
        };
        for (const QString& id : ids)
            QTRY_COMPARE_WITH_TIMEOUT(count(id, QStringLiteral("turn.completed")), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(adapter.isIdle(), 3000);
        const auto states = [&host] {
            QStringList result;
            const QJsonObject listed = host.handleCommand(
                request(QStringLiteral("session.list"), QStringLiteral("list-states")),
                QStringLiteral("desktop-test"));
            for (const QJsonValue& value : listed.value(QStringLiteral("sessions")).toArray())
                result.append(value.toObject().value(QStringLiteral("state")).toString());
            result.sort();
            return result;
        };
        const QStringList before = states();
        QCOMPARE(before.size(), 3);

        emit adapter.browserToolsReloader()->restartRequested();
        QVERIFY(!adapter.isReady());
        QCOMPARE(stops.count(), 0);
        // Reattaching clients see the same states and no failure events.
        QCOMPARE(states(), before);
        QVERIFY(!states().contains(QStringLiteral("offline")));
        for (const QString& id : ids) {
            QCOMPARE(count(id, QStringLiteral("runtime.stopped")), 0);
            QCOMPARE(count(id, QStringLiteral("runtime.error")), 0);
        }

        // The next turn resumes the thread in a new provider process.
        const QJsonObject next = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("start-next"),
                    {{QStringLiteral("worktree_path"),
                      directory.filePath(QStringLiteral("worktree-0"))},
                     {QStringLiteral("prompt"), QStringLiteral("again")}},
                    ids.first(), 1),
            QStringLiteral("desktop-test"));
        QVERIFY2(next.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(next).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE_WITH_TIMEOUT(count(ids.first(), QStringLiteral("turn.completed")), 2, 5000);
        QCOMPARE(count(ids.first(), QStringLiteral("runtime.error")), 0);
        QFile log(providerLog);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QCOMPARE(log.readAll().count("args:"), 2);
        adapter.stop();
    }

    void liveCodexDaemonSmoke() {
        if (!qEnvironmentVariableIsSet("KETPLUS_RUN_LIVE_DAEMON_TEST")) {
            QSKIP("Set KETPLUS_RUN_LIVE_DAEMON_TEST=1 to make an authenticated model call.");
        }

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter adapter;
        QVERIFY2(adapter.isAvailable(), "Codex CLI is not installed or discoverable.");
        SessionHost host(&journal, &adapter);
        const QByteArray token("live-test-token-with-at-least-32-bytes");
        const QString endpoint = directory.filePath("live-daemon.sock");
        LocalIpcServer server(&host, endpoint, token);
        QVERIFY2(server.listen(), qPrintable(server.errorString()));

        TestClient client;
        authenticate(client, endpoint, token, "live-hello");
        client.send(request(
            "session.start_turn", "live-turn",
            {{"worktree_path", directory.path()},
             {"prompt",
              "Reply with exactly KETPLUS_DAEMON_LIVE_OK. Do not use tools or modify files."},
             {"options", QJsonObject{{"model", "gpt-5.6-luna"}, {"effort", "medium"}}}}));
        QTRY_VERIFY_WITH_TIMEOUT(client.hasResponse("live-turn"), 20'000);
        const QJsonObject accepted = client.takeResponse("live-turn").value("payload").toObject();
        QVERIFY2(accepted.value("ok").toBool(),
                 qPrintable(QJsonDocument(accepted).toJson(QJsonDocument::Compact)));
        const QString sessionId = accepted.value("session_id").toString();

        const auto finished = [&journal, &sessionId] {
            const QJsonArray events = journal.eventsAfter(sessionId, 1, 0);
            for (const QJsonValue& value : events) {
                const QString type = value.toObject().value("event_type").toString();
                if (type == "turn.completed" || type == "runtime.error" ||
                    type == "runtime.stopped") {
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(finished(), 180'000);

        QString reply;
        QString liveError;
        for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
            const QJsonObject event = value.toObject();
            const QString type = event.value("event_type").toString();
            if (type == "assistant.delta") {
                reply.append(event.value("payload").toObject().value("delta").toString());
            } else if (type == "runtime.error" || type == "runtime.stopped") {
                liveError = event.value("payload").toObject().value("message").toString();
            }
        }
        QVERIFY2(liveError.isEmpty(), qPrintable(liveError));
        QVERIFY2(reply.contains("KETPLUS_DAEMON_LIVE_OK"), qPrintable(reply));
    }
};

QTEST_GUILESS_MAIN(DaemonLifecycleTest)
#include "DaemonLifecycleTest.moc"
