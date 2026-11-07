#include "agent/AgentWorkspaceView.h"
#include "app/MainWindow.h"
#include "app/SpotlightDialog.h"
#include "editor/EditorWidget.h"
#include "ui/FindReplaceBar.h"
#include "ui/Theme.h"
#include "workspace/QuickOpenPopup.h"
#include "workspace/SearchPanel.h"

#include <ScintillaMessages.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QQuickItem>
#include <QQuickView>
#include <QSettings>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest>

namespace {

constexpr unsigned int message(const Scintilla::Message value) {
    return static_cast<unsigned int>(value);
}

QAction* actionWithText(QObject& parent, const QString& text) {
    for (auto* action : parent.findChildren<QAction*>()) {
        if (action->text() == text) {
            return action;
        }
    }
    return nullptr;
}

// Only the current tab of a QTabWidget is visible, so this is the active editor.
ketplus::EditorWidget* visibleEditor(QWidget& window) {
    for (auto* editor : window.findChildren<ketplus::EditorWidget*>()) {
        if (editor->isVisible()) {
            return editor;
        }
    }
    return nullptr;
}

QString visibleFilePath(QWidget& window) {
    const auto* editor = visibleEditor(window);
    return editor == nullptr ? QString() : editor->document().filePath();
}

QString writeFile(const QDir& root, const QString& relativePath, const QByteArray& content) {
    root.mkpath(QFileInfo(root.filePath(relativePath)).path());
    QFile file(root.filePath(relativePath));
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size()) {
        return {};
    }
    file.close();
    return QFileInfo(file.fileName()).canonicalFilePath();
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class MainWindowEditorFeaturesTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void runsFileEditAndViewCommands();
    void routesFindToFocusedTranscriptOrEditor();
    void findsFilesInSpotlightAndNavigatesBack();
    void splitsViewsAndSharesTheDocument();
    void searchesAndReplacesAcrossFiles();
    void showsAndChangesFileFormat();
    void goesToADeclarationInTheSameFile();

  private:
    QTemporaryDir settingsDirectory_;
};

void MainWindowEditorFeaturesTest::initTestCase() {
    QVERIFY(settingsDirectory_.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory_.path());
    QSettings().clear();
}

void MainWindowEditorFeaturesTest::runsFileEditAndViewCommands() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QDir root(workspace.path());
    const QString first = writeFile(root, QStringLiteral("a.cpp"), "int alpha = 1;\n");
    const QString second = writeFile(root, QStringLiteral("b.txt"), "beta\n");
    QVERIFY(!first.isEmpty() && !second.isEmpty());

    ketplus::ThemeManager theme(*qApp);
    ketplus::MainWindow window(theme);
    window.resize(1100, 700);
    window.openFolder(workspace.path());
    window.show();
    window.openFile(first);
    window.openFile(second);
    QTRY_COMPARE(visibleFilePath(window), second);

    const QStringList recentFiles =
        QSettings().value(QStringLiteral("workspace/recentFiles")).toStringList();
    QVERIFY(recentFiles.contains(first));
    QVERIFY(recentFiles.contains(second));

    auto* closeTab = actionWithText(window, QStringLiteral("&Close Tab"));
    auto* reopenClosedTab = actionWithText(window, QStringLiteral("&Reopen Closed Tab"));
    QVERIFY(closeTab && reopenClosedTab);
    QVERIFY(!reopenClosedTab->isEnabled());
    closeTab->trigger();
    QTRY_COMPARE(visibleFilePath(window), first);
    QVERIFY(reopenClosedTab->isEnabled());
    reopenClosedTab->trigger();
    QTRY_COMPARE(visibleFilePath(window), second);

    auto* editor = visibleEditor(window);
    QVERIFY(editor);
    editor->goToLine(1);
    auto* paletteAction = window.findChild<QAction*>(QStringLiteral("commandPaletteAction"));
    QVERIFY(paletteAction);
    paletteAction->trigger();
    auto* palette = window.findChild<ketplus::QuickOpenPopup*>();
    QVERIFY(palette && palette->isVisible());
    auto* paletteQuery = palette->findChild<QLineEdit*>();
    QVERIFY(paletteQuery);
    paletteQuery->setText(QStringLiteral("Duplicate Line"));
    QVERIFY(!palette->visibleItems().isEmpty());
    QVERIFY(palette->visibleItems().first().label.endsWith(QStringLiteral("Duplicate Line")));
    QTest::keyClick(paletteQuery, Qt::Key_Return);
    QVERIFY(!palette->isVisible());
    QCOMPARE(editor->text(), QByteArray("beta\nbeta\n"));

    auto* saveAll = actionWithText(window, QStringLiteral("Save A&ll"));
    QVERIFY(saveAll);
    saveAll->trigger();
    QCOMPARE(readFile(second), QByteArray("beta\nbeta\n"));
    QVERIFY(!editor->document().isModified());

    auto* wordWrap = actionWithText(window, QStringLiteral("Word Wrap"));
    QVERIFY(wordWrap);
    wordWrap->setChecked(true);
    QCOMPARE(editor->send(message(Scintilla::Message::GetWrapMode)), 1);
    wordWrap->setChecked(false);
    QCOMPARE(editor->send(message(Scintilla::Message::GetWrapMode)), 0);
}

void MainWindowEditorFeaturesTest::findsFilesInSpotlightAndNavigatesBack() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QDir root(workspace.path());
    const QString source =
        writeFile(root, QStringLiteral("src/app/MainWindow.cpp"), "void open() {}\n");
    const QString notes = writeFile(root, QStringLiteral("notes.txt"), "notes\n");
    QVERIFY(!source.isEmpty() && !notes.isEmpty());

    ketplus::ThemeManager theme(*qApp);
    ketplus::MainWindow window(theme);
    window.resize(1100, 700);
    window.openFolder(workspace.path());
    window.show();
    window.openFile(notes);
    QTRY_COMPARE(visibleFilePath(window), notes);

    auto* goToFile = window.findChild<QAction*>(QStringLiteral("goToFileAction"));
    QVERIFY(goToFile);
    goToFile->trigger();
    auto* dialog = window.findChild<ketplus::SpotlightDialog*>(QStringLiteral("spotlightDialog"));
    QVERIFY(dialog);
    auto* query = dialog->findChild<QLineEdit*>(QStringLiteral("spotlightQuery"));
    auto* results = dialog->findChild<QListWidget*>(QStringLiteral("spotlightResults"));
    QVERIFY(query && results);

    // The path index is collected off the UI thread and fills Spotlight when ready.
    query->setText(QStringLiteral("MainWindow"));
    QTRY_COMPARE(results->count(), 1);
    QCOMPARE(results->currentItem()->data(Qt::UserRole).toString(),
             QStringLiteral("file:") + source);
    QTest::keyClick(query, Qt::Key_Return);
    QTRY_COMPARE(visibleFilePath(window), source);

    auto* back = actionWithText(window, QStringLiteral("Bac&k"));
    QVERIFY(back && back->isEnabled());
    back->trigger();
    QTRY_COMPARE(visibleFilePath(window), notes);
}

void MainWindowEditorFeaturesTest::splitsViewsAndSharesTheDocument() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QString source = writeFile(QDir(workspace.path()), QStringLiteral("a.cpp"), "int a;\n");
    QVERIFY(!source.isEmpty());

    ketplus::ThemeManager theme(*qApp);
    ketplus::MainWindow window(theme);
    window.resize(1100, 700);
    window.openFolder(workspace.path());
    window.show();
    window.openFile(source);
    QTRY_COMPARE(visibleFilePath(window), source);
    auto* tabEditor = visibleEditor(window);
    QVERIFY(tabEditor);

    auto* splitRight = actionWithText(window, QStringLiteral("Split Right"));
    auto* closeSplit = actionWithText(window, QStringLiteral("Close Split"));
    QVERIFY(splitRight && closeSplit);
    splitRight->trigger();
    auto* splitEditor = window.findChild<ketplus::EditorWidget*>(QStringLiteral("splitEditor"));
    QVERIFY(splitEditor && splitEditor->isVisible());
    QVERIFY(closeSplit->isEnabled());
    QCOMPARE(splitEditor->text(), tabEditor->text());

    splitEditor->send(message(Scintilla::Message::AppendText), 1, reinterpret_cast<sptr_t>("x"));
    QCOMPARE(tabEditor->text(), QByteArray("int a;\nx"));
    QVERIFY(tabEditor->document().isModified());

    closeSplit->trigger();
    QTRY_VERIFY(window.findChild<ketplus::EditorWidget*>(QStringLiteral("splitEditor")) == nullptr);
    QVERIFY(!closeSplit->isEnabled());
    QCOMPARE(tabEditor->text(), QByteArray("int a;\nx"));

    // The split group has its own tab bar whose close button closes the split with the mouse.
    splitRight->trigger();
    auto* splitTabs = window.findChild<QTabBar*>(QStringLiteral("splitTabBar"));
    QVERIFY(splitTabs && splitTabs->isVisible());
    QCOMPARE(splitTabs->count(), 1);
    auto* closeButton = qobject_cast<QToolButton*>(splitTabs->tabButton(0, QTabBar::RightSide));
    QVERIFY(closeButton);
    closeButton->click();
    QTRY_VERIFY(window.findChild<ketplus::EditorWidget*>(QStringLiteral("splitEditor")) == nullptr);
    QVERIFY(!closeSplit->isEnabled());
}

void MainWindowEditorFeaturesTest::goesToADeclarationInTheSameFile() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QString source =
        writeFile(QDir(workspace.path()), QStringLiteral("a.cpp"),
                  "class Widget {};\n\nint main() {\n    Widget widget;\n    return 0;\n}\n");
    QVERIFY(!source.isEmpty());

    ketplus::ThemeManager theme(*qApp);
    ketplus::MainWindow window(theme);
    window.resize(1100, 700);
    window.openFolder(workspace.path());
    window.show();
    window.openFile(source);
    QTRY_COMPARE(visibleFilePath(window), source);
    auto* editor = visibleEditor(window);
    QVERIFY(editor);

    // Put the caret on the use of Widget, then ask where it is declared.
    editor->goToLine(4);
    editor->send(message(Scintilla::Message::SetCurrentPos),
                 static_cast<uptr_t>(editor->send(message(Scintilla::Message::GetCurrentPos)) + 6));
    auto* goToDefinition = actionWithText(window, QStringLiteral("Go to &Definition"));
    QVERIFY(goToDefinition);
    goToDefinition->trigger();
    QTRY_COMPARE(editor->currentLine(), 1);
}

void MainWindowEditorFeaturesTest::searchesAndReplacesAcrossFiles() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QDir root(workspace.path());
    const QString openPath = writeFile(root, QStringLiteral("open.cpp"), "alpha();\nalpha++;\n");
    const QString closedPath = writeFile(root, QStringLiteral("closed.txt"), "alpha beta\r\n");
    QVERIFY(!openPath.isEmpty() && !closedPath.isEmpty());

    ketplus::ThemeManager theme(*qApp);
    ketplus::MainWindow window(theme);
    window.resize(1100, 700);
    window.openFolder(workspace.path());
    window.show();
    window.openFile(openPath);
    QTRY_COMPARE(visibleFilePath(window), openPath);

    QVERIFY(!window.findChild<QToolButton*>(QStringLiteral("searchActivityButton")));
    auto* searchAction = window.findChild<QAction*>(QStringLiteral("searchInFilesAction"));
    QVERIFY(searchAction);
    QCOMPARE(searchAction->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    searchAction->trigger();
    auto* panel = window.findChild<ketplus::SearchPanel*>(QStringLiteral("searchPanel"));
    QVERIFY(panel && panel->isVisible());

    QLineEdit* query = nullptr;
    QLineEdit* replacement = nullptr;
    for (auto* edit : panel->findChildren<QLineEdit*>()) {
        if (edit->accessibleName() == QStringLiteral("Search in files")) {
            query = edit;
        } else if (edit->accessibleName() == QStringLiteral("Replace in files with")) {
            replacement = edit;
        }
    }
    QVERIFY(query && replacement);
    query->setText(QStringLiteral("alpha"));
    QTest::keyClick(query, Qt::Key_Return);
    QTRY_VERIFY(!panel->isSearching() && panel->resultMatchCount() == 3);
    QCOMPARE(panel->resultPaths().size(), 2);

    replacement->setText(QStringLiteral("omega"));
    // Accept the confirmation dialog that Replace All shows before writing.
    QTimer confirm;
    connect(&confirm, &QTimer::timeout, &window, [] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* box = qobject_cast<QMessageBox*>(widget);
                box != nullptr && box->isVisible()) {
                box->button(QMessageBox::Yes)->click();
            }
        }
    });
    confirm.start(50);
    QPushButton* replaceAll = nullptr;
    for (auto* button : panel->findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Replace All")) {
            replaceAll = button;
        }
    }
    QVERIFY(replaceAll && replaceAll->isEnabled());
    replaceAll->click();
    confirm.stop();

    auto* openEditor = visibleEditor(window);
    QVERIFY(openEditor);
    QCOMPARE(openEditor->text(), QByteArray("omega();\nomega++;\n"));
    QVERIFY(openEditor->document().isModified());
    QCOMPARE(readFile(closedPath), QByteArray("omega beta\r\n"));
    QTRY_VERIFY(!panel->isSearching());

    auto* explorerButton =
        window.findChild<QAbstractButton*>(QStringLiteral("explorerActivityButton"));
    QVERIFY(explorerButton);
    explorerButton->click();
    QVERIFY(!panel->isVisible());
}

void MainWindowEditorFeaturesTest::showsAndChangesFileFormat() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QDir root(workspace.path());
    const QString unixSource = writeFile(root, QStringLiteral("unix.cpp"), "int a;\n");
    const QString windowsText = writeFile(root, QStringLiteral("windows.txt"), "one\r\ntwo\r\n");
    QVERIFY(!unixSource.isEmpty() && !windowsText.isEmpty());

    ketplus::ThemeManager theme(*qApp);
    ketplus::MainWindow window(theme);
    window.resize(1100, 700);
    window.openFolder(workspace.path());
    window.show();

    QToolButton* encoding = nullptr;
    QToolButton* lineEnding = nullptr;
    QToolButton* language = nullptr;
    for (auto* button : window.findChildren<QToolButton*>()) {
        if (button->accessibleName() == QStringLiteral("File encoding")) {
            encoding = button;
        } else if (button->accessibleName() == QStringLiteral("Line endings")) {
            lineEnding = button;
        } else if (button->accessibleName() == QStringLiteral("Language")) {
            language = button;
        }
    }
    QVERIFY(encoding && lineEnding && language);

    window.openFile(unixSource);
    QTRY_COMPARE(visibleFilePath(window), unixSource);
    QCOMPARE(encoding->text(), QStringLiteral("UTF-8"));
    QCOMPARE(lineEnding->text(), QStringLiteral("LF"));
    QCOMPARE(language->text(), QStringLiteral("C / C++"));

    window.openFile(windowsText);
    QTRY_COMPARE(visibleFilePath(window), windowsText);
    QCOMPARE(lineEnding->text(), QStringLiteral("CRLF"));
    QCOMPARE(language->text(), QStringLiteral("Plain Text"));

    auto* python = actionWithText(window, QStringLiteral("Python"));
    QVERIFY(python);
    python->trigger();
    QCOMPARE(language->text(), QStringLiteral("Python"));
    QCOMPARE(visibleEditor(window)->syntaxName(), QStringLiteral("python"));

    auto* lf = actionWithText(window, QStringLiteral("LF — Unix and macOS"));
    QVERIFY(lf);
    lf->trigger();
    QCOMPARE(lineEnding->text(), QStringLiteral("LF"));
    QCOMPARE(visibleEditor(window)->text(), QByteArray("one\ntwo\n"));
}

QTEST_MAIN(MainWindowEditorFeaturesTest)
#include "MainWindowEditorFeaturesTest.moc"

void MainWindowEditorFeaturesTest::routesFindToFocusedTranscriptOrEditor() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QDir root(workspace.path());
    const QString file = writeFile(root, QStringLiteral("a.cpp"), "int alpha = 1;\n");
    QVERIFY(!file.isEmpty());

    ketplus::ThemeManager theme(*qApp);
    ketplus::MainWindow window(theme);
    window.resize(1100, 700);
    window.openFolder(workspace.path());
    window.show();
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.openFile(file);
    QTRY_COMPARE(visibleFilePath(window), file);
    auto* find = actionWithText(window, QStringLiteral("&Find…"));
    auto* findNext = actionWithText(window, QStringLiteral("Find &Next"));
    auto* agentAction = actionWithText(window, QStringLiteral("Agent Workspace"));
    auto* bar = window.findChild<ketplus::FindReplaceBar*>();
    QVERIFY(find && findNext && agentAction && bar);

    // A focused editor keeps Edit > Find.
    auto* editor = visibleEditor(window);
    QVERIFY(editor);
    editor->setFocus(Qt::ShortcutFocusReason);
    QTRY_VERIFY(editor->hasFocus());
    find->trigger();
    QTRY_VERIFY(bar->isVisible());
    bar->hide();

    // A focused agent workspace takes Find and Find Next; the workspace registers no competing
    // QML shortcuts for them.
    agentAction->setChecked(true);
    auto* agent = window.findChild<ketplus::AgentWorkspaceView*>();
    QVERIFY(agent && agent->isVisible());
    QVERIFY(agent->findActionsProvided());
    agent->resetMessages({QVariantMap{{"messageKey", "a"},
                                      {"kind", "assistant"},
                                      {"meta", "Codex"},
                                      {"body", "alpha then alpha"},
                                      {"turnId", "turn-1"},
                                      {"filePath", ""}}},
                         QStringLiteral("workspace:routing"));
    auto* transcript =
        agent->quickView()->rootObject()->findChild<QQuickItem*>(QStringLiteral("agentTranscript"));
    QTRY_VERIFY(transcript != nullptr && transcript->opacity() == 1.0);
    agent->setFocus(Qt::ShortcutFocusReason);
    QTRY_VERIFY(agent->hasFocusWithin());
    find->trigger();
    QTRY_VERIFY(transcript->property("findOpen").toBool());
    QVERIFY(!bar->isVisible());
    QVariant ignored;
    QMetaObject::invokeMethod(transcript, "setFindQuery", Q_RETURN_ARG(QVariant, ignored),
                              Q_ARG(QVariant, QStringLiteral("alpha")));
    QCOMPARE(transcript->property("findCurrent").toInt(), 0);
    findNext->trigger();
    QCOMPARE(transcript->property("findCurrent").toInt(), 1);
    QMetaObject::invokeMethod(transcript, "closeFind", Q_RETURN_ARG(QVariant, ignored));

    // Back in the editor, Find goes to the editor again.
    agentAction->setChecked(false);
    editor = visibleEditor(window);
    QVERIFY(editor);
    editor->setFocus(Qt::ShortcutFocusReason);
    QTRY_VERIFY(editor->hasFocus());
    find->trigger();
    QTRY_VERIFY(bar->isVisible());
    QVERIFY(!transcript->property("findOpen").toBool());
}
