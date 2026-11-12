#include "FakeMachineBackend.h"
#include "FakeRelayLink.h"
#include "KetDialog.h"
#include "KetSidebarTree.h"
#include "MachineHosts.h"
#include "agent/AgentWorkspaceView.h"
#include "app/MainWindow.h"
#include "control/ControlV2Client.h"
#include "daemon/FakeSshLink.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineComposition.h"
#include "machine/MachineSelection.h"
#include "machine/MachineWorkspaceView.h"
#include "remote/LinuxMachineModel.h"
#include "remote/LinuxMachines.h"
#include "remote/LinuxMachinesSection.h"
#include "remote/RemoteControlView.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"

#include <QAction>
#include <QFrame>
#include <QJsonArray>
#include <QLabel>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace ketplus::remote {
class RemoteControlTestAccess {
  public:
    static void signIn(ControlV2Client& client) {
        client.credential_.insert(QStringLiteral("deviceId"), QStringLiteral("test-desktop"));
    }
    static void entitle(ControlV2Client& client, const QJsonArray& entitlements) {
        client.credential_.insert(QStringLiteral("entitlements"), entitlements);
    }
};
} // namespace ketplus::remote

using namespace ketplus;
using namespace ketplus::test;

namespace {

QStringList sidebarTitles(const MainWindow& window) {
    QStringList titles;
    for (const auto* row : window.findChildren<ket::SidebarRow*>())
        if (row->isVisibleTo(&window))
            titles.append(row->text());
    return titles;
}

bool shown(const QWidget* widget) { return widget != nullptr && !widget->isHidden(); }

} // namespace

// ADR 0119 in the window: the picker's machine drives the sidebar and the Agent Workspace, every
// way into the Agent Workspace shows that machine, and This Mac comes back as it was.
class MachineWindowTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir settings_;
    QTemporaryDir workspace_;

  private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid() && workspace_.isValid());
        QStandardPaths::setTestModeEnabled(true); // Connections' SSH store stays out of the user's
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        qputenv("KETPLUS_CONVERSATION_WORKTREES",
                QDir(settings_.path()).filePath("conversation-worktrees.sqlite").toUtf8());
    }

    void thePickedMachineDrivesTheWindow() {
        ThemeManager theme(*qApp);
        MainWindow window(theme);
        window.openFolder(workspace_.path());
        window.resize(1100, 760);
        window.show();
        auto* bar = window.findChild<ActivityBar*>();
        QVERIFY(bar != nullptr);
        FakeMachineBackend ssh("fp-ssh", Route::Ssh);
        ssh.answer = sshHost();
        MachineSelection* selection = window.machineComposition()->selection();
        selection->setBackend("fp-ssh", &ssh);

        emit bar->machineRequested("fp-ssh");
        auto* view = window.machineComposition()->existingView();
        QVERIFY(shown(view));
        QTRY_VERIFY(sidebarTitles(window).contains("Fix the build"));
        QVERIFY(!shown(window.findChild<AgentWorkspaceView*>()));

        // A sidebar row opens on the machine; its transcript is the machine's.
        emit bar->conversationRequested("/home/dev/app-fix", "linux-1");
        QTRY_COMPARE(transcriptRows(*view), (QStringList{"Build it", "Built on Linux."}));
        QCOMPARE(ssh.calls("transcriptWindow").size(), 1);

        // View > Agent Workspace and the compact window keep the machine's workspace.
        for (auto* action : window.findChildren<QAction*>())
            if (action->text() == QStringLiteral("Agent Workspace")) {
                action->setChecked(false);
                action->setChecked(true);
            }
        QVERIFY(shown(view));
        QVERIFY(!shown(window.findChild<AgentWorkspaceView*>()));
        emit bar->modeRequested(ActivityBar::Mode::AgentWorkspace);
        QVERIFY(shown(view));
        QVERIFY(!shown(window.findChild<AgentWorkspaceView*>()));

        // The backend going away keeps the machine picked, Offline, with no local lists.
        ssh.setState(State::Offline, "Host unreachable.");
        QTRY_VERIFY(!sidebarTitles(window).contains("Fix the build"));
        QCOMPARE(selection->machineId(), QStringLiteral("fp-ssh"));
        ssh.setState(State::Connected);
        QTRY_VERIFY(sidebarTitles(window).contains("Fix the build"));

        // This Mac again: the machine's workspace hides and the conversation is let go.
        emit bar->machineRequested("local");
        QCOMPARE(selection->machineId(), QStringLiteral("local"));
        QVERIFY(!shown(view));
        QVERIFY(shown(window.findChild<AgentWorkspaceView*>()));
        QTRY_VERIFY(!sidebarTitles(window).contains("Fix the build"));
        QCOMPARE(ssh.closed, (QStringList{"linux-1"}));
    }

    void aMachineWithoutABackendShowsNoLocalLists() {
        ThemeManager theme(*qApp);
        MainWindow window(theme);
        window.openFolder(workspace_.path());
        window.show();
        FakeMachineBackend ssh("fp-late", Route::Ssh);
        ssh.answer = sshHost();
        MachineSelection* selection = window.machineComposition()->selection();

        // Picked before Connections registers a route: nothing is listed, and the selection
        // does not bounce back to This Mac.
        selection->select("fp-late");
        QCOMPARE(selection->machineId(), QStringLiteral("fp-late"));
        QCOMPARE(selection->backend(), nullptr);
        QVERIFY(!sidebarTitles(window).contains("Fix the build"));

        // The route arriving binds it.
        selection->setBackend("fp-late", &ssh);
        QTRY_COMPARE(ssh.calls("listConversations").size(), 1);
        QVERIFY(shown(window.machineComposition()->existingView()));
        QTRY_VERIFY(sidebarTitles(window).contains("Fix the build"));
    }

    // The app's one set of Linux machines (ADR 0119): main() attaches and starts it once, and
    // every window reads from it. Two windows share one backend per machine (one SSH tunnel, one
    // Relay link and so one outbox writer per host), a prompt opens in one window only, and
    // closing a window leaves the other connected.
    void twoWindowsShareOneSetOfLinuxMachines() {
        QTemporaryDir store;
        const QString socket =
            QStringLiteral("ketplus-machine-shared-%1").arg(QCoreApplication::applicationPid());
        remote::ssh::testing::FakeDaemon daemon(socket);
        int tunnels = 0;
        int relayLinks = 0;
        auto shared = std::make_unique<LinuxMachines>();
        shared->attachSshBackend(
            store.filePath("machines.json"),
            [socket,
             &tunnels](const remote::ssh::SshTarget&) -> std::unique_ptr<remote::ssh::SshLink> {
                ++tunnels;
                return std::make_unique<remote::ssh::testing::FakeLink>(socket);
            });
        shared->attachRelayBackend([&relayLinks](const QString& relayId) {
            ++relayLinks;
            return std::make_unique<testing::FakeRelayLink>(relayId);
        });
        shared->start();

        ThemeManager theme(*qApp);
        auto first = std::make_unique<MainWindow>(theme);
        MainWindow second(theme);
        for (MainWindow* window : {first.get(), &second}) {
            window->openFolder(workspace_.path());
            window->resize(1100, 760);
            window->show();
            window->useLinuxMachines(shared.get());
            auto* connections = window->findChild<RemoteControlView*>();
            QVERIFY(connections != nullptr);
            QVERIFY(!shown(connections)); // the picker lists them before Connections opens
            QCOMPARE(connections->linuxMachines()->model(), shared->model());
        }

        // One SSH tunnel, registered in both windows' MachineSelection.
        shared->model()->requestSshConnect("build-node-01");
        const QString id = daemon.fingerprint;
        MachineSelection* firstSelection = first->machineComposition()->selection();
        MachineSelection* secondSelection = second.machineComposition()->selection();
        QTRY_VERIFY(firstSelection->backendFor(id) != nullptr &&
                    secondSelection->backendFor(id) != nullptr);
        QCOMPARE(firstSelection->backendFor(id), secondSelection->backendFor(id));
        QCOMPARE(firstSelection->backendFor(id), shared->machineBackend(id));
        QCOMPARE(tunnels, 1);

        // One Relay link per host once the account is entitled, in both windows too.
        remote::RemoteControlTestAccess::signIn(*shared->client_);
        remote::RemoteControlTestAccess::entitle(*shared->client_,
                                                 QJsonArray{QStringLiteral("ketplus.relay")});
        shared->loading_ = true; // keeps the Relay list request off the network
        shared->syncEntitlement();
        QVERIFY(shared->model()->relayEntitled());
        shared->relay_->setListed({{QStringLiteral("relay-1"), QStringLiteral("SHA256:relayHost"),
                                    QStringLiteral("builder"), QDateTime::currentDateTimeUtc()}});
        QTRY_VERIFY(secondSelection->backendFor("SHA256:relayHost") != nullptr);
        QCOMPARE(firstSelection->backendFor("SHA256:relayHost"),
                 secondSelection->backendFor("SHA256:relayHost"));
        QCOMPARE(relayLinks, 1);

        // Connections is shown in the second window: a prompt opens its card there, once.
        auto* secondBar = second.findChild<ActivityBar*>();
        emit secondBar->modeRequested(ActivityBar::Mode::Remote);
        shared->model()->promptSshConnect("build-node-01");
        const auto cards = [](MainWindow& window) {
            return window.findChildren<ket::Dialog*>(QStringLiteral("linuxPairingDialog")).size();
        };
        QTRY_COMPARE(cards(second), 1);
        QCOMPARE(cards(*first), 0);
        delete second.findChild<ket::Dialog*>(QStringLiteral("linuxPairingDialog"));

        // A Disconnect and a Connect made in the first window show in the second window's rows.
        const auto stateIn = [&id](MainWindow& window) {
            for (auto* row : window.findChildren<QFrame*>(QStringLiteral("linuxMachineRow")))
                if (row->property("machineId").toString() == id)
                    if (auto* state = row->findChild<QLabel*>(QStringLiteral("linuxMachineState")))
                        return state->text();
            return QString();
        };
        first->findChild<RemoteControlView*>()->linuxMachines()->model()->requestDisconnect(id);
        QTRY_VERIFY(stateIn(second).contains(QStringLiteral("Offline")));
        QCOMPARE(stateIn(*first), stateIn(second));
        first->findChild<RemoteControlView*>()->linuxMachines()->model()->requestConnect(id);
        QTRY_VERIFY(stateIn(second).contains(QStringLiteral("Connected")));
        QCOMPARE(tunnels, 1);

        // Closing the first window neither drops nor restarts a connection.
        first.reset();
        QCOMPARE(shared->model()->machine(id)->state, LinuxMachine::State::Connected);
        QCOMPARE(secondSelection->backendFor(id), shared->machineBackend(id));
        QCOMPARE(tunnels, 1);
        QCOMPARE(relayLinks, 1);
        QTest::qWait(50);
        QCOMPARE(shared->model()->machine(id)->state, LinuxMachine::State::Connected);
        emit secondBar->machineRequested("local");

        // Quitting without closing the window first: the set goes before the window, as in
        // main(). The window stays usable, with no machines, until it is destroyed.
        shared.reset();
        auto* section = second.findChild<RemoteControlView*>()->linuxMachines();
        QVERIFY(section->machines() == nullptr);
        QCOMPARE(secondSelection->backendFor(id), nullptr);
        emit secondBar->modeRequested(ActivityBar::Mode::Remote);
        QTest::qWait(20);
    }

    // Connections registers each listed machine's backend (MainWindowRemote's syncBackends call
    // site). A machine picked before its backend exists shows its workspace once Connections
    // registers it; a listed machine without a backend keeps Connections open without bouncing
    // to This Mac; This Mac comes back.
    void connectionsRegistersThePickedMachine() {
        ThemeManager theme(*qApp);
        MainWindow window(theme);
        window.openFolder(workspace_.path());
        window.resize(1100, 760);
        window.show();
        auto* bar = window.findChild<ActivityBar*>();
        emit bar->modeRequested(ActivityBar::Mode::Remote);
        auto* connections = window.findChild<RemoteControlView*>();
        QVERIFY(shown(connections));
        LinuxMachinesSection* section = connections->linuxMachines();
        QTemporaryDir store;
        const QString socket =
            QStringLiteral("ketplus-machine-window-%1").arg(QCoreApplication::applicationPid());
        remote::ssh::testing::FakeDaemon daemon(socket);
        section->attachSshBackend(
            store.filePath("machines.json"),
            [socket](const remote::ssh::SshTarget&) -> std::unique_ptr<remote::ssh::SshLink> {
                return std::make_unique<remote::ssh::testing::FakeLink>(socket);
            });
        MachineSelection* selection = window.machineComposition()->selection();
        section->model()->upsert({.id = "fp-idle", .name = "idle-node"});

        // Picked from the picker with no backend: Connections opens on it, nothing is listed.
        emit bar->machineRequested("fp-idle");
        QCOMPARE(selection->machineId(), QStringLiteral("fp-idle"));
        QCOMPARE(selection->backend(), nullptr);
        QVERIFY(shown(connections));
        QVERIFY(window.machineComposition()->catalog()->repositories().isEmpty());

        // Connecting a machine over SSH registers its backend; picking it shows its workspace.
        section->model()->requestSshConnect("build-node-01");
        const QString id = daemon.fingerprint;
        QTRY_VERIFY(selection->backendFor(id) != nullptr);
        QCOMPARE(selection->machineId(), QStringLiteral("fp-idle")); // not re-picked
        emit bar->machineRequested(id);
        auto* view = window.machineComposition()->existingView();
        QVERIFY(shown(view));
        QVERIFY(!shown(connections));
        QTRY_VERIFY(daemon.requests.contains("workspace.list"));
        QTRY_COMPARE(window.machineComposition()->catalog()->repositories().size(), 1);

        // Back to the machine without a backend, from that remote workspace: Connections opens
        // on it, with no re-entry into This Mac and none of the other machine's lists.
        emit bar->machineRequested("fp-idle");
        QCOMPARE(selection->machineId(), QStringLiteral("fp-idle"));
        QVERIFY(shown(connections));
        QVERIFY(!shown(view));
        QVERIFY(window.machineComposition()->catalog()->repositories().isEmpty());

        // Connections registering its backend later shows its workspace, still not This Mac.
        FakeMachineBackend late("fp-idle", Route::Relay);
        late.answer = sshHost();
        selection->syncBackends({"fp-idle", id}, [&](const QString& machine) {
            return machine == "fp-idle" ? &late : section->machineBackend(machine);
        });
        QVERIFY(shown(view));
        QTRY_COMPARE(late.calls("listConversations").size(), 1);
        QCOMPARE(selection->machineId(), QStringLiteral("fp-idle"));

        emit bar->machineRequested("local");
        QCOMPARE(selection->machineId(), QStringLiteral("local"));
        QVERIFY(shown(window.findChild<AgentWorkspaceView*>()));
        QVERIFY(!shown(view));
        section->model()->setMachines({}); // no machinesChanged reaches a destroyed fake
    }
};

QTEST_MAIN(MachineWindowTest)
#include "MachineWindowTest.moc"
