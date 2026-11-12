#include "FakeMachineBackend.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineConversationSession.h"
#include "machine/MachineSelection.h"
#include "machine/MachineSidebar.h"
#include "machine/MachineWorkspaceView.h"

#include <QComboBox>
#include <QFrame>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QtTest>

using namespace ketplus;
using test::FakeMachineBackend;
using Route = remote::MachineBackend::Route;
using State = remote::MachineBackend::ConnectionState;

namespace {

QJsonObject journalEvent(const QString& session, qint64 cursor, const QString& type,
                         const QJsonObject& payload) {
    return {{"session_id", session},
            {"session_generation", 1},
            {"cursor", cursor},
            {"event_type", type},
            {"payload", payload}};
}

// A Linux host over SSH: the daemon's local protocol, a repository with a linked worktree.
FakeMachineBackend::Answer sshHost() {
    return [](const FakeMachineBackend::Call& call, QJsonObject* result, QString*) {
        if (call.operation == "listRepositories") {
            *result = {{"repositories",
                        QJsonArray{QJsonObject{
                            {"id", "/home/dev/app"},
                            {"label", "app"},
                            {"path", "/home/dev/app"},
                            {"available", true},
                            {"worktrees", QJsonArray{QJsonObject{{"id", "/home/dev/app"},
                                                                 {"label", "app"},
                                                                 {"path", "/home/dev/app"},
                                                                 {"available", true}},
                                                     QJsonObject{{"id", "/home/dev/app-fix"},
                                                                 {"label", "app-fix"},
                                                                 {"path", "/home/dev/app-fix"},
                                                                 {"available", true}}}}}}}};
        } else if (call.operation == "listConversations") {
            *result = {{"sessions", QJsonArray{QJsonObject{{"session_id", "linux-1"},
                                                           {"session_generation", 1},
                                                           {"worktree_path", "/home/dev/app-fix"},
                                                           {"title", "Fix the build"},
                                                           {"state", "ready"}}}}};
        } else if (call.operation == "transcriptWindow") {
            *result = {
                {"events",
                 QJsonArray{journalEvent(call.sessionId, 1, "user.message", {{"text", "Build it"}}),
                            journalEvent(call.sessionId, 2, "assistant.delta",
                                         {{"item_id", "a"}, {"delta", "Built on Linux."}})}}};
        } else if (call.operation == "pendingApprovals") {
            *result = {{"pending_approvals", QJsonArray{}}};
        } else if (call.operation == "createSession") {
            *result = {{"session_id", "linux-2"}, {"session_generation", 1}};
        }
    };
}

// A host through the Relay: salted ids, no host paths, one worktree still verifying.
FakeMachineBackend::Answer relayHost(bool* verified) {
    return [verified](const FakeMachineBackend::Call& call, QJsonObject* result, QString*) {
        if (call.operation == "listRepositories") {
            QJsonArray worktrees{QJsonObject{{"worktreeId", "wt-main"}, {"label", "main"}}};
            QJsonArray pending;
            if (*verified)
                worktrees.append(QJsonObject{{"worktreeId", "wt-new"}, {"label", "feature"}});
            else
                pending.append(QJsonObject{{"kind", "worktree"},
                                           {"id", "wt-new"},
                                           {"machineId", "relay-machine"},
                                           {"reason", "verification_pending"}});
            *result = {{"repositories", QJsonArray{QJsonObject{{"id", "repo-salted"},
                                                               {"label", "site"},
                                                               {"available", true},
                                                               {"worktrees", worktrees}}}},
                       {"pending", pending}};
        } else if (call.operation == "listConversations") {
            *result = {{"sessions", QJsonArray{QJsonObject{{"id", "relay-1"},
                                                           {"generation", 4},
                                                           {"repositoryId", "repo-salted"},
                                                           {"worktreeId", "wt-main"},
                                                           {"title", "Deploy"}}}}};
        } else if (call.operation == "pendingApprovals") {
            *result = {{"pending_approvals", QJsonArray{}}};
        }
    };
}

QStringList transcriptRows(const MachineWorkspaceView& view) {
    QStringList rows;
    auto* list = view.findChild<QListWidget*>("machineTranscript");
    for (int i = 0; i < list->count(); ++i)
        rows.append(list->item(i)->text());
    return rows;
}

} // namespace

class MachineCompositionTest final : public QObject {
    Q_OBJECT

  private slots:
    void selectionKnowsOnlyRegisteredMachines() {
        FakeMachineBackend local("local", Route::Local);
        auto linux = std::make_unique<FakeMachineBackend>("fp-linux", Route::Ssh);
        MachineSelection selection(&local);
        QSignalSpy changed(&selection, &MachineSelection::selectionChanged);
        selection.select("fp-linux"); // not registered yet: nothing changes
        QCOMPARE(selection.machineId(), QStringLiteral("local"));
        QCOMPARE(changed.size(), 0);

        selection.setBackend("fp-linux", linux.get());
        selection.select("fp-linux");
        QCOMPARE(selection.backend(), linux.get());
        QCOMPARE(selection.machineIds(), (QStringList{"local", "fp-linux"}));
        // A destroyed backend is forgotten and the window returns to This Mac.
        linux.reset();
        QCOMPARE(selection.machineId(), QStringLiteral("local"));
        QCOMPARE(selection.backend(), &local);
        QCOMPARE(changed.size(), 2);
    }

    void switchingMachinesReplacesEveryViewAndRestores() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.answer = sshHost();
        bool verified = true;
        FakeMachineBackend relay("fp-relay", Route::Relay);
        relay.answer = relayHost(&verified);
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);

        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        auto rows = machineSidebarRepositories(catalog, {});
        QCOMPARE(rows.size(), 1); // one repository, its linked worktree included
        QCOMPARE(rows.first().name, QStringLiteral("app"));
        QCOMPARE(rows.first().conversations.first().toMap().value("path").toString(),
                 QStringLiteral("/home/dev/app-fix"));

        view.openConversation("linux-1");
        QTRY_COMPARE(transcriptRows(view), (QStringList{"Build it", "Built on Linux."}));
        QCOMPARE(ssh.calls("transcriptWindow").first().generation, 1);

        // An answer from the previous machine still on its way is dropped.
        ssh.hold = true;
        catalog.refresh();
        QTRY_COMPARE(ssh.calls("listRepositories").size(), 2);
        catalog.setBackend(&relay);
        ssh.release();
        QTRY_COMPARE(catalog.conversations().size(), 1);
        QCOMPARE(catalog.conversations().first().sessionId, QStringLiteral("relay-1"));
        rows = machineSidebarRepositories(catalog, {});
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().path, QStringLiteral("repo-salted"));
        QVERIFY(transcriptRows(view).isEmpty()); // the SSH transcript is not shown here
        QVERIFY(view.findChild<QComboBox*>("machineWorktreePicker")->isVisibleTo(&view));

        // Back to the SSH machine: its conversation and transcript return as they were.
        ssh.hold = false;
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        QCOMPARE(view.openSessionId(), QStringLiteral("linux-1"));
        QCOMPARE(transcriptRows(view), (QStringList{"Build it", "Built on Linux."}));
        QCOMPARE(machineSidebarRepositories(catalog, view.openSessionId())
                     .first()
                     .conversations.first()
                     .toMap()
                     .value("selected")
                     .toBool(),
                 true);
    }

    void offlineAndReconnectingFollowTheAdr() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.answer = sshHost();
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        view.openConversation("linux-1");
        QTRY_COMPARE(transcriptRows(view).size(), 2);
        auto* composer = view.findChild<QPlainTextEdit*>("machineComposer");
        auto* send = view.findChild<QPushButton*>("machineSend");
        auto* reason = view.findChild<QLabel*>("machineComposerReason");
        auto* banner = view.findChild<QLabel*>("machineStateBanner");
        QVERIFY(composer->isEnabled());

        // Reconnecting: last lists stay, read-only; the composer says why it is disabled.
        ssh.setState(State::Reconnecting, "Connection lost.");
        QVERIFY(catalog.readOnly());
        QCOMPARE(catalog.conversations().size(), 1);
        QVERIFY(!composer->isEnabled());
        QVERIFY(!send->isEnabled());
        QCOMPARE(reason->text(), QStringLiteral("Reconnecting… Connection lost."));
        QCOMPARE(transcriptRows(view).size(), 2); // readable from the cache

        // Offline: no list, the reason instead; never This Mac's lists.
        ssh.setState(State::Offline, "Host unreachable.");
        QVERIFY(catalog.conversations().isEmpty());
        QVERIFY(machineSidebarRepositories(catalog, {}).isEmpty());
        QCOMPARE(banner->text(), QStringLiteral("Host unreachable."));
        QCOMPARE(transcriptRows(view).size(), 2);

        // Recovery is automatic: lists, transcript and approvals are read again.
        ssh.clearCalls();
        ssh.setState(State::Connected);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        QCOMPARE(ssh.calls("transcriptWindow").size(), 1);
        QCOMPARE(ssh.calls("pendingApprovals").size(), 1);
        QVERIFY(composer->isEnabled());
        QVERIFY(banner->isHidden());
    }

    void pendingWorktreesFillInOnCatalogChanged() {
        bool verified = false;
        FakeMachineBackend relay("fp-relay", Route::Relay);
        relay.answer = relayHost(&verified);
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&relay);
        QTRY_COMPARE(catalog.pendingWorktrees().size(), 1);
        auto* picker = view.findChild<QComboBox*>("machineWorktreePicker");
        auto* model = qobject_cast<QStandardItemModel*>(picker->model());
        QCOMPARE(picker->count(), 2);
        QCOMPARE(picker->itemText(1), QStringLiteral("wt-new · verifying…"));
        QVERIFY(!model->item(1)->isEnabled());
        QCOMPARE(picker->currentIndex(), 0);

        verified = true;
        relay.announceCatalogChange();
        QTRY_VERIFY(catalog.pendingWorktrees().isEmpty());
        QCOMPARE(picker->count(), 2);
        QCOMPARE(picker->itemData(1).toString(), QStringLiteral("wt-new"));
        QVERIFY(model->item(1)->isEnabled());
    }

    void newConversationStartsThroughCreateSession() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.answer = sshHost();
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        QSignalSpy opened(&view, &MachineWorkspaceView::conversationOpened);
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.repositories().size(), 1);
        view.startNewConversation("/home/dev/app");
        auto* picker = view.findChild<QComboBox*>("machineWorktreePicker");
        picker->setCurrentIndex(picker->findData("/home/dev/app-fix"));
        view.findChild<QPlainTextEdit*>("machineComposer")->setPlainText("Add a test");
        view.findChild<QPushButton*>("machineSend")->click();

        QCOMPARE(ssh.calls("createSession").size(), 1);
        QCOMPARE(ssh.calls("createSession").first().request,
                 (QJsonObject{{"worktree_id", "/home/dev/app-fix"},
                              {"repository_id", "/home/dev/app"},
                              {"prompt", "Add a test"}}));
        QCOMPARE(view.openSessionId(), QStringLiteral("linux-2"));
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.first().at(0).toString(), QStringLiteral("fp-ssh"));
        QVERIFY(view.session()->running());

        // A prompt in the running conversation steers it on the same machine.
        view.findChild<QPlainTextEdit*>("machineComposer")->setPlainText("Also lint");
        view.findChild<QPushButton*>("machineSend")->click();
        QCOMPARE(ssh.calls("steerTurn").size(), 1);
        QCOMPARE(ssh.calls("steerTurn").first().sessionId, QStringLiteral("linux-2"));
    }

    void approvalRoundTripUsesTheConversationsMachine() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        auto base = sshHost();
        bool pending = true;
        QString resolveError;
        ssh.answer = [&](const FakeMachineBackend::Call& call, QJsonObject* result,
                         QString* error) {
            if (call.operation == "pendingApprovals") {
                *result = {{"pending_approvals",
                            pending ? QJsonArray{QJsonObject{{"approval_id", "ap-1"},
                                                             {"kind", "command"},
                                                             {"command", "make install"}}}
                                    : QJsonArray{}}};
            } else if (call.operation == "resolveApproval") {
                *error = resolveError;
            } else {
                base(call, result, error);
            }
        };
        FakeMachineBackend other("fp-other", Route::Relay);
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        view.openConversation("linux-1");
        QTRY_COMPARE(view.findChildren<QFrame*>("machineApprovalCard").size(), 1);

        // Disabled while the machine cannot act.
        ssh.setState(State::Reconnecting, "Connection lost.");
        QVERIFY(!view.findChild<QPushButton*>("machineApprove")->isEnabled());
        ssh.setState(State::Connected);
        QTRY_VERIFY(view.findChild<QPushButton*>("machineApprove")->isEnabled());

        view.findChild<QPushButton*>("machineApprove")->click();
        const auto resolved = ssh.calls("resolveApproval");
        QCOMPARE(resolved.size(), 1);
        QCOMPARE(resolved.first().sessionId, QStringLiteral("linux-1"));
        QCOMPARE(resolved.first().request,
                 (QJsonObject{{"approval_id", "ap-1"},
                              {"resolution", QJsonObject{{"decision", "accept"}}}}));
        QVERIFY(other.calls("resolveApproval").isEmpty());
        ssh.deliver(journalEvent("linux-1", 9, "approval.resolved", {{"approval_id", "ap-1"}}));
        QTRY_VERIFY(view.findChildren<QFrame*>("machineApprovalCard").isEmpty());

        // An answer that fails marks the card expired.
        ssh.deliver(journalEvent("linux-1", 10, "approval.requested", {{"approval_id", "ap-1"}}));
        QTRY_COMPARE(view.findChildren<QFrame*>("machineApprovalCard").size(), 1);
        resolveError = "The approval expired.";
        view.findChild<QPushButton*>("machineDecline")->click();
        QTRY_VERIFY(view.findChild<QFrame*>("machineApprovalCard")->property("expired").toBool());
        QVERIFY(view.findChild<QPushButton*>("machineApprove") == nullptr);
    }

    void transcriptIgnoresOtherSessionsAndRepeats() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.answer = sshHost();
        MachineConversationSession session(&ssh, "linux-1", 1);
        session.load();
        QCOMPARE(session.transcript().items().size(), 2);
        ssh.deliver(
            journalEvent("linux-1", 2, "assistant.delta", {{"item_id", "a"}, {"delta", "x"}}));
        ssh.deliver(journalEvent("other", 3, "user.message", {{"text", "elsewhere"}}));
        ssh.deliver(
            journalEvent("linux-1", 3, "assistant.delta", {{"item_id", "a"}, {"delta", "!"}}));
        QCOMPARE(session.transcript().items().size(), 2);
        QCOMPARE(session.transcript().items().last().text, QStringLiteral("Built on Linux.!"));
    }
};

QTEST_MAIN(MachineCompositionTest)
#include "MachineCompositionTest.moc"
