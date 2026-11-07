#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexAppServer.h"
#include "app/AgentWorktreeCreator.h"
#include "app/AgentWorktreeDialog.h"
#include "app/MainWindow.h"
#include "app/WorktreesWorkspacePage.h"
#include "editor/EditorWidget.h"
#include "git/GitChangesPanel.h"
#include "git/GitService.h"
#include "git/GitTypes.h"
#include "ui/FindReplaceBar.h"
#include "ui/Theme.h"
#include "workspace/AtlasIndex.h"
#include "workspace/ExplorerPanel.h"

#include "app/SpotlightDialog.h"
#include "workspace/ActivityBar.h"
#include <QAbstractButton>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QQuickItem>
#include <QQuickView>
#include <QRadioButton>
#include <QSaveFile>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QStatusBar>
#include <QStyleOptionButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QtTest>

#include <algorithm>

namespace {

QQuickItem* visualItemNamed(QQuickItem* item, const QString& name) {
    if (item->objectName() == name)
        return item;
    for (auto* child : item->childItems())
        if (auto* match = visualItemNamed(child, name))
            return match;
    return nullptr;
}

} // namespace

class AgentWorkspaceShellTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir conversationWorktrees_;

  private slots:
    void initTestCase() {
        QVERIFY(conversationWorktrees_.isValid());
        qputenv("KETPLUS_CONVERSATION_WORKTREES",
                QDir(conversationWorktrees_.path())
                    .filePath(QStringLiteral("conversation-worktrees.sqlite"))
                    .toUtf8());
    }
    void transcriptFileReopenReadsExternalChangesAndKeepsDirtyEdits() {
        QTemporaryDir settings;
        QTemporaryDir workspace;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace.path());
        window.resize(1200, 800);
        window.show();
        window.findChild<QAbstractButton*>("agentWorkspaceActivityButton")->click();
        const auto path = workspace.filePath("refresh.txt");
        const auto write = [&](const QByteArray& text) {
            QSaveFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(text), text.size());
            QVERIFY(file.commit());
        };
        write("VERSION 1\n");
        auto* agent = window.findChild<ketplus::AgentWorkspaceView*>();
        QVERIFY(agent);
        const auto link = QUrl::fromLocalFile(path).toString();
        emit agent->fileOpenRequested(link);
        auto* tabs = window.findChild<QTabWidget*>("agentDetailTabs");
        QVERIFY(tabs);
        auto* editor = qobject_cast<ketplus::EditorWidget*>(tabs->currentWidget());
        QVERIFY(editor);
        QCOMPARE(editor->text(), QByteArray("VERSION 1\n"));
        const auto count = tabs->count();
        write("VERSION 2\n");
        emit agent->fileOpenRequested(link);
        QCOMPARE(tabs->count(), count);
        QCOMPARE(tabs->currentWidget(), editor);
        QCOMPARE(editor->text(), QByteArray("VERSION 2\n"));
        editor->replaceAllText("MY UNSAVED EDITS\n");
        QVERIFY(editor->document().isModified());
        write("VERSION 3\n");
        emit agent->fileOpenRequested(link);
        QCOMPARE(editor->text(), QByteArray("MY UNSAVED EDITS\n"));
        QVERIFY(editor->document().isModified());
        QFile disk(path);
        QVERIFY(disk.open(QIODevice::ReadOnly));
        QCOMPARE(disk.readAll(), QByteArray("VERSION 3\n"));
        editor->undoEdit();
    }
    void agentDetailEditorHasFindAndGoToLine() {
        QTemporaryDir settings;
        QTemporaryDir workspace;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace.path());
        window.resize(1440, 900);
        window.show();
        window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"))->click();
        QFile file(QDir(workspace.path()).filePath(QStringLiteral("notes.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("first\nsecond needle\nthird\nfourth needle\n");
        file.close();
        window.openFile(file.fileName());
        auto* detailTabs = window.findChild<QTabWidget*>(QStringLiteral("agentDetailTabs"));
        QVERIFY(detailTabs);
        ketplus::EditorWidget* editor = nullptr;
        QTRY_VERIFY((editor = qobject_cast<ketplus::EditorWidget*>(detailTabs->currentWidget())) !=
                    nullptr);
        QTRY_VERIFY(editor->isVisible());
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        editor->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(editor->hasFocus() || editor->isAncestorOf(QApplication::focusWidget()));

        QAction* findAction = nullptr;
        QAction* goToLineAction = nullptr;
        for (auto* action : window.findChildren<QAction*>()) {
            if (action->text() == QStringLiteral("&Find…"))
                findAction = action;
            if (action->text() == QStringLiteral("Go to &Line…"))
                goToLineAction = action;
        }
        QVERIFY(findAction && goToLineAction);

        // Find opens a bar beside the agent editor, not in the hidden main editor area.
        findAction->trigger();
        auto* findBar = window.findChild<ketplus::FindReplaceBar*>();
        QVERIFY(findBar);
        QTRY_VERIFY(findBar->isVisible());
        QVERIFY(detailTabs->parentWidget()->isAncestorOf(findBar));
        auto* query = findBar->findChild<QLineEdit*>();
        QVERIFY(query);
        query->setText(QStringLiteral("needle"));
        QTest::keyClick(query, Qt::Key_Return);
        QTRY_COMPARE(editor->currentLine(), 2);
        QTest::keyClick(query, Qt::Key_Return);
        QTRY_COMPARE(editor->currentLine(), 4);

        // Go to Line moves the agent editor's cursor.
        QTimer::singleShot(0, [] {
            QTRY_VERIFY(qobject_cast<QInputDialog*>(QApplication::activeModalWidget()));
            auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            dialog->setIntValue(3);
            dialog->accept();
        });
        goToLineAction->trigger();
        QCOMPARE(editor->currentLine(), 3);

        // The shortcuts reach the agent editor too.
        findBar->hide();
        const auto focusEditor = [&] {
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(&window));
            editor->setFocus(Qt::OtherFocusReason);
            QTRY_VERIFY(QApplication::focusWidget() != nullptr &&
                        (QApplication::focusWidget() == editor ||
                         editor->isAncestorOf(QApplication::focusWidget())));
        };
        focusEditor();
        QTest::keySequence(QApplication::focusWidget(), QKeySequence::Find);
        QTRY_VERIFY(findBar->isVisible());
        focusEditor();
        bool dialogShown = false;
        QTimer::singleShot(0, [&dialogShown] {
            QTRY_VERIFY(qobject_cast<QInputDialog*>(QApplication::activeModalWidget()));
            dialogShown = true;
            auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            dialog->setIntValue(1);
            dialog->accept();
        });
        QTest::keySequence(QApplication::focusWidget(), QKeySequence(QStringLiteral("Ctrl+L")));
        QTRY_VERIFY(dialogShown);
        QCOMPARE(editor->currentLine(), 1);
    }
    void transcriptSelectionOwnsEditingShortcuts() {
        QTemporaryDir settings;
        QTemporaryDir workspace;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace.path());
        window.resize(1440, 900);
        window.show();
        window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"))->click();
        auto* agentWorkspace = window.findChild<ketplus::AgentWorkspaceView*>();
        QVERIFY(agentWorkspace);
        const QString answer = QStringLiteral("Alpha beta gamma delta epsilon zeta eta theta");
        agentWorkspace->resetMessages(
            {QVariantMap{{"messageKey", "a"}, {"kind", "assistant"}, {"body", answer}}},
            "shortcuts");

        // An agent detail editor with its own selection stays the current editor.
        QFile file(QDir(workspace.path()).filePath(QStringLiteral("notes.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("EDITOR TEXT\n");
        file.close();
        window.openFile(file.fileName());
        ketplus::EditorWidget* editor = nullptr;
        QTRY_VERIFY((editor = window.findChild<ketplus::EditorWidget*>()) != nullptr);
        for (auto* candidate : window.findChildren<ketplus::EditorWidget*>())
            if (candidate->isVisible())
                editor = candidate;
        editor->selectAllText();
        QAction* copyAction = nullptr;
        QAction* undoAction = nullptr;
        QAction* redoAction = nullptr;
        QAction* cutAction = nullptr;
        QAction* pasteAction = nullptr;
        QAction* selectAllAction = nullptr;
        for (auto* action : window.findChildren<QAction*>()) {
            if (action->shortcut() == QKeySequence(QKeySequence::Copy))
                copyAction = action;
            if (action->shortcut() == QKeySequence(QKeySequence::Undo))
                undoAction = action;
            if (action->shortcut() == QKeySequence(QKeySequence::Redo))
                redoAction = action;
            if (action->shortcut() == QKeySequence(QKeySequence::Cut))
                cutAction = action;
            if (action->shortcut() == QKeySequence(QKeySequence::Paste))
                pasteAction = action;
            if (action->shortcut() == QKeySequence(QKeySequence::SelectAll))
                selectAllAction = action;
        }
        QVERIFY(copyAction && undoAction && redoAction && cutAction && pasteAction &&
                selectAllAction);
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        editor->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(copyAction->isEnabled());
        QVERIFY(!undoAction->isEnabled());

        auto* quick = agentWorkspace->quickView();
        auto* transcript = quick->rootObject()->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = visualItemNamed(transcript, QStringLiteral("messageBody"))) &&
                    body->isVisible() && body->width() > 120);
        const QPointF from = body->mapToScene(QPointF(2, body->height() / 2));
        const QPointF to = body->mapToScene(QPointF(120, body->height() / 2));
        QTest::mousePress(quick, Qt::LeftButton, {}, from.toPoint());
        for (int step = 1; step <= 10; ++step)
            QTest::mouseMove(quick, (from + (to - from) * step / 10.0).toPoint(), 5);
        QTest::mouseRelease(quick, Qt::LeftButton, {}, to.toPoint(), 5);
        const QString selected = body->property("selectedText").toString();
        QVERIFY2(selected.size() > 3 && answer.startsWith(selected), qPrintable(selected));

        // Pressing into the transcript moves keyboard focus there. Copy stays enabled: macOS
        // swallows the shortcut of a disabled menu item. The selected text claims it first.
        QTRY_VERIFY(agentWorkspace->isAncestorOf(QApplication::focusWidget()));
        QVERIFY(copyAction->isEnabled() && undoAction->isEnabled());
        QKeyEvent shortcutOverride(QEvent::ShortcutOverride, Qt::Key_C, Qt::ControlModifier);
        shortcutOverride.ignore();
        QCoreApplication::sendEvent(QGuiApplication::focusObject(), &shortcutOverride);
        QVERIFY(shortcutOverride.isAccepted());
        QGuiApplication::clipboard()->setText(QStringLiteral("before"));
        QTest::keySequence(quick, QKeySequence::Copy);
        QCOMPARE(QGuiApplication::clipboard()->text(), selected);

        // Edit > Copy chosen from the menu copies the transcript selection, not the editor's.
        QGuiApplication::clipboard()->setText(QStringLiteral("before"));
        copyAction->trigger();
        QCOMPARE(QGuiApplication::clipboard()->text(), selected);
        QVERIFY(editor->hasSelection());

        // Edit menu commands must reach the Quick text item without reactivating
        // their QAction through the window shortcut map (which used to recurse).
        auto* prompt = quick->rootObject()->findChild<QQuickItem*>("agentPrompt");
        QVERIFY(prompt);
        prompt->forceActiveFocus(Qt::OtherFocusReason);
        QTRY_COMPARE(quick->activeFocusItem(), prompt);
        QTest::keyClick(quick, 'x');
        QTRY_COMPARE(prompt->property("text").toString(), QStringLiteral("x"));
        QSignalSpy undoTriggers(undoAction, &QAction::triggered);
        undoAction->trigger();
        QCOMPARE(undoTriggers.count(), 1);
        QTRY_COMPARE(prompt->property("text").toString(), QString());
        redoAction->trigger();
        QTRY_COMPARE(prompt->property("text").toString(), QStringLiteral("x"));
        selectAllAction->trigger();
        QCOMPARE(prompt->property("selectedText").toString(), QStringLiteral("x"));
        copyAction->trigger();
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("x"));
        cutAction->trigger();
        QTRY_COMPARE(prompt->property("text").toString(), QString());
        pasteAction->trigger();
        QTRY_COMPARE(prompt->property("text").toString(), QStringLiteral("x"));
        QTest::keySequence(quick, QKeySequence::Undo);
        QTRY_COMPARE(prompt->property("text").toString(), QString());

        // Back in the editor, the commands follow its state again.
        editor->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(!agentWorkspace->isAncestorOf(QApplication::focusWidget()));
        QVERIFY(copyAction->isEnabled());
        QVERIFY(!undoAction->isEnabled());
    }
    void deleteConfirmationReportsFailureWithHiddenStatusBar() {
        QTemporaryDir settings;
        QTemporaryDir workspace;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace.path());
        window.show();
        auto* agent =
            window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
        QVERIFY(agent);
        agent->click();
        auto* view = window.findChild<ketplus::AgentWorkspaceView*>();
        auto* manager = window.findChild<ketplus::AgentSessionManager*>();
        QVERIFY(view && manager);
        window.statusBar()->hide();
        QSignalSpy result(manager, &ketplus::AgentSessionManager::conversationDeletionFinished);
        QTimer::singleShot(0, &window, [&window] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("conversationDeleteDialog"));
            QVERIFY(dialog);
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            QVERIFY(buttons);
            auto* linked =
                dialog->findChild<QCheckBox*>(QStringLiteral("deleteLinkedWorktreeCheck"));
            auto* force = dialog->findChild<QCheckBox*>(QStringLiteral("forceDeleteWorktreeCheck"));
            QVERIFY(linked && force);
            QVERIFY(!linked->isEnabled());
            QVERIFY(!force->isEnabled());
            buttons->button(QDialogButtonBox::Ok)->click();
        });
        view->requestDeleteConversation(QStringLiteral("missing-conversation"));
        QCOMPARE(result.size(), 1);
        auto* error = window.findChild<QMessageBox*>(QStringLiteral("conversationDeletionError"));
        QVERIFY(error);
        QVERIFY(error->isVisible());
        QVERIFY(error->text().contains(QStringLiteral("could not be found")));
        error->accept();
    }

    void productSidebarMatchesDesignGeometryAndAvoidsAgentDuplication() {
        QTemporaryDir settings;
        QTemporaryDir workspace;
        QVERIFY(settings.isValid());
        QVERIFY(workspace.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace.path());
        window.resize(1440, 900);
        window.show();

        auto* sidebar = window.findChild<ketplus::ActivityBar*>(QStringLiteral("activityBar"));
        auto* search = window.findChild<QAbstractButton*>(QStringLiteral("quickSearchButton"));
        auto* addRepository =
            window.findChild<QToolButton*>(QStringLiteral("productAddRepositoryButton"));
        auto* settingsAction = window.findChild<QAction*>(QStringLiteral("productSettingsAction"));
        auto* identity = window.findChild<QToolButton*>(QStringLiteral("productIdentityButton"));
        QVERIFY(sidebar && search && addRepository && settingsAction && identity);
        auto* productSurface = window.findChild<QWidget*>(QStringLiteral("productSurface"));
        auto* mainSplit = window.findChild<QSplitter*>(QStringLiteral("mainSplit"));
        QVERIFY(productSurface);
        QVERIFY(mainSplit);
        QVERIFY(sidebar->testAttribute(Qt::WA_StyledBackground));
        QVERIFY(productSurface->testAttribute(Qt::WA_StyledBackground));
        QCOMPARE(sidebar->grab().toImage().pixelColor(1, 1),
                 QColor(theme.palette().sidebarBackground));
        QCOMPARE(sidebar->width(), ketplus::ActivityBar::kWidth);
        QCOMPARE(sidebar->minimumWidth(), ketplus::ActivityBar::kWidth);
        QCOMPARE(sidebar->maximumWidth(), ketplus::ActivityBar::kWidth);
        QCOMPARE(search->height(), 32);
        QVERIFY(window.findChild<QWidget*>(QStringLiteral("productTopBar")) == nullptr);
        QVERIFY(window.findChild<QToolButton*>(QStringLiteral("productBranchButton")) == nullptr);
        QVERIFY(window.findChild<QToolButton*>(QStringLiteral("productTerminalButton")) == nullptr);
        QVERIFY(window.findChild<QToolButton*>(QStringLiteral("productThemeButton")) == nullptr);
        QCOMPARE(mainSplit->mapTo(productSurface, QPoint()).y(), 0);
        QTRY_VERIFY([&] {
            const auto groups = sidebar->findChildren<QAbstractButton*>(
                QStringLiteral("productRepositoryGroupButton"));
            return std::any_of(
                groups.cbegin(), groups.cend(), [&workspace](const QToolButton* group) {
                    return group->accessibleName() == QFileInfo(workspace.path()).fileName() &&
                           group->property("activeRepository").toBool();
                });
        }());
        QVERIFY(
            sidebar->findChildren<QToolButton*>(QStringLiteral("productWorktreeButton")).isEmpty());
        const auto repositoryHeadings = sidebar->findChildren<QLabel*>();
        QVERIFY(std::none_of(
            repositoryHeadings.cbegin(), repositoryHeadings.cend(), [](const QLabel* label) {
                return label->property("kvRole") == QStringLiteral("productSidebarSection") &&
                       label->text() == QStringLiteral("Repositories");
            }));
        QVERIFY(sidebar->findChild<QComboBox*>(QStringLiteral("productMachineSelect")));
        QVERIFY(settingsAction->text().startsWith(QStringLiteral("Settings")));
        QVERIFY(window.findChild<QToolButton*>(QStringLiteral("productSettingsButton")) == nullptr);
        QVERIFY(identity->text().contains(QStringLiteral("Personal workspace")));
        QVERIFY(sidebar->isVisible());

        search->click();
        QTRY_VERIFY(window.findChild<ketplus::SpotlightDialog*>() != nullptr);
        window.findChild<ketplus::SpotlightDialog*>()->close();

        auto* agent =
            window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
        QVERIFY(agent);
        agent->click();
        QTRY_VERIFY(sidebar->isVisible());
        QCOMPARE(sidebar->mode(), ketplus::ActivityBar::Mode::AgentWorkspace);
        QTRY_VERIFY(!window.statusBar()->isVisible());

        auto* agentWorkspace = window.findChild<ketplus::AgentWorkspaceView*>();
        QVERIFY(agentWorkspace);
        QVERIFY(!agentWorkspace->productSidebarVisible());
        const auto capture = qEnvironmentVariable("KETPLUS_PRODUCT_CAPTURE");
        if (!capture.isEmpty()) {
            agentWorkspace->setSessionState("Transcript integration", "Ready", false);
            agentWorkspace->resetMessages(
                {QVariantMap{{"kind", "assistant"},
                             {"body", "The new transcript keeps the product sidebar and header. "
                                      "Command output stays hidden."}},
                 QVariantMap{{"kind", "tool"},
                             {"subtype", "command"},
                             {"title", "Ran: git status --short"},
                             {"command", "git status --short"},
                             {"status", "completed"}},
                 QVariantMap{{"kind", "user"}, {"body", "Continue"}}},
                "capture");
            window.raise();
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QTest::qWait(400);
            QVERIFY(window.grab().save(capture));
            // Embedded QQuickView content is not included in QWidget::grab().
            QVERIFY(agentWorkspace->quickView()->grabWindow().save(capture + ".transcript.png"));
        }
        agentWorkspace->requestProductNavigation(QStringLiteral("explorer"));
        auto* explorer = window.findChild<ketplus::ExplorerPanel*>();
        QVERIFY(explorer);
        QVERIFY(explorer->testAttribute(Qt::WA_StyledBackground));
        QTRY_VERIFY(sidebar->isVisible());
        QTRY_COMPARE(explorer->width(), 224);
        QCOMPARE(sidebar->mode(), ketplus::ActivityBar::Mode::Explorer);
        QTRY_VERIFY(window.statusBar()->isVisible());
        QCOMPARE(window.statusBar()->contentsMargins().left(), ketplus::ActivityBar::kWidth);

        QAction* explorerAction = nullptr;
        for (auto* action : window.findChildren<QAction*>()) {
            if (action->text() == QStringLiteral("Explorer")) {
                explorerAction = action;
                break;
            }
        }
        QVERIFY(explorerAction);
        QVERIFY(explorerAction->isChecked());
    }

    void keepsRepositoryIdentityAndHidesPrunableWorktrees() {
        QTemporaryDir settings;
        QTemporaryDir directory;
        QVERIFY(settings.isValid());
        QVERIFY(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

        const QString repository = directory.filePath(QStringLiteral("ketviet"));
        const QString linked = directory.filePath(QStringLiteral("ketviet-feature"));
        const QString linkedTwo = directory.filePath(QStringLiteral("ketviet-feature-two"));
        const QString linkedThree = directory.filePath(QStringLiteral("ketviet-feature-three"));
        const QString promoted = directory.filePath(QStringLiteral("ketviet-feature-promoted"));
        const QString stale = directory.filePath(QStringLiteral("ketviet-hospitality-stale"));
        QVERIFY(QDir().mkpath(repository));
        const auto git = [&repository](const QStringList& arguments) {
            QProcess process;
            process.start(QStringLiteral("git"),
                          QStringList{QStringLiteral("-C"), repository} + arguments);
            return process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit &&
                   process.exitCode() == 0;
        };
        QVERIFY(git({QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"),
                     QStringLiteral("main")}));
        QVERIFY(git({QStringLiteral("config"), QStringLiteral("user.email"),
                     QStringLiteral("sidebar-test@example.invalid")}));
        QVERIFY(git({QStringLiteral("config"), QStringLiteral("user.name"),
                     QStringLiteral("Sidebar Test")}));
        QFile readme(QDir(repository).filePath(QStringLiteral("README.md")));
        QVERIFY(readme.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(readme.write("test\n") > 0);
        readme.close();
        QVERIFY(git({QStringLiteral("add"), QStringLiteral("README.md")}));
        QVERIFY(git({QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("initial")}));
        QVERIFY(git({QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("--detach"),
                     linked, QStringLiteral("HEAD")}));
        QVERIFY(git({QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("--detach"),
                     linkedTwo, QStringLiteral("HEAD")}));
        QVERIFY(git({QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("--detach"),
                     linkedThree, QStringLiteral("HEAD")}));
        QVERIFY(git({QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("--detach"),
                     promoted, QStringLiteral("HEAD")}));
        QVERIFY(git({QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("--detach"),
                     stale, QStringLiteral("HEAD")}));
        QVERIFY(QFile::remove(QDir(stale).filePath(QStringLiteral(".git"))));

        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(repository);
        window.resize(1200, 800);
        window.show();
        auto* agent =
            window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
        QVERIFY(agent);
        agent->click();
        const auto activeRepositoryGroup = [&window] {
            for (auto* group : window.findChildren<QToolButton*>(
                     QStringLiteral("productRepositoryGroupButton"))) {
                if (group->property("activeRepository").toBool())
                    return group->accessibleName();
            }
            return QString();
        };

        // Worktrees are carried by conversations, not listed as a sidebar level.
        QTRY_COMPARE(activeRepositoryGroup(), QStringLiteral("ketviet"));
        QVERIFY(
            window.findChildren<QToolButton*>(QStringLiteral("productWorktreeButton")).isEmpty());

        window.switchToWorktree(linked);
        auto* agentView = window.findChild<ketplus::AgentWorkspaceView*>();
        QVERIFY(agentView);
        QTRY_COMPARE(agentView->activePath(), QFileInfo(linked).canonicalFilePath());
        QTest::qWait(300);
        // A linked worktree keeps the main repository identity.
        QCOMPARE(activeRepositoryGroup(), QStringLiteral("ketviet"));
        auto* sourceControl =
            window.findChild<QAbstractButton*>(QStringLiteral("sourceControlActivityButton"));
        QVERIFY(sourceControl);
        QTRY_VERIFY(sourceControl->isEnabled());

        const auto showWorktreesPage = [&window] {
            auto* manage = window.findChild<QAction*>(QStringLiteral("manageWorktreesAction"));
            QVERIFY(manage);
            manage->trigger();
        };
        showWorktreesPage();
        ketplus::WorktreesWorkspacePage* worktreesPage = nullptr;
        QTRY_VERIFY((worktreesPage = window.findChild<ketplus::WorktreesWorkspacePage*>()) !=
                        nullptr &&
                    worktreesPage->isVisible());
        auto* pruneButton =
            worktreesPage->findChild<QPushButton*>(QStringLiteral("worktreesPruneButton"));
        auto* worktreeCreator = window.findChild<ketplus::AgentWorktreeCreator*>(
            QStringLiteral("agentWorktreeCreator"));
        QVERIFY(pruneButton && worktreeCreator);
        QTRY_VERIFY(pruneButton->isEnabled());
        QSignalSpy pruned(worktreeCreator, &ketplus::AgentWorktreeCreator::pruned);
        pruneButton->click();
        QTRY_COMPARE_WITH_TIMEOUT(pruned.count(), 1, 5000);
        QTRY_VERIFY(!pruneButton->isEnabled());
        QToolButton* promotedButton = nullptr;
        for (auto* button : worktreesPage->findChildren<QToolButton*>(
                 QStringLiteral("worktreesWorkspaceOpenButton"))) {
            if (button->toolTip() == QFileInfo(promoted).canonicalFilePath())
                promotedButton = button;
        }
        QVERIFY(promotedButton);
        QCOMPARE(promotedButton->height(), 40);
        auto* promotedRow = promotedButton->parentWidget()->parentWidget();
        QVERIFY(promotedRow);
        QCOMPARE(promotedRow->objectName(), QStringLiteral("worktreesWorkspaceRow"));
        QCOMPARE(promotedRow->height(), 76);
        auto* name = promotedButton->findChild<QLabel*>(QStringLiteral("worktreesWorkspaceName"));
        auto* conversations =
            promotedButton->findChild<QLabel*>(QStringLiteral("worktreesWorkspaceMeta"));
        auto* worktreeIcon =
            promotedRow->findChild<QLabel*>(QStringLiteral("worktreesWorkspaceIcon"));
        QVERIFY(name && conversations && worktreeIcon);
        QVERIFY(conversations->geometry().top() - name->geometry().bottom() <= 6);
        QCOMPARE(worktreeIcon->mapTo(promotedRow, QPoint()).y(),
                 name->mapTo(promotedRow, QPoint()).y());
        if (const QString screenshot =
                qEnvironmentVariable("KETPLUS_WORKTREES_WORKSPACE_SCREENSHOT");
            !screenshot.isEmpty()) {
            QTest::qWait(50);
            window.repaint();
            QVERIFY(window.grab().save(screenshot));
        }
        promotedButton->click();
        QTRY_VERIFY(agentView->isVisible());
        QTRY_COMPARE(agentView->activePath(), QFileInfo(promoted).canonicalFilePath());

        auto* editorSplit = window.findChild<QSplitter*>(QStringLiteral("editorSplit"));
        auto* explorerButton =
            window.findChild<QAbstractButton*>(QStringLiteral("explorerActivityButton"));
        QVERIFY(editorSplit && explorerButton);
        showWorktreesPage();
        QTRY_VERIFY(worktreesPage->isVisible());
        explorerButton->click();
        QTRY_VERIFY(!worktreesPage->isVisible());
        QTRY_VERIFY(editorSplit->isVisible());
        QTRY_VERIFY(window.findChild<ketplus::ExplorerPanel*>()->isVisible());
        showWorktreesPage();
        QTRY_VERIFY(worktreesPage->isVisible());
        QTRY_VERIFY(sourceControl->isEnabled());
        sourceControl->click();
        QTRY_VERIFY(!worktreesPage->isVisible());
        QTRY_VERIFY(editorSplit->isVisible());
        QTRY_VERIFY(window.findChild<ketplus::GitChangesPanel*>()->isVisible());

        const QString title = window.windowTitle();
        window.switchToWorktree(stale);
        QCOMPARE(window.windowTitle(), title);
        QCOMPARE(activeRepositoryGroup(), QStringLiteral("ketviet"));

        QSignalSpy created(worktreeCreator, &ketplus::AgentWorktreeCreator::created);
        worktreeCreator->create(repository, repository, QStringLiteral("feature/created-active"),
                                QStringLiteral("main"));
        QTRY_COMPARE_WITH_TIMEOUT(created.count(), 1, 5000);
        const QString createdPath =
            QFileInfo(directory.filePath(QStringLiteral("ketviet-feature-created-active")))
                .canonicalFilePath();
        QVERIFY(!createdPath.isEmpty());
        QTRY_COMPARE(agentView->activePath(), createdPath);
        QTRY_VERIFY(([&window] {
            for (auto* process : window.findChildren<QProcess*>()) {
                if (process->state() != QProcess::NotRunning) {
                    return false;
                }
            }
            return true;
        })());
    }

    // Another repository's saved view (here Explorer) must not replace the conversation the
    // sidebar asked for.
    void sidebarConversationInAnotherRepositoryOpensAgentWorkspace() {
        QTemporaryDir settings;
        QTemporaryDir directory;
        QVERIFY(settings.isValid());
        QVERIFY(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        const auto repository = [&directory](const QString& name) {
            const QString path = directory.filePath(name);
            QDir().mkpath(path);
            QProcess git;
            git.start(QStringLiteral("git"),
                      {QStringLiteral("-C"), path, QStringLiteral("init"), QStringLiteral("-q")});
            git.waitForFinished(5000);
            return QFileInfo(path).canonicalFilePath();
        };
        const QString first = repository(QStringLiteral("first"));
        const QString second = repository(QStringLiteral("second"));
        QVERIFY(!first.isEmpty() && !second.isEmpty());

        ketplus::ThemeManager theme(*qApp);
        {
            // Leave the second repository in Explorer; closing the window saves that view.
            ketplus::MainWindow window(theme);
            window.openFolder(second);
            window.resize(1200, 800);
            window.show();
            auto* explorerButton =
                window.findChild<QAbstractButton*>(QStringLiteral("explorerActivityButton"));
            QVERIFY(explorerButton);
            explorerButton->click();
            QVERIFY(window.close());
        }

        ketplus::MainWindow window(theme);
        window.openFolder(first);
        window.resize(1200, 800);
        window.show();
        auto* agentButton =
            window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
        QVERIFY(agentButton);
        agentButton->click();
        auto* view = window.findChild<ketplus::AgentWorkspaceView*>();
        auto* manager = window.findChild<ketplus::AgentSessionManager*>();
        auto* sidebar = window.findChild<ketplus::ActivityBar*>(QStringLiteral("activityBar"));
        QVERIFY(view && manager && sidebar);
        manager->activate(second);
        const QString conversationId = manager->activeConversationId(second);
        QVERIFY(!conversationId.isEmpty());

        emit sidebar->conversationRequested(second, conversationId);
        QTRY_COMPARE(view->activePath(), second);
        QTest::qWait(200);
        QVERIFY(view->isVisible());
        auto* explorer = window.findChild<ketplus::ExplorerPanel*>();
        QVERIFY(explorer == nullptr || !explorer->isVisible());
        QCOMPARE(manager->activeConversationId(second), conversationId);
    }

    void sidebarSelectionDoesNotReactivateThePreviousConversation() {
        QTemporaryDir settings;
        QTemporaryDir workspace;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace.path());
        window.show();
        auto* button = window.findChild<QAbstractButton*>("agentWorkspaceActivityButton");
        QVERIFY(button);
        button->click();
        auto* view = window.findChild<ketplus::AgentWorkspaceView*>();
        auto* manager = window.findChild<ketplus::AgentSessionManager*>();
        auto* sidebar = window.findChild<ketplus::ActivityBar*>("activityBar");
        QVERIFY(view && manager && sidebar);
        const QString path = view->activePath();
        const QString first = manager->activeConversationId(path);
        view->requestNewSession();
        QTRY_VERIFY(manager->activeConversationId(path) != first);
        QSignalSpy resets(manager, &ketplus::AgentSessionManager::messagesReset);
        emit sidebar->conversationRequested(path, first);
        QCOMPARE(manager->activeConversationId(path), first);
        QCOMPARE(resets.size(), 1);
        resets.clear();
        emit sidebar->conversationRequested(path, first);
        QCOMPARE(resets.size(), 0);
    }

    void sidebarConversationInAnotherWorktreeResetsTranscriptOnce() {
        QTemporaryDir settings;
        QTemporaryDir directory;
        QVERIFY(settings.isValid());
        QVERIFY(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

        const QString repository = directory.filePath(QStringLiteral("ketviet"));
        const QString linked = directory.filePath(QStringLiteral("ketviet-feature"));
        QVERIFY(QDir().mkpath(repository));
        const auto git = [&repository](const QStringList& arguments) {
            QProcess process;
            process.start(QStringLiteral("git"),
                          QStringList{QStringLiteral("-C"), repository} + arguments);
            return process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit &&
                   process.exitCode() == 0;
        };
        QVERIFY(git({QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"),
                     QStringLiteral("main")}));
        QVERIFY(git({QStringLiteral("config"), QStringLiteral("user.email"),
                     QStringLiteral("single-reset-test@example.invalid")}));
        QVERIFY(git({QStringLiteral("config"), QStringLiteral("user.name"),
                     QStringLiteral("Single Reset Test")}));
        QFile readme(QDir(repository).filePath(QStringLiteral("README.md")));
        QVERIFY(readme.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(readme.write("test\n") > 0);
        readme.close();
        QVERIFY(git({QStringLiteral("add"), QStringLiteral("README.md")}));
        QVERIFY(git({QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("initial")}));
        QVERIFY(git({QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-qb"),
                     QStringLiteral("feature/single-reset"), linked, QStringLiteral("HEAD")}));

        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(repository);
        window.resize(1200, 800);
        window.show();
        auto* agentButton =
            window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
        QVERIFY(agentButton);
        agentButton->click();
        auto* view = window.findChild<ketplus::AgentWorkspaceView*>();
        auto* manager = window.findChild<ketplus::AgentSessionManager*>();
        auto* sidebar = window.findChild<ketplus::ActivityBar*>(QStringLiteral("activityBar"));
        QVERIFY(view && manager && sidebar);
        const QString repositoryPath = QFileInfo(repository).canonicalFilePath();
        const QString linkedPath = QFileInfo(linked).canonicalFilePath();

        // The linked worktree holds two conversations; the second one is its current one.
        manager->activate(linkedPath);
        const QString earlier = manager->activeConversationId(linkedPath);
        manager->renameConversation(linkedPath, earlier, QStringLiteral("Earlier"));
        manager->newSession(linkedPath);
        const QString later = manager->activeConversationId(linkedPath);
        QVERIFY(!earlier.isEmpty() && later != earlier);
        view->setActivePath(repositoryPath);
        manager->activate(repositoryPath);
        QTest::qWait(200);

        QStringList published;
        connect(manager, &ketplus::AgentSessionManager::messagesReset, &window,
                [&published, manager, linkedPath](const QString& path, const QVariantList&) {
                    if (path == linkedPath)
                        published.append(manager->activeConversationId(path));
                });
        emit sidebar->conversationRequested(linkedPath, earlier);
        QCOMPARE(view->activePath(), linkedPath);
        QCOMPARE(manager->activeConversationId(linkedPath), earlier);
        // Only the requested conversation reaches the transcript: the worktree's current one is
        // never published first.
        QCOMPARE(published, QStringList{earlier});
    }

    void sidebarConversationOpensItsWorktree() {
        QTemporaryDir settings;
        QTemporaryDir directory;
        QVERIFY(settings.isValid());
        QVERIFY(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

        const QString repository = directory.filePath(QStringLiteral("ketviet"));
        const QString linked = directory.filePath(QStringLiteral("ketviet-feature"));
        QVERIFY(QDir().mkpath(repository));
        const auto git = [&repository](const QStringList& arguments) {
            QProcess process;
            process.start(QStringLiteral("git"),
                          QStringList{QStringLiteral("-C"), repository} + arguments);
            return process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit &&
                   process.exitCode() == 0;
        };
        QVERIFY(git({QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"),
                     QStringLiteral("main")}));
        QVERIFY(git({QStringLiteral("config"), QStringLiteral("user.email"),
                     QStringLiteral("conversation-switch-test@example.invalid")}));
        QVERIFY(git({QStringLiteral("config"), QStringLiteral("user.name"),
                     QStringLiteral("Conversation Switch Test")}));
        QFile readme(QDir(repository).filePath(QStringLiteral("README.md")));
        QVERIFY(readme.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(readme.write("test\n") > 0);
        readme.close();
        QVERIFY(git({QStringLiteral("add"), QStringLiteral("README.md")}));
        QVERIFY(git({QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("initial")}));
        QVERIFY(git({QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-qb"),
                     QStringLiteral("feature/sidebar-switch"), linked, QStringLiteral("HEAD")}));

        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(repository);
        window.resize(1200, 800);
        window.show();

        auto* agentButton =
            window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
        auto* explorerButton =
            window.findChild<QAbstractButton*>(QStringLiteral("explorerActivityButton"));
        QVERIFY(agentButton && explorerButton);
        agentButton->click();

        auto* view = window.findChild<ketplus::AgentWorkspaceView*>();
        auto* manager = window.findChild<ketplus::AgentSessionManager*>();
        QVERIFY(view && manager);
        const QString repositoryPath = QFileInfo(repository).canonicalFilePath();
        const QString linkedPath = QFileInfo(linked).canonicalFilePath();
        manager->renameConversation(repositoryPath, manager->activeConversationId(repositoryPath),
                                    QStringLiteral("Main conversation"));
        manager->activate(linkedPath);
        manager->renameConversation(linkedPath, manager->activeConversationId(linkedPath),
                                    QStringLiteral("Feature conversation"));
        view->setActivePath(repositoryPath);
        manager->activate(repositoryPath);

        explorerButton->click();
        QTRY_VERIFY(!view->isVisible());
        const auto conversationButton = [&window](const QString& title) -> QToolButton* {
            for (auto* button : window.findChildren<QAbstractButton*>(
                     QStringLiteral("productConversationButton"))) {
                if (button->accessibleName() == title)
                    return button;
            }
            return nullptr;
        };
        // Repository > conversation: both worktrees' conversations share one repository group.
        QTRY_VERIFY(conversationButton(QStringLiteral("Feature conversation")) != nullptr);
        QVERIFY(conversationButton(QStringLiteral("Main conversation")) != nullptr);
        QCOMPARE(conversationButton(QStringLiteral("Feature conversation"))
                     ->property("worktreePath")
                     .toString(),
                 linkedPath);
        conversationButton(QStringLiteral("Feature conversation"))->click();

        // Opening a conversation switches to the worktree it runs in.
        QTRY_COMPARE(view->activePath(), linkedPath);
        QTRY_VERIFY(view->isVisible());
        QTRY_VERIFY(conversationButton(QStringLiteral("Feature conversation")) != nullptr &&
                    conversationButton(QStringLiteral("Feature conversation"))->isChecked());
        QVERIFY(conversationButton(QStringLiteral("Main conversation")) != nullptr);
        QTRY_VERIFY(([&window] {
            for (auto* process : window.findChildren<QProcess*>()) {
                if (process->state() != QProcess::NotRunning) {
                    return false;
                }
            }
            return true;
        })());

        // A deleted worktree keeps its conversation under the repository and opens as history,
        // never through openFolder (which would reject the missing path with a modal error).
        conversationButton(QStringLiteral("Main conversation"))->click();
        QTRY_COMPARE(view->activePath(), repositoryPath);
        QVERIFY(QDir(linkedPath).removeRecursively());
        QTRY_VERIFY(conversationButton(QStringLiteral("Feature conversation")) != nullptr);
        const QString featureId = conversationButton(QStringLiteral("Feature conversation"))
                                      ->property("conversationId")
                                      .toString();
        bool modalShown = false;
        // The old path opened a blocking QMessageBox; close it so a regression fails instead of
        // hanging.
        QTimer::singleShot(500, &window, [&modalShown] {
            if (auto* modal = QApplication::activeModalWidget()) {
                modalShown = true;
                modal->close();
            }
        });
        conversationButton(QStringLiteral("Feature conversation"))->click();
        QTRY_COMPARE(view->activePath(), linkedPath);
        QCOMPARE(manager->activeConversationId(linkedPath), featureId);
        QTest::qWait(600);
        QVERIFY(!modalShown);
        QTRY_VERIFY(conversationButton(QStringLiteral("Feature conversation")) != nullptr &&
                    conversationButton(QStringLiteral("Feature conversation"))->isChecked());
        QVERIFY(!conversationButton(QStringLiteral("Main conversation"))->isChecked());
    }

    void listsUserCreatedConversationInSidebarBeforeFirstTurn() {
        QTemporaryDir settings;
        QTemporaryDir workspace;
        QVERIFY(settings.isValid());
        QVERIFY(workspace.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace.path());
        window.resize(1200, 800);
        window.show();
        auto* agent =
            window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
        QVERIFY(agent);
        agent->click();
        auto* view = window.findChild<ketplus::AgentWorkspaceView*>();
        QVERIFY(view);
        const auto conversationButton = [&window](const QString& title) -> QToolButton* {
            for (auto* button : window.findChildren<QAbstractButton*>(
                     QStringLiteral("productConversationButton"))) {
                if (button->accessibleName() == title)
                    return button;
            }
            return nullptr;
        };
        QVERIFY(conversationButton(QStringLiteral("New conversation")) == nullptr);
        view->requestNewSession();
        QTRY_VERIFY(conversationButton(QStringLiteral("New conversation")) != nullptr);
        QVERIFY(conversationButton(QStringLiteral("New conversation"))->isChecked());
    }

    void listsLinkedWorktreeConversationInSidebarBeforeFirstTurn() {
        QTemporaryDir settings;
        QTemporaryDir directory;
        QVERIFY(settings.isValid());
        QVERIFY(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

        const QString repository = directory.filePath(QStringLiteral("ketviet"));
        QVERIFY(QDir().mkpath(repository));
        const auto git = [&repository](const QStringList& arguments) {
            QProcess process;
            process.start(QStringLiteral("git"),
                          QStringList{QStringLiteral("-C"), repository} + arguments);
            return process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit &&
                   process.exitCode() == 0;
        };
        QVERIFY(git({QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"),
                     QStringLiteral("main")}));
        QVERIFY(git({QStringLiteral("config"), QStringLiteral("user.email"),
                     QStringLiteral("listed-worktree-test@example.invalid")}));
        QVERIFY(git({QStringLiteral("config"), QStringLiteral("user.name"),
                     QStringLiteral("Listed Worktree Test")}));
        QFile readme(QDir(repository).filePath(QStringLiteral("README.md")));
        QVERIFY(readme.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(readme.write("test\n") > 0);
        readme.close();
        QVERIFY(git({QStringLiteral("add"), QStringLiteral("README.md")}));
        QVERIFY(git({QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("initial")}));

        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(repository);
        window.resize(1200, 800);
        window.show();
        auto* agent =
            window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
        QVERIFY(agent);
        agent->click();
        auto* view = window.findChild<ketplus::AgentWorkspaceView*>();
        QVERIFY(view);
        const auto conversationButton = [&window](const QString& title) -> QToolButton* {
            for (auto* button : window.findChildren<QAbstractButton*>(
                     QStringLiteral("productConversationButton"))) {
                if (button->accessibleName() == title)
                    return button;
            }
            return nullptr;
        };
        QVERIFY(conversationButton(QStringLiteral("New conversation")) == nullptr);

        QTimer::singleShot(0, &window, [&window] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("agentConversationDialog"));
            QVERIFY(dialog);
            QVERIFY(dialog->findChild<QCheckBox*>(QStringLiteral("createWorktreeCheck")) ==
                    nullptr);
            auto* base = dialog->findChild<QComboBox*>(QStringLiteral("worktreeBaseWorktree"));
            auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("worktreeBranchName"));
            auto* create =
                dialog->findChild<QRadioButton*>(QStringLiteral("createNewWorktreeRadio"));
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            QVERIFY(base && name && create && buttons);
            QVERIFY(base->count() >= 1);
            // The second section cuts a branch; the first runs in a checkout as it is.
            QVERIFY(!name->isEnabled());
            create->setChecked(true);
            QVERIFY(name->isEnabled());
            QCOMPARE(name->placeholderText(), QStringLiteral("new branch name"));
            name->setText(QStringLiteral("feature/sidebar-listed"));
            QTRY_VERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
            buttons->button(QDialogButtonBox::Ok)->click();
        });
        view->requestNewSession();

        QTRY_VERIFY_WITH_TIMEOUT(conversationButton(QStringLiteral("New conversation")) != nullptr,
                                 5000);
        const QString linkedPath =
            QFileInfo(directory.filePath(QStringLiteral("ketviet-feature-sidebar-listed")))
                .canonicalFilePath();
        QVERIFY(!linkedPath.isEmpty());
        QCOMPARE(conversationButton(QStringLiteral("New conversation"))
                     ->property("worktreePath")
                     .toString(),
                 linkedPath);
        QVERIFY(conversationButton(QStringLiteral("New conversation"))->isChecked());
        auto* manager = window.findChild<ketplus::AgentSessionManager*>();
        QVERIFY(manager);
        QCOMPARE(manager->ownedWorktrees(linkedPath).size(), 1);
        const QString conversationId = manager->activeConversationId(linkedPath);
        QVERIFY(!conversationId.isEmpty());
        QVERIFY(manager->conversationOwnsWorktree(linkedPath, conversationId));
        QTimer::singleShot(0, &window, [&window, linkedPath] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("conversationDeleteDialog"));
            QVERIFY(dialog);
            auto* linked =
                dialog->findChild<QCheckBox*>(QStringLiteral("deleteLinkedWorktreeCheck"));
            QVERIFY(linked);
            QVERIFY(linked->isEnabled());
            // The dialog names every checkout the deletion would remove.
            auto* paths = dialog->findChild<QLabel*>(QStringLiteral("linkedWorktreePaths"));
            QVERIFY(paths);
            QVERIFY(paths->text().contains(QFileInfo(linkedPath).fileName()));
            // Forcing shows what is lost next to the choice, not only in a tooltip.
            auto* force = dialog->findChild<QCheckBox*>(QStringLiteral("forceDeleteWorktreeCheck"));
            auto* warning =
                dialog->findChild<QLabel*>(QStringLiteral("forceDeleteWorktreeWarning"));
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            QVERIFY(force && warning && buttons);
            QVERIFY(force->text().contains(QStringLiteral("discarding")));
            linked->setChecked(true);
            QVERIFY(force->isEnabled());
            QVERIFY(!warning->isVisibleTo(dialog));
            force->setChecked(true);
            QVERIFY(warning->isVisibleTo(dialog));
            QCOMPARE(buttons->button(QDialogButtonBox::Ok)->text(),
                     QStringLiteral("Delete and Discard Changes"));
            force->setChecked(false);
            QVERIFY(!warning->isVisibleTo(dialog));
            QCOMPARE(buttons->button(QDialogButtonBox::Ok)->text(), QStringLiteral("Delete"));
            dialog->reject();
        });
        view->requestDeleteConversation(conversationId);
        QCOMPARE(manager->ownedWorktrees(linkedPath).size(), 1);

        // Deleting the conversation keeps a worktree that has an unsaved editor, like the
        // sidebar's "Delete worktree…" does.
        const QString notes = QDir(linkedPath).filePath(QStringLiteral("notes.txt"));
        {
            QFile file(notes);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("saved\n"), 6);
        }
        window.openFile(notes);
        ketplus::EditorWidget* notesEditor = nullptr;
        QTRY_VERIFY([&] {
            for (auto* editor : window.findChildren<ketplus::EditorWidget*>()) {
                if (QFileInfo(editor->document().filePath()).canonicalFilePath() ==
                    QFileInfo(notes).canonicalFilePath())
                    notesEditor = editor;
            }
            return notesEditor != nullptr;
        }());
        notesEditor->replaceAllText("unsaved\n");
        QVERIFY(notesEditor->document().isModified());
        // Another conversation, born in the main checkout, now runs in the linked worktree.
        auto* appServer = window.findChild<ketplus::CodexAppServer*>();
        QVERIFY(appServer);
        emit appServer->daemonSessionDiscovered(
            QJsonObject{{QStringLiteral("session_id"), QStringLiteral("running-elsewhere")},
                        {QStringLiteral("session_generation"), 1},
                        {QStringLiteral("conversation_path"), repository},
                        {QStringLiteral("worktree_path"), linkedPath},
                        {QStringLiteral("state"), QStringLiteral("running")}});
        QVERIFY(manager->runningCheckoutPaths().contains(linkedPath));
        QSignalSpy deleted(manager, &ketplus::AgentSessionManager::conversationDeletionFinished);
        QTimer::singleShot(0, &window, [&window] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("conversationDeleteDialog"));
            QVERIFY(dialog);
            auto* linked =
                dialog->findChild<QCheckBox*>(QStringLiteral("deleteLinkedWorktreeCheck"));
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            QVERIFY(linked && buttons);
            linked->setChecked(true);
            buttons->button(QDialogButtonBox::Ok)->click();
        });
        view->requestDeleteConversation(conversationId);
        QTRY_COMPARE_WITH_TIMEOUT(deleted.size(), 1, 5000);
        QMessageBox* kept = nullptr;
        QTRY_VERIFY((kept = window.findChild<QMessageBox*>(
                         QStringLiteral("worktreeDeletionSkipped"))) != nullptr);
        QVERIFY(kept->text().contains(QStringLiteral("unsaved changes")));
        QVERIFY(kept->text().contains(QStringLiteral("an agent is running")));
        kept->accept();
        QVERIFY(QFileInfo(QDir(linkedPath).filePath(QStringLiteral(".git"))).isFile());
    }

    void restoresConversationConfigurationFromConversationStore() {
        QTemporaryDir settings;
        QTemporaryDir workspace;
        QVERIFY(settings.isValid());
        QVERIFY(workspace.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

        const QString path = QFileInfo(workspace.path()).canonicalFilePath();
        {
            ketplus::ThemeManager theme(*qApp);
            ketplus::MainWindow window(theme);
            window.openFolder(path);
            window.show();
            auto* agentButton =
                window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
            QVERIFY(agentButton);
            agentButton->click();
            auto* manager = window.findChild<ketplus::AgentSessionManager*>();
            QVERIFY(manager);
            manager->setConversationConfiguration(path, QStringLiteral("stale-model"),
                                                  QStringLiteral("low"),
                                                  QStringLiteral("read-only"));
            QVERIFY(window.close());
        }

        {
            ketplus::AgentSessionManager manager;
            manager.activate(path);
            manager.setConversationConfiguration(path, QStringLiteral("persisted-model"),
                                                 QStringLiteral("high"),
                                                 QStringLiteral("danger-full-access"));
        }

        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow reopened(theme);
        reopened.openFolder(path);
        reopened.show();
        auto* view = reopened.findChild<ketplus::AgentWorkspaceView*>();
        auto* manager = reopened.findChild<ketplus::AgentSessionManager*>();
        QVERIFY(view && manager);
        QTRY_COMPARE(view->conversationModel(), QStringLiteral("persisted-model"));
        QCOMPARE(view->conversationEffort(), QStringLiteral("high"));
        QCOMPARE(view->conversationPermission(), QStringLiteral("danger-full-access"));
        const QVariantMap configuration = manager->conversationConfiguration(path);
        QCOMPARE(configuration.value(QStringLiteral("model")).toString(),
                 QStringLiteral("persisted-model"));
        QCOMPARE(configuration.value(QStringLiteral("effort")).toString(), QStringLiteral("high"));
        QCOMPARE(configuration.value(QStringLiteral("permission")).toString(),
                 QStringLiteral("danger-full-access"));
    }

    void selectsMissingWorktreeWithoutChangingEditorWorkspace() {
        QTemporaryDir settings;
        QTemporaryDir directory;
        QVERIFY(settings.isValid());
        QVERIFY(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        const QString missing =
            QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath("removed-worktree");
        QVERIFY(QDir().mkdir(missing));
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(directory.path());
        window.show();
        auto* button = window.findChild<QAbstractButton*>("agentWorkspaceActivityButton");
        QVERIFY(button);
        button->click();
        auto* view = window.findChild<ketplus::AgentWorkspaceView*>();
        QVERIFY(view);
        auto* manager = window.findChild<ketplus::AgentSessionManager*>();
        QVERIFY(manager);
        manager->activate(missing);
        const QString id = manager->activeConversationId(missing);
        manager->renameConversation(missing, id, "Retained conversation");
        QVERIFY(QDir().rmdir(missing));
        const QString title = window.windowTitle();
        view->selectWorktree(missing);
        QCOMPARE(view->activePath(), missing);
        QCOMPARE(manager->activeConversationId(missing), id);
        QCOMPARE(view->sessionTitle(), QStringLiteral("Retained conversation"));
        QCOMPARE(view->sessionStatus(), QStringLiteral("Worktree missing"));
        // The registry key may outlive its checkout after the daemon moves execution.
        emit manager->executionContextChanged(missing, id, "replacement", directory.path(), 2);
        QCOMPARE(view->executionPath(), directory.path());
        QVERIFY(view->sessionStatus() != QStringLiteral("Worktree missing"));
        emit manager->sessionStateChanged(missing, "Retained conversation", "Working", true);
        QCOMPARE(view->sessionStatus(), QStringLiteral("Working"));
        QCOMPARE(window.windowTitle(), title);
        view->selectWorktree(directory.path());
        QCOMPARE(view->activePath(), QFileInfo(directory.path()).canonicalFilePath());
        QVERIFY(view->sessionStatus() != QStringLiteral("Worktree missing"));

        const QString currentPath = view->activePath();
        const QString currentId = manager->activeConversationId(currentPath);
        manager->renameConversation(currentPath, currentId, "Current conversation");
        view->selectWorktree(missing);
        QCOMPARE(view->activePath(), missing);
        auto* sidebar = window.findChild<ketplus::ActivityBar*>("activityBar");
        QVERIFY(sidebar);
        emit sidebar->conversationRequested(currentPath, currentId);
        QCOMPARE(view->activePath(), currentPath);
        QCOMPARE(manager->activeConversationId(currentPath), currentId);
        QCOMPARE(view->sessionTitle(), QStringLiteral("Current conversation"));
        QVERIFY(view->sessionStatus() != QStringLiteral("Worktree missing"));
        QCOMPARE(window.windowTitle(), title);
    }

    // A conversation whose worktree was deleted still opens its repository: Atlas listed the
    // Atlases of whichever repository was open before, under a sidebar showing another.
    void missingWorktreeConversationOpensItsRepository() {
        QTemporaryDir settings;
        QTemporaryDir directory;
        QVERIFY(settings.isValid());
        QVERIFY(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

        const QString ketplus = directory.filePath(QStringLiteral("ketplus"));
        const QString ketviet = directory.filePath(QStringLiteral("ketviet"));
        const QString linked = directory.filePath(QStringLiteral("ketplus-refactor"));
        const auto git = [](const QString& repository, const QStringList& arguments) {
            QProcess process;
            process.start(QStringLiteral("git"),
                          QStringList{QStringLiteral("-C"), repository} + arguments);
            return process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit &&
                   process.exitCode() == 0;
        };
        for (const QString& repository : {ketplus, ketviet}) {
            QVERIFY(QDir().mkpath(repository));
            QVERIFY(git(repository, {QStringLiteral("init"), QStringLiteral("-q"),
                                     QStringLiteral("-b"), QStringLiteral("main")}));
            QVERIFY(git(repository, {QStringLiteral("config"), QStringLiteral("user.email"),
                                     QStringLiteral("atlas-root-test@example.invalid")}));
            QVERIFY(git(repository, {QStringLiteral("config"), QStringLiteral("user.name"),
                                     QStringLiteral("Atlas Root Test")}));
            QVERIFY(git(repository, {QStringLiteral("commit"), QStringLiteral("-q"),
                                     QStringLiteral("--allow-empty"), QStringLiteral("-m"),
                                     QStringLiteral("initial")}));
        }
        QVERIFY(
            git(ketplus, {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-qb"),
                          QStringLiteral("refactor"), linked, QStringLiteral("HEAD")}));
        const QString atlas = QDir(ketviet).filePath(QStringLiteral("design/crm.ketatlas"));
        QVERIFY(QDir().mkpath(atlas));
        QFile manifest(QDir(atlas).filePath(QStringLiteral("atlas.json")));
        QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(manifest.write(R"({"version":1,"title":"CRM","screens":[],"flows":[]})") > 0);
        manifest.close();

        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(ketplus);
        window.resize(1200, 800);
        window.show();
        auto* agentButton =
            window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
        QVERIFY(agentButton);
        agentButton->click();
        auto* view = window.findChild<ketplus::AgentWorkspaceView*>();
        auto* manager = window.findChild<ketplus::AgentSessionManager*>();
        auto* index = window.findChild<ketplus::AtlasIndex*>();
        QVERIFY(view && manager && index);
        const QString ketplusPath = QFileInfo(ketplus).canonicalFilePath();
        const QString ketvietPath = QFileInfo(ketviet).canonicalFilePath();
        const QString linkedPath = QFileInfo(linked).canonicalFilePath();
        manager->activate(linkedPath);
        const QString id = manager->activeConversationId(linkedPath);
        manager->renameConversation(linkedPath, id, QStringLiteral("Refactor conversation"));
        const auto conversationButton = [&window]() -> QToolButton* {
            for (auto* button : window.findChildren<QAbstractButton*>(
                     QStringLiteral("productConversationButton"))) {
                if (button->accessibleName() == QStringLiteral("Refactor conversation"))
                    return button;
            }
            return nullptr;
        };
        QTRY_VERIFY(conversationButton() != nullptr);

        window.openFolder(ketviet);
        QVERIFY(QDir(linkedPath).removeRecursively());
        QCOMPARE(index->workspaceRoot(), ketvietPath);
        QTRY_COMPARE(index->atlases().size(), 1);

        // Still listed under ketplus in the sidebar; that is the row the reader clicks.
        QTRY_VERIFY(conversationButton() != nullptr);
        conversationButton()->click();
        QCOMPARE(index->workspaceRoot(), ketplusPath);
        QTRY_VERIFY(index->atlases().isEmpty());
        // The conversation itself still opens as history of its deleted worktree.
        QCOMPARE(view->activePath(), linkedPath);
        QCOMPARE(manager->activeConversationId(linkedPath), id);
        QCOMPARE(view->sessionStatus(), QStringLiteral("Worktree missing"));

        // Source Control shows the window's folder, but the sidebar keeps this conversation
        // selected and the status bar says which folder is shown instead.
        QAction* sourceControl = nullptr;
        for (auto* action : window.findChildren<QAction*>()) {
            if (action->text() == QStringLiteral("Source Control"))
                sourceControl = action;
        }
        QVERIFY(sourceControl);
        sourceControl->setChecked(true);
        QTRY_VERIFY(conversationButton() != nullptr &&
                    conversationButton()->property("selectedConversation").toBool());
        QVERIFY(
            window.statusBar()->currentMessage().contains(QStringLiteral("worktree was deleted")));
    }

    void derivesWorktreeNameFromBranchName() {
        QCOMPARE(ketplus::AgentWorktreeCreator::directoryNameForBranch(
                     QStringLiteral(" feature/agent-ui ")),
                 QStringLiteral("feature-agent-ui"));
        QCOMPARE(ketplus::AgentWorktreeCreator::worktreePath(QStringLiteral("/projects/ketplus"),
                                                             QStringLiteral("feature/agent-ui")),
                 QStringLiteral("/projects/ketplus-feature-agent-ui"));
    }

    void createsBranchAndWorktree() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString repository = directory.filePath(QStringLiteral("ketplus"));
        QVERIFY(QDir().mkpath(repository));

        const auto git = [&repository](const QStringList& arguments) {
            QProcess process;
            process.setWorkingDirectory(repository);
            process.start(QStringLiteral("git"), arguments);
            if (!process.waitForFinished(5000)) {
                return false;
            }
            return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        };
        QVERIFY(git({QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"),
                     QStringLiteral("develop")}));
        QVERIFY(git({QStringLiteral("config"), QStringLiteral("user.email"),
                     QStringLiteral("agent-worktree-test@example.invalid")}));
        QVERIFY(git({QStringLiteral("config"), QStringLiteral("user.name"),
                     QStringLiteral("Agent Worktree Test")}));
        QFile readme(QDir(repository).filePath(QStringLiteral("README.md")));
        QVERIFY(readme.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(readme.write("test\n"), 5);
        readme.close();
        QVERIFY(git({QStringLiteral("add"), QStringLiteral("README.md")}));
        QVERIFY(git({QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("initial")}));
        QVERIFY(git({"branch", "base"}));
        QVERIFY(git({"update-ref", "refs/remotes/origin/base", "HEAD"}));
        QVERIFY(git({"commit", "--allow-empty", "-qm", "later"}));

        ketplus::AgentWorktreeDialog dialog(repository);
        dialog.show();
        auto* bases = dialog.findChild<QComboBox*>("worktreeBaseBranch");
        auto* name = dialog.findChild<QLineEdit*>("worktreeBranchName");
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        QVERIFY(bases && name && buttons);
        QTRY_VERIFY(bases->isEnabled());
        QCOMPARE(dialog.baseBranch(), QStringLiteral("refs/heads/develop"));
        QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
        QVERIFY(bases->findData("refs/heads/base") >= 0);
        QVERIFY(bases->findData("refs/remotes/origin/base") >= 0);
        bases->setCurrentIndex(bases->findData("refs/remotes/origin/base"));
        name->setText("feature/agent-ui");
        QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
        if (const auto screenshot = qEnvironmentVariable("KETPLUS_WORKTREE_DIALOG_SCREENSHOT");
            !screenshot.isEmpty())
            QVERIFY(dialog.grab().save(screenshot));
        dialog.hide();

        ketplus::GitWorktree current;
        current.path = repository;
        current.branch = QStringLiteral("develop");
        current.current = true;
        ketplus::AgentWorktreeDialog conversation(
            repository, nullptr, ketplus::AgentWorktreeDialog::Purpose::Conversation, repository,
            {current});
        conversation.show();
        QVERIFY(conversation.minimumWidth() >= 560);
        QVERIFY(conversation.findChild<QCheckBox*>(QStringLiteral("createWorktreeCheck")) ==
                nullptr);
        auto* conversationBases =
            conversation.findChild<QComboBox*>(QStringLiteral("worktreeBaseWorktree"));
        auto* conversationName = conversation.findChild<QLineEdit*>("worktreeBranchName");
        auto* conversationBranches =
            conversation.findChild<QComboBox*>(QStringLiteral("worktreeBaseBranch"));
        auto* conversationCreate =
            conversation.findChild<QRadioButton*>(QStringLiteral("createNewWorktreeRadio"));
        auto* conversationExisting =
            conversation.findChild<QRadioButton*>(QStringLiteral("useExistingWorktreeRadio"));
        auto* conversationButtons = conversation.findChild<QDialogButtonBox*>();
        auto* conversationError =
            conversation.findChild<QLabel*>(QStringLiteral("worktreeDialogError"));
        QVERIFY(conversationBases && conversationName && conversationBranches &&
                conversationCreate && conversationExisting && conversationButtons &&
                conversationError);
        // The first section is the default: run where the reader already is.
        QVERIFY(conversationExisting->isChecked());
        {
            ketplus::ThemeManager theme(*qApp);
            for (const bool dark : {true, false}) {
                theme.previewMode(dark ? ketplus::ThemeManager::Mode::Dark
                                       : ketplus::ThemeManager::Mode::Light);
                QTest::qWait(30);
                QStyleOptionButton option;
                option.initFrom(conversationExisting);
                const QRect indicator = conversationExisting->style()->subElementRect(
                    QStyle::SE_RadioButtonIndicator, &option, conversationExisting);
                const auto image = conversationExisting->grab().toImage();
                QVERIFY(!image.isNull());
                const QColor centre = image.pixelColor(
                    (QPointF(indicator.center()) * image.devicePixelRatio()).toPoint());
                QCOMPARE(centre, conversationExisting->palette().color(QPalette::Text));
                if (const auto capture = qEnvironmentVariable("KETPLUS_RADIO_SCREENSHOT");
                    !capture.isEmpty())
                    QVERIFY(
                        conversation.grab().save(capture + (dark ? ".dark.png" : ".light.png")));
            }
            theme.cancelPreview();
        }
        QCOMPARE(conversationBases->count(), 1);
        QVERIFY(conversationBases->isEnabled());
        QVERIFY(!conversationName->isEnabled());
        QVERIFY(!conversationBranches->isEnabled());
        QCOMPARE(conversation.selectedWorktreePath(), QFileInfo(repository).canonicalFilePath());
        QVERIFY(!conversation.createWorktree());
        QVERIFY(conversationButtons->button(QDialogButtonBox::Ok)->isEnabled());
        QTRY_VERIFY(conversation.property("localBranchesReady").toBool());

        // A worktree made outside KetPlus while the dialog is open joins the list,
        // and the one the reader picked stays picked.
        const QString elsewherePath = directory.filePath(QStringLiteral("made-elsewhere"));
        QVERIFY(QDir().mkpath(elsewherePath));
        ketplus::GitWorktree elsewhere;
        elsewhere.path = elsewherePath;
        elsewhere.branch = QStringLiteral("elsewhere");
        conversation.setWorktrees({current, elsewhere});
        QCOMPARE(conversationBases->count(), 2);
        QCOMPARE(conversation.selectedWorktreePath(), QFileInfo(repository).canonicalFilePath());
        const QString elsewhereCanonical = QFileInfo(elsewherePath).canonicalFilePath();
        conversationBases->setCurrentIndex(conversationBases->findData(elsewhereCanonical));
        conversation.setWorktrees({elsewhere, current});
        QCOMPARE(conversation.selectedWorktreePath(), elsewhereCanonical);
        QVERIFY(conversationBases->isEnabled());
        conversation.setWorktrees({current});
        QCOMPARE(conversationBases->count(), 1);
        QCOMPARE(conversation.selectedWorktreePath(), QFileInfo(repository).canonicalFilePath());
        QTRY_VERIFY(conversation.property("localBranchesReady").toBool());

        // The second section names a branch to cut, and one to cut it from.
        conversationCreate->setChecked(true);
        QVERIFY(conversation.createWorktree());
        QVERIFY(conversationName->isEnabled());
        QVERIFY(conversationBranches->isEnabled());
        // The section not chosen is inert, so only one of them can be acted on.
        QVERIFY(!conversationBases->isEnabled());
        QCOMPARE(conversationName->placeholderText(), QStringLiteral("new branch name"));
        QVERIFY(conversationBranches->findData("refs/heads/develop") >= 0);
        QVERIFY(conversationBranches->findData("refs/remotes/origin/base") >= 0);
        QCOMPARE(conversationBranches->findData(QStringLiteral("HEAD")), -1);
        QCOMPARE(conversation.baseBranch(), QStringLiteral("refs/heads/develop"));
        QVERIFY(!conversationButtons->button(QDialogButtonBox::Ok)->isEnabled());
        conversationName->setText(QStringLiteral("develop"));
        QTRY_VERIFY(conversationButtons->button(QDialogButtonBox::Ok)->isEnabled());
        if (const auto screenshot = qEnvironmentVariable("KETPLUS_CONVERSATION_DIALOG_SCREENSHOT");
            !screenshot.isEmpty())
            QVERIFY(conversation.grab().save(screenshot));
        conversationButtons->button(QDialogButtonBox::Ok)->click();
        QVERIFY(conversation.isVisible());
        QVERIFY(conversationError->isVisible());
        QVERIFY(conversationError->text().contains(QStringLiteral("already exists")));
        conversation.hide();

        ketplus::AgentWorktreeCreator creator;
        QSignalSpy created(&creator, &ketplus::AgentWorktreeCreator::created);
        QSignalSpy failed(&creator, &ketplus::AgentWorktreeCreator::failed);
        creator.create(repository, repository, dialog.branchName(), dialog.baseBranch());
        QTRY_COMPARE_WITH_TIMEOUT(created.count(), 1, 5000);
        QCOMPARE(failed.count(), 0);

        const QString expectedPath = directory.filePath(QStringLiteral("ketplus-feature-agent-ui"));
        QCOMPARE(created.constFirst().at(0).toString(), expectedPath);
        QVERIFY(QFileInfo::exists(QDir(expectedPath).filePath(QStringLiteral("README.md"))));

        QProcess branch;
        branch.setWorkingDirectory(expectedPath);
        branch.start(QStringLiteral("git"),
                     {QStringLiteral("branch"), QStringLiteral("--show-current")});
        QVERIFY(branch.waitForFinished(5000));
        QCOMPARE(QString::fromUtf8(branch.readAllStandardOutput()).trimmed(),
                 QStringLiteral("feature/agent-ui"));
        branch.start("git", {"log", "-1", "--format=%s"});
        QVERIFY(branch.waitForFinished(5000));
        QCOMPARE(branch.readAllStandardOutput().trimmed(), QByteArray("initial"));

        QSignalSpy removed(&creator, &ketplus::AgentWorktreeCreator::removed);
        QFile untracked(QDir(expectedPath).filePath("keep.txt"));
        QVERIFY(untracked.open(QIODevice::WriteOnly));
        untracked.write("do not delete");
        untracked.close();
        creator.remove(repository, expectedPath);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(removed.count(), 0);
        QVERIFY(untracked.exists());
        QVERIFY(untracked.remove());
        QVERIFY(git({"worktree", "lock", expectedPath}));
        creator.remove(repository, expectedPath);
        QTRY_COMPARE(failed.count(), 2);
        QVERIFY(QFileInfo::exists(expectedPath));
        QVERIFY(git({"worktree", "unlock", expectedPath}));
        creator.remove(repository, expectedPath);
        QTRY_COMPARE(removed.count(), 1);
        QVERIFY(!QFileInfo::exists(expectedPath));
        QVERIFY(git({"show-ref", "--verify", "refs/heads/feature/agent-ui"}));
        creator.remove(repository, repository);
        QCOMPARE(failed.count(), 3);

        creator.create(repository, repository, QStringLiteral("feature/force-delete"),
                       QStringLiteral("HEAD"));
        QTRY_COMPARE_WITH_TIMEOUT(created.count(), 2, 5000);
        const QString forcePath =
            directory.filePath(QStringLiteral("ketplus-feature-force-delete"));
        QFile dirty(QDir(forcePath).filePath(QStringLiteral("dirty.txt")));
        QVERIFY(dirty.open(QIODevice::WriteOnly));
        dirty.write("keep");
        dirty.close();
        creator.remove(repository, forcePath);
        QTRY_COMPARE(failed.count(), 4);
        QVERIFY(QFileInfo::exists(forcePath));
        creator.remove(repository, forcePath, true);
        QTRY_COMPARE_WITH_TIMEOUT(removed.count(), 2, 5000);
        QVERIFY(!QFileInfo::exists(forcePath));
    }

    // A changed file that has since been committed has no diff. With the status bar hidden
    // in the agent workspace, clicking it used to do nothing visible; it opens instead.
    void unchangedChatFileOpensInsteadOfEmptyDiff() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString repository = directory.filePath(QStringLiteral("committed"));
        QVERIFY(QDir().mkpath(repository));
        const auto git = [&repository](const QStringList& arguments) {
            QProcess process;
            process.start(QStringLiteral("git"),
                          QStringList{QStringLiteral("-C"), repository} + arguments);
            return process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit &&
                   process.exitCode() == 0;
        };
        QVERIFY(git({QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"),
                     QStringLiteral("main")}));
        QVERIFY(git({QStringLiteral("config"), QStringLiteral("user.email"),
                     QStringLiteral("diff-test@example.invalid")}));
        QVERIFY(git(
            {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Diff Test")}));
        const QString filePath = QDir(repository).filePath(QStringLiteral("done.cpp"));
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(file.write("int done = 1;\n") > 0);
        file.close();
        QVERIFY(git({QStringLiteral("add"), QStringLiteral("done.cpp")}));
        QVERIFY(git({QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("initial")}));

        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(repository);
        window.resize(1200, 800);
        window.show();
        auto* agent =
            window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
        QVERIFY(agent);
        agent->click();
        QTRY_VERIFY(!window.statusBar()->isVisible());
        auto* agentWorkspace = window.findChild<ketplus::AgentWorkspaceView*>();
        QVERIFY(agentWorkspace);
        agentWorkspace->setActivePath(repository);
        auto* gitService = window.findChild<ketplus::GitService*>();
        QVERIFY(gitService);
        QTRY_VERIFY_WITH_TIMEOUT(gitService->snapshot().isRepository(), 5000);
        agentWorkspace->requestFileDiff(QStringLiteral("done.cpp"));
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QTabWidget*>("agentDetailTabs") != nullptr, 5000);
        auto* detailTabs = window.findChild<QTabWidget*>("agentDetailTabs");
        QTRY_COMPARE_WITH_TIMEOUT(detailTabs->count(), 1, 5000);
        auto* editor = qobject_cast<ketplus::EditorWidget*>(detailTabs->currentWidget());
        QVERIFY2(editor, "an unchanged file should open in an editor tab");
        QCOMPARE(QFileInfo(editor->document().filePath()).canonicalFilePath(),
                 QFileInfo(filePath).canonicalFilePath());
    }

    void opensChatFilesInClosableDetailTabs() {
        QTemporaryDir settings;
        QTemporaryDir workspace;
        QVERIFY(settings.isValid());
        QVERIFY(workspace.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

        const QString filePath = workspace.filePath(QStringLiteral("index.html"));
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(file.write("<!doctype html><title>Agent detail</title>") > 0);
        file.close();
        const QString secondFilePath = workspace.filePath(QStringLiteral("second.html"));
        QFile secondFile(secondFilePath);
        QVERIFY(secondFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(secondFile.write("<!doctype html><title>Second detail</title>") > 0);
        secondFile.close();

        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", QByteArrayLiteral("/usr/bin:/bin"));
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace.path());
        window.resize(1100, 700);
        window.show();

        auto* agentButton = window.findChild<QAbstractButton*>("agentWorkspaceActivityButton");
        QVERIFY(agentButton);
        agentButton->click();
        auto* agentWorkspace = window.findChild<ketplus::AgentWorkspaceView*>();
        QVERIFY(agentWorkspace);
        QTRY_VERIFY(agentWorkspace->isVisible());
        QTRY_COMPARE(agentWorkspace->repositoryName(), QFileInfo(workspace.path()).fileName());
        auto* sourceButton = window.findChild<QAbstractButton*>("sourceControlActivityButton");
        QVERIFY(sourceButton);
        // A folder without Git disables the activity button; invoke the shell slot instead.
        QAction* sourceAction = nullptr;
        for (auto* action : window.findChildren<QAction*>())
            if (action->text() == QStringLiteral("Source Control"))
                sourceAction = action;
        QVERIFY(sourceAction);
        sourceAction->setChecked(true);
        // Repositories are added from the persistent product sidebar, which opens a
        // folder chooser; Spotlight only searches files and Atlases. Source Control has
        // no duplicate picker in its 224 px context panel.
        auto* addRepository = window.findChild<QToolButton*>("productAddRepositoryButton");
        QVERIFY(addRepository);
        QCOMPARE(addRepository->toolTip(), QStringLiteral("Add repository"));
        QVERIFY(!window.findChild<ketplus::SpotlightDialog*>());
        agentButton->click();

        QTemporaryDir externalWorkspace;
        QVERIFY(externalWorkspace.isValid());
        const QString externalFilePath = externalWorkspace.filePath(QStringLiteral("external.cpp"));
        QFile externalFile(externalFilePath);
        QVERIFY(externalFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(externalFile.write("int external = 1;\n") > 0);
        externalFile.close();
        QSignalSpy externalFileRequested(&window, &ketplus::MainWindow::repositoryFileRequested);
        QSignalSpy externalDiffRequested(&window,
                                         &ketplus::MainWindow::repositoryFileDiffRequested);
        agentWorkspace->setActivePath(externalWorkspace.path());
        agentWorkspace->openRelativeFile(QStringLiteral("external.cpp"));
        QCOMPARE(externalFileRequested.count(), 0);
        auto* externalTabs = window.findChild<QTabWidget*>("agentDetailTabs");
        QVERIFY(externalTabs);
        QCOMPARE(externalTabs->count(), 1);
        auto* externalEditor = qobject_cast<ketplus::EditorWidget*>(externalTabs->currentWidget());
        QVERIFY(externalEditor);
        QCOMPARE(QFileInfo(externalEditor->document().filePath()).canonicalFilePath(),
                 QFileInfo(externalFilePath).canonicalFilePath());
        agentWorkspace->openRelativeFile(QStringLiteral("external.cpp:42"));
        QCOMPARE(externalFileRequested.count(), 0);
        QCOMPARE(externalTabs->count(), 1);
        // Non-Git files fall back to the editor in the same window, even for a diff link.
        agentWorkspace->requestFileDiff(QStringLiteral("external.cpp"));
        QCOMPARE(externalDiffRequested.count(), 0);
        QCOMPARE(externalTabs->count(), 1);
        externalTabs->tabCloseRequested(0);
        QCOMPARE(externalTabs->count(), 0);
        agentWorkspace->setActivePath(workspace.path());

        window.openFile(filePath);
        auto* detailTabs = window.findChild<QTabWidget*>("agentDetailTabs");
        auto* editorSplit = window.findChild<QSplitter*>("editorSplit");
        QVERIFY(detailTabs);
        QVERIFY(editorSplit);
        QTRY_VERIFY(detailTabs->isVisible());
        QCOMPARE(detailTabs->count(), 1);
        auto* editor = qobject_cast<ketplus::EditorWidget*>(detailTabs->currentWidget());
        QVERIFY(editor);
        QCOMPARE(QFileInfo(editor->document().filePath()).canonicalFilePath(),
                 QFileInfo(filePath).canonicalFilePath());
        QVERIFY(!editorSplit->isVisible());
        window.openFile(filePath);
        QCOMPARE(detailTabs->count(), 1);

        const auto triggerTabAction = [](QTabBar* tabBar, const int index,
                                         const QString& actionText) {
            const QPoint position = tabBar->tabRect(index).center();
            if (!QMetaObject::invokeMethod(tabBar, "customContextMenuRequested",
                                           Qt::DirectConnection, Q_ARG(QPoint, position))) {
                return false;
            }
            QCoreApplication::processEvents();
            auto* menu =
                tabBar->window()->findChild<QMenu*>(QStringLiteral("agentDetailTabContextMenu"));
            if (menu == nullptr) {
                return false;
            }
            for (QAction* action : menu->actions()) {
                if (action->text() == actionText) {
                    action->trigger();
                    QCoreApplication::processEvents();
                    return true;
                }
            }
            menu->close();
            return false;
        };

        window.openFile(secondFilePath);
        QCOMPARE(detailTabs->count(), 2);
        QVERIFY(triggerTabAction(detailTabs->tabBar(), 0, QStringLiteral("Close Other Tabs")));
        QCOMPARE(detailTabs->count(), 1);
        auto* retainedEditor = qobject_cast<ketplus::EditorWidget*>(detailTabs->currentWidget());
        QVERIFY(retainedEditor);
        QCOMPARE(QFileInfo(retainedEditor->document().filePath()).canonicalFilePath(),
                 QFileInfo(filePath).canonicalFilePath());

        window.openFile(secondFilePath);
        QCOMPARE(detailTabs->count(), 2);
        QVERIFY(triggerTabAction(detailTabs->tabBar(), 0, QStringLiteral("Close All Tabs")));
        QCOMPARE(detailTabs->count(), 0);
        QTRY_VERIFY(!detailTabs->isVisible());
        QVERIFY(agentWorkspace->isVisible());
        QTRY_VERIFY(([&window] {
            for (auto* process : window.findChildren<QProcess*>()) {
                if (process->program().endsWith(QStringLiteral("/git")) &&
                    process->state() != QProcess::NotRunning) {
                    return false;
                }
            }
            return true;
        })());

        if (oldPath.isNull()) {
            qunsetenv("PATH");
        } else {
            qputenv("PATH", oldPath);
        }
    }
};

QTEST_MAIN(AgentWorkspaceShellTest)
#include "AgentWorkspaceShellTest.moc"
