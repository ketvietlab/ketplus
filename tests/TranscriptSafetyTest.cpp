#include "KetAppearance.h"
#include "agent/AgentWorkspaceView.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QFile>
#include <QImage>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QtTest>

// #160, carried from #79 and #80: what a model writes stays text on this Mac. Rich text in an
// answer cannot reach the network, and the layout trace records geometry, never words.
class TranscriptSafetyTest final : public QObject {
    Q_OBJECT

    QTemporaryDir directory_;
    QString tracePath_;

    static QQuickItem* find(QQuickItem* item, const QString& name) {
        if (!item)
            return nullptr;
        if (item->objectName() == name && item->isVisible())
            return item;
        for (auto* child : item->childItems())
            if (auto* match = find(child, name))
                return match;
        return nullptr;
    }

    static QColor linkColour(const QTextDocument* document) {
        for (auto block = document->begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it)
                if (it.fragment().charFormat().isAnchor())
                    return it.fragment().charFormat().foreground().color();
        return {};
    }

  private slots:
    void initTestCase() {
        QVERIFY(directory_.isValid());
        // The trace opens once per process, on first use, so it is named before any view exists.
        tracePath_ = directory_.filePath("layout-trace.jsonl");
        qputenv("KETPLUS_CHAT_LAYOUT_TRACE", tracePath_.toUtf8());
    }

    void modelOutputCannotReachTheNetwork() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString origin = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        const QString answer =
            QStringLiteral("Here is the result.\n\n![chart](%1/chart.png)\n\n"
                           "<img src=\"%1/raw.png\"> <script>alert(1)</script>\n\n"
                           "<style>@import url(%1/theme.css);</style> "
                           "<link rel=\"stylesheet\" href=\"%1/link.css\">\n\n"
                           "Done, see [the docs](%1/docs). ![logo][brand]\n\n"
                           "[brand]: %1/logo.png")
                .arg(origin);
        ketplus::AgentWorkspaceView view;
        view.applyTheme(ketplus::currentThemePalette());
        view.resize(900, 640);
        view.show();
        view.resetMessages(
            {QVariantMap{{"kind", "assistant"}, {"meta", "Agent"}, {"body", answer}}});
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = find(view.quickView()->rootObject(), "messageBody")) != nullptr);
        auto* quickDocument = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
        QVERIFY(quickDocument && quickDocument->textDocument());
        const QTextDocument* document = quickDocument->textDocument();
        QTRY_VERIFY2(document->toPlainText().contains("Done, see the docs."),
                     qPrintable(document->toPlainText()));

        // Raw HTML shows as the characters the model wrote; none of it becomes markup.
        const QString shown = document->toPlainText();
        QVERIFY2(shown.contains("<script>alert(1)</script>"), qPrintable(shown));
        QVERIFY(shown.contains("<img src="));
        QVERIFY(shown.contains("<style>"));
        // The inline image reads as a link to it, so the reader still sees where it points.
        QVERIFY(shown.contains("Here is the result.\nchart\n"));
        // A reference-style image stays an image, but its engine refuses the remote load.
        QVERIFY(document->toPlainText().contains(QChar::ObjectReplacementCharacter));

        // Nothing is fetched while the answer is laid out, painted or resized.
        for (const int width : {900, 520, 1200}) {
            view.resize(width, 640);
            QTest::qWait(150);
        }
        QVERIFY(!server.waitForNewConnection(300));
        QVERIFY(!server.hasPendingConnections());
    }

    // #79: rendered answers are re-styled, not served from a stale render, when the theme changes.
    void themeChangeRestylesRenderedAnswers() {
        auto& appearance = ket::Appearance::instance();
        appearance.setMode(ket::Appearance::Mode::Light);
        ketplus::AgentWorkspaceView view;
        view.applyTheme(ketplus::currentThemePalette());
        view.resize(900, 640);
        view.show();
        view.resetMessages({QVariantMap{{"kind", "assistant"},
                                        {"meta", "Agent"},
                                        {"body", "See [the guide](docs/guide.md) first."}}});
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = find(view.quickView()->rootObject(), "messageBody")) != nullptr);
        const auto document = [&] {
            auto* item = find(view.quickView()->rootObject(), "messageBody");
            auto* quick =
                item ? qvariant_cast<QQuickTextDocument*>(item->property("textDocument")) : nullptr;
            return quick ? quick->textDocument() : nullptr;
        };
        QTRY_VERIFY(document() && linkColour(document()).isValid());
        const QColor light = linkColour(document());
        // The app re-themes the view from this signal; nothing else is called.
        appearance.setMode(ket::Appearance::Mode::Dark);
        QTRY_VERIFY(document() && linkColour(document()).isValid() &&
                    linkColour(document()) != light);
        appearance.setMode(ket::Appearance::Mode::Light);
        QTRY_COMPARE(linkColour(document()), light);
    }

    // #79: a large local image in an answer is decoded at the size it is drawn, not its own.
    void largeInlineImageDecodesAtThumbnailSize() {
        const QString path = directory_.filePath("large.png");
        QImage large(4000, 3000, QImage::Format_RGB32);
        large.fill(Qt::darkCyan);
        QVERIFY(large.save(path));
        ketplus::AgentWorkspaceView view;
        view.applyTheme(ketplus::currentThemePalette());
        view.resize(900, 640);
        view.show();
        const QUrl url = QUrl::fromLocalFile(path);
        view.resetMessages(
            {QVariantMap{{"kind", "assistant"},
                         {"meta", "Agent"},
                         {"body", "The capture:\n\n![capture](" + url.toString() + ")"}}});
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = find(view.quickView()->rootObject(), "messageBody")) != nullptr);
        auto* document =
            qvariant_cast<QQuickTextDocument*>(body->property("textDocument"))->textDocument();
        QImage decoded;
        QTRY_VERIFY(!(decoded = qvariant_cast<QImage>(
                          document->resource(QTextDocument::ImageResource, url)))
                         .isNull());
        const qreal density = qMax<qreal>(1.0, qGuiApp->devicePixelRatio());
        QVERIFY2(qMax(decoded.width(), decoded.height()) <= qCeil(88 * density),
                 qPrintable(QStringLiteral("%1x%2").arg(decoded.width()).arg(decoded.height())));
        QCOMPARE(decoded.width() * 3, decoded.height() * 4);
    }

    void traceRecordsGeometryNotWords() {
        const QString secret = QStringLiteral("Zephyr-quartz-7731");
        ketplus::AgentWorkspaceView view;
        view.applyTheme(ketplus::currentThemePalette());
        view.resize(900, 640);
        view.show();
        QVariantList rows;
        for (int index = 0; index < 40; ++index)
            rows.append(QVariantMap{{"kind", index % 2 ? "assistant" : "user"},
                                    {"meta", secret},
                                    {"title", secret},
                                    {"body", secret + QStringLiteral(" paragraph %1 ").arg(index) +
                                                 QString(400, u'x')}});
        view.resetMessages(rows, "trace-scope"); // The scope is a key; the rows carry the words.
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = find(view.quickView()->rootObject(), "messageBody")) != nullptr);
        view.resize(700, 500);
        QTest::qWait(300);

        QFile trace(tracePath_);
        QVERIFY(trace.open(QIODevice::ReadOnly));
        const QByteArray recorded = trace.readAll();
        QVERIFY2(!recorded.isEmpty(), "the trace should have recorded this session");
        const auto at = recorded.indexOf(secret.toUtf8());
        QVERIFY2(at < 0, recorded.mid(qMax(0, int(at) - 300), 400).constData());
        QVERIFY(!recorded.contains("paragraph"));
    }
};

QTEST_MAIN(TranscriptSafetyTest)
#include "TranscriptSafetyTest.moc"
