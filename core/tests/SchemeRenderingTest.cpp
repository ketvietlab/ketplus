#include "preview/MarkdownPreviewPane.h"
#include "preview/MermaidRenderer.h"
#include "ui/Theme.h"
#include "ui/theme/SchemeDocumentSyntax.h"
#include <QScopeGuard>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSyntaxHighlighter>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextLayout>
#include <QtTest>
using namespace ketplus;
class SchemeRenderingTest : public QObject {
    Q_OBJECT
  private slots:
    void emptyUntitledSourceReplacesThePlaceholder() {
        MarkdownPreviewPane pane;
        const auto palette = colorSchemePalette(builtinColorScheme(SchemeKind::Light));
        pane.showEmpty(palette);
        auto* browser = pane.findChild<QTextBrowser*>();
        QVERIFY(browser);
        QVERIFY(!browser->document()->isEmpty());
        pane.setSource({}, {}, palette);
        QTRY_VERIFY(browser->document()->isEmpty());
    }
    void fencedSyntaxRefreshesWithinTheSameDocument() {
        QTextDocument document;
        document.setMarkdown(QStringLiteral("```cpp\nint n = 7; // note\n```"));
        const auto before = document.toPlainText();
        auto scheme = resolveColorScheme(builtinColorScheme(SchemeKind::Light));
        auto& style = scheme.syntax[QStringLiteral("keyword")];
        style.fg = QColor(11, 22, 33);
        style.bold = style.italic = style.underline = true;
        applySchemeDocumentSyntax(&document, scheme);
        auto* highlighter = document.findChild<QSyntaxHighlighter*>();
        QVERIFY(highlighter);
        const auto keyword = [&] {
            for (const auto& format : document.begin().layout()->formats())
                if (format.start == 0 && format.length == 3)
                    return format.format;
            return QTextCharFormat{};
        };
        QCOMPARE(keyword().foreground().color(), QColor(11, 22, 33));
        QCOMPARE(keyword().fontWeight(), QFont::Bold);
        QVERIFY(keyword().fontItalic());
        QVERIFY(keyword().fontUnderline());
        style.fg = QColor(44, 55, 66);
        style.bold = style.italic = style.underline = false;
        applySchemeDocumentSyntax(&document, scheme);
        QCOMPARE(document.findChild<QSyntaxHighlighter*>(), highlighter);
        QCOMPARE(keyword().foreground().color(), QColor(44, 55, 66));
        QCOMPARE(keyword().fontWeight(), QFont::Normal);
        QVERIFY(!keyword().fontItalic());
        QVERIFY(!keyword().fontUnderline());
        QCOMPARE(document.toPlainText(), before);
        QSignalSpy changes(&document, &QTextDocument::contentsChanged);
        applySchemeDocumentSyntax(&document, scheme);
        QCOMPARE(changes.size(), 0);
    }
    void mermaidCacheTracksTheColorConfiguration() {
        const auto previous = qgetenv("KETPLUS_MMDC");
        const bool wasSet = qEnvironmentVariableIsSet("KETPLUS_MMDC");
        const auto restore = qScopeGuard([&] {
            if (wasSet)
                qputenv("KETPLUS_MMDC", previous);
            else
                qunsetenv("KETPLUS_MMDC");
        });
        qputenv("KETPLUS_MMDC", "/missing/mmdc");
        QTemporaryDir directory;
        ThemeManager manager(*qApp, nullptr, directory.path());
        manager.setMode(ThemeManager::Mode::Light);
        MermaidRenderer renderer;
        const auto first = renderer.request(QStringLiteral("graph LR; A-->B"), false).cacheKey;
        auto custom = builtinColorScheme(SchemeKind::Light);
        custom.id = QStringLiteral("custom");
        custom.ui[QStringLiteral("chrome.panelBackground")] = QColor(11, 22, 33);
        manager.previewColorScheme(custom);
        QTRY_COMPARE(manager.colorScheme().id, QStringLiteral("custom"));
        const auto second = renderer.request(QStringLiteral("graph LR; A-->B"), false).cacheKey;
        QVERIFY(first != second);
        manager.cancelPreview();
        QCOMPARE(renderer.request(QStringLiteral("graph LR; A-->B"), false).cacheKey, first);
    }
    void previewThemePreservesDocumentSelectionAndScroll() {
        QTemporaryDir directory;
        ThemeManager manager(*qApp, nullptr, directory.path());
        manager.setMode(ThemeManager::Mode::Light);
        MarkdownPreviewPane pane;
        pane.resize(420, 320);
        pane.applyColorScheme(manager.colorScheme());
        pane.setSource(QStringLiteral("# Heading\n\nA paragraph.\n\n").repeated(50),
                       QStringLiteral("test.md"), manager.palette());
        pane.show();
        auto* browser = pane.findChild<QTextBrowser*>();
        QVERIFY(browser);
        QTRY_VERIFY(browser->document()->toPlainText().contains(QStringLiteral("Heading")));
        QTRY_VERIFY(browser->verticalScrollBar()->maximum() > 100);
        QCoreApplication::processEvents();
        auto* document = browser->document();
        const auto text = document->toPlainText();
        QTextCursor cursor(document);
        cursor.setPosition(2);
        cursor.setPosition(8, QTextCursor::KeepAnchor);
        browser->setTextCursor(cursor);
        browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum() / 2);
        const auto scroll = browser->verticalScrollBar()->value();
        auto scheme = manager.colorScheme();
        scheme.ui[QStringLiteral("chrome.textMain")] = QColor(22, 33, 44);
        pane.applyColorScheme(scheme);
        QTest::qWait(80);
        QCOMPARE(browser->document(), document);
        QCOMPARE(document->toPlainText(), text);
        QCOMPARE(browser->textCursor().selectionStart(), 2);
        QCOMPARE(browser->textCursor().selectionEnd(), 8);
        QVERIFY(qAbs(browser->verticalScrollBar()->value() - scroll) <= 1);
    }
};
QTEST_MAIN(SchemeRenderingTest)
#include "SchemeRenderingTest.moc"
