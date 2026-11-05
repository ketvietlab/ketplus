#include "app/MainWindow.h"
#include "editor/EditorWidget.h"
#include "terminal/TerminalPanel.h"
#include "ui/Theme.h"
#include <QAction>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QToolButton>
#include <QtTest>

class DesktopDocumentModesTest final : public QObject {
    Q_OBJECT
  private slots:
    void editorModeAndMenuStayInSync() {
        QTemporaryDir directory;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        qputenv("KETPLUS_DAEMON_SOCKET", directory.filePath("isolated.sock").toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", directory.filePath("isolated-token").toUtf8());
        qputenv("KETPLUS_CONVERSATION_WORKTREES", directory.filePath("worktrees.sqlite").toUtf8());
        const auto path = directory.filePath("notes.md");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("# Desktop preview\n\nA **formatted** document.\n");
        file.close();
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.resize(1200, 800);
        window.openFolder(directory.path());
        window.show();
        window.findChild<QToolButton*>("explorerActivityButton")->click();
        window.openFile(path);
        ketplus::EditorWidget* editor = nullptr;
        for (auto* candidate : window.findChildren<ketplus::EditorWidget*>())
            if (QFileInfo(candidate->document().filePath()).canonicalFilePath() ==
                QFileInfo(path).canonicalFilePath())
                editor = candidate;
        QVERIFY(editor);
        QTRY_VERIFY(editor->isMarkdownPreviewVisible());
        auto* browser = editor->findChild<QTextBrowser*>("markdownPreviewBrowser");
        QVERIFY(browser);
        QTRY_VERIFY(browser->toPlainText().contains("Desktop preview"));
        QAction* preview = nullptr;
        for (auto* action : window.findChildren<QAction*>())
            if (action->text() == "Markdown Preview")
                preview = action;
        QVERIFY(preview);
        QVERIFY(preview->isChecked());
        auto* source = editor->findChild<QToolButton*>("markdownMode-source");
        QVERIFY(source);
        source->click();
        QVERIFY(!editor->isMarkdownPreviewVisible());
        QVERIFY(!preview->isChecked());
        preview->trigger();
        QVERIFY(editor->isMarkdownPreviewVisible());
        const auto capture = qEnvironmentVariable("KETPLUS_DOCUMENT_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(editor->grab().save(capture));
    }
    void exportedTerminalIncludesNewControls() {
        ketplus::TerminalPanel terminal;
        QStringList controls;
        for (auto* button : terminal.findChildren<QToolButton*>())
            controls.append(button->accessibleName());
        QVERIFY(controls.contains("Find in terminal"));
        QVERIFY(controls.contains("Clear terminal scrollback"));
        QVERIFY(controls.contains("Restart terminal session"));
    }
};
QTEST_MAIN(DesktopDocumentModesTest)
#include "DesktopDocumentModesTest.moc"
