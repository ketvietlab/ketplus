#include "FakeMachineBackend.h"
#include "KetSidebarTree.h"
#include "MachineHosts.h"
#include "agent/AgentWorkspaceView.h"
#include "app/MainWindow.h"
#include "machine/MachineComposition.h"
#include "machine/MachineSelection.h"
#include "machine/MachineWorkspaceView.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"

#include <QAction>
#include <QSettings>
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
};

QTEST_MAIN(MachineWindowTest)
#include "MachineWindowTest.moc"
