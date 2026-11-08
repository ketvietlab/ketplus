#include "agent/AgentMessageFormatter.h"
#include "agent/AgentSessionManager.h"
#include "agent/CodexAppServer.h"
#include "agent/CodexRateLimits.h"
#include "agent/ProviderRuntimeStatus.h"
#include "ipc/LocalIpcServer.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "service/DaemonService.h"
#include "storage/LocalJournal.h"

#include "CodexBrowserTools.h"
#include "CodexLaunchReloader.h"
#include "DaemonClient.h"
#include "FakeCodex.h"
#include "FakeCursor.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>
#include <memory>

using namespace ketplus;
using namespace ketplus::daemon;

namespace {

int errorRows(const QVariantList& messages) {
    int rows = 0;
    for (const QVariant& value : messages)
        rows += value.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("error");
    return rows;
}

// Stands in for ketplus_daemon: accepts every hello and records the other requests, which the
// test answers or leaves waiting.
class FakeDaemon final : public QObject {
  public:
    explicit FakeDaemon(const QString& path) {
        QLocalServer::removeServer(path);
        listening = server_.listen(path);
        connect(&server_, &QLocalServer::newConnection, this, [this] {
            while (QLocalSocket* socket = server_.nextPendingConnection()) {
                ++connections;
                current = socket;
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QLocalSocket::readyRead, this,
                        [this, socket, buffer] { read(socket, *buffer); });
                connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    void answer(const QJsonObject& request, const QJsonObject& payload) {
        if (!current)
            return;
        const QJsonObject envelope{
            {QStringLiteral("correlation_id"), request.value(QStringLiteral("message_id"))},
            {QStringLiteral("message_type"), request.value(QStringLiteral("message_type"))},
            {QStringLiteral("payload"), payload}};
        const QByteArray body = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
        QByteArray frame(4, Qt::Uninitialized);
        const auto size = static_cast<quint32>(body.size());
        frame[0] = static_cast<char>((size >> 24U) & 0xffU);
        frame[1] = static_cast<char>((size >> 16U) & 0xffU);
        frame[2] = static_cast<char>((size >> 8U) & 0xffU);
        frame[3] = static_cast<char>(size & 0xffU);
        current->write(frame + body);
        current->flush();
    }

    QJsonArray capabilities;
    bool listening{false};
    int connections{0};
    QPointer<QLocalSocket> current;
    // Every request but daemon.hello, across connections, in the order received.
    QList<QJsonObject> requests;
    std::function<void(QLocalSocket*)> afterHello;
    std::function<void(const QJsonObject&)> onRequest;

  private:
    void read(QLocalSocket* socket, QByteArray& buffer) {
        buffer.append(socket->readAll());
        while (buffer.size() >= 4) {
            const quint32 size = (static_cast<quint32>(static_cast<uchar>(buffer[0])) << 24U) |
                                 (static_cast<quint32>(static_cast<uchar>(buffer[1])) << 16U) |
                                 (static_cast<quint32>(static_cast<uchar>(buffer[2])) << 8U) |
                                 static_cast<quint32>(static_cast<uchar>(buffer[3]));
            if (buffer.size() < 4 + static_cast<qsizetype>(size))
                return;
            const QJsonObject request =
                QJsonDocument::fromJson(buffer.mid(4, static_cast<qsizetype>(size))).object();
            buffer.remove(0, 4 + static_cast<qsizetype>(size));
            if (request.value(QStringLiteral("message_type")).toString() ==
                QStringLiteral("daemon.hello")) {
                answer(request, {{QStringLiteral("ok"), true},
                                 {QStringLiteral("protocol_minor"), 1},
                                 {QStringLiteral("capabilities"), capabilities}});
                if (afterHello)
                    afterHello(socket);
                continue;
            }
            requests.append(request);
            if (onRequest)
                onRequest(request);
        }
    }

    QLocalServer server_;
};

// Points DaemonClient at `directory` with a valid token and without starting a real daemon.
QString useFakeDaemon(const QTemporaryDir& directory) {
    const QString socket = directory.filePath(QStringLiteral("fake.sock"));
    const QString token = directory.filePath(QStringLiteral("local.token"));
    QFile file(token);
    if (file.open(QIODevice::WriteOnly))
        file.write(QByteArray(40, 'k'));
    qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
    qputenv("KETPLUS_DAEMON_TOKEN_FILE", token.toUtf8());
    qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
    return socket;
}

void stopUsingFakeDaemon() {
    qunsetenv("KETPLUS_DAEMON_SOCKET");
    qunsetenv("KETPLUS_DAEMON_TOKEN_FILE");
    qunsetenv("KETPLUS_DISABLE_DAEMON_AUTOSTART");
}

} // namespace

class DesktopDaemonClientTest final : public QObject {
    Q_OBJECT

  private slots:
    // DaemonService reads the machine's cloud credentials from the user's data directory, not
    // from the temporary state it is given. Without test paths, a run from a shell that set
    // KETPLUS_CONTROL_URL published these fixtures as live conversations of the real machine.
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void answersAgentPromptThroughDaemon() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString worktree = directory.filePath(QStringLiteral("worktree"));
        QVERIFY(QDir().mkpath(worktree));
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        const QString providerLog = directory.filePath(QStringLiteral("provider.log"));
        QVERIFY(writeFakeCodex(executable, providerLog));

        const QString socket = directory.filePath(QStringLiteral("daemon.sock"));
        const QString state = directory.filePath(QStringLiteral("journal.sqlite3"));
        const QString token = directory.filePath(QStringLiteral("local.token"));
        qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
        qputenv("KETPLUS_DAEMON_STATE", state.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", token.toUtf8());

        DaemonService service({state, token, socket, executable});
        QVERIFY2(service.start(), qPrintable(service.errorString()));

        AgentSessionManager desktop;
        QSignalSpy errors(&desktop, &AgentSessionManager::errorOccurred);
        desktop.activate(worktree);
        desktop.submitPrompt(worktree, QStringLiteral("ask"));
        const auto hasQuestion = [&] {
            for (const auto& value : desktop.messages(worktree))
                if (value.toMap().value("subtype") == "question")
                    return true;
            return false;
        };
        QTRY_VERIFY2_WITH_TIMEOUT(hasQuestion(),
                                  errors.isEmpty() ? "No question was received"
                                                   : qPrintable(errors.last().first().toString()),
                                  3000);
        // This provider request is explicitly nonblocking: it must not pretend to
        // pause the conversation while the user considers their answer.
        QVERIFY(desktop.isRunning(worktree));

        QVariantMap question;
        for (const QVariant& value : desktop.messages(worktree)) {
            const QVariantMap message = value.toMap();
            if (message.value(QStringLiteral("subtype")) == QStringLiteral("question")) {
                question = message;
                break;
            }
        }
        QVERIFY(!question.isEmpty());
        const QVariantList questions = question.value(QStringLiteral("questions")).toList();
        QCOMPARE(questions.size(), 1);
        const QString optionId = questions.first()
                                     .toMap()
                                     .value(QStringLiteral("options"))
                                     .toList()
                                     .first()
                                     .toMap()
                                     .value(QStringLiteral("id"))
                                     .toString();
        QVERIFY(!optionId.isEmpty());
        desktop.answerQuestion(worktree, question.value(QStringLiteral("messageKey")).toString(),
                               {{QStringLiteral("scope"),
                                 QVariantMap{{QStringLiteral("option_ids"), QStringList{optionId}},
                                             {QStringLiteral("custom_answer"), QString{}}}}});

        QTRY_VERIFY_WITH_TIMEOUT(!desktop.isRunning(worktree), 3000);
        const auto answeredQuestion = [&desktop, &worktree] {
            for (const QVariant& value : desktop.messages(worktree)) {
                const QVariantMap message = value.toMap();
                if (message.value(QStringLiteral("subtype")) == QStringLiteral("question")) {
                    return message;
                }
            }
            return QVariantMap{};
        }();
        QCOMPARE(answeredQuestion.value(QStringLiteral("questionAnswers"))
                     .toMap()
                     .value(QStringLiteral("scope"))
                     .toString(),
                 QStringLiteral("Current workspace"));

        QFile log(providerLog);
        QVERIFY(log.open(QIODevice::ReadOnly));
        const QByteArray logContents = log.readAll();
        QVERIFY(logContents.contains("features.default_mode_request_user_input=true"));
        QVERIFY(logContents.contains("\"scope\": {\"answers\": [\"Current workspace\"]}"));

        // Reattach through the real daemon journal, not a synthetic UI notification.
        AgentSessionManager reopened;
        reopened.activate(worktree);
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.messages(worktree).isEmpty(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.historyState(worktree).value("loading").toBool(), 5000);
        // Bounded history omits settled historical approval payloads. Reattach
        // must not resurrect a pending request or permit a second answer.
        for (const auto& value : reopened.messages(worktree)) {
            const auto entry = value.toMap();
            QVERIFY(entry.value("subtype") != "question" || !entry.value("running").toBool());
        }
        QSignalSpy replayErrors(&reopened, &AgentSessionManager::errorOccurred);
        reopened.answerQuestion(worktree, question.value("messageKey").toString(),
                                {{"scope", "Another answer"}});
        QCOMPARE(replayErrors.size(), 1);

        const auto deletedId = desktop.activeConversationId(worktree);
        QSignalSpy deletion(&desktop, &AgentSessionManager::conversationDeletionFinished);
        QVERIFY(desktop.deleteConversation(worktree, deletedId));
        QTRY_VERIFY(desktop.activeConversationId(worktree) != deletedId);
        QTRY_VERIFY_WITH_TIMEOUT(reopened.activeConversationId(worktree) != deletedId, 5000);
        QCOMPARE(deletion.size(), 1);
        QCOMPARE(deletion.first().at(1).toString(), deletedId);
        QVERIFY(deletion.first().at(2).toString().isEmpty());
        QVERIFY2(errors.isEmpty(),
                 errors.isEmpty() ? "" : qPrintable(errors.last().first().toString()));

        qunsetenv("KETPLUS_DAEMON_SOCKET");
        qunsetenv("KETPLUS_DAEMON_STATE");
        qunsetenv("KETPLUS_DAEMON_TOKEN_FILE");
        QSettings().remove(ketplus::agent_message::persistedKey(worktree));
    }

    void restoresLargeSessionSetWithoutReconnectLoop() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString socket = directory.filePath(QStringLiteral("daemon.sock"));
        const QString state = directory.filePath(QStringLiteral("journal.sqlite3"));
        const QString token = directory.filePath(QStringLiteral("local.token"));
        qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
        qputenv("KETPLUS_DAEMON_STATE", state.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", token.toUtf8());

        constexpr int sessionCount = 14;
        {
            LocalJournal journal(state);
            QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
            const QString padding(220 * 1024, QLatin1Char('x'));
            for (int index = 0; index < sessionCount; ++index) {
                const QString worktree =
                    directory.filePath(QStringLiteral("worktree-%1").arg(index));
                QVERIFY(QDir().mkpath(worktree));
                const QString sessionId = QStringLiteral("large-session-%1").arg(index);
                QVERIFY(journal.upsertSession({sessionId,
                                               worktree,
                                               QStringLiteral("codex"),
                                               {},
                                               {},
                                               QStringLiteral("ready"),
                                               1,
                                               0,
                                               0,
                                               {}}));
                QString error;
                QCOMPARE(journal.appendEvent(
                             sessionId, 1, QStringLiteral("turn.completed"),
                             {{QStringLiteral("turn_id"), QStringLiteral("turn-%1").arg(index)},
                              {QStringLiteral("padding"), padding}},
                             &error),
                         1);
                QVERIFY2(error.isEmpty(), qPrintable(error));
            }
        }

        DaemonService service({state, token, socket, {}});
        QVERIFY2(service.start(), qPrintable(service.errorString()));

        const QString firstWorktree = directory.filePath(QStringLiteral("worktree-0"));
        const QString focusedPath = QFileInfo(firstWorktree).canonicalFilePath();
        QSettings().setValue(
            ketplus::agent_message::persistedKey(firstWorktree),
            QVariantMap{{QStringLiteral("path"), focusedPath},
                        {QStringLiteral("activeId"), QStringLiteral("large-session-0")},
                        {QStringLiteral("conversations"),
                         QVariantList{QVariantMap{
                             {QStringLiteral("id"), QStringLiteral("large-session-0")},
                             {QStringLiteral("threadId"), QStringLiteral("large-session-0")},
                             {QStringLiteral("title"), QStringLiteral("Focused conversation")}}}}});
        CodexAppServer appServer;
        AgentSessionManager desktop(&appServer);
        QSignalSpy restoreState(&desktop, &AgentSessionManager::restoreStateChanged);
        desktop.activate(firstWorktree);
        QTRY_VERIFY_WITH_TIMEOUT(!restoreState.isEmpty(), 3000);
        QCOMPARE(restoreState.constFirst().constFirst().toBool(), true);
        QTRY_VERIFY_WITH_TIMEOUT(!desktop.isRestoring(), 10'000);
        QCOMPARE(restoreState.at(1).constFirst().toBool(), false);

        QTRY_VERIFY_WITH_TIMEOUT(!appServer.isRestoring(), 10'000);
        QTest::qWait(250);
        QCOMPARE(restoreState.size(), 2);

        qunsetenv("KETPLUS_DAEMON_SOCKET");
        qunsetenv("KETPLUS_DAEMON_STATE");
        qunsetenv("KETPLUS_DAEMON_TOKEN_FILE");
        QSettings().remove(ketplus::agent_message::persistedKey(firstWorktree));
    }

    void desktopDisconnectDoesNotOwnTurnAndReplaysTranscript() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString worktree = directory.filePath(QStringLiteral("worktree"));
        QVERIFY(QDir().mkpath(worktree));
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(executable, directory.filePath(QStringLiteral("provider.log"))));

        const QString socket = directory.filePath(QStringLiteral("daemon.sock"));
        const QString state = directory.filePath(QStringLiteral("journal.sqlite3"));
        const QString token = directory.filePath(QStringLiteral("local.token"));
        qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
        qputenv("KETPLUS_DAEMON_STATE", state.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", token.toUtf8());

        // This fixture owns only fake Codex. Avoid discovering the host's Claude/Cursor
        // runtimes: provider.models would wait for their cold starts before replay checks.
        DaemonService service({state, token, socket, executable,
                               directory.filePath(QStringLiteral("missing-claude")),
                               directory.filePath(QStringLiteral("missing-cursor"))});
        QVERIFY2(service.start(), qPrintable(service.errorString()));

        {
            AgentSessionManager desktop;
            QSignalSpy models(&desktop, &AgentSessionManager::modelsLoaded);
            QTRY_COMPARE_WITH_TIMEOUT(models.size(), 1, 5000);
            QCOMPARE(models.first().first().toList().first().toMap().value(QStringLiteral("model")),
                     QStringLiteral("gpt-5.6-luna"));
            desktop.rateLimits()->refresh();
            QTRY_VERIFY_WITH_TIMEOUT(
                desktop.rateLimits()->weeklyLimit().value(QStringLiteral("available")).toBool(),
                2000);
            QCOMPARE(
                desktop.rateLimits()->weeklyLimit().value(QStringLiteral("usedPercent")).toInt(),
                42);
            desktop.activate(worktree);
            desktop.submitPrompt(worktree, QStringLiteral("continue"),
                                 {{QStringLiteral("provider"), QStringLiteral("codex")},
                                  {QStringLiteral("model"), QStringLiteral("gpt-5.6-luna")},
                                  {QStringLiteral("effort"), QStringLiteral("medium")}});
            QTRY_VERIFY_WITH_TIMEOUT(desktop.isRunning(worktree), 2000);
            const auto providerStarted = [&directory] {
                QFile log(directory.filePath(QStringLiteral("provider.log")));
                return log.open(QIODevice::ReadOnly) && log.readAll().contains("turn:thread-1");
            };
            QTRY_VERIFY_WITH_TIMEOUT(providerStarted(), 2000);
            desktop.renameConversation(worktree, desktop.activeConversationId(worktree),
                                       QStringLiteral("Renamed across daemon"));
            const auto providerRenamed = [&directory] {
                QFile log(directory.filePath(QStringLiteral("provider.log")));
                return log.open(QIODevice::ReadOnly) &&
                       log.readAll().contains("name:thread-1:Renamed across daemon");
            };
            QTRY_VERIFY_WITH_TIMEOUT(providerRenamed(), 2000);
        }

        QTest::qWait(250);

        AgentSessionManager reopened;
        QSignalSpy restoreState(&reopened, &AgentSessionManager::restoreStateChanged);
        reopened.activate(worktree);
        QTRY_VERIFY_WITH_TIMEOUT(reopened.messages(worktree).size() >= 2, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.isRunning(worktree), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.isRestoring(), 3000);
        QVERIFY(restoreState.size() >= 2);
        QCOMPARE(restoreState.constFirst().constFirst().toBool(), true);
        QCOMPARE(restoreState.constLast().constFirst().toBool(), false);
        const QVariantList transcript = reopened.messages(worktree);
        QCOMPARE(transcript.first().toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("user"));
        QCOMPARE(transcript.first().toMap().value(QStringLiteral("body")).toString(),
                 QStringLiteral("continue"));
        QCOMPARE(transcript.last().toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("assistant"));
        QCOMPARE(transcript.last().toMap().value(QStringLiteral("body")).toString(),
                 QStringLiteral("finished"));
        QCOMPARE(reopened.title(worktree), QStringLiteral("Renamed across daemon"));

        qunsetenv("KETPLUS_DAEMON_SOCKET");
        qunsetenv("KETPLUS_DAEMON_STATE");
        qunsetenv("KETPLUS_DAEMON_TOKEN_FILE");
        QSettings().remove(ketplus::agent_message::persistedKey(worktree));
    }

    // A provider saved from an earlier launch (or before another client moved the conversation)
    // must not be paired with the model of the agent the daemon now runs it on: Claude rejects
    // a Codex model, and every turn of that conversation fails.
    void adoptsTheDaemonAgentOverAStaleSavedProvider() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString worktree = directory.filePath(QStringLiteral("worktree"));
        QVERIFY(QDir().mkpath(worktree));
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(executable, directory.filePath(QStringLiteral("provider.log"))));
        const QString socket = directory.filePath(QStringLiteral("daemon.sock"));
        const QString state = directory.filePath(QStringLiteral("journal.sqlite3"));
        const QString token = directory.filePath(QStringLiteral("local.token"));
        qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
        qputenv("KETPLUS_DAEMON_STATE", state.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", token.toUtf8());
        const auto cleanup = qScopeGuard([&worktree] {
            qunsetenv("KETPLUS_DAEMON_SOCKET");
            qunsetenv("KETPLUS_DAEMON_STATE");
            qunsetenv("KETPLUS_DAEMON_TOKEN_FILE");
            QSettings().remove(ketplus::agent_message::persistedKey(worktree));
            QSettings().remove(
                ketplus::agent_message::persistedKey(QFileInfo(worktree).canonicalFilePath()));
        });
        DaemonService service({state, token, socket, executable,
                               directory.filePath(QStringLiteral("missing-claude")),
                               directory.filePath(QStringLiteral("missing-cursor"))});
        QVERIFY2(service.start(), qPrintable(service.errorString()));

        {
            AgentSessionManager desktop;
            QSignalSpy models(&desktop, &AgentSessionManager::modelsLoaded);
            QTRY_COMPARE_WITH_TIMEOUT(models.size(), 1, 5000);
            desktop.activate(worktree);
            desktop.submitPrompt(worktree, QStringLiteral("continue"),
                                 {{QStringLiteral("provider"), QStringLiteral("codex")},
                                  {QStringLiteral("model"), QStringLiteral("gpt-5.6-luna")},
                                  {QStringLiteral("effort"), QStringLiteral("medium")}});
            QTRY_VERIFY_WITH_TIMEOUT(desktop.messages(worktree).size() >= 2, 3000);
            QTRY_VERIFY_WITH_TIMEOUT(!desktop.isRunning(worktree), 3000);
        }
        QTest::qWait(250);

        // The saved conversation names Claude while the daemon runs it on Codex.
        const QString key =
            ketplus::agent_message::persistedKey(QFileInfo(worktree).canonicalFilePath());
        QVariantMap saved = QSettings().value(key).toMap();
        QVariantList conversations = saved.value(QStringLiteral("conversations")).toList();
        QVERIFY(!conversations.isEmpty());
        for (QVariant& conversation : conversations) {
            QVariantMap value = conversation.toMap();
            value.insert(QStringLiteral("provider"), QStringLiteral("claude"));
            conversation = value;
        }
        saved.insert(QStringLiteral("conversations"), conversations);
        QSettings().setValue(key, saved);

        AgentSessionManager reopened;
        reopened.activate(worktree);
        QTRY_VERIFY_WITH_TIMEOUT(reopened.messages(worktree).size() >= 2, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(reopened.conversationConfiguration(worktree)
                                      .value(QStringLiteral("provider"))
                                      .toString(),
                                  QStringLiteral("codex"), 3000);
        QCOMPARE(
            reopened.conversationConfiguration(worktree).value(QStringLiteral("model")).toString(),
            QStringLiteral("gpt-5.6-luna"));

        // Choosing another agent here for the next turn is kept: the daemon's model is the
        // current agent's and must not replace the chosen agent's model.
        reopened.setConversationConfiguration(worktree, QStringLiteral("claude-opus-5-5"),
                                              QStringLiteral("high"), {}, QStringLiteral("claude"));
        QTest::qWait(300);
        const QVariantMap chosen = reopened.conversationConfiguration(worktree);
        QCOMPARE(chosen.value(QStringLiteral("provider")).toString(), QStringLiteral("claude"));
        QCOMPARE(chosen.value(QStringLiteral("model")).toString(),
                 QStringLiteral("claude-opus-5-5"));
    }

    void negotiatesFeaturesAndReportsProviderCapabilities() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(executable, directory.filePath(QStringLiteral("provider.log"))));
        const QString socket = directory.filePath(QStringLiteral("daemon.sock"));
        const QString state = directory.filePath(QStringLiteral("journal.sqlite3"));
        const QString token = directory.filePath(QStringLiteral("local.token"));
        qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
        qputenv("KETPLUS_DAEMON_STATE", state.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", token.toUtf8());
        DaemonService service({state, token, socket, executable});
        QVERIFY2(service.start(), qPrintable(service.errorString()));

        remote::DaemonClient client;
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 3000);
        QCOMPARE(client.protocolMinor(), 1);
        QVERIFY(client.hasFeature(QStringLiteral("provider.runtime_status")));
        QVERIFY(client.hasFeature(QStringLiteral("session.fork")));
        QVERIFY(!client.hasFeature(QStringLiteral("not.a.feature")));
        QVERIFY(client.hasCapability(QStringLiteral("storage.retention.read")));
        QVERIFY(client.hasCapability(QStringLiteral("storage.retention.apply")));

        CodexAppServer storageBridge;
        QTRY_VERIFY(storageBridge.isReady());
        QJsonObject storageState;
        storageBridge.call("daemon/storageRetention/read", {},
                           [&](const QJsonObject& result, const QString& error) {
                               QVERIFY(error.isEmpty());
                               storageState = result;
                           });
        QTRY_VERIFY(storageState.value("ok").toBool());
        QCOMPARE(storageState.value("policy").toObject().value("full_detail_days").toInt(), 30);
        QVERIFY(storageState.value("size_policy_supported").toBool());
        QJsonObject sizeState;
        storageBridge.call("daemon/storageSize/read", {},
                           [&](const QJsonObject& result, const QString& error) {
                               QVERIFY(error.isEmpty());
                               sizeState = result;
                           });
        QTRY_VERIFY(sizeState.value("ok").toBool());
        QCOMPARE(sizeState.value("revision").toInteger(), 0);
        sizeState = {};
        storageBridge.call(
            "daemon/storageSize/apply",
            {{"expected_revision", 0},
             {"policy", QJsonObject{{"limit_bytes", 2'000'000'000LL}, {"warning_percent", 90}}}},
            [&](const QJsonObject& result, const QString& error) {
                QVERIFY(error.isEmpty());
                sizeState = result;
            });
        QTRY_VERIFY(sizeState.value("ok").toBool());
        QCOMPARE(sizeState.value("revision").toInteger(), 1);
        const auto expected = storageState.value("revision");
        storageState = {};
        storageBridge.call("daemon/storageRetention/apply",
                           {{"expected_revision", expected},
                            {"policy", QJsonObject{{"full_detail_days", 90},
                                                   {"automatic_detail_retention", false}}}},
                           [&](const QJsonObject& result, const QString& error) {
                               QVERIFY(error.isEmpty());
                               storageState = result;
                           });
        QTRY_VERIFY(storageState.value("ok").toBool());
        QCOMPARE(storageState.value("revision").toInteger(), expected.toInteger() + 1);
        QCOMPARE(storageState.value("policy").toObject().value("full_detail_days").toInt(), 90);
        QVERIFY(
            !storageState.value("policy").toObject().value("automatic_detail_retention").toBool());

        AgentSessionManager desktop;
        ProviderRuntimeStatus* status = desktop.providerStatus();
        QVERIFY(status->state().value(QStringLiteral("gated")).toBool());
        QVERIFY(!status->supports(QStringLiteral("codex"), QStringLiteral("turnSteer")));
        status->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(
            status->supports(QStringLiteral("codex"), QStringLiteral("turnSteer")), 5000);
        // No provider offers compaction through the daemon yet.
        QVERIFY(!status->supports(QStringLiteral("codex"), QStringLiteral("threadCompact")));
    }

    void largeTombstoneCatalogCrossesRealIpcInBoundedPages() {
        QTemporaryDir directory;
        const auto socket = useFakeDaemon(directory);
        const auto restore = qScopeGuard(stopUsingFakeDaemon);
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(journal.isOpen());
        CodexAdapter adapter(directory.filePath("missing-fixture-provider"));
        SessionHost host(&journal, &adapter);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == journal.databasePath())
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery query(db);
        QVERIFY(query.exec("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n "
                           "WHERE i<30000) INSERT INTO session_tombstones "
                           "SELECT 'deleted-'||i,1,1,'operation-'||i,datetime('now') FROM n"));
        query.finish();
        LocalIpcServer ipc(&host, socket, QByteArray(40, 'k'));
        QVERIFY2(ipc.listen(), qPrintable(ipc.errorString()));
        CodexAppServer bridge;
        QSet<QString> deleted;
        connect(&bridge, &CodexAppServer::daemonSessionDeleted, &bridge,
                [&](const QString& id, qint64 generation) {
                    QCOMPARE(generation, 1);
                    deleted.insert(id);
                });
        QTRY_COMPARE_WITH_TIMEOUT(deleted.size(), 30000, 15000);
        QTRY_VERIFY(!bridge.isRestoring());
        QVERIFY(bridge.isReady());
        QCOMPARE(ipc.connectionCount(), 1);
    }

    void catalogRestartsForDeletionBehindCursorAndDiscoveryAlsoPages() {
        QTemporaryDir directory;
        FakeDaemon daemon(useFakeDaemon(directory));
        const auto restore = qScopeGuard(stopUsingFakeDaemon);
        QVERIFY(daemon.listening);
        LocalJournal journal(directory.filePath("journal.sqlite"));
        CodexAdapter adapter(directory.filePath("missing-provider"));
        SessionHost host(&journal, &adapter);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (QSqlDatabase::database(name).databaseName() == journal.databasePath())
                db = QSqlDatabase::database(name);
        QSqlQuery q(db);
        QVERIFY(q.exec("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n "
                       "WHERE i<200) INSERT INTO session_tombstones "
                       "SELECT 'middle-'||i,1,1,'op','now' FROM n"));
        q.finish();
        int changed = 0, walks = 0;
        bool injected = false, discover = false;
        daemon.onRequest = [&](const QJsonObject& request) {
            if (request.value("message_type") != "session.list") {
                daemon.answer(request, {{"ok", true}});
                return;
            }
            const auto payload = request.value("payload").toObject();
            QCOMPARE(payload.value("deleted_sessions_page_version").toInt(), 1);
            if (!payload.contains("deleted_sessions_snapshot"))
                ++walks;
            auto result = host.handleCommand(request, "desktop");
            if (result.value("error_code") == "session_catalog_changed")
                ++changed;
            if (discover && result.contains("sessions"))
                result["sessions"] = QJsonArray{QJsonObject{{"session_id", "fresh"},
                                                            {"session_generation", 1},
                                                            {"worktree_path", directory.path()},
                                                            {"last_event_cursor", 1}}};
            daemon.answer(request, result);
            if (!injected) {
                injected = true;
                // No broadcast: this models a retained deletion written between catalog pages.
                QVERIFY(q.exec("INSERT INTO session_tombstones VALUES('aaa',1,1,'op','now')"));
                q.finish();
            }
        };
        CodexAppServer bridge;
        QSet<QString> removed;
        connect(&bridge, &CodexAppServer::daemonSessionDeleted, &bridge,
                [&](const QString& id, qint64) { removed.insert(id); });
        QSignalSpy discovered(&bridge, &CodexAppServer::daemonSessionDiscovered);
        QTRY_COMPARE(removed.size(), 201);
        QTRY_VERIFY(!bridge.isRestoring());
        QVERIFY(removed.contains("aaa"));
        QCOMPARE(changed, 1);
        QCOMPARE(walks, 2);
        discover = true;
        daemon.current->write(
            protocol::encodeFrame({{"message_type", "session.event"},
                                   {"session_id", "fresh"},
                                   {"session_generation", 1},
                                   {"cursor", 1},
                                   {"payload", QJsonObject{{"event_type", "session.created"}}}}));
        daemon.current->flush();
        QTRY_COMPARE(discovered.size(), 1);
        QCOMPARE(discovered.first().first().toJsonObject().value("session_id").toString(), "fresh");
        QCOMPARE(walks, 3);
    }

    void continuouslyChangingCatalogStopsAfterBoundedRetries() {
        QTemporaryDir directory;
        FakeDaemon daemon(useFakeDaemon(directory));
        const auto restore = qScopeGuard(stopUsingFakeDaemon);
        QVERIFY(daemon.listening);
        int calls = 0;
        daemon.onRequest = [&](const QJsonObject& request) {
            if (request.value("message_type") != "session.list")
                return;
            ++calls;
            daemon.answer(request, {{"ok", false},
                                    {"error_code", "session_catalog_changed"},
                                    {"error_message", "Catalog changed."}});
        };
        CodexAppServer bridge;
        QTRY_COMPARE(calls, 4);
        QTRY_VERIFY(!bridge.isRestoring());
        QTest::qWait(50);
        QCOMPARE(calls, 4);
    }

    void tombstonesArePagedBeforeReadingAFreshLiveCatalog() {
        QTemporaryDir directory;
        FakeDaemon daemon(useFakeDaemon(directory));
        const auto restore = qScopeGuard(stopUsingFakeDaemon);
        QVERIFY(daemon.listening);
        const QJsonObject first{{"session_id", "a"}, {"session_generation", 1}};
        const QJsonObject second{{"session_id", "b"}, {"session_generation", 2}};
        int pages = 0;
        daemon.onRequest = [&](const QJsonObject& request) {
            if (request.value("message_type") != "session.list")
                return;
            const auto payload = request.value("payload").toObject();
            QCOMPARE(payload.value("deleted_sessions_page_version").toInt(), 1);
            ++pages;
            if (pages <= 2) {
                QVERIFY(payload.value("deleted_sessions_only").toBool());
                if (pages == 1)
                    QVERIFY(!payload.contains("deleted_sessions_after"));
                else
                    QCOMPARE(payload.value("deleted_sessions_after").toObject(), first);
                daemon.answer(request,
                              {{"ok", true},
                               {"deleted_sessions_page_version", 1},
                               {"deleted_sessions", QJsonArray{pages == 1 ? first : second}},
                               {"deleted_sessions_snapshot", "fixture-snapshot"},
                               {"deleted_sessions_next", pages == 1 ? first : QJsonObject{}},
                               // A catalog obtained before the tombstone walk must not
                               // resurrect an entry deleted while pages were in flight.
                               {"sessions", QJsonArray{QJsonObject{{"session_id", "stale"}}}}});
            } else {
                QCOMPARE(pages, 3);
                QVERIFY(!payload.value("deleted_sessions_only").toBool());
                QVERIFY(!payload.contains("deleted_sessions_after"));
                daemon.answer(request,
                              {{"ok", true},
                               {"deleted_sessions_page_version", 1},
                               {"deleted_sessions", QJsonArray{first}},
                               {"deleted_sessions_snapshot", "fixture-snapshot"},
                               {"deleted_sessions_next", first},
                               {"transcript_window_version", 1},
                               {"sessions", QJsonArray{QJsonObject{{"session_id", "live"},
                                                                   {"session_generation", 3}}}}});
            }
        };
        CodexAppServer bridge;
        QSignalSpy deleted(&bridge, &CodexAppServer::daemonSessionDeleted);
        QSignalSpy discovered(&bridge, &CodexAppServer::daemonSessionDiscovered);
        QTRY_COMPARE(pages, 3);
        QTRY_COMPARE(discovered.size(), 1);
        QCOMPARE(discovered.first().first().toJsonObject().value("session_id").toString(), "live");
        QCOMPARE(deleted.size(), 3); // The fresh catalog's first page is idempotent.
        QCOMPARE(deleted.at(1).first().toString(), "b");
        QVERIFY(!bridge.isRestoring());
    }
    void malformedTombstoneContinuationStopsWithoutApplyingIt_data() {
        QTest::addColumn<QString>("fault");
        for (const auto& name :
             {"repeated-cursor", "unordered-row", "missing-version", "read-error"})
            QTest::newRow(name) << QString(name);
    }
    void malformedTombstoneContinuationStopsWithoutApplyingIt() {
        QFETCH(QString, fault);
        QTemporaryDir directory;
        FakeDaemon daemon(useFakeDaemon(directory));
        const auto restore = qScopeGuard(stopUsingFakeDaemon);
        QVERIFY(daemon.listening);
        const QJsonObject first{{"session_id", "a"}, {"session_generation", 1}};
        const QJsonObject second{{"session_id", "b"}, {"session_generation", 1}};
        int pages = 0;
        daemon.onRequest = [&](const QJsonObject& request) {
            if (request.value("message_type") != "session.list")
                return;
            ++pages;
            QVERIFY(pages <= 2);
            QJsonObject result{{"ok", true},
                               {"deleted_sessions_page_version", 1},
                               {"deleted_sessions", QJsonArray{pages == 1 ? first : second}},
                               {"deleted_sessions_snapshot", "fixture-snapshot"},
                               {"deleted_sessions_next", pages == 1 ? first : QJsonObject{}}};
            if (pages == 2) {
                if (fault == "repeated-cursor")
                    result["deleted_sessions_next"] = first;
                else if (fault == "unordered-row")
                    result["deleted_sessions"] = QJsonArray{first};
                else if (fault == "missing-version")
                    result.remove("deleted_sessions_page_version");
                else
                    result = {{"ok", false},
                              {"error_code", "journal_error"},
                              {"error_message", "fixture read failure"}};
            }
            daemon.answer(request, result);
        };
        CodexAppServer bridge;
        QSignalSpy deleted(&bridge, &CodexAppServer::daemonSessionDeleted);
        QSignalSpy discovered(&bridge, &CodexAppServer::daemonSessionDiscovered);
        QTRY_COMPARE(pages, 2);
        QTRY_VERIFY(!bridge.isRestoring());
        QCOMPARE(deleted.size(), 1);
        QCOMPARE(discovered.size(), 0);
        QTest::qWait(20);
        QCOMPARE(pages, 2);
    }
    void reconnectIgnoresThePriorTombstoneWalk() {
        QTemporaryDir directory;
        FakeDaemon daemon(useFakeDaemon(directory));
        const auto restore = qScopeGuard(stopUsingFakeDaemon);
        QVERIFY(daemon.listening);
        const QJsonObject first{{"session_id", "a"}, {"session_generation", 1}};
        const QJsonObject stale{{"session_id", "stale"}, {"session_generation", 1}};
        const QJsonObject current{{"session_id", "current"}, {"session_generation", 2}};
        bool interrupted = false;
        bool replayed = false;
        bool completed = false;
        daemon.onRequest = [&](const QJsonObject& request) {
            if (request.value("message_type") != "session.list")
                return;
            const auto payload = request.value("payload").toObject();
            QJsonObject result{{"ok", true},
                               {"deleted_sessions_page_version", 1},
                               {"deleted_sessions_snapshot", "fixture-snapshot"},
                               {"deleted_sessions_next", QJsonObject{}}};
            if (daemon.connections == 1) {
                if (payload.contains("deleted_sessions_after")) {
                    interrupted = true;
                    daemon.current->disconnectFromServer();
                    return;
                }
                result["deleted_sessions"] = QJsonArray{first};
                result["deleted_sessions_next"] = first;
            } else if (payload.contains("deleted_sessions_after")) {
                replayed = true;
                result["deleted_sessions"] = QJsonArray{stale};
            } else {
                result["deleted_sessions"] = QJsonArray{current};
                if (!payload.value("deleted_sessions_only").toBool()) {
                    result["sessions"] = QJsonArray{};
                    result["transcript_window_version"] = 1;
                    completed = true;
                }
            }
            daemon.answer(request, result);
        };
        CodexAppServer bridge;
        QSignalSpy deleted(&bridge, &CodexAppServer::daemonSessionDeleted);
        QTRY_VERIFY(interrupted);
        QTRY_VERIFY(replayed);
        QTRY_VERIFY(completed);
        QTRY_VERIFY(!bridge.isRestoring());
        QVERIFY(daemon.connections >= 2);
        QVERIFY(deleted.size() >= 2);
        for (const auto& signal : deleted)
            QVERIFY(signal.first().toString() != "stale");
    }

    void legacyDaemonCatalogStillRestores() {
        QTemporaryDir directory;
        FakeDaemon daemon(useFakeDaemon(directory));
        const auto restore = qScopeGuard(stopUsingFakeDaemon);
        QVERIFY(daemon.listening);
        int pages = 0;
        daemon.onRequest = [&](const QJsonObject& request) {
            if (request.value("message_type") != "session.list")
                return;
            ++pages;
            daemon.answer(request,
                          {{"ok", true},
                           {"transcript_window_version", 1},
                           {"deleted_sessions", QJsonArray{QJsonObject{{"session_id", "gone"},
                                                                       {"session_generation", 1}}}},
                           {"sessions", QJsonArray{}}});
        };
        CodexAppServer bridge;
        QSignalSpy deleted(&bridge, &CodexAppServer::daemonSessionDeleted);
        QTRY_COMPARE(deleted.size(), 1);
        QVERIFY(!bridge.isRestoring());
        QCOMPARE(pages, 1);
    }

    void storageCapabilitiesAndDisconnectedCallsAreNotReplayed_data() {
        QTest::addColumn<QString>("prefix");
        QTest::newRow("retention") << QString("storage.retention.");
        QTest::newRow("size") << QString("storage.size.");
    }
    void storageCapabilitiesAndDisconnectedCallsAreNotReplayed() {
        QFETCH(QString, prefix);
        QTemporaryDir directory;
        FakeDaemon daemon(useFakeDaemon(directory));
        const auto restore = qScopeGuard(stopUsingFakeDaemon);
        QVERIFY(daemon.listening);
        daemon.capabilities = {prefix + "read", prefix + "apply"};
        remote::DaemonClient client;
        QVERIFY(!client.hasCapability(prefix + "read"));
        QTRY_VERIFY(client.isReady());
        QVERIFY(client.hasCapability(prefix + "apply"));
        int failures = 0;
        const auto failed = [&](const QJsonObject&, const QString& error) {
            QVERIFY(error.contains("disconnected"));
            ++failures;
        };
        client.call(prefix + "apply", {}, {}, -1, failed);
        for (int i = 0; i < 15; ++i)
            client.call("session.attach", {{"index", i}}, "s", 1, {});
        client.call(prefix + "read", {}, {}, -1, failed);
        QTRY_COMPARE(daemon.requests.size(), 16);
        daemon.requests.clear();
        daemon.capabilities = {};
        daemon.current->disconnectFromServer();
        QTRY_COMPARE(failures, 2);
        QTRY_COMPARE(daemon.requests.size(), 15);
        QTRY_VERIFY(client.isReady());
        QVERIFY(!client.hasCapability(prefix + "read"));
        for (const auto& request : daemon.requests)
            QCOMPARE(request.value("message_type").toString(), "session.attach");
    }

    void storageNeverStartsDirectProvider() {
        QTemporaryDir directory;
        const auto executable = directory.filePath("fake-codex");
        const auto log = directory.filePath("provider.log");
        QVERIFY(writeFakeCodex(executable, log));
        CodexAppServer server(executable);
        for (const auto& method : {"daemon/storageRetention/read", "daemon/storageSize/read",
                                   "daemon/storageSize/apply", "daemon/storageUnknown/read"}) {
            bool rejected = false;
            server.call(method, {}, [&](const QJsonObject&, const QString& error) {
                rejected = error.contains("local KetPlus daemon");
            });
            QVERIFY(rejected);
        }
        QVERIFY(server.isIdle());
        QVERIFY(!QFile::exists(log));
    }
    void storageBridgeRefusesOldDaemonAndForwardsLocalOperations_data() {
        QTest::addColumn<QString>("family");
        QTest::newRow("retention") << QString("retention");
        QTest::newRow("size") << QString("size");
    }
    void storageBridgeRefusesOldDaemonAndForwardsLocalOperations() {
        QFETCH(QString, family);
        const QString wire = "storage." + family + ".";
        const QString ui = family == "size" ? "daemon/storageSize/" : "daemon/storageRetention/";
        QTemporaryDir directory;
        FakeDaemon daemon(useFakeDaemon(directory));
        const auto restore = qScopeGuard(stopUsingFakeDaemon);
        QVERIFY(daemon.listening);
        CodexAppServer bridge;
        QTRY_VERIFY(bridge.isReady());
        bool rejected = false;
        bridge.call(ui + "read", {}, [&](const QJsonObject&, const QString& error) {
            rejected = error.contains("updated local");
        });
        QVERIFY(rejected);
        for (const auto& request : daemon.requests)
            QVERIFY(!request.value("message_type").toString().startsWith("storage."));
        daemon.capabilities = {wire + "read", wire + "apply"};
        daemon.current->disconnectFromServer();
        QTRY_VERIFY(!bridge.isReady());
        QTRY_VERIFY(bridge.isReady());
        for (const auto& bad :
             {ui + "unknown", ui + "extra/read", QString("daemon/storageUnknown/read")}) {
            bool denied = false;
            bridge.call(bad, {}, [&](const QJsonObject&, const QString& error) {
                denied = !error.isEmpty();
            });
            QVERIFY(denied);
        }
        bool done = false;
        daemon.onRequest = [&](const QJsonObject& request) {
            if (request.value("message_type") == wire + "apply") {
                QCOMPARE(request.value("payload").toObject().value("expected_revision").toInt(), 4);
                daemon.answer(request, {{"ok", true}, {"revision", 5}});
            }
        };
        bridge.call(ui + "apply", {{"expected_revision", 4}},
                    [&](const QJsonObject& result, const QString& error) {
                        QVERIFY(error.isEmpty());
                        QCOMPARE(result.value("revision").toInt(), 5);
                        done = true;
                    });
        QTRY_VERIFY(done);
    }

    void keepsBackingOffWhileTheDaemonDropsEachConnection() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        FakeDaemon daemon(useFakeDaemon(directory));
        const auto restore = qScopeGuard(stopUsingFakeDaemon);
        QVERIFY(daemon.listening);
        // Accepts hello, then drops the connection, as a daemon that overran its write queue.
        daemon.afterHello = [](QLocalSocket* socket) { socket->disconnectFromServer(); };

        remote::DaemonClient client;
        // The app keeps asking while the connection is down.
        QTimer asking;
        connect(&asking, &QTimer::timeout, &client,
                [&client] { client.call(QStringLiteral("remote.status"), {}, {}, -1, {}); });
        asking.start(20);
        QTest::qWait(1'500);
        asking.stop();

        // 0, 0.1, 0.3, 0.7 s and on: each hello no longer starts the backoff over.
        QVERIFY2(daemon.connections >= 2, qPrintable(QString::number(daemon.connections)));
        QVERIFY2(daemon.connections <= 5, qPrintable(QString::number(daemon.connections)));
    }

    void disconnectedCredentialsAreFailedAndNeverReplayed() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        FakeDaemon daemon(useFakeDaemon(directory));
        const auto restore = qScopeGuard(stopUsingFakeDaemon);
        QVERIFY(daemon.listening);
        remote::DaemonClient client;
        int failures = 0;
        const auto failed = [&](const QJsonObject& result, const QString& error) {
            QVERIFY(result.isEmpty());
            QVERIFY(error.contains(QStringLiteral("disconnected")));
            ++failures;
        };
        const auto save = [&](remote::DaemonClient::ResponseCallback callback) {
            client.call(QStringLiteral("provider.claude.credentials.save"),
                        {{QStringLiteral("key"), QStringLiteral("sk-ant-reconnect-fixture")}}, {},
                        -1, std::move(callback));
        };
        save(failed);
        save({}); // Fire-and-forget callers must not crash on disconnect either.
        for (int i = 0; i < 14; ++i)
            client.call(QStringLiteral("session.attach"), {{QStringLiteral("index"), i}},
                        QStringLiteral("s"), 1, {});
        // Both sent and not-yet-sent credential operations must be discarded.
        save(failed);
        save({});
        QTRY_COMPARE_WITH_TIMEOUT(daemon.requests.size(), 16, 3000);
        daemon.requests.clear();
        daemon.current->disconnectFromServer();
        QTRY_COMPARE_WITH_TIMEOUT(failures, 2, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(daemon.requests.size(), 14, 3000);
        QTest::qWait(100);
        QCOMPARE(daemon.requests.size(), 14);
        for (const auto& request : daemon.requests) {
            QCOMPARE(request.value(QStringLiteral("message_type")).toString(),
                     QStringLiteral("session.attach"));
            QVERIFY(!QJsonDocument(request).toJson().contains("sk-ant-"));
        }
    }
    void sendsSixteenCallsAtATimeAndMergesQueuedReads() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        FakeDaemon daemon(useFakeDaemon(directory));
        const auto restore = qScopeGuard(stopUsingFakeDaemon);
        QVERIFY(daemon.listening);

        remote::DaemonClient client;
        QStringList answered;
        const auto record = [&answered](const QString& name) {
            return [&answered, name](const QJsonObject& result, const QString& error) {
                answered.append(error.isEmpty()
                                    ? name + QLatin1Char('=') +
                                          result.value(QStringLiteral("value")).toString()
                                    : name + QStringLiteral("!") + error);
            };
        };
        // Queued before the connection is ready, as after a reconnect.
        for (int index = 0; index < 5; ++index) {
            client.call(QStringLiteral("session.pending_approvals"),
                        {{QStringLiteral("after_id"), 0}}, QStringLiteral("s"), 1,
                        record(QStringLiteral("approvals-%1").arg(index)));
        }
        client.call(QStringLiteral("session.pending_approvals"), {{QStringLiteral("after_id"), 5}},
                    QStringLiteral("s"), 1, record(QStringLiteral("approvals-later")));
        // Writes are never merged, even when identical.
        for (int index = 0; index < 2; ++index) {
            client.call(QStringLiteral("session.mark_seen"), {{QStringLiteral("cursor"), 9}},
                        QStringLiteral("s"), 1, record(QStringLiteral("seen-%1").arg(index)));
        }
        for (int index = 0; index < 24; ++index) {
            client.call(QStringLiteral("session.attach"), {{QStringLiteral("after_cursor"), index}},
                        QStringLiteral("s"), 1, record(QStringLiteral("attach-%1").arg(index)));
        }
        constexpr int distinct = 1 + 1 + 2 + 24;
        constexpr int window = 16;

        QTRY_COMPARE_WITH_TIMEOUT(daemon.requests.size(), window, 3000);
        QTest::qWait(200);
        QCOMPARE(daemon.requests.size(), window);
        const QList<QJsonObject> firstSent = daemon.requests;
        QCOMPARE(firstSent.at(0).value(QStringLiteral("message_type")).toString(),
                 QStringLiteral("session.pending_approvals"));
        QCOMPARE(firstSent.at(1).value(QStringLiteral("payload")).toObject(),
                 QJsonObject({{QStringLiteral("after_id"), 5}}));

        // The daemon drops the connection with all of them unanswered: they are sent again first,
        // in the order they were made, with the same ids.
        daemon.requests.clear();
        daemon.current->disconnectFromServer();
        QTRY_COMPARE_WITH_TIMEOUT(daemon.requests.size(), window, 3000);
        QTest::qWait(200);
        QCOMPARE(daemon.requests.size(), window);
        for (int index = 0; index < window; ++index) {
            QCOMPARE(daemon.requests.at(index).value(QStringLiteral("message_id")),
                     firstSent.at(index).value(QStringLiteral("message_id")));
        }

        // Each answer lets one more call out, until every call has been sent once.
        daemon.onRequest = [&daemon](const QJsonObject& request) {
            daemon.answer(request, {{QStringLiteral("ok"), true},
                                    {QStringLiteral("value"),
                                     request.value(QStringLiteral("message_type")).toString()}});
        };
        for (const QJsonObject& request : QList<QJsonObject>(daemon.requests))
            daemon.onRequest(request);
        QTRY_COMPARE_WITH_TIMEOUT(answered.size(), 5 + 1 + 2 + 24, 3000);
        QCOMPARE(daemon.requests.size(), distinct);
        for (int index = 0; index < 5; ++index) {
            QVERIFY(answered.contains(
                QStringLiteral("approvals-%1=session.pending_approvals").arg(index)));
        }
        QVERIFY(answered.contains(QStringLiteral("seen-1=session.mark_seen")));
    }

    void forksConversationThroughDaemon() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString worktree = directory.filePath(QStringLiteral("worktree"));
        QVERIFY(QDir().mkpath(worktree));
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        const QString providerLog = directory.filePath(QStringLiteral("provider.log"));
        QVERIFY(writeFakeCodex(executable, providerLog));
        const QString socket = directory.filePath(QStringLiteral("daemon.sock"));
        const QString state = directory.filePath(QStringLiteral("journal.sqlite3"));
        const QString token = directory.filePath(QStringLiteral("local.token"));
        qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
        qputenv("KETPLUS_DAEMON_STATE", state.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", token.toUtf8());
        const auto cleanup = qScopeGuard([&worktree] {
            qunsetenv("KETPLUS_DAEMON_SOCKET");
            qunsetenv("KETPLUS_DAEMON_STATE");
            qunsetenv("KETPLUS_DAEMON_TOKEN_FILE");
            QSettings().remove(ketplus::agent_message::persistedKey(worktree));
            QSettings().remove(
                ketplus::agent_message::persistedKey(QFileInfo(worktree).canonicalFilePath()));
        });
        DaemonService service({state, token, socket, executable});
        QVERIFY2(service.start(), qPrintable(service.errorString()));

        AgentSessionManager desktop;
        desktop.activate(worktree);
        desktop.submitPrompt(worktree, QStringLiteral("hello"));
        QTRY_VERIFY_WITH_TIMEOUT(desktop.isRunning(worktree), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!desktop.isRunning(worktree), 5000);
        const QString sourceId = desktop.activeConversationId(worktree);
        const auto conversation = [&desktop, &worktree](const QString& id) {
            for (const QVariant& value : desktop.conversations(worktree))
                if (value.toMap().value(QStringLiteral("id")).toString() == id)
                    return value.toMap();
            return QVariantMap{};
        };
        desktop.providerStatus()->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(conversation(sourceId).value(QStringLiteral("forkable")).toBool(),
                                 5000);

        desktop.submitPrompt(worktree, QStringLiteral("hang"));
        QTRY_VERIFY_WITH_TIMEOUT(desktop.isRunning(worktree), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(conversation(sourceId).value(QStringLiteral("forkable")).toBool(),
                                 3000);
        QSignalSpy forked(&desktop, &AgentSessionManager::conversationForkFinished);
        desktop.forkConversation(worktree, sourceId);
        QTRY_COMPARE_WITH_TIMEOUT(forked.size(), 1, 5000);
        const QString forkId = forked.first().at(2).toString();
        QVERIFY2(!forkId.isEmpty(), qPrintable(forked.first().at(3).toString()));
        QCOMPARE(forked.first().at(1).toString(), sourceId);
        QCOMPARE(desktop.activeConversationId(worktree), forkId);
        QCOMPARE(desktop.conversations(worktree).size(), 2);
        QVERIFY(conversation(forkId).value(QStringLiteral("title")).toString().endsWith(" (fork)"));
        QVERIFY(!conversation(forkId).value(QStringLiteral("ownsWorktree")).toBool());

        // The fork shows the copied transcript, then continues on its own provider thread.
        const auto hasBody = [&desktop, &worktree](const QString& kind, const QString& body) {
            for (const QVariant& value : desktop.messages(worktree)) {
                const QVariantMap message = value.toMap();
                if (message.value(QStringLiteral("kind")).toString() == kind &&
                    message.value(QStringLiteral("body")).toString() == body)
                    return true;
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(hasBody(QStringLiteral("user"), QStringLiteral("hello")), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(hasBody(QStringLiteral("assistant"), QStringLiteral("finished")),
                                 5000);
        desktop.submitPrompt(worktree, QStringLiteral("again"));
        QTRY_VERIFY_WITH_TIMEOUT(hasBody(QStringLiteral("user"), QStringLiteral("again")), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!desktop.isRunning(worktree), 5000);
        QFile log(providerLog);
        QVERIFY(log.open(QIODevice::ReadOnly));
        const QByteArray logContents = log.readAll();
        QVERIFY(logContents.contains("fork:"));
        QVERIFY(logContents.contains("turn:fork-of-thread-1"));

        desktop.selectConversation(worktree, sourceId);
        QTRY_VERIFY_WITH_TIMEOUT(hasBody(QStringLiteral("user"), QStringLiteral("hello")), 5000);
        QVERIFY(!hasBody(QStringLiteral("user"), QStringLiteral("again")));
        QVERIFY(desktop.isRunning(worktree));
    }

    void submitsCursorPromptThroughDaemon() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString worktree = directory.filePath(QStringLiteral("worktree"));
        QVERIFY(QDir().mkpath(worktree));
        const QString codex = directory.filePath(QStringLiteral("fake-codex"));
        const QString cursor = directory.filePath(QStringLiteral("fake-cursor"));
        const QString cursorLog = directory.filePath(QStringLiteral("cursor.log"));
        QVERIFY(writeFakeCodex(codex, directory.filePath(QStringLiteral("codex.log"))));
        QVERIFY(writeFakeCursor(cursor, cursorLog));

        const QString socket = directory.filePath(QStringLiteral("daemon.sock"));
        const QString state = directory.filePath(QStringLiteral("journal.sqlite3"));
        const QString token = directory.filePath(QStringLiteral("local.token"));
        qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
        qputenv("KETPLUS_DAEMON_STATE", state.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", token.toUtf8());

        DaemonService service({state, token, socket, codex, {}, cursor});
        QVERIFY2(service.start(), qPrintable(service.errorString()));

        AgentSessionManager desktop;
        QSignalSpy errors(&desktop, &AgentSessionManager::errorOccurred);
        desktop.activate(worktree);
        desktop.submitPrompt(worktree, QStringLiteral("Inspect the tree"),
                             {{QStringLiteral("provider"), QStringLiteral("cursor")},
                              {QStringLiteral("model"), QStringLiteral("auto")},
                              {QStringLiteral("permission"), QStringLiteral("workspace-write")}});
        QCOMPARE(errors.size(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(
            desktop.isRunning(worktree) || !desktop.messages(worktree).isEmpty(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!desktop.isRunning(worktree), 4000);
        QCOMPARE(errors.size(), 0);
        const QVariantList transcript = desktop.messages(worktree);
        QVERIFY(transcript.size() >= 2);
        QCOMPARE(transcript.first().toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("user"));
        QCOMPARE(transcript.last().toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("assistant"));
        QCOMPARE(transcript.last().toMap().value(QStringLiteral("body")).toString(),
                 QStringLiteral("finished"));
        QFile log(cursorLog);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QVERIFY(log.readAll().contains("prompt:"));

        qunsetenv("KETPLUS_DAEMON_SOCKET");
        qunsetenv("KETPLUS_DAEMON_STATE");
        qunsetenv("KETPLUS_DAEMON_TOKEN_FILE");
        QSettings().remove(ketplus::agent_message::persistedKey(worktree));
    }

    void browserSettingsReloadThroughDaemonIsSilent() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        const QString providerLog = directory.filePath(QStringLiteral("provider.log"));
        QVERIFY(writeFakeCodex(executable, providerLog));
        // A fake source checkout, so enabled and disabled settings give different arguments.
        const QString app = directory.filePath(QStringLiteral("checkout/apps/browser"));
        QVERIFY(QDir().mkpath(app + QStringLiteral("/src")));
        QVERIFY(QDir().mkpath(app + QStringLiteral("/node_modules/@modelcontextprotocol/server")));
        QVERIFY(QDir().mkpath(app + QStringLiteral("/node_modules/playwright-core")));
        {
            QFile mcp(app + QStringLiteral("/src/mcp.mjs"));
            QVERIFY(mcp.open(QIODevice::WriteOnly));
        }
        const QString node = directory.filePath(QStringLiteral("node"));
        {
            QFile nodeFile(node);
            QVERIFY(nodeFile.open(QIODevice::WriteOnly));
        }
        QVERIFY(QFile::setPermissions(node, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                QFileDevice::ExeOwner));
        const QString config = directory.filePath(QStringLiteral("browser-tools.json"));
        qputenv("KETPLUS_BROWSER_TOOLS_CONFIG", config.toUtf8());
        browser_tools::Settings settings;
        settings.browserAppPath = app;
        settings.nodePath = node;
        QVERIFY(browser_tools::saveSettings(settings, config));

        const QString socket = directory.filePath(QStringLiteral("daemon.sock"));
        const QString state = directory.filePath(QStringLiteral("journal.sqlite3"));
        const QString token = directory.filePath(QStringLiteral("local.token"));
        qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
        qputenv("KETPLUS_DAEMON_STATE", state.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", token.toUtf8());
        const auto cleanup = qScopeGuard([] {
            qunsetenv("KETPLUS_BROWSER_TOOLS_CONFIG");
            qunsetenv("KETPLUS_DAEMON_SOCKET");
            qunsetenv("KETPLUS_DAEMON_STATE");
            qunsetenv("KETPLUS_DAEMON_TOKEN_FILE");
        });
        DaemonService service({state, token, socket, executable});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        const auto logText = [&providerLog] {
            QFile log(providerLog);
            return log.open(QIODevice::ReadOnly) ? log.readAll() : QByteArray();
        };

        QStringList worktrees;
        {
            AgentSessionManager desktop;
            QSignalSpy errors(&desktop, &AgentSessionManager::errorOccurred);
            for (int index = 0; index < 3; ++index) {
                const QString worktree =
                    directory.filePath(QStringLiteral("worktree-%1").arg(index));
                QVERIFY(QDir().mkpath(worktree));
                worktrees.append(worktree);
                desktop.activate(worktree);
                desktop.submitPrompt(worktree, QStringLiteral("hello"));
                QTRY_COMPARE_WITH_TIMEOUT(logText().count("turn:"), index + 1, 3000);
                QTRY_VERIFY_WITH_TIMEOUT(!desktop.isRunning(worktree), 3000);
            }
            QCOMPARE(logText().count("args:"), 1);
            QVERIFY(logText().contains("ketplus_browser"));

            settings.enabled = false;
            QVERIFY(browser_tools::saveSettings(settings, config));
            browser_tools::notifySettingsChanged();
            QTest::qWait(1200); // debounce, then the idle provider restarts

            for (const QString& worktree : worktrees) {
                QCOMPARE(errorRows(desktop.messages(worktree)), 0);
                QVERIFY(desktop.status(worktree) != QStringLiteral("Error"));
            }
            QVERIFY2(errors.isEmpty(),
                     errors.isEmpty() ? "" : qPrintable(errors.last().first().toString()));

            // The next turn resumes in the relaunched provider, now without browser tools.
            desktop.activate(worktrees.first());
            desktop.submitPrompt(worktrees.first(), QStringLiteral("again"));
            QTRY_COMPARE_WITH_TIMEOUT(logText().count("turn:"), 4, 3000);
            QTRY_VERIFY_WITH_TIMEOUT(!desktop.isRunning(worktrees.first()), 3000);
            QCOMPARE(logText().count("args:"), 2);
            const QByteArray lastArgs = logText().mid(logText().lastIndexOf("args:"));
            QVERIFY(!lastArgs.left(lastArgs.indexOf('\n')).contains("ketplus_browser"));
            QCOMPARE(errorRows(desktop.messages(worktrees.first())), 0);
            QVERIFY(errors.isEmpty());
        }

        // Reattaching replays no error rows or offline state.
        AgentSessionManager reopened;
        QSignalSpy reopenedErrors(&reopened, &AgentSessionManager::errorOccurred);
        for (const QString& worktree : worktrees) {
            reopened.activate(worktree);
            QTRY_VERIFY_WITH_TIMEOUT(!reopened.messages(worktree).isEmpty(), 3000);
            QTRY_VERIFY_WITH_TIMEOUT(!reopened.historyState(worktree).value("loading").toBool(),
                                     3000);
            QCOMPARE(errorRows(reopened.messages(worktree)), 0);
            QVERIFY(reopened.status(worktree) != QStringLiteral("Error"));
        }
        QVERIFY(reopenedErrors.isEmpty());
        for (const QString& worktree : worktrees)
            QSettings().remove(ketplus::agent_message::persistedKey(worktree));
    }

    void switchingExecutionKeepsDesktopConversationAcrossReconnect() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto a = directory.filePath("repo");
        const auto b = directory.filePath("feature");
        QVERIFY(QDir().mkpath(a));
        QCOMPARE(QProcess::execute("git", {"-C", a, "init", "-q"}), 0);
        QCOMPARE(QProcess::execute("git", {"-C", a, "-c", "user.name=Fixture", "-c",
                                           "user.email=fixture@example.invalid", "-c",
                                           "commit.gpgsign=false", "commit", "--allow-empty", "-qm",
                                           "initial"}),
                 0);
        QCOMPARE(QProcess::execute("git", {"-C", a, "worktree", "add", "-b", "feature", b}), 0);
        const auto executable = directory.filePath("fake-codex");
        const auto log = directory.filePath("provider.log");
        QVERIFY(writeFakeCodex(executable, log));
        const auto socket = directory.filePath("daemon.sock");
        const auto state = directory.filePath("journal.sqlite");
        const auto token = directory.filePath("token");
        qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
        qputenv("KETPLUS_DAEMON_STATE", state.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", token.toUtf8());
        DaemonService service({state, token, socket, executable});
        QVERIFY(service.start());
        QString id;
        {
            AgentSessionManager desktop;
            desktop.activate(a);
            desktop.submitPrompt(a, "hello");
            QTRY_VERIFY_WITH_TIMEOUT(!desktop.isRunning(a) && !desktop.messages(a).isEmpty(), 5000);
            id = desktop.activeConversationId(a);
            QJsonObject result;
            QString error;
            bool done = false;
            desktop.executionWorktreeCommand(
                a, "session.worktree.switch",
                {{"worktree_path", b}, {"expected_context_revision", 1}},
                [&](const QJsonObject& r, const QString& e) {
                    result = r;
                    error = e;
                    done = true;
                });
            QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(result.value("context_revision").toInteger(), 2);
            QCOMPARE(desktop.activeConversationId(a), id);
            desktop.submitPrompt(a, "again");
            QTRY_VERIFY_WITH_TIMEOUT(!desktop.isRunning(a), 5000);
            QCOMPARE(errorRows(desktop.messages(a)), 0);
            QFile providerLog(log);
            QVERIFY(providerLog.open(QIODevice::ReadOnly));
            QVERIFY(providerLog.readAll().contains(QFileInfo(b).canonicalFilePath().toUtf8()));
        }
        AgentSessionManager reopened;
        reopened.activate(a);
        QTRY_COMPARE_WITH_TIMEOUT(reopened.activeConversationId(a), id, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!reopened.messages(a).isEmpty(), 3000);
        QCOMPARE(errorRows(reopened.messages(a)), 0);
        QSettings().remove(ketplus::agent_message::persistedKey(a));
    }

    void directModeReloadWaitsForTurnsAndIsSilent() {
        qunsetenv("KETPLUS_DAEMON_SOCKET");
        qunsetenv("KETPLUS_DAEMON_STATE");
        qunsetenv("KETPLUS_DAEMON_TOKEN_FILE");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString worktree = directory.filePath(QStringLiteral("worktree"));
        QVERIFY(QDir().mkpath(worktree));
        const QString executable = directory.filePath(QStringLiteral("fake-codex"));
        const QString providerLog = directory.filePath(QStringLiteral("provider.log"));
        QVERIFY(writeFakeCodex(executable, providerLog));
        const auto turnCount = [&providerLog] {
            QFile log(providerLog);
            return log.open(QIODevice::ReadOnly) ? log.readAll().count("turn:") : 0;
        };

        CodexAppServer server(executable);
        QSignalSpy reloads(&server, &CodexAppServer::serverReloaded);
        QSignalSpy stops(&server, &CodexAppServer::serverStopped);
        AgentSessionManager desktop(&server);
        QSignalSpy errors(&desktop, &AgentSessionManager::errorOccurred);
        desktop.activate(worktree);
        desktop.submitPrompt(worktree, QStringLiteral("hello"));
        QTRY_COMPARE_WITH_TIMEOUT(turnCount(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!desktop.isRunning(worktree), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(server.isIdle(), 3000);

        // Turns on a thread the manager does not show, so only the reload behaviour is observed.
        const auto turn = [](const QString& text) {
            return QJsonObject{
                {QStringLiteral("threadId"), QStringLiteral("raw-thread")},
                {QStringLiteral("input"),
                 QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                        {QStringLiteral("text"), text}}}}};
        };
        bool responded = false;
        bool idleAtResponse = true;
        server.call(QStringLiteral("turn/start"), turn(QStringLiteral("slow-start")),
                    [&](const QJsonObject&, const QString&) {
                        responded = true;
                        idleAtResponse = server.isIdle();
                        emit server.browserToolsReloader()->restartRequested();
                    });
        QTRY_VERIFY_WITH_TIMEOUT(responded, 3000);
        QVERIFY(!idleAtResponse);
        QCOMPARE(reloads.count(), 0);
        QVERIFY(server.isReady());
        QTRY_VERIFY_WITH_TIMEOUT(server.isIdle(), 3000);

        bool failedResponded = false;
        server.call(
            QStringLiteral("turn/start"), turn(QStringLiteral("error-final")),
            [&failedResponded](const QJsonObject&, const QString&) { failedResponded = true; });
        QTRY_VERIFY_WITH_TIMEOUT(failedResponded && server.isIdle(), 3000);

        emit server.browserToolsReloader()->restartRequested();
        QCOMPARE(reloads.count(), 1);
        QCOMPARE(stops.count(), 0);
        QVERIFY(!server.isReady());
        QCOMPARE(errorRows(desktop.messages(worktree)), 0);
        QVERIFY(desktop.status(worktree) != QStringLiteral("Error"));

        desktop.submitPrompt(worktree, QStringLiteral("again"));
        QTRY_COMPARE_WITH_TIMEOUT(turnCount(), 4, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!desktop.isRunning(worktree), 3000);
        QCOMPARE(desktop.messages(worktree).last().toMap().value(QStringLiteral("body")).toString(),
                 QStringLiteral("finished"));
        QCOMPARE(errorRows(desktop.messages(worktree)), 0);
        QVERIFY2(errors.isEmpty(),
                 errors.isEmpty() ? "" : qPrintable(errors.last().first().toString()));
        server.stop();
        QSettings().remove(ketplus::agent_message::persistedKey(worktree));
    }

    void liveDesktopDaemonSmoke() {
        if (!qEnvironmentVariableIsSet("KETPLUS_RUN_LIVE_DESKTOP_DAEMON_TEST")) {
            QSKIP("Set KETPLUS_RUN_LIVE_DESKTOP_DAEMON_TEST=1 to call Luna through Desktop IPC.");
        }
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString socket = directory.filePath(QStringLiteral("daemon.sock"));
        const QString state = directory.filePath(QStringLiteral("journal.sqlite3"));
        const QString token = directory.filePath(QStringLiteral("local.token"));
        qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
        qputenv("KETPLUS_DAEMON_STATE", state.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", token.toUtf8());

        DaemonService service({state, token, socket, {}});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        AgentSessionManager desktop;
        desktop.activate(directory.path());
        desktop.submitPrompt(
            directory.path(),
            QStringLiteral("Reply with exactly KETPLUS_DESKTOP_DAEMON_OK. Do not use tools or "
                           "modify files."),
            {{QStringLiteral("provider"), QStringLiteral("codex")},
             {QStringLiteral("model"), QStringLiteral("gpt-5.6-luna")},
             {QStringLiteral("effort"), QStringLiteral("low")}});

        const auto receivedExpectedReply = [&desktop, &directory] {
            for (const QVariant& value : desktop.messages(directory.path())) {
                const QVariantMap message = value.toMap();
                if (message.value(QStringLiteral("kind")) == QStringLiteral("assistant") &&
                    message.value(QStringLiteral("body"))
                        .toString()
                        .contains(QStringLiteral("KETPLUS_DESKTOP_DAEMON_OK"))) {
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(receivedExpectedReply(), 180'000);
        QTRY_VERIFY_WITH_TIMEOUT(!desktop.isRunning(directory.path()), 10'000);

        qunsetenv("KETPLUS_DAEMON_SOCKET");
        qunsetenv("KETPLUS_DAEMON_STATE");
        qunsetenv("KETPLUS_DAEMON_TOKEN_FILE");
        QSettings().remove(ketplus::agent_message::persistedKey(directory.path()));
    }
};

QTEST_GUILESS_MAIN(DesktopDaemonClientTest)
#include "DesktopDaemonClientTest.moc"
