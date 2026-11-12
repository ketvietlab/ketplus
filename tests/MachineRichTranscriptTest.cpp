#include "machine/MachineRichTranscript.h"
#include "FakeMachineBackend.h"
#include "MachineHosts.h"
#include "agent/AgentMessageModel.h"
#include "agent/AgentWorkspaceView.h"
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
