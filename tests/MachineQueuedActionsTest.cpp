#include "FakeMachineBackend.h"
#include "MachineHosts.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineConversationActions.h"
#include "machine/MachineWorkspaceView.h"

#include <QJsonArray>
#include <QLabel>
#include <QtTest>

#include <memory>

using namespace ketplus;
using namespace ketplus::test;

namespace {

using Feature = remote::MachineBackend::Feature;

// A Linux host with two conversations. A fork is listed only once `host.listFork` is set, as
// the Relay lists a fork after it verified it. `queued` answers the fork as the Relay does.
struct ForkingHost {
    bool listFork{false};
    bool queued{false};
    QString forkId;

    FakeMachineBackend::Answer answer() {
        return [this, base = sshHost()](const FakeMachineBackend::Call& call, QJsonObject* result,
                                        QString* error) {
            base(call, result, error);
            if (call.operation == "forkConversation") {
                forkId = call.request.value("fork_session_id").toString();
                *result = queued ? QJsonObject{{"command_id", "cmd-fork"}, {"status", "queued"}}
                                 : QJsonObject{{"session_id", forkId}, {"session_generation", 1}};
            } else if (call.operation == "listConversations") {
                QJsonArray rows;
                const auto row = [](const QString& id, const QString& title) {
                    return QJsonObject{{"session_id", id},
                                       {"session_generation", 1},
                                       {"worktree_path", "/home/dev/app"},
                                       {"title", title},
                                       {"state", "ready"}};
                };
                rows.append(row("c-1", "First"));
                rows.append(row("c-2", "Second"));
                if (listFork && !forkId.isEmpty())
                    rows.append(row(forkId, "Fork"));
                *result = {{"sessions", rows}};
            }
        };
    }
};

bool listed(const MachineCatalog& catalog, const QString& sessionId) {
    return !catalog.conversation(sessionId).sessionId.isEmpty();
}

QString statusLine(const MachineWorkspaceView& view) {
    return view.findChild<QLabel*>("machineComposerReason")->text();
}

} // namespace

// T-21: a fork the machine confirmed opens once it is listed, but only while the reader still
// reads the conversation they forked; a stale pending fork is dropped, never left to pull the
// view away later.
class MachineQueuedActionsTest final : public QObject {
    Q_OBJECT

  private slots:
    void openingAnotherConversationDropsAPendingFork() {
        ForkingHost host;
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.features = {Feature::Fork};
        ssh.answer = host.answer();
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 2);
        view.openConversation("c-1");

        view.actions()->fork("c-1");
        QVERIFY(!host.forkId.isEmpty());
        QCOMPARE(view.pendingFork(), host.forkId); // confirmed, not listed yet
        QCOMPARE(view.openSessionId(), QStringLiteral("c-1"));

        view.openConversation("c-2");
        QVERIFY(view.pendingFork().isEmpty());
        host.listFork = true;
        catalog.refresh();
        QTRY_VERIFY(listed(catalog, host.forkId));
        QCOMPARE(view.openSessionId(), QStringLiteral("c-2"));
    }

    void startingANewConversationDropsAPendingFork() {
        ForkingHost host;
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.features = {Feature::Fork};
        ssh.answer = host.answer();
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 2);
        view.openConversation("c-1");
        view.actions()->fork("c-1");
        QVERIFY(!view.pendingFork().isEmpty());

        view.startNewConversation();
        QVERIFY(view.pendingFork().isEmpty());
        host.listFork = true;
        catalog.refresh();
        QTRY_VERIFY(listed(catalog, host.forkId));
        QVERIFY(view.openSessionId().isEmpty()); // still the new-conversation composer
    }

    // The Relay confirms the fork after the reader moved on: it is listed, nothing opens.
    void aForkConfirmedAfterTheReaderMovedOnIsOnlyListed() {
        ForkingHost host;
        host.queued = true;
        FakeMachineBackend relay("fp-relay", Route::Relay);
        relay.features = {Feature::Fork};
        relay.answer = host.answer();
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&relay);
        QTRY_COMPARE(catalog.conversations().size(), 2);
        view.openConversation("c-1");

        view.actions()->fork("c-1");
        QCOMPARE(statusLine(view), QStringLiteral("Fork sent. Waiting for the machine…"));
        view.openConversation("c-2");

        host.listFork = true;
        relay.complete("cmd-fork", {{"session_id", host.forkId}, {"session_generation", 1}});
        QTRY_VERIFY(listed(catalog, host.forkId));
        QCOMPARE(view.openSessionId(), QStringLiteral("c-2"));
        QVERIFY(view.pendingFork().isEmpty());
        QVERIFY(view.actions()->pendingStatus().isEmpty());
    }

    // A fork that fails or expires leaves nothing pending, and says why.
    void aFailedOrExpiredForkLeavesNothingPending() {
        for (const QString& failure :
             {QStringLiteral("The command expired before the host answered."),
              QStringLiteral("The host could not fork this conversation.")}) {
            ForkingHost host;
            host.queued = true;
            FakeMachineBackend relay("fp-relay", Route::Relay);
            relay.features = {Feature::Fork};
            relay.answer = host.answer();
            MachineCatalog catalog;
            MachineWorkspaceView view(&catalog);
            catalog.setBackend(&relay);
            QTRY_COMPARE(catalog.conversations().size(), 2);
            view.openConversation("c-1");

            view.actions()->fork("c-1");
            relay.complete("cmd-fork", {}, failure);
            QTRY_COMPARE(statusLine(view), failure);
            QVERIFY(view.pendingFork().isEmpty());
            QVERIFY(view.actions()->pendingStatus().isEmpty());
            host.listFork = true; // even if the id shows up later, nothing opens
            catalog.refresh();
            QTRY_VERIFY(listed(catalog, host.forkId));
            QCOMPARE(view.openSessionId(), QStringLiteral("c-1"));
        }
    }

    // A machine that goes away takes its pending fork and its queued commands with it.
    void aMachineGoingAwayDropsWhatItsCommandsWaitFor() {
        ForkingHost confirmed;
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.features = {Feature::Fork};
        ssh.answer = confirmed.answer();
        MachineCatalog catalog;
        MachineWorkspaceView view(&catalog);
        catalog.setBackend(&ssh);
        QTRY_COMPARE(catalog.conversations().size(), 2);
        view.openConversation("c-1");
        view.actions()->fork("c-1");
        QVERIFY(!view.pendingFork().isEmpty());
        catalog.setBackend(nullptr);
        QVERIFY(view.pendingFork().isEmpty());

        ForkingHost queued;
        queued.queued = true;
        auto relay = std::make_unique<FakeMachineBackend>("fp-relay", Route::Relay);
        relay->features = {Feature::Fork};
        relay->answer = queued.answer();
        catalog.setBackend(relay.get());
        QTRY_COMPARE(catalog.conversations().size(), 2);
        view.openConversation("c-1");
        view.actions()->fork("c-1");
        QCOMPARE(view.actions()->pendingStatus(),
                 QStringLiteral("Fork sent. Waiting for the machine…"));
        relay.reset();
        QVERIFY(view.actions()->pendingStatus().isEmpty());
        QVERIFY(view.pendingFork().isEmpty());
    }
};

QTEST_MAIN(MachineQueuedActionsTest)
#include "MachineQueuedActionsTest.moc"
