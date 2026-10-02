#include "editor/EditorWidget.h"
#include "git/GitDiffView.h"
#include "preview/MarkdownModeController.h"
#include "preview/MarkdownPreviewPane.h"
#include <QElapsedTimer>
#include <QFile>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextFrame>
#include <QToolButton>
#include <QtTest>

class MarkdownModesTest final : public QObject {
    Q_OBJECT
  private:
    static void write(const QString& path, const QByteArray& text) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(text), text.size());
    }
    static ketplus::ThemePalette palette() {
        ketplus::ThemePalette result;
        result.panelBackground = result.surfaceRaised = "#ffffff";
        result.textMain = "#222222";
        result.border = "#dddddd";
        result.accent = "#5167c4";
        result.accentSubtle = "#eef0fb";
        return result;
    }
  private slots:
    void editorDefaultsToPreviewAndReusesRenderedDocument() {
        QTemporaryDir directory;
        const auto path = directory.filePath("notes.md");
        const auto content =
            QByteArray("# Heading\n\nA **paragraph** with `code`.\n\n").repeated(200);
        write(path, content);
        ketplus::EditorWidget editor;
        editor.resize(1100, 700);
        editor.applyTheme(palette());
        editor.show();
        QVERIFY(editor.loadFile(path).ok);
        QVERIFY(editor.isMarkdownPreviewVisible());
        auto* browser = editor.findChild<QTextBrowser*>("markdownPreviewBrowser");
        QVERIFY(browser);
        QTRY_VERIFY(browser->toPlainText().contains("paragraph"));
        QTest::qWait(50);
        auto* bar = editor.findChild<QWidget*>("markdownModeBar");
        QVERIFY(bar);
        QVERIFY(qAbs(bar->geometry().center().x() - editor.rect().center().x()) <= 1);
        QCOMPARE(editor.height() - bar->geometry().bottom() - 1, 100);
        QSignalSpy changes(browser->document(), &QTextDocument::contentsChanged);
        QElapsedTimer elapsed;
        elapsed.start();
        for (int i = 0; i < 30; ++i) {
            editor.setMarkdownPreviewVisible(false);
            editor.setMarkdownPreviewVisible(true);
        }
        qInfo() << "30 cached source/preview switches ms:" << elapsed.elapsed();
        QTest::qWait(250);
        QCOMPARE(changes.size(), 0);
        QCOMPARE(editor.text(), content);
        QVERIFY(!editor.document().isModified());
        editor.setMarkdownPreviewVisible(false);
        editor.replaceAllText("# Edited live buffer\n");
        editor.setMarkdownPreviewVisible(true);
        QTRY_VERIFY(browser->toPlainText().contains("Edited live buffer"));
        QVERIFY(editor.document().isModified());
        editor.setMarkdownPreviewVisible(false);
        editor.undoEdit();
        QCOMPARE(editor.text(), content);
        editor.setMarkdownPreviewVisible(true);
        QTRY_VERIFY(browser->toPlainText().contains("paragraph"));
    }
    void previewCapsWidthWithoutGiantVerticalMargins() {
        ketplus::MarkdownPreviewPane preview;
        preview.resize(1600, 700);
        preview.show();
        preview.setSource(QString("A long paragraph. ").repeated(80), "/tmp/notes.md", palette());
        auto* browser = preview.findChild<QTextBrowser*>("markdownPreviewBrowser");
        QTRY_VERIFY(!browser->toPlainText().isEmpty());
        const auto format = browser->document()->rootFrame()->frameFormat();
        const auto width =
            browser->viewport()->width() - format.leftMargin() - format.rightMargin();
        QVERIFY2(width <= 961, qPrintable(QString::number(width)));
        QVERIFY(width > 900);
        QVERIFY(format.topMargin() <= 32);
        QVERIFY(format.bottomMargin() <= 32);
    }
    void sourceControlDefaultsToDiffAndPreviewsTheIndex() {
        QTemporaryDir directory;
        const auto git = [&](QStringList args) {
            QProcess process;
            process.setWorkingDirectory(directory.path());
            process.start("git", args);
            QVERIFY(process.waitForFinished(10000));
            QCOMPARE(process.exitCode(), 0);
        };
        git({"init", "-q"});
        const auto path = directory.filePath("notes.md");
        write(path, "# Staged snapshot\n");
        git({"add", "--", "notes.md"});
        write(path, "# Unstaged working copy\n");
        ketplus::GitDiffView diff;
        diff.resize(1100, 700);
        diff.show();
        diff.setDiff(path, ketplus::GitDiffMode::Staged,
                     "--- /dev/null\n+++ b/notes.md\n@@ -0,0 +1 @@\n+# Staged snapshot\n",
                     palette());
        auto* modes = diff.findChild<ketplus::MarkdownModeController*>();
        QVERIFY(modes);
        QCOMPARE(modes->mode(), "diff");
        modes->setMode("preview");
        auto* browser = diff.findChild<QTextBrowser*>("markdownPreviewBrowser");
        QTRY_VERIFY(browser->toPlainText().contains("Staged snapshot"));
        QVERIFY(!browser->toPlainText().contains("Unstaged working copy"));
        modes->setMode("source");
        auto* source = diff.findChild<QPlainTextEdit*>("markdownDiffSource");
        QVERIFY(source->isReadOnly());
        QCOMPARE(source->toPlainText(), "# Staged snapshot\n");
        modes->setMode("diff");
        diff.setDiff(path, ketplus::GitDiffMode::Unstaged, "", palette());
        modes->setMode("preview");
        QTRY_VERIFY(browser->toPlainText().contains("Unstaged working copy"));
    }
};
QTEST_MAIN(MarkdownModesTest)
#include "MarkdownModesTest.moc"
