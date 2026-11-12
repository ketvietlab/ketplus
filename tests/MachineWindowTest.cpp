#include "FakeMachineBackend.h"
#include "KetSidebarTree.h"
#include "MachineHosts.h"
#include "agent/AgentWorkspaceView.h"
#include "app/MainWindow.h"
#include "daemon/FakeSshLink.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineComposition.h"
#include "machine/MachineSelection.h"
#include "machine/MachineWorkspaceView.h"
#include "remote/LinuxMachineModel.h"
#include "remote/LinuxMachinesSection.h"
#include "remote/RemoteControlView.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"

#include <QAction>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

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
