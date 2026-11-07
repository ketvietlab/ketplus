#include "app/MainWindow.h"
#include "editor/EditorWidget.h"
#include "terminal/TerminalPanel.h"
#include "ui/Theme.h"
#include <QAbstractButton>
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
        window.findChild<QAbstractButton*>("explorerActivityButton")->click();
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
    void savingAHibernatedTabKeepsTheFile() {
        QTemporaryDir directory;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        qputenv("KETPLUS_DAEMON_SOCKET", directory.filePath("isolated.sock").toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", directory.filePath("isolated-token").toUtf8());
        qputenv("KETPLUS_CONVERSATION_WORKTREES", directory.filePath("worktrees.sqlite").toUtf8());
        const auto path = directory.filePath("notes.txt");
        const QByteArray original("keep these bytes\n");
        const auto write = [&path](const QByteArray& bytes) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        QVERIFY(write(original));
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(directory.path());
        window.show();
        window.openFile(path);
        ketplus::EditorWidget* editor = nullptr;
        for (auto* candidate : window.findChildren<ketplus::EditorWidget*>())
            if (QFileInfo(candidate->document().filePath()).canonicalFilePath() ==
                QFileInfo(path).canonicalFilePath())
                editor = candidate;
        QVERIFY(editor);
        // Unloaded, briefly missing (a failed restore), then back with the same bytes.
        QVERIFY(editor->hibernate());
        QVERIFY(QFile::remove(path));
        QVERIFY(!editor->restoreFromDisk().ok);
        QVERIFY(write(original));
        QAction* save = nullptr;
        for (auto* action : window.findChildren<QAction*>())
            if (action->text() == "&Save")
                save = action;
        QVERIFY(save);
        save->trigger();
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
        QVERIFY(!editor->isHibernated());
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
