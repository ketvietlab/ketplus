#include "agent/ChatMarkdown.h"
#include "agent/AgentWorkspaceView.h"
#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QDesktopServices>
#include <QFile>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QtTest>

using ketplus::chat_markdown::escapeRawHtml;

namespace {

QString rendered(const QString& markdown) {
    QTextDocument document;
    document.setMarkdown(markdown);
    return document.toPlainText();
}

QQuickItem* findItem(QQuickItem* parent, const QString& name) {
    if (parent->objectName() == name)
        return parent;
    for (auto* child : parent->childItems())
        if (auto* item = findItem(child, name))
            return item;
    return nullptr;
}

// An answer shaped like the one that lost most of its text: a placeholder in prose, followed by
// more prose, code spans and a code block that contain HTML of their own.
const QString placeholderAnswer =
    QStringLiteral("- Titled \"<title> (fork)\", then continues on its own.\n"
                   "- See `docs/decisions/0016.md` and `<title>` for details.\n"
                   "\n"
                   "```html\n"
                   "<div>kept</div>\n"
                   "```\n"
                   "\n"
                   "Final words.");

} // namespace

class ChatMarkdownTest final : public QObject {
    Q_OBJECT
    QList<QUrl> openedUrls_;
  public slots:
    void captureUrl(const QUrl& url) { openedUrls_.append(url); }
  private slots:
    void fileLinksOfferRevealOnRightClick() {
        QTemporaryDir directory;
        QFile file(directory.filePath("10k rows.csv"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        ketplus::AgentWorkspaceView view;
        view.resize(900, 700);
        ketplus::ThemeManager theme(*qApp);
        view.applyTheme(theme.palette());
        view.selectWorktree(directory.path());
        view.show();
        const auto href = QUrl::fromLocalFile(file.fileName()).toString();
        view.resetMessages(
            {QVariantMap{{"kind", "assistant"}, {"body", "Download [CSV](<" + href + ">) here."}}});
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = findItem(view.quickView()->rootObject(), "messageBody")) != nullptr);
        auto* quickDocument = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
        QVERIFY(quickDocument);
        auto* document = quickDocument->textDocument();
        const int position = document->toPlainText().indexOf("CSV");
        QVERIFY(position >= 0);
        QTest::qWait(100);
        const auto block = document->findBlock(position);
        const auto line = block.layout()->lineForTextPosition(position - block.position());
        const auto rect = document->documentLayout()->blockBoundingRect(block);
        const QPointF point(rect.left() + line.cursorToX(position - block.position() + 1),
                            rect.top() + line.y() + line.height() / 2);
        view.quickView()->requestActivate();
        QTest::mouseClick(view.quickView(), Qt::RightButton, Qt::NoModifier,
                          body->mapToScene(point).toPoint());
        auto* menu = view.quickView()->rootObject()->findChild<QObject*>("transcriptContextMenu");
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("visible").toBool());
        QCOMPARE(menu->property("fileTarget").toString(), QFileInfo(file).canonicalFilePath());
        auto* reveal = menu->findChild<QObject*>("revealTranscriptFileMenuItem");
        QVERIFY(reveal);
        QVERIFY(reveal->property("visible").toBool());
        QVERIFY(view.revealableFilePath("https://example.com/file.csv").isEmpty());
        QVERIFY(view.revealableFilePath(directory.filePath("missing.csv")).isEmpty());
        QCOMPARE(view.revealableFilePath(file.fileName() + ":12:3"),
                 QFileInfo(file).canonicalFilePath());
        view.setExecutionBranch("feature", directory.path(), 2);
        QVERIFY(view.revealableFilePath("10k rows.csv").isEmpty());
        QCOMPARE(view.revealableFilePath(href), QFileInfo(file).canonicalFilePath());
    }
    void cleanup() { QDesktopServices::unsetUrlHandler("http"); }
    void preparesBareUrls_data() {
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("expected");
        const QString url = "http://127.0.0.1:60550/?flow=ketphone-device";
        const QString linked = '<' + url + '>';
        QTest::newRow("localhost query") << "Flow: " + url << "Flow: " + linked;
        QTest::newRow("https localhost") << QString("https://localhost:8080/a_b_c?x=1&y=2#part")
                                         << QString("<https://localhost:8080/a_b_c?x=1&y=2#part>");
        QTest::newRow("ipv6") << QString("http://[::1]:8080/a") << QString("<http://[::1]:8080/a>");
        QTest::newRow("punctuation") << "(" + url + "), next." << "(" + linked + "), next.";
        QTest::newRow("balanced parentheses") << QString("https://example.com/wiki/A_(B).")
                                              << QString("<https://example.com/wiki/A_(B)>.");
        QTest::newRow("underscore path") << QString("https://example.com/_a_/b?x=_c_")
                                         << QString("<https://example.com/_a_/b?x=_c_>");
        QTest::newRow("uppercase")
            << QString("HTTPS://example.com/a") << QString("<HTTPS://example.com/a>");
        QTest::newRow("list and table")
            << "- " + url + "\n\n| " + url + " |" << "- " + linked + "\n\n| " + linked + " |";
        QTest::newRow("inline code") << '`' + url + '`' << '`' + url + '`';
        QTest::newRow("fenced code") << "```text\n" + url + "\n```" << "```text\n" + url + "\n```";
        QTest::newRow("open fence") << "~~~\n" + url << "~~~\n" + url;
        QTest::newRow("indented code") << "Text\n\n    " + url << "Text\n\n    " + url;
        QTest::newRow("explicit link") << "[Open](" + url + ")" << "[Open](" + url + ")";
        // #160: a remote image is a link to it; a local one stays an image.
        QTest::newRow("remote image") << "![chart](" + url + ")" << "[chart](" + url + ")";
        QTest::newRow("remote image in angle brackets")
            << "See ![chart](<" + url + ">)." << "See [chart](<" + url + ">).";
        QTest::newRow("local image")
            << QString("![shot](file:///tmp/a.png) http://example.com/x")
            << QString("![shot](file:///tmp/a.png) <http://example.com/x>");
        QTest::newRow("relative image") << QString("![shot](docs/a.png) http://example.com/x")
                                        << QString("![shot](docs/a.png) <http://example.com/x>");
        // An escaped raw tag right after a URL ends the URL, so the autolink closes.
        QTest::newRow("url before escaped tag")
            << QString("<style>@import url(http://example.com/t.css);</style> after")
            << QString("\\<style>@import url(<http://example.com/t.css>);\\</style> after");
        QTest::newRow("url label")
            << '[' + url + "](https://example.com)" << '[' + url + "](https://example.com)";
        QTest::newRow("reference label") << '[' + url + "][ref]" << '[' + url + "][ref]";
        QTest::newRow("reference definition") << "[ref]: " + url << "[ref]: " + url;
        QTest::newRow("autolink") << linked << linked;
        QTest::newRow("bold") << "**" + url + "**" << "**" + linked + "**";
        QTest::newRow("italic") << '_' + url + '_' << '_' + linked + '_';
        QTest::newRow("bold sentence") << "**" + url + "**." << "**" + linked + "**.";
        QTest::newRow("shortcut reference")
            << '[' + url + "]\n\n[" + url + "]: https://example.com"
            << '[' + url + "]\n\n[" + url + "]: https://example.com";
        QTest::newRow("quote code") << ">     " + url << ">     " + url;
        QTest::newRow("quote fence") << "> ```\n> " + url + "\n> ```\n\n" + url
                                     << "> ```\n> " + url + "\n> ```\n\n" + linked;
        QTest::newRow("list code") << "- item\n\n      " + url << "- item\n\n      " + url;
        QTest::newRow("list prose") << "- item\n\n  " + url << "- item\n\n  " + linked;
        QTest::newRow("unsupported scheme") << QString("javascript:alert(1) ftp://example.com/a")
                                            << QString("javascript:alert(1) ftp://example.com/a");
        QTest::newRow("incomplete") << QString("http:// https://") << QString("http:// https://");
        QTest::newRow("raw html") << "<title> " + url << "\\<title> " + linked;
    }

    void preparesBareUrls() {
        QFETCH(QString, source);
        QFETCH(QString, expected);
        const auto prepared = ketplus::chat_markdown::prepareForDisplay(source);
        QCOMPARE(prepared, expected);
        QCOMPARE(ketplus::chat_markdown::prepareForDisplay(prepared), prepared);
    }

    void bareUrlInTranscript_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void bareUrlInTranscript() {
        QFETCH(bool, dark);
        const QString url =
            QStringLiteral("http://127.0.0.1:60551/__atlas/crm/"
                           "screen?screen=partner-detail&id=PARTNER-70&ketphone=away");
        ketplus::AgentWorkspaceView view;
        view.resize(900, 700);
        ketplus::ThemeManager theme(*qApp);
        theme.previewMode(dark ? ketplus::ThemeManager::Mode::Dark
                               : ketplus::ThemeManager::Mode::Light);
        view.applyTheme(theme.palette());
        view.show();
        view.raise();
        view.activateWindow();
        view.resetMessages({QVariantMap{{"kind", "assistant"}, {"body", "Open: " + url}}});
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = findItem(view.quickView()->rootObject(), "messageBody")) != nullptr);
        auto* quickDocument = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
        QVERIFY(quickDocument);
        auto* document = quickDocument->textDocument();
        QTRY_VERIFY(document->toPlainText().contains(url));
        const int position = document->toPlainText().indexOf(url);
        QCOMPARE(view.messageLinkAt(quickDocument, position + 1), url);
        QCOMPARE(view.messageLinkAt(quickDocument, position + url.size() - 2), url);
        view.quickView()->requestActivate();
        QTest::qWait(100);
        const QTextBlock block = document->findBlock(position);
        const auto line = block.layout()->lineForTextPosition(position - block.position());
        QVERIFY(line.isValid());
        const auto rect = document->documentLayout()->blockBoundingRect(block);
        const QPointF point(rect.left() + line.cursorToX(position - block.position() + 2),
                            rect.top() + line.y() + line.height() / 2);
        auto* hover = body->findChild<QObject*>("messageBodyLinkHover");
        QVERIFY(hover);
        view.quickView()->requestActivate();
        QTest::mouseMove(view.quickView(), QPoint(0, 0));
        QTest::mouseMove(view.quickView(), body->mapToScene(point).toPoint());
        QTRY_COMPARE(hover->property("cursorShape").toInt(), int(Qt::PointingHandCursor));
        openedUrls_.clear();
        QDesktopServices::setUrlHandler("http", this, "captureUrl");
        QTest::mouseClick(view.quickView(), Qt::LeftButton, Qt::NoModifier,
                          body->mapToScene(point).toPoint());
        QTRY_COMPARE(openedUrls_.size(), 1);
        QCOMPARE(openedUrls_.front(), QUrl(url));
        QVariant copied;
        QVERIFY(QMetaObject::invokeMethod(view.quickView()->rootObject(), "conversationCopyText",
                                          Q_RETURN_ARG(QVariant, copied)));
        QVERIFY(copied.toString().contains("Open: " + url));
        QVERIFY(!copied.toString().contains('<' + url + '>'));
    }

    void escapesHtmlOutsideCode_data() {
        QTest::addColumn<QString>("markdown");
        QTest::addColumn<QString>("expected");
        QTest::newRow("placeholder") << QStringLiteral("a \"<title> (fork)\" b")
                                     << QStringLiteral("a \"\\<title> (fork)\" b");
        QTest::newRow("closing, comment, instruction")
            << QStringLiteral("a </div> b <!-- c --> d <?x?>")
            << QStringLiteral("a \\</div> b \\<!-- c --> d \\<?x?>");
        QTest::newRow("heading and table") << QStringLiteral("## <Title>\n\n| a | <b> |")
                                           << QStringLiteral("## \\<Title>\n\n| a | \\<b> |");
        QTest::newRow("code span") << QStringLiteral("keep `<title>` here <b>")
                                   << QStringLiteral("keep `<title>` here \\<b>");
        QTest::newRow("double backtick span")
            << QStringLiteral("``a ` <b>`` <i>") << QStringLiteral("``a ` <b>`` \\<i>");
        QTest::newRow("triple backtick span")
            << QStringLiteral("```a``` <b>") << QStringLiteral("```a``` \\<b>");
        QTest::newRow("span across lines")
            << QStringLiteral("`a\n<b>` c") << QStringLiteral("`a\n<b>` c");
        QTest::newRow("unclosed backtick")
            << QStringLiteral("`x <b>") << QStringLiteral("`x \\<b>");
        QTest::newRow("unclosed backtick stops at paragraph")
            << QStringLiteral("`a\n\n<b>` c") << QStringLiteral("`a\n\n\\<b>` c");
        QTest::newRow("fenced block") << QStringLiteral("```html\n<div>\n```\n<p>")
                                      << QStringLiteral("```html\n<div>\n```\n\\<p>");
        QTest::newRow("tilde fence in list") << QStringLiteral("- item\n\n  ~~~\n  <div>\n  ~~~")
                                             << QStringLiteral("- item\n\n  ~~~\n  <div>\n  ~~~");
        QTest::newRow("unclosed fence")
            << QStringLiteral("```\n<div>") << QStringLiteral("```\n<div>");
        QTest::newRow("indented code block")
            << QStringLiteral("Text\n\n    <div>\n    </div>\n\nAfter <b>")
            << QStringLiteral("Text\n\n    <div>\n    </div>\n\nAfter \\<b>");
        QTest::newRow("list continuation") << QStringLiteral("- item\n\n    more <b>")
                                           << QStringLiteral("- item\n\n    more \\<b>");
        QTest::newRow("autolinks")
            << QStringLiteral("<https://example.com/a?b=1> <me@example.com>")
            << QStringLiteral("<https://example.com/a?b=1> <me@example.com>");
        QTest::newRow("already escaped")
            << QStringLiteral("\\<b> stays") << QStringLiteral("\\<b> stays");
        QTest::newRow("comparisons")
            << QStringLiteral("a < b, a<3, x <= y") << QStringLiteral("a < b, a<3, x <= y");
        QTest::newRow("link destination")
            << QStringLiteral("[doc](<a b.md>)") << QStringLiteral("[doc](<a b.md>)");
        QTest::newRow("trailing newline") << QStringLiteral("<b>\n") << QStringLiteral("\\<b>\n");
    }

    void escapesHtmlOutsideCode() {
        QFETCH(QString, markdown);
        QFETCH(QString, expected);
        QCOMPARE(escapeRawHtml(markdown), expected);
    }

    void keepsEveryWordAfterAnUnclosedTag() {
        const QString text = rendered(escapeRawHtml(placeholderAnswer));
        QVERIFY(
            text.contains(QStringLiteral("Titled \"<title> (fork)\", then continues on its own.")));
        QVERIFY(
            text.contains(QStringLiteral("See docs/decisions/0016.md and <title> for details.")));
        QVERIFY(text.contains(QStringLiteral("<div>kept</div>")));
        QVERIFY(text.contains(QStringLiteral("Final words.")));
        QVERIFY(!text.contains(u'\\'));
    }

    void transcriptShowsTheWholeAnswer() {
        ketplus::AgentWorkspaceView view;
        QCOMPARE(view.renderChatMarkdown(placeholderAnswer), escapeRawHtml(placeholderAnswer));
        // The Mermaid path rewrites the text line by line and must keep the escaping.
        QCOMPARE(view.renderChatMarkdown(QStringLiteral("A mermaid <b> diagram")),
                 QStringLiteral("A mermaid \\<b> diagram"));

        view.resize(900, 700);
        ketplus::ThemeManager theme(*qApp);
        view.applyTheme(theme.palette());
        view.show();
        view.resetMessages({QVariantMap{{"kind", "assistant"},
                                        {"body", placeholderAnswer},
                                        {"meta", "Assistant"},
                                        {"filePath", ""}}});
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = findItem(view.quickView()->rootObject(), "messageBody")) != nullptr);
        auto* document = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
        QVERIFY(document);
        QTRY_VERIFY(document->textDocument()->toPlainText().contains("Final words."));
        QVERIFY(document->textDocument()->toPlainText().contains("then continues on its own."));
        // Copying keeps the answer's own Markdown, not the escaped rendering.
        QVariant copied;
        QVERIFY(QMetaObject::invokeMethod(view.quickView()->rootObject(), "conversationCopyText",
                                          Q_RETURN_ARG(QVariant, copied)));
        QVERIFY(copied.toString().contains(placeholderAnswer));
    }
};

QTEST_MAIN(ChatMarkdownTest)
#include "ChatMarkdownTest.moc"
