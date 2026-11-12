#include "machine/MachineRichTranscript.h"
#include "FakeMachineBackend.h"
#include "MachineHosts.h"
#include "agent/AgentMessageModel.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/DaemonTranscriptProjection.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineConversationSession.h"
#include "machine/MachineWorkspaceView.h"

#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QQuickItem>
#include <QQuickView>
#include <QtTest>

using namespace ketplus;
using namespace ketplus::test;

namespace {

QJsonObject turnEvent(const QString& session, qint64 cursor, const QString& type,
                      const QJsonObject& payload) {
    QJsonObject event = journalEvent(session, cursor, type, payload);
    event.insert("turn_sequence", 1);
    return event;
}

QJsonObject completed(const QString& session, qint64 cursor, const QJsonObject& item) {
    return turnEvent(session, cursor, "item.completed", {{"item", item}});
}

// One turn as a coding agent leaves it: the prompt, a command, a file edit with its diff and a
// Markdown answer.
QJsonArray richTurn(const QString& session) {
    return {
        turnEvent(session, 1, "user.message", {{"text", "Fix the build"}}),
        completed(session, 2,
                  {{"type", "commandExecution"},
                   {"id", "cmd-1"},
                   {"command", "make test"},
                   {"aggregatedOutput", "2 tests passed"},
                   {"exitCode", 0},
                   {"status", "completed"}}),
        completed(session, 3,
                  {{"type", "fileChange"},
                   {"id", "fc-1"},
                   {"status", "completed"},
                   {"changes", QJsonArray{QJsonObject{{"path", "src/main.cpp"},
                                                      {"kind", "update"},
                                                      {"diff", "@@ -1 +1 @@\n-old\n+new\n"}}}}}),
        completed(session, 4,
                  {{"type", "agentMessage"},
                   {"id", "answer-1"},
                   {"text", "**Fixed.** The `main` target builds:\n\n- tests pass"}}),
    };
}

// `base` with the rich turn as the conversation's newest window.
FakeMachineBackend::Answer richHost(FakeMachineBackend::Answer base) {
    return [base](const FakeMachineBackend::Call& call, QJsonObject* result, QString* error) {
        base(call, result, error);
        if (call.operation == "transcriptWindow")
            *result = {{"events", richTurn(call.sessionId)}, {"snapshot_cursor", 4}};
    };
}

// A turn whose streamed answer is split across two windows: the prompt, the command and the
// answer's first delta on the older page; the rest of the answer and the edit on the newest.
// When the older page lands, the answer row shown first becomes the whole answer.
QJsonArray splitTurn(const QString& session) {
    const auto delta = [&](qint64 cursor, const QString& text) {
        return turnEvent(session, cursor, "assistant.delta",
                         {{"item_id", "answer-1"}, {"delta", text}});
    };
    const QJsonArray turn = richTurn(session);
    return {
        turn.at(0), turn.at(1), delta(3, "**Fixed.** The `main` "), delta(4, "target builds."),
        completed(session, 5,
                  {{"type", "fileChange"},
                   {"id", "fc-1"},
                   {"status", "completed"},
                   {"changes", QJsonArray{QJsonObject{{"path", "src/main.cpp"},
                                                      {"kind", "update"},
                                                      {"diff", "@@ -1 +1 @@\n-old\n+new\n"}}}}})};
}

FakeMachineBackend::Answer splitHost(FakeMachineBackend::Answer base) {
    return [base](const FakeMachineBackend::Call& call, QJsonObject* result, QString* error) {
        base(call, result, error);
        const QJsonArray turn = splitTurn(call.sessionId);
        const bool older =
            call.operation == "transcriptPage" ||
            (call.operation == "transcriptWindow" && call.request.contains("before_cursor"));
        if (call.operation != "transcriptWindow" && !older)
            return;
        const QJsonArray events = older ? QJsonArray{turn.at(0), turn.at(1), turn.at(2)}
                                        : QJsonArray{turn.at(3), turn.at(4)};
        const qint64 first = older ? 1 : 4;
        *result = {{"events", events},
                   {"has_more_before", !older},
                   {"snapshot_cursor", 5},
                   {"before_cursor", first},
                   {"first_event_cursor", first}};
    };
}

QVariantMap rowWhere(AgentMessageModel* model, const QString& field, const QString& value) {
    for (int index = 0; index < model->sourceCount(); ++index)
        if (model->source(index).value(field).toString() == value)
            return model->source(index);
    return {};
}

// What the rich renderer shows for the conversation: a user row, the command as a tool row
// with its output, the edit as a file row with its diff, and the Markdown answer.
void verifyRichRows(MachineWorkspaceView& view) {
    AgentWorkspaceView* renderer = view.transcript()->view();
    QVERIFY(renderer->transcriptOnly());
    AgentMessageModel* model = renderer->messageModel();
    QVERIFY(model != nullptr);
    QTRY_VERIFY(model->sourceCount() >= 4);

    QCOMPARE(rowWhere(model, "body", "Fix the build").value("kind").toString(),
             QStringLiteral("user"));
    const QVariantMap command = rowWhere(model, "command", "make test");
    QCOMPARE(command.value("kind").toString(), QStringLiteral("tool"));
    const QVariantMap edit = rowWhere(model, "subtype", "fileChange");
    QCOMPARE(edit.value("kind").toString(), QStringLiteral("file"));
    const QVariantList files = edit.value("fileChanges").toList();
    QCOMPARE(files.size(), 1);
    QVERIFY(files.first().toMap().value("hasDiff").toBool());
    const QVariantMap answer = rowWhere(model, "kind", "assistant");
    QVERIFY2(
        answer.value("body").toString().startsWith("**Fixed.**"),
        qPrintable(QStringList(answer.keys()).join(',') + " / " + answer.value("body").toString()));
    // The delegate renders the body as Markdown with This Mac's renderer (raw HTML escaped).
    QVERIFY(renderer->renderChatMarkdown(answer.value("body").toString()).contains("Fixed."));
    // The rows reach the QML transcript.
    QTRY_VERIFY(model->count() > 0);

    // Only the transcript is shown: the machine view keeps its own composer.
    QQuickItem* root = renderer->quickView()->rootObject();
    QVERIFY(root != nullptr);
    auto* composer = root->findChild<QQuickItem*>("composerProviderSetup");
    QVERIFY(composer == nullptr || !composer->isVisible());
    QVERIFY(view.findChild<QPlainTextEdit*>("machineComposer")->isVisibleTo(&view));
    QVERIFY(view.findChild<QLabel*>("machineTranscriptNote") == nullptr);

    // A diff of a file on the other machine says why it is unavailable instead of opening
    // a file of this Mac.
    renderer->requestFileDiff("src/main.cpp");
    QTRY_VERIFY(view.findChild<QLabel*>("machineComposerReason")
                    ->text()
                    .contains(QStringLiteral("other machine")));
}

} // namespace

class MachineRichTranscriptTest final : public QObject {
    Q_OBJECT

  private slots:
    // ketplus#598: a conversation on a Linux machine over SSH renders through This Mac's rich
    // renderer, and a live event in the session.event shape updates it in place.
    void sshConversationRendersRichRows() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.answer = richHost(sshHost());
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        view.resize(900, 700);
        view.show();
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        view.openConversation("linux-1");
        verifyRichRows(view);

        // eventReceived() nests the event: payload {event_type, data}.
        ssh.deliver(
            {{"session_id", "linux-1"},
             {"session_generation", 1},
             {"cursor", 5},
             {"turn_sequence", 1},
             {"payload", QJsonObject{{"event_type", "item.completed"},
                                     {"data", QJsonObject{{"item", QJsonObject{
                                                                       {"type", "agentMessage"},
                                                                       {"id", "answer-2"},
                                                                       {"text", "Pushed `fix`."},
                                                                   }}}}}}});
        QTRY_COMPARE(transcriptRows(view).last(), QStringLiteral("Pushed `fix`."));
    }

    // An older page that joins a turn already on screen keeps every row rich: the command it
    // brings is a tool row, the answer it completes becomes one row, and the edit keeps its diff.
    // The rows are exactly what This Mac projects from the whole turn at once.
    void anOlderPageJoiningATurnKeepsItsRichRows() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.answer = splitHost(sshHost());
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        view.resize(900, 700);
        view.show();
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        view.openConversation("linux-1");
        AgentWorkspaceView* renderer = view.transcript()->view();
        AgentMessageModel* model = renderer->messageModel();
        QTRY_VERIFY(!rowWhere(model, "kind", "assistant").isEmpty());
        QVERIFY(rowWhere(model, "command", "make test").isEmpty());
        QVERIFY(renderer->historyState().value("hasOlder").toBool());

        renderer->loadOlderMessages(0, 0, 1);
        QTRY_COMPARE(rowWhere(model, "command", "make test").value("kind").toString(),
                     QStringLiteral("tool"));
        QTRY_VERIFY(renderer->historyState().value("requestToken").toLongLong() == 1);
        QCOMPARE(rowWhere(model, "body", "Fix the build").value("kind").toString(),
                 QStringLiteral("user"));
        const QVariantMap edit = rowWhere(model, "subtype", "fileChange");
        QCOMPARE(edit.value("kind").toString(), QStringLiteral("file"));
        QVERIFY(edit.value("fileChanges").toList().first().toMap().value("hasDiff").toBool());
        // The answer split across the pages is one row with the whole Markdown.
        QTRY_COMPARE(rowWhere(model, "kind", "assistant").value("body").toString(),
                     QStringLiteral("**Fixed.** The `main` target builds."));
        const QVariantList whole =
            agent_message::daemonWindowMessages({{QStringLiteral("events"), splitTurn("linux-1")}});
        QCOMPARE(view.transcript()->rows(), whole);
        QCOMPARE(model->sourceCount(), int(whole.size()));
        QVERIFY(!renderer->historyState().value("hasOlder").toBool());
    }

    // The same rows through the Relay route.
    void relayConversationRendersRichRows() {
        FakeMachineBackend relay("fp-relay", Route::Relay);
        bool verified = true;
        relay.answer = richHost(relayHost(&verified));
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        view.resize(900, 700);
        view.show();
        catalog.setBackend(&relay);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        view.openConversation("relay-1");
        verifyRichRows(view);
    }
};

QTEST_MAIN(MachineRichTranscriptTest)
#include "MachineRichTranscriptTest.moc"
