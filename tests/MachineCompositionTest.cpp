#include "DaemonMachineBackend.h"
#include "FakeMachineBackend.h"
#include "MachineHosts.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineConversationSession.h"
#include "machine/MachineSelection.h"
#include "machine/MachineSidebar.h"
#include "machine/MachineWorkspaceView.h"

#include <QComboBox>
#include <QDir>
#include <QFrame>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus;
using namespace ketplus::test;

class MachineCompositionTest final : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;

  private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    }
    void workspaceDisplayMarkerKeepsTheExecutionCatalog() {
        const QJsonArray workspaces{
            QJsonObject{{"path", "/no-git/repository"},
                        {"available", true},
                        {"sidebar_repository_visible", false}},
            QJsonObject{{"path", "/actual/repository"},
                        {"available", true},
                        {"sidebar_repository_visible", true}},
            QJsonObject{{"path", "/legacy/checkout"}, {"available", false}}};
        const auto grouped =
            remote::DaemonMachineBackend::groupWorkspacesByRepository({{"workspaces", workspaces}});
        const auto rows = remote::repositoriesFromListing(grouped);
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows.at(0).label, QStringLiteral("repository"));
        QVERIFY(!rows.at(0).sidebarVisible);
        QCOMPARE(rows.at(0).worktrees.size(), 1);
        QCOMPARE(rows.at(0).worktrees.first().id, QStringLiteral("/no-git/repository"));
        QCOMPARE(rows.at(1).label, QStringLiteral("repository"));
        QVERIFY(rows.at(1).sidebarVisible);
        QVERIFY(rows.at(2).sidebarVisible); // unknown is compatible, not confirmed no-Git
    }
    void sidebarPlacementSurvivesExecutionChangesAndReconnect_data() {
        QTest::addColumn<bool>("relay");
        QTest::addColumn<bool>("noRepository");
        QTest::newRow("local-host-repository") << false << false;
        QTest::newRow("local-host-no-repository") << false << true;
        QTest::newRow("paired-repository") << true << false;
        QTest::newRow("paired-no-repository") << true << true;
    }
    void sidebarPlacementSurvivesExecutionChangesAndReconnect() {
        QFETCH(bool, relay);
        QFETCH(bool, noRepository);
        FakeMachineBackend backend("synthetic-machine", relay ? Route::Relay : Route::Ssh);
        const auto origin = noRepository ? QString{} : QStringLiteral("/synthetic/repository");
        QJsonObject conversation{
            {"session_id", "stable-conversation"},         {"session_generation", 1},
            {"title", "Filter this conversation"},         {"state", "ready"},
            {"worktree_path", "/synthetic/execution-one"}, {"repository_id", "execution-one"}};
        if (relay) {
            conversation.insert("sidebarPlacementKnown", true);
            conversation.insert("sidebarRepositoryId", origin);
            conversation.insert("sidebarRepositoryLabel", "repository");
        } else {
            conversation.insert("sidebar_repository_path", origin);
        }
        backend.answer = [&](const FakeMachineBackend::Call& call, QJsonObject* result, QString*) {
            if (call.operation == "listRepositories")
                *result = {
                    {"repositories",
                     QJsonArray{QJsonObject{
                         {"id", "execution-catalog"},
                         {"label", "repository"},
                         {"sidebarVisible", false},
                         {"worktrees", QJsonArray{QJsonObject{{"id", "/synthetic/execution-one"},
                                                              {"label", "execution-one"}}}}}}}};
            else if (call.operation == "listConversations")
                *result = {{"sessions", QJsonArray{conversation}}};
        };
        MachineCatalog catalog;
        catalog.setBackend(&backend);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        QCOMPARE(catalog.repositories().size(), 1); // kept for execution/worktree selection
        QVERIFY(!catalog.repositories().first().sidebarVisible);
        SidebarRepositoryList sidebar;
        sidebar.resize(280, 600);
        sidebar.show();
        sidebar.setFilterText("Filter this");
        QSignalSpy activated(&sidebar, &SidebarRepositoryList::conversationRequested);
        const auto check = [&] {
            const auto rows = machineSidebarRepositories(catalog, "stable-conversation");
            QCOMPARE(rows.size(), 1);
            QCOMPARE(rows.first().path, origin);
            QCOMPARE(rows.first().name, noRepository ? QObject::tr("Other conversations")
                                                     : QStringLiteral("repository"));
            QVERIFY(rows.first().active);
            QCOMPARE(rows.first().conversations.size(), 1);
            QCOMPARE(rows.first().conversations.first().toMap().value("path").toString(),
                     conversation.value("worktree_path").toString());
            sidebar.setRepositories(rows);
            QCOMPARE(sidebar.filterText(), QStringLiteral("Filter this"));
            auto* button = sidebar.findChild<QAbstractButton*>("productConversationButton");
            QVERIFY(button);
            QCOMPARE(button->property("repositoryPath").toString(), origin);
            const auto previousActivations = activated.size();
            button->click();
            QCOMPARE(activated.size(), previousActivations + 1);
            QCOMPARE(activated.last().at(0).toString(),
                     conversation.value("worktree_path").toString());
            QCOMPARE(activated.last().at(1).toString(), QStringLiteral("stable-conversation"));
            QCOMPARE(sidebar.filterText(), QStringLiteral("Filter this"));
        };
        check();
        conversation.insert("worktree_path", "/synthetic/execution-two");
        conversation.insert("repository_id", "execution-two");
        catalog.refresh();
        QTRY_COMPARE(catalog.conversations().first().worktreeId,
                     QStringLiteral("/synthetic/execution-two"));
        check();
        catalog.setBackend(nullptr);
        catalog.setBackend(&backend);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        check();
        sidebar.setFilterText({});
        QCOMPARE(sidebar.findChildren<QAbstractButton*>("productConversationButton").size(), 1);
        QVERIFY(sidebar.layoutProblems().isEmpty());
        const auto captures = qEnvironmentVariable("KETPLUS_ORIGIN_CAPTURE_DIR");
        if (!captures.isEmpty()) {
            QVERIFY(QDir().mkpath(captures));
            const auto name =
                QStringLiteral("selected-%1-%2.png")
                    .arg(relay ? "paired" : "ssh", noRepository ? "no-repository" : "repository");
            QVERIFY(sidebar.grab().save(QDir(captures).filePath(name)));
        }
        // Membership changes placement for display, while execution remains in execution-two.
        if (relay)
            conversation.insert("team", QJsonObject{{"id", "team-one"},
                                                    {"role", "developer"},
                                                    {"sidebarPlacementKnown", true},
                                                    {"sidebarRepositoryId", "team-origin"},
                                                    {"sidebarRepositoryLabel", "Team repository"}});
        else
            conversation.insert("sidebar_team_repository_path", "team-origin");
        catalog.refresh();
        QTRY_COMPARE(catalog.conversations().first().sidebarRepositoryId,
                     QStringLiteral("team-origin"));
        const auto rows = machineSidebarRepositories(catalog, "stable-conversation");
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().path, QStringLiteral("team-origin"));
        QCOMPARE(rows.first().conversations.first().toMap().value("path").toString(),
                 QStringLiteral("/synthetic/execution-two"));
    }
    void selectionKeepsThePickedMachineWithoutABackend() {
        FakeMachineBackend local("local", Route::Local);
        auto linux = std::make_unique<FakeMachineBackend>("fp-linux", Route::Ssh);
        MachineSelection selection(&local);
        QSignalSpy changed(&selection, &MachineSelection::selectionChanged);
        // Picked before Connections registers it: selected, with no backend (never This Mac's).
        selection.select("fp-linux");
        QCOMPARE(selection.machineId(), QStringLiteral("fp-linux"));
        QCOMPARE(selection.backend(), nullptr);
        QCOMPARE(changed.size(), 1);

        // The backend arriving later binds the selection.
        selection.setBackend("fp-linux", linux.get());
        QCOMPARE(selection.backend(), linux.get());
        QCOMPARE(changed.size(), 2);
        QCOMPARE(changed.last().at(1).value<remote::MachineBackend*>(), linux.get());
        QCOMPARE(selection.machineIds(), (QStringList{"local", "fp-linux"}));

        // A destroyed backend unbinds it; the machine stays picked until Connections drops it.
        linux.reset();
        QCOMPARE(selection.machineId(), QStringLiteral("fp-linux"));
        QCOMPARE(selection.backend(), nullptr);
        QCOMPARE(changed.size(), 3);
        selection.syncBackends({}, [](const QString&) { return nullptr; });
        QCOMPARE(selection.machineId(), QStringLiteral("local"));
        QCOMPARE(selection.backend(), &local);
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

    // A machine with more conversations than one frame holds lists them in live pages
    // (ADR 0131), every page after the last one read, into one list.
    void aLongConversationListIsReadInPages() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        const QList<QStringList> pages{{"a", "b"}, {"c", "d"}, {"e"}};
        QStringList afters;
        ssh.answer = [&](const FakeMachineBackend::Call& call, QJsonObject* result, QString*) {
            if (call.operation != "listConversations") {
                *result = {{"repositories", QJsonArray{}}};
                return;
            }
            QCOMPARE(call.request.value("live_sessions_page_version").toInt(), 1);
            const QString after = call.request.value("live_sessions_after").toString();
            afters << after;
            const int index = after.isEmpty() ? 0 : after == "b" ? 1 : 2;
            QJsonArray sessions;
            for (const QString& id : pages.at(index))
                sessions.append(QJsonObject{{"session_id", id}, {"session_generation", 1}});
            *result = {{"ok", true},
                       {"sessions", sessions},
                       {"live_sessions_page_version", 1},
                       {"live_sessions_next", index < 2 ? pages.at(index).last() : QString{}}};
        };
        MachineCatalog catalog;
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 5);
        QStringList ids;
        for (const auto& conversation : catalog.conversations())
            ids << conversation.sessionId;
        QCOMPARE(ids, (QStringList{"a", "b", "c", "d", "e"}));
        QCOMPARE(afters, (QStringList{"", "b", "d"}));
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

    void connectionsSyncRegistersReplacesAndForgets() {
        FakeMachineBackend local("local", Route::Local);
        FakeMachineBackend relay("fp-a", Route::Relay);
        FakeMachineBackend ssh("fp-a", Route::Ssh);
        FakeMachineBackend other("fp-b", Route::Ssh);
        MachineSelection selection(&local);
        QHash<QString, remote::MachineBackend*> section{{"fp-a", &relay}, {"fp-b", &other}};
        const auto lookup = [&section](const QString& id) { return section.value(id); };
        selection.syncBackends({"fp-a", "fp-b", "fp-c"}, lookup); // fp-c has no backend yet
        QCOMPARE(selection.machineIds(), (QStringList{"local", "fp-a", "fp-b"}));
        selection.select("fp-a");
        QSignalSpy changed(&selection, &MachineSelection::selectionChanged);

        // SSH added for the Relay host: the selected machine rebinds to the new backend.
        section.insert("fp-a", &ssh);
        selection.syncBackends({"fp-a", "fp-b"}, lookup);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.first().at(1).value<remote::MachineBackend*>(), &ssh);
        QCOMPARE(selection.backend(), &ssh);

        // The selected machine removed: the window returns to This Mac.
        selection.syncBackends({"fp-b"}, lookup);
        QCOMPARE(selection.machineId(), QStringLiteral("local"));
        QCOMPARE(selection.machineIds(), (QStringList{"local", "fp-b"}));
    }

    void queuedRelayWritesFinishOnCommandCompleted() {
        bool verified = true;
        FakeMachineBackend relay("fp-relay", Route::Relay);
        auto base = relayHost(&verified);
        relay.answer = [&](const FakeMachineBackend::Call& call, QJsonObject* result,
                           QString* error) {
            if (call.operation == "createSession" || call.operation == "resolveApproval")
                *result = {{"status", "queued"}, {"command_id", call.operation + "-cmd"}};
            else if (call.operation == "pendingApprovals")
                *result = {{"pending_approvals", QJsonArray{QJsonObject{{"approval_id", "ap-9"},
                                                                        {"kind", "command"},
                                                                        {"command", "deploy"}}}}};
            else
                base(call, result, error);
        };
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&relay);
        QTRY_COMPARE(catalog.repositories().size(), 1);
        auto* reason = view.findChild<QLabel*>("machineComposerReason");
        view.startNewConversation();
        view.findChild<QPlainTextEdit*>("machineComposer")->setPlainText("Ship it");
        view.findChild<QPushButton*>("machineSend")->click();
        QCOMPARE(relay.calls("createSession").first().request.value("repository_id").toString(),
                 QStringLiteral("repo-salted"));
        QVERIFY(view.openSessionId().isEmpty());
        QCOMPARE(reason->text(),
                 QStringLiteral("Sent. Waiting for the machine to start the conversation…"));
        QVERIFY(!view.findChild<QPushButton*>("machineSend")->isEnabled());

        relay.complete("other-cmd", {{"session_id", "nope"}});
        QVERIFY(view.openSessionId().isEmpty());
        relay.complete("createSession-cmd", {{"session_id", "relay-2"}, {"session_generation", 1}});
        QCOMPARE(view.openSessionId(), QStringLiteral("relay-2"));

        // A queued approval answer that fails on the host marks the card expired.
        QTRY_COMPARE(view.findChildren<QFrame*>("machineApprovalCard").size(), 1);
        view.findChild<QPushButton*>("machineApprove")->click();
        QCOMPARE(reason->text(), QStringLiteral("Sent. Waiting for the machine to confirm…"));
        relay.complete("resolveApproval-cmd", {}, "approval_expired");
        QTRY_VERIFY(view.findChild<QFrame*>("machineApprovalCard")->property("expired").toBool());
        QCOMPARE(reason->text(), QStringLiteral("approval_expired"));
    }

    void heldWritesSayTheyWaitForTheMachine() {
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.answer = sshHost();
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 1);
        view.openConversation("linux-1");
        QTRY_COMPARE(transcriptRows(view).size(), 2);
        auto* reason = view.findChild<QLabel*>("machineComposerReason");

        ssh.hold = true; // the route holds the prompt until the hello
        view.findChild<QPlainTextEdit*>("machineComposer")->setPlainText("Run the tests");
        view.findChild<QPushButton*>("machineSend")->click();
        QCOMPARE(reason->text(), QStringLiteral("Sending…"));
        ssh.setState(State::Reconnecting, "Connection lost.");
        QVERIFY(reason->text().startsWith("Waiting for the machine to reconnect."));
        QVERIFY(reason->text().contains("15 minutes"));

        ssh.hold = false;
        ssh.setState(State::Connected);
        ssh.release();
        QTRY_VERIFY(reason->text().isEmpty());
        QCOMPARE(ssh.calls("startTurn").size(), 1);
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
