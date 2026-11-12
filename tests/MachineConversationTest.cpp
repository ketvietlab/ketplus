#include "FakeMachineBackend.h"
#include "MachineHosts.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineConversationSession.h"
#include "machine/MachineSidebar.h"
#include "machine/MachineWorkspaceView.h"

#include <QFrame>
#include <QJsonArray>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QtTest>

using namespace ketplus;
using namespace ketplus::test;

namespace {

// An event as the Relay delivers it (ADR 0119 Control v2 spelling).
QJsonObject relayEvent(const QString& session, qint64 cursor, const QString& type,
                       const QJsonObject& payload) {
    return {{"sessionId", session}, {"sessionGeneration", 4}, {"cursor", cursor},
            {"eventType", type},    {"turnSequence", 1},      {"payload", payload}};
}

QPushButton* option(QWidget& view, const QString& id) {
    for (auto* button : view.findChildren<QPushButton*>("machineQuestionOption"))
        if (button->property("optionId").toString() == id)
            return button;
    return nullptr;
}

} // namespace

// Review round 1 of T-14: the Relay's event spelling, whole question requests, the listing's
// running state, conversations outside listed repositories, and conversationClosed().
class MachineConversationTest final : public QObject {
    Q_OBJECT

  private slots:
    void relayEventsReachTheTranscriptAndTheCatalog() {
        bool verified = true;
        FakeMachineBackend relay("fp-relay", Route::Relay);
        auto base = relayHost(&verified);
        relay.answer = [&](const FakeMachineBackend::Call& call, QJsonObject* result,
                           QString* error) {
            if (call.operation == "transcriptWindow")
                *result = {{"events", QJsonArray{relayEvent(call.sessionId, 1, "user.message",
                                                            {{"text", "Deploy"}})}}};
            else
                base(call, result, error);
        };
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&relay);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        view.openConversation("relay-1");
        QTRY_COMPARE(transcriptRows(view), (QStringList{"Deploy"}));
        QCOMPARE(relay.calls("transcriptWindow").first().generation, 4);

        const auto listings = relay.calls("listConversations").size();
        relay.deliver(relayEvent("relay-1", 2, "assistant.delta",
                                 {{"item_id", "a"}, {"delta", "Deployed."}}));
        relay.deliver(
            relayEvent("other", 3, "assistant.delta", {{"item_id", "b"}, {"delta", "elsewhere"}}));
        QCOMPARE(transcriptRows(view), (QStringList{"Deploy", "Deployed."}));
        relay.deliver(relayEvent("relay-1", 4, "turn.completed", {}));
        QTRY_COMPARE(relay.calls("listConversations").size(), listings + 1);
    }

    void questionsAreAnsweredWhole() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        auto base = sshHost();
        const QJsonArray questions{
            QJsonObject{{"id", "q-env"},
                        {"revision", 2},
                        {"header", "Target"},
                        {"prompt", "Where to?"},
                        {"selection", "single"},
                        {"options", QJsonArray{QJsonObject{{"id", "staging"}, {"label", "Staging"}},
                                               QJsonObject{{"id", "prod"}, {"label", "Prod"}}}}},
            QJsonObject{{"id", "q-checks"},
                        {"revision", 1},
                        {"prompt", "Which checks?"},
                        {"selection", "multiple"},
                        {"allow_custom", true},
                        {"options", QJsonArray{QJsonObject{{"id", "lint"}, {"label", "Lint"}},
                                               QJsonObject{{"id", "unit"}, {"label", "Unit"}}}}},
            QJsonObject{{"id", "q-token"}, {"revision", 1}, {"prompt", "Token"}, {"secret", true}}};
        ssh.answer = [&](const FakeMachineBackend::Call& call, QJsonObject* result,
                         QString* error) {
            if (call.operation == "pendingApprovals")
                *result = {
                    {"pending_approvals", QJsonArray{QJsonObject{{"approval_id", "qa-1"},
                                                                 {"kind", "input"},
                                                                 {"revision", 3},
                                                                 {"questions", questions}}}}};
            else
                base(call, result, error);
        };
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        view.openConversation("linux-1");
        QTRY_COMPARE(view.findChildren<QFrame*>("machineQuestionCard").size(), 1);
        auto* submit = view.findChild<QPushButton*>("machineQuestionSubmit");
        auto answers = view.findChildren<QLineEdit*>("machineQuestionAnswer");
        QCOMPARE(answers.size(), 2); // the custom answer and the secret, which hides its text
        QCOMPARE(answers.last()->echoMode(), QLineEdit::Password);

        option(view, "prod")->click();
        option(view, "staging")->click(); // single selection: replaces Prod
        QVERIFY(!option(view, "prod")->isChecked());
        option(view, "lint")->click();
        option(view, "unit")->click();
        QVERIFY(!submit->isEnabled()); // the token is still unanswered
        answers.last()->setText("s3cret");
        QVERIFY(submit->isEnabled());
        submit->click();

        const auto answered = ssh.calls("answerQuestion");
        QCOMPARE(answered.size(), 1);
        const QJsonObject resolution = answered.first().request.value("resolution").toObject();
        QCOMPARE(resolution.value("request_id").toString(), QStringLiteral("qa-1"));
        QCOMPARE(resolution.value("revision").toInt(), 3);
        QCOMPARE(resolution.value("action").toString(), QStringLiteral("answer"));
        QCOMPARE(resolution.value("answers").toArray(),
                 (QJsonArray{QJsonObject{{"question_id", "q-env"},
                                         {"question_revision", 2},
                                         {"option_ids", QJsonArray{"staging"}}},
                             QJsonObject{{"question_id", "q-checks"},
                                         {"question_revision", 1},
                                         {"option_ids", QJsonArray{"lint", "unit"}}},
                             QJsonObject{{"question_id", "q-token"},
                                         {"question_revision", 1},
                                         {"option_ids", QJsonArray{}},
                                         {"custom_answer", "s3cret"}}}));
    }

    void theListingSaysWhetherATurnStillRuns() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        auto base = sshHost();
        QString state = "running";
        ssh.answer = [&](const FakeMachineBackend::Call& call, QJsonObject* result,
                         QString* error) {
            base(call, result, error);
            if (call.operation == "listConversations") {
                QJsonArray sessions = result->value("sessions").toArray();
                QJsonObject row = sessions.first().toObject();
                row.insert("state", state);
                *result = {{"sessions", QJsonArray{row}}};
            }
        };
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        view.openConversation("linux-1");
        QVERIFY(view.session()->running());

        // The turn ended while the link was down; the listing after reconnect says so.
        ssh.setState(State::Reconnecting, "Connection lost.");
        state = "ready";
        ssh.setState(State::Connected);
        QTRY_VERIFY(!view.session()->running());
        view.findChild<QPlainTextEdit*>("machineComposer")->setPlainText("Next step");
        view.findChild<QPushButton*>("machineSend")->click();
        QCOMPARE(ssh.calls("startTurn").size(), 1);
        QVERIFY(ssh.calls("steerTurn").isEmpty());
    }

    void conversationsOutsideListedRepositoriesStayListed() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        auto base = sshHost();
        ssh.answer = [&](const FakeMachineBackend::Call& call, QJsonObject* result,
                         QString* error) {
            base(call, result, error);
            if (call.operation == "listConversations") {
                QJsonArray sessions = result->value("sessions").toArray();
                sessions.append(QJsonObject{{"session_id", "scratch-1"},
                                            {"session_generation", 1},
                                            {"worktree_path", "/tmp/scratch"},
                                            {"title", "Scratch"}});
                *result = {{"sessions", sessions}};
            }
        };
        MachineCatalog catalog;
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 2);
        const auto rows = machineSidebarRepositories(catalog, {});
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.last().path, QStringLiteral("/tmp/scratch"));
        QCOMPARE(rows.last().name, QStringLiteral("scratch"));
        QCOMPARE(rows.last().conversations.first().toMap().value("sessionId").toString(),
                 QStringLiteral("scratch-1"));
    }

    void closingAConversationTellsItsMachine() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.answer = sshHost();
        bool verified = true;
        FakeMachineBackend relay("fp-relay", Route::Relay);
        relay.answer = relayHost(&verified);
        MachineCatalog catalog;
        auto view = std::make_unique<MachineWorkspaceView>(&catalog);
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        view->openConversation("linux-1");
        QVERIFY(ssh.closed.isEmpty());

        // Switching machines stops following it; switching back follows it again.
        catalog.setBackend(&relay);
        QCOMPARE(ssh.closed, (QStringList{"linux-1"}));
        view->openConversation("relay-1");
        catalog.setBackend(&ssh);
        QCOMPARE(relay.closed, (QStringList{"relay-1"}));
        QTRY_COMPARE(view->openSessionId(), QStringLiteral("linux-1"));

        // Deselected for a new conversation, then a conversation open when the view goes.
        view->startNewConversation();
        QCOMPARE(ssh.closed, (QStringList{"linux-1", "linux-1"}));
        view->openConversation("linux-1");
        view.reset();
        QCOMPARE(ssh.closed, (QStringList{"linux-1", "linux-1", "linux-1"}));
    }
};

QTEST_MAIN(MachineConversationTest)
#include "MachineConversationTest.moc"
