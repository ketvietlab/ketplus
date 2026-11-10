#include "KetDialog.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexAppServer.h"
#include "app/ExecutionWorktreeDialog.h"
#include "app/MainWindow.h"
#include "editor/EditorWidget.h"
#include "git/GitService.h"
#include "ui/Theme.h"
#include "workspace/ExplorerPanel.h"
#include "workspace/SidebarRepositoryList.h"
#include <QAbstractButton>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtTest>
class WorktreePickerTest : public QObject {
    Q_OBJECT
  private slots:
    void filesContextDoesNotNavigateConversation() {
        QTemporaryDir directory;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        qputenv("KETPLUS_DAEMON_SOCKET", directory.filePath("isolated.sock").toUtf8());
        qputenv("KETPLUS_CONVERSATION_WORKTREES", directory.filePath("worktrees.sqlite").toUtf8());
        const auto first = directory.filePath("first");
        const auto second = directory.filePath("second");
        QDir().mkpath(first);
        QDir().mkpath(second);
        QProcess git;
        git.start("git", {"-C", second, "init", "-q"});
        QVERIFY(git.waitForFinished());
        QCOMPARE(git.exitCode(), 0);
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(first);
        window.show();
        window.findChild<QAbstractButton*>("agentWorkspaceActivityButton")->click();
        auto* agent = window.findChild<ketplus::AgentWorkspaceView*>();
        QVERIFY(agent);
        const auto before = agent->activePath();
        agent->setExecutionBranch("agent-branch", first, 2);
        window.setFileContextWorktree(second);
        QCOMPARE(agent->activePath(), before);
        QCOMPARE(window.findChild<ketplus::ExplorerPanel*>()->rootPath(),
                 QFileInfo(second).canonicalFilePath());
        auto* service = window.findChild<ketplus::GitService*>();
        QVERIFY(service);
        QTRY_COMPARE(service->snapshot().repositoryRoot, QFileInfo(second).canonicalFilePath());
        QCOMPARE(agent->executionPath(), first);
        QCOMPARE(agent->property("executionBranch").toString(), QString("agent-branch"));
        window.setFileContextWorktree(second);
        QCOMPARE(agent->activePath(), before);
    }

    void acknowledgedRepositorySwitchUpdatesConversationHeaderFilesAndSidebar() {
        QTemporaryDir directory;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        qputenv("KETPLUS_DAEMON_SOCKET", directory.filePath("isolated-switch.sock").toUtf8());
        qputenv("KETPLUS_CONVERSATION_WORKTREES", directory.filePath("switches.sqlite").toUtf8());
        const auto a = directory.filePath("repo-a"), b = directory.filePath("repo-b");
        for (const auto& root : {a, b}) {
            QVERIFY(QDir().mkpath(root));
            QCOMPARE(QProcess::execute("git", {"-C", root, "init", "-q"}), 0);
        }
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(a);
        window.show();
        window.findChild<QAbstractButton*>("agentWorkspaceActivityButton")->click();
        auto* agent = window.findChild<ketplus::AgentWorkspaceView*>();
        auto* manager = window.findChild<ketplus::AgentSessionManager*>();
        auto* server = manager->findChild<ketplus::CodexAppServer*>();
        QVERIFY(agent && manager && server);
        const auto anchor = agent->activePath();
        emit server->daemonSessionDiscovered({{"session_id", "same-conversation"},
                                              {"session_generation", 1},
                                              {"conversation_path", anchor},
                                              {"worktree_path", anchor},
                                              {"context_revision", 1},
                                              {"adapter", "codex"},
                                              {"state", "running"},
                                              {"title", "Keep history"}});
        manager->selectConversation(anchor, "same-conversation");
        emit server->executionContextChanged("same-conversation", "feature/b", b, 2);
        QCOMPARE(agent->activePath(), anchor);
        QCOMPARE(manager->activeConversationId(anchor), QString("same-conversation"));
        QCOMPARE(agent->executionPath(), b);
        QCOMPARE(agent->repositoryName(), QString("repo-b"));
        QCOMPARE(window.findChild<ketplus::ExplorerPanel*>()->rootPath(),
                 QFileInfo(b).canonicalFilePath());
        QCOMPARE(manager->conversations(anchor).first().toMap().value("executionPath").toString(),
                 b);
        QTRY_VERIFY(window.findChild<QAbstractButton*>("productRepositoryGroupButton"));
        const auto groups = window.findChildren<QAbstractButton*>("productRepositoryGroupButton");
        QCOMPARE(groups.first()->toolTip(), QFileInfo(b).canonicalFilePath());
        emit server->executionContextChanged("same-conversation", "old", a, 1);
        QCOMPARE(agent->executionPath(), b);
        manager->activate(anchor, false);
        QCOMPARE(agent->executionPath(), b);
        QVERIFY(manager->isRunning(anchor));
        // A removed linked tree keeps the conversation in the target repository group.
        QCOMPARE(QProcess::execute("git", {"-C", b, "-c", "user.name=Fixture", "-c",
                                           "user.email=fixture@example.invalid", "-c",
                                           "commit.gpgsign=false", "commit", "--allow-empty", "-qm",
                                           "fixture"}),
                 0);
        const auto linked = directory.filePath("b-linked");
        QCOMPARE(QProcess::execute("git", {"-C", b, "worktree", "add", "-b", "linked", linked}), 0);
        emit server->executionContextChanged("same-conversation", "linked",
                                             QFileInfo(linked).canonicalFilePath(), 3);
        QCOMPARE(QProcess::execute("git", {"-C", b, "worktree", "remove", linked}), 0);
        manager->activate(anchor, false);
        QTest::qWait(50);
        bool targetActive = false;
        for (auto* group : window.findChildren<QAbstractButton*>("productRepositoryGroupButton")) {
            if (group->toolTip() == QFileInfo(b).canonicalFilePath())
                targetActive = group->property("activeRepository").toBool();
        }
        QVERIFY(targetActive);
    }

    void transcriptFilesSwitchCheckoutInSameWindow() {
        QTemporaryDir directory;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        qputenv("KETPLUS_DAEMON_SOCKET", directory.filePath("isolated.sock").toUtf8());
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        qputenv("KETPLUS_CONVERSATION_WORKTREES", directory.filePath("worktrees.sqlite").toUtf8());
        const auto first = directory.filePath("first");
        const auto second = directory.filePath("linked");
        QDir().mkpath(first);
        const auto runGit = [&](const QStringList& args) {
            QProcess git;
            git.start("git", QStringList{"-C", first} + args);
            return git.waitForFinished() && git.exitCode() == 0;
        };
        QVERIFY(runGit({"init", "-q"}));
        QVERIFY(runGit({"-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit",
                        "--allow-empty", "-qm", "fixture"}));
        QVERIFY(runGit({"worktree", "add", "--detach", second}));
        const auto filePath = second + "/example.txt";
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("worktree fixture\n");
        file.close();
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(first);
        window.show();
        window.findChild<QAbstractButton*>("agentWorkspaceActivityButton")->click();
        auto* agent = window.findChild<ketplus::AgentWorkspaceView*>();
        auto* service = window.findChild<ketplus::GitService*>();
        QVERIFY(agent && service);
        const auto conversation = agent->activePath();
        agent->setSessionState("Conversation", "Working", true);
        QSignalSpy routedFile(&window, &ketplus::MainWindow::repositoryFileRequested);
        QSignalSpy routedDiff(&window, &ketplus::MainWindow::repositoryFileDiffRequested);
        QSignalSpy diffReady(service, &ketplus::GitService::diffReady);
        agent->fileDiffRequested(filePath);
        QTRY_VERIFY(!diffReady.isEmpty());
        QVERIFY2(diffReady.last().last().toString().isEmpty(),
                 qPrintable(diffReady.last().last().toString()));
        QCOMPARE(service->snapshot().repositoryRoot, QFileInfo(second).canonicalFilePath());
        QCOMPARE(agent->activePath(), conversation);
        QCOMPARE(routedDiff.count(), 0);
        window.setFileContextWorktree(first);
        agent->fileOpenRequested(filePath);
        QCOMPARE(window.findChild<ketplus::ExplorerPanel*>()->rootPath(),
                 QFileInfo(second).canonicalFilePath());
        QCOMPARE(agent->activePath(), conversation);
        QCOMPARE(routedFile.count(), 0);
        bool opened = false;
        for (auto* editor : window.findChildren<ketplus::EditorWidget*>())
            opened |= editor->document().filePath() == filePath ||
                      editor->document().filePath() == QFileInfo(filePath).canonicalFilePath();
        QVERIFY(opened);
    }

    void unsupportedAgentKeepsExecutionAndScrollLoadsMore() {
        QWidget parent; // the picker is a card inside this window
        parent.resize(900, 600);
        parent.show();
        QString selected;
        QStringList methods;
        ketplus::showExecutionWorktreeDialog(
            &parent,
            [&](const QString& type, const QJsonObject& params, auto done) {
                methods.append(type);
                QJsonArray entries;
                const int start = params.value("offset").toInt();
                for (int i = start; i < qMin(start + 100, 240); ++i)
                    entries.append(QJsonObject{{"branch", QString("branch-%1").arg(i)},
                                               {"worktree_path", QString("/repo/tree-%1").arg(i)}});
                done({{"worktrees", entries},
                      {"total", 240},
                      {"worktree_path", "/repo/tree-0"},
                      {"switch_supported", false},
                      {"switch_available", false}},
                     {});
            },
            [&](const QString& path) { selected = path; }, "/repo/tree-0");
        auto* dialog = parent.findChild<ket::Dialog*>("executionWorktreeDialog");
        QVERIFY(dialog);
        // The card pads its body, as the mock's KetDialog does.
        QCOMPARE(dialog->body()->layout()->contentsMargins(), QMargins());
        auto* list = dialog->findChild<QListWidget*>("executionWorktreeList");
        QVERIFY(!list->isEnabled());
        QCOMPARE(list->count(), 100);
        QVERIFY(list->item(0)->flags() & Qt::ItemIsEnabled);
        QTest::qWait(30);
        list->verticalScrollBar()->setValue(list->verticalScrollBar()->maximum());
        QTRY_COMPARE(list->count(), 200);
        for (auto* button : dialog->findChildren<QPushButton*>()) {
            QVERIFY(button->text() != "Previous");
            QVERIFY(!button->text().startsWith("Next"));
        }
        list->itemClicked(list->item(0));
        QVERIFY(selected.isEmpty());
        for (const auto& method : methods)
            QCOMPARE(method, "session.worktrees");
    }
};
QTEST_MAIN(WorktreePickerTest)
#include "WorktreePickerTest.moc"
