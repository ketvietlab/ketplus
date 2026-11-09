#include "agent/AgentWorkspaceView.h"
#include "preview/MermaidRenderer.h"
#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QJSValue>
#include <QProcess>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QUuid>
#include <QtTest>

class ChatMermaidTest final : public QObject {
    Q_OBJECT
    struct Environment {
        bool set = qEnvironmentVariableIsSet("KETPLUS_MMDC");
        QByteArray value = qgetenv("KETPLUS_MMDC");
        QByteArray path = qgetenv("PATH");
        ~Environment() {
            if (set)
                qputenv("KETPLUS_MMDC", value);
            else
                qunsetenv("KETPLUS_MMDC");
            qputenv("PATH", path);
        }
    };
    static QQuickItem* findItem(QQuickItem* parent, const QString& name) {
        if (parent->objectName() == name)
            return parent;
        for (auto* child : parent->childItems())
            if (auto* item = findItem(child, name))
                return item;
        return nullptr;
    }
  private slots:
    void preservesIncompleteAndNestedFences() {
        Environment environment;
        qputenv("KETPLUS_MMDC", "/missing/mmdc");
        ketplus::AgentWorkspaceView view;
        const QString incomplete = "Before\n\n```mermaid\nflowchart LR\nA --> B";
        // Mermaid is source until the reader asks for the diagram: nothing is rendered, framed or
        // reserved, and a fence still streaming is only ever source.
        QCOMPARE(view.renderChatMarkdown(incomplete, "m"), incomplete);
        QCOMPARE(view.renderChatMarkdown(incomplete + "\n```\n\nAfter", "m"),
                 incomplete + "\n```\n\nAfter");
        view.setMermaidMode("m", 0, true);
        QCOMPARE(view.renderChatMarkdown(incomplete, "m"), incomplete);
        const QString incompleteCode = "Before\n\n```cpp\nint main() {}";
        QCOMPARE(view.renderChatMarkdown(incompleteCode + "\nmermaid"),
                 incompleteCode + "\nmermaid");
        const QString nested = "````markdown\n```mermaid\nA --> B\n```\n````";
        view.setMermaidMode("", 0, true);
        QCOMPARE(view.renderChatMarkdown(nested), nested);
        // Once switched, a closed fence becomes the diagram (here: Mermaid is missing).
        const auto missing = view.renderChatMarkdown(incomplete + "\n```\n\nAfter", "m");
        QVERIFY(missing.contains("mermaid-placeholder.svg"));
        QVERIFY(missing.endsWith("After"));
        // The mode belongs to one block of one message.
        QCOMPARE(view.renderChatMarkdown(incomplete + "\n```\n\nAfter", "other"),
                 incomplete + "\n```\n\nAfter");
        view.setMermaidMode("m", 0, false);
        QVERIFY(!view.renderChatMarkdown(incomplete + "\n```", "m").contains("placeholder"));
    }

    // The switch's button for one side, found by its object name.
    static QQuickItem* switchButton(QQuickItem* root, const QString& name) {
        auto* button = findItem(root, name);
        return button && button->isVisible() ? button : nullptr;
    }

    void rendersIntoChatAndOpensDiagram_data() {
        QTest::addColumn<int>("diagramWidth");
        QTest::addColumn<int>("diagramHeight");
        QTest::newRow("wide") << 3200 << 400;
        QTest::newRow("tall") << 400 << 3200;
        QTest::newRow("compact") << 320 << 160;
    }

    void rendersIntoChatAndOpensDiagram() {
#ifdef Q_OS_WIN
        QSKIP("Fake mmdc requires a POSIX shell");
#else
        QFETCH(int, diagramWidth);
        QFETCH(int, diagramHeight);
        Environment environment;
        QTemporaryDir temp;
        QFile cli(temp.filePath("mmdc"));
        QVERIFY(cli.open(QIODevice::WriteOnly));
        QByteArray fixture(
            "#!/bin/sh\n"
            "while [ \"$#\" -gt 0 ]; do\n"
            "  if [ \"$1\" = \"-o\" ]; then output=\"$2\"; shift 2; else shift; fi\n"
            "done\n"
            "printf '%s' '<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" "
            "height=\"160\"><rect width=\"320\" height=\"160\" fill=\"#16a34a\"/><text x=\"20\" "
            "y=\"80\" fill=\"white\">Mermaid chat preview</text></svg>' > \"$output\"\n");
        fixture.replace("width=\"320\"", "width=\"" + QByteArray::number(diagramWidth) + "\"");
        fixture.replace("height=\"160\"", "height=\"" + QByteArray::number(diagramHeight) + "\"");
        cli.write(fixture);
        cli.close();
        QVERIFY(cli.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("KETPLUS_MMDC", cli.fileName().toUtf8());
        ketplus::AgentWorkspaceView view;
        view.resize(1100, 800);
        ketplus::ThemeManager theme(*qApp);
        auto palette = theme.palette();
        view.applyTheme(palette);
        view.show();
        const auto unique = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto markdown =
            QString("Before\n\n```mermaid\nflowchart LR\nA[%1] --> B\n```\n\nAfter").arg(unique);
        QSignalSpy changed(&view, &ketplus::AgentWorkspaceView::mermaidChanged);
        view.resetMessages({QVariantMap{{"kind", "assistant"},
                                        {"body", markdown},
                                        {"meta", "Assistant"},
                                        {"filePath", ""},
                                        {"messageKey", "diagram-answer"}}});
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = findItem(view.quickView()->rootObject(), "messageBody")) != nullptr);
        // Source first: the diagram is neither rendered nor asked for.
        QTRY_VERIFY(body->property("text").toString().contains("flowchart LR"));
        QVERIFY(!body->property("text").toString().contains("![Mermaid diagram]"));
        QTest::qWait(300);
        QCOMPARE(changed.count(), 0);
        QQuickItem* toDiagram = nullptr;
        QTRY_VERIFY((toDiagram = switchButton(body, "mermaidSwitchDiagram")) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(toDiagram, "clicked"));
        QTRY_VERIFY(changed.count() > 0);
        const auto rendered = view.renderChatMarkdown(markdown, "diagram-answer");
        QVERIFY(rendered.contains("![Mermaid diagram]"));
        QVERIFY(rendered.contains("Before"));
        QVERIFY(rendered.endsWith("After"));
        QTRY_VERIFY(body->property("text").toString().contains("![Mermaid diagram]"));
        auto* document = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
        QVERIFY(document);
        QTRY_VERIFY(
            document->textDocument()->toPlainText().contains(QChar::ObjectReplacementCharacter));
        QVERIFY(body->property("selectByMouse").toBool());
        QVariant copied;
        QVERIFY(QMetaObject::invokeMethod(view.quickView()->rootObject(), "conversationCopyText",
                                          Q_RETURN_ARG(QVariant, copied)));
        QVERIFY(copied.toString().contains(markdown));
        const auto match = QRegularExpression("<([^>]+)>").match(rendered);
        QVERIFY(match.hasMatch());
        QCOMPARE(QImageReader(QUrl(match.captured(1)).toLocalFile()).format(), QByteArray("svg"));
        // As in the mock, a click on the diagram opens it in the image viewer.
        const auto viewer = view.mermaidViewerImage(QUrl(match.captured(1)));
        QCOMPARE(viewer.value("caption").toString(), QStringLiteral("Mermaid diagram"));
        const QImage large(viewer.value("source").toUrl().toLocalFile());
        // Large enough to stay sharp when the viewer fits it to the screen and zooms in.
        const QSize screen = view.quickView()->screen()->size();
        QVERIFY(qMax(large.width(), large.height()) >= qMax(screen.width(), screen.height()));
        QCOMPARE(qreal(large.width()) / large.height(), qreal(diagramWidth) / diagramHeight);
        if (!palette.dark)
            QCOMPARE(large.pixelColor(0, 0).alpha(), 255);
        auto* root = view.quickView()->rootObject();
        // The viewer is a popup, not an item of the visual tree.
        auto* lightbox = root->property("imageViewer").value<QObject*>();
        QVERIFY(lightbox);
        QQuickItem* frame = nullptr;
        QTRY_VERIFY((frame = findItem(body, "mermaidDiagramFrame")) && frame->isVisible() &&
                    frame->height() > 40);
        // The middle of the diagram's part inside the window, even when it is taller than that.
        const QRectF inWindow = frame->mapRectToScene(frame->boundingRect())
                                    .intersected(QRectF(QPointF(), view.quickView()->size()));
        QVERIFY(inWindow.height() > 40);
        const QPoint centre = inWindow.center().toPoint();
        QTest::mouseClick(view.quickView(), Qt::LeftButton, {}, centre);
        QTRY_VERIFY(lightbox->property("opened").toBool());
        QVariant shown = lightbox->property("item");
        if (shown.metaType() == QMetaType::fromType<QJSValue>())
            shown = shown.value<QJSValue>().toVariant();
        const auto item = shown.toMap();
        QCOMPARE(item.value("caption").toString(), QStringLiteral("Mermaid diagram"));
        QCOMPARE(item.value("source").toString(), viewer.value("source").toUrl().toString());
        const auto viewerCapture = qEnvironmentVariable("KETPLUS_MERMAID_VIEWER_CAPTURE");
        if (!viewerCapture.isEmpty() && diagramWidth == 320) {
            QTest::qWait(300);
            QVERIFY(view.quickView()->grabWindow().save(viewerCapture));
        }
        QVERIFY(QMetaObject::invokeMethod(lightbox, "close"));
        QTRY_VERIFY(!lightbox->property("opened").toBool());
        QVERIFY(view.mermaidViewerImage(QUrl::fromLocalFile("/tmp/unregistered.svg")).isEmpty());
        const auto capture = qEnvironmentVariable("KETPLUS_CHAT_MERMAID_CAPTURE");
        if (!capture.isEmpty()) {
            QTest::qWait(150);
            QVERIFY(view.quickView()->grabWindow().save(capture));
        }
        const auto before = changed.count();
        palette.dark = !palette.dark;
        view.applyTheme(palette);
        // Ready at once when the viewer already asked for the dark rendering, else once drawn.
        QTRY_VERIFY(changed.count() > before);
        QTRY_VERIFY(
            view.renderChatMarkdown(markdown, "diagram-answer").contains("![Mermaid diagram]"));
        QVERIFY(view.renderChatMarkdown(markdown, "diagram-answer") != rendered);
        const QString two = markdown + "\n\n~~~MERMAID\nflowchart TD\nX --> Y\n~~~";
        // The second diagram is a different block: still source until it is switched.
        QCOMPARE(view.renderChatMarkdown(two, "diagram-answer").count("![Mermaid diagram]"), 1);
        view.setMermaidMode("diagram-answer", 1, true);
        view.renderChatMarkdown(two, "diagram-answer");
        QTRY_COMPARE(view.renderChatMarkdown(two, "diagram-answer").count("![Mermaid diagram]"), 2);
#endif
    }

    // A Mermaid block is source until the reader switches it. Nothing is rendered before that, and
    // the diagram then takes the height it needs rather than a reserved frame.
    void rowGrowsOnlyWhenTheReaderSwitches() {
#ifdef Q_OS_WIN
        QSKIP("Fake mmdc requires a POSIX shell");
#else
        Environment environment;
        QTemporaryDir temp;
        QFile cli(temp.filePath("mmdc"));
        QVERIFY(cli.open(QIODevice::WriteOnly));
        // Slow enough that the pending frame is laid out and observable first.
        cli.write(("#!/bin/sh\ntouch '" + temp.filePath("ran").toUtf8() +
                   "'\nsleep 2\n"
                   "while [ \"$#\" -gt 0 ]; do\n"
                   "  if [ \"$1\" = \"-o\" ]; then output=\"$2\"; shift 2; else shift; fi\n"
                   "done\n"
                   "printf '%s' '<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"300\" "
                   "height=\"900\"><rect width=\"300\" height=\"900\" fill=\"#2563eb\"/></svg>' "
                   "> \"$output\"\n"));
        cli.close();
        QVERIFY(cli.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("KETPLUS_MMDC", cli.fileName().toUtf8());

        ketplus::AgentWorkspaceView view;
        view.resize(1100, 1400);
        ketplus::ThemeManager theme(*qApp);
        view.applyTheme(theme.palette());
        view.show();
        QString source = QStringLiteral("flowchart TD\n");
        for (int node = 0; node < 30; ++node)
            source += QStringLiteral("N%1[Step %1 %2] --> N%3\n")
                          .arg(node)
                          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))
                          .arg(node + 1);
        const auto markdown =
            QStringLiteral("Intro paragraph.\n\n```mermaid\n%1```\n\nClosing paragraph.")
                .arg(source);
        view.resetMessages({QVariantMap{{"kind", "assistant"},
                                        {"body", markdown},
                                        {"meta", "Assistant"},
                                        {"filePath", ""},
                                        {"messageKey", "grows-on-switch"}}});

        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = findItem(view.quickView()->rootObject(), "messageBody")) != nullptr);
        QTRY_VERIFY(body->property("text").toString().contains("N29"));
        QTest::qWait(600);
        QVERIFY(!QFile::exists(temp.filePath("ran")));
        QVERIFY(!body->property("text").toString().contains("mermaid-pending.svg"));
        const qreal sourceHeight = body->height();
        QQuickItem* toDiagram = nullptr;
        QTRY_VERIFY((toDiagram = switchButton(body, "mermaidSwitchDiagram")) != nullptr);
        const auto capture = qEnvironmentVariable("KETPLUS_CHAT_MERMAID_CAPTURE");
        if (!capture.isEmpty()) {
            QTest::qWait(150);
            QVERIFY(view.quickView()->grabWindow().save(capture + "-source.png"));
        }

        QVERIFY(QMetaObject::invokeMethod(toDiagram, "clicked"));
        QTRY_VERIFY(body->property("text").toString().contains("mermaid-pending.svg"));
        QVERIFY(!body->property("text").toString().contains("N29"));
        QTRY_VERIFY(findItem(body, "mermaidPendingLabel") != nullptr);
        QTRY_VERIFY(QFile::exists(temp.filePath("ran")));
        QTRY_VERIFY_WITH_TIMEOUT(body->property("text").toString().contains("![Mermaid diagram]"),
                                 10000);
        QTRY_VERIFY(findItem(body, "mermaidPendingLabel") == nullptr);
        QTest::qWait(150);
        const qreal diagramHeight = body->height();
        if (!capture.isEmpty()) {
            QTest::qWait(600);
            QVERIFY(view.quickView()->grabWindow().save(capture + "-rendered.png"));
        }
        qInfo("source body height %.1f, diagram body height %.1f", sourceHeight, diagramHeight);
        // The 300x900 diagram is shown at its own height: no reserved frame crops it.
        QVERIFY(diagramHeight > 900);
        QQuickItem* frame = nullptr;
        QTRY_VERIFY((frame = findItem(body, "mermaidDiagramFrame")) != nullptr);
        QVERIFY(frame->height() > 900);
        QTRY_VERIFY((switchButton(body, "mermaidSwitchSource")) != nullptr);

        // And back: the source returns exactly as it was.
        QVERIFY(QMetaObject::invokeMethod(switchButton(body, "mermaidSwitchSource"), "clicked"));
        QTRY_VERIFY(body->property("text").toString().contains("N29"));
        QTRY_VERIFY(qAbs(body->height() - sourceHeight) < 1.0);
#endif
    }

    // Streams an answer whose Mermaid fence arrives in pieces, the way a provider sends it.
    void streamsMermaidAsSourceAndSwitchesOnDemand() {
#ifdef Q_OS_WIN
        QSKIP("Fake mmdc requires a POSIX shell");
#else
        Environment environment;
        QTemporaryDir temp;
        QFile cli(temp.filePath("mmdc"));
        QVERIFY(cli.open(QIODevice::WriteOnly));
        cli.write("#!/bin/sh\nsleep 1\n"
                  "while [ \"$#\" -gt 0 ]; do\n"
                  "  if [ \"$1\" = \"-o\" ]; then output=\"$2\"; shift 2; else shift; fi\n"
                  "done\n"
                  "printf '%s' '<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"300\" "
                  "height=\"200\"><rect width=\"300\" height=\"200\" fill=\"#2563eb\"/></svg>' "
                  "> \"$output\"\n");
        cli.close();
        QVERIFY(cli.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("KETPLUS_MMDC", cli.fileName().toUtf8());

        ketplus::AgentWorkspaceView view;
        view.resize(1100, 800);
        ketplus::ThemeManager theme(*qApp);
        view.applyTheme(theme.palette());
        view.show();
        // Both with a paragraph before the fence and with an answer that opens with it.
        for (const QString& intro : {QStringLiteral("Here is the diagram.\n\n"), QString{}}) {
            // Unique source per pass, so the second pass is not served from the render cache.
            QString fence = QStringLiteral("```mermaid\nflowchart TD\n%%%% %1\n")
                                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
            for (int node = 0; node < 12; ++node)
                fence += QStringLiteral("N%1[Step %1] --> N%2\n").arg(node).arg(node + 1);
            const QString full = intro + fence + QStringLiteral("```\n\nDone.");
            const auto message = [](const QString& body, const QString& meta) {
                return QVariantMap{{"kind", "assistant"},
                                   {"body", body},
                                   {"meta", meta},
                                   {"filePath", ""},
                                   {"messageKey", "streamed-answer"}};
            };
            // The second pass is a new answer under the same key: back to source.
            view.setMermaidMode("streamed-answer", 0, false);
            view.resetMessages({message(QString{}, QStringLiteral("Codex · streaming"))});
            QQuickItem* body = nullptr;
            QTRY_VERIFY((body = findItem(view.quickView()->rootObject(), "messageBody")) !=
                        nullptr);
            for (qsizetype length = 16; length < full.size(); length += 24) {
                view.updateMessage(0,
                                   message(full.left(length), QStringLiteral("Codex · streaming")));
                QTest::qWait(40);
                // Never a reserved frame or a half-drawn diagram while it streams.
                QVERIFY(!body->property("text").toString().contains("mermaid-pending.svg"));
                QVERIFY(!body->property("text").toString().contains("![Mermaid"));
            }
            view.updateMessage(0, message(full, QStringLiteral("Codex")));
            QTRY_VERIFY(body->property("text").toString().contains("Done."));
            QVERIFY2(body->property("text").toString().contains("Step 11"),
                     qPrintable(body->property("text").toString()));
            QQuickItem* toDiagram = nullptr;
            QTRY_VERIFY((toDiagram = switchButton(body, "mermaidSwitchDiagram")) != nullptr);
            QVERIFY(QMetaObject::invokeMethod(toDiagram, "clicked"));
            QTRY_VERIFY_WITH_TIMEOUT(
                body->property("text").toString().contains("![Mermaid diagram]"), 10000);
            QVERIFY(body->property("text").toString().contains("Done."));
            const auto capture = qEnvironmentVariable("KETPLUS_CHAT_MERMAID_CAPTURE");
            if (!capture.isEmpty()) {
                QTest::qWait(300);
                QVERIFY(view.quickView()->grabWindow().save(capture + "-final.png"));
            }
        }
#endif
    }

    void switchingToMermaidWhileStreamingIsSmooth_data() {
        QTest::addColumn<QString>("reading");
        QTest::newRow("reading-above-the-diagram") << "above";
        QTest::newRow("reading-below-the-diagram") << "below";
        QTest::newRow("following-the-tail") << "tail";
    }

    // The reader switches a block to its diagram while the same answer is still streaming in.
    // Rendering must neither stall the GUI thread nor move what the reader is looking at.
    void switchingToMermaidWhileStreamingIsSmooth() {
#ifdef Q_OS_WIN
        QSKIP("Fake mmdc requires a POSIX shell");
#else
        QFETCH(QString, reading);
        Environment environment;
        QTemporaryDir temp;
        QFile cli(temp.filePath("mmdc"));
        QVERIFY(cli.open(QIODevice::WriteOnly));
        cli.write("#!/bin/sh\nsleep 0.4\n"
                  "while [ \"$#\" -gt 0 ]; do\n"
                  "  if [ \"$1\" = \"-o\" ]; then output=\"$2\"; shift 2; else shift; fi\n"
                  "done\n"
                  "printf '%s' '<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"420\" "
                  "height=\"360\"><rect width=\"420\" height=\"360\" fill=\"#2563eb\"/></svg>' "
                  "> \"$output\"\n");
        cli.close();
        QVERIFY(cli.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("KETPLUS_MMDC", cli.fileName().toUtf8());

        ketplus::AgentWorkspaceView view;
        view.resize(1000, 700);
        ketplus::ThemeManager theme(*qApp);
        view.applyTheme(theme.palette());
        view.show();
        view.setSessionState("Streaming", "Working", true);
        const QString above = "Reading this paragraph above the diagram should stay still.";
        const QString below = "Reading this paragraph below the diagram should stay still.";
        QString source;
        for (int i = 0; i < 30; ++i)
            source += i == 12 ? above + "\n\n"
                              : QString("Before %1 has enough text to wrap on a normal window, so "
                                        "the row is tall.\n\n")
                                    .arg(i);
        source += QStringLiteral("```mermaid\nflowchart TD\n%%%% %1\n")
                      .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        for (int node = 0; node < 15; ++node)
            source += QStringLiteral("N%1[Step %1] --> N%2\n").arg(node).arg(node + 1);
        source += QStringLiteral("```\n\n");
        for (int i = 0; i < 30; ++i)
            source += i == 12 ? below + "\n\n"
                              : QString("After %1 has enough text to wrap on a normal window, so "
                                        "the row is tall.\n\n")
                                    .arg(i);
        const auto message = [](const QString& body) {
            return QVariantMap{{"kind", "assistant"},
                               {"body", body},
                               {"meta", "Codex · streaming"},
                               {"filePath", ""},
                               {"messageKey", "streaming-diagram"}};
        };
        auto* root = view.quickView()->rootObject();
        auto* transcript = findItem(root, "agentTranscript");
        QVERIFY(transcript);
        transcript->setProperty("cacheBuffer", 100000);
        view.resetMessages({message(source)}, "smooth-switch");
        QTRY_COMPARE(transcript->opacity(), 1.0);
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = findItem(root, "messageBody")) != nullptr);
        QTRY_VERIFY(body->property("text").toString().contains("After 29"));
        auto* quickDocument = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
        QVERIFY(quickDocument);
        const QString wanted = reading == "below" ? below : above;
        const auto markerY = [&] {
            auto* document = quickDocument->textDocument();
            for (auto block = document->begin(); block.isValid(); block = block.next())
                if (block.text() == wanted)
                    return body
                        ->mapToItem(
                            transcript,
                            QPointF(0, document->documentLayout()->blockBoundingRect(block).top()))
                        .y();
            return -99999.0;
        };
        const bool following = reading == "tail";
        if (!following) {
            QVERIFY(markerY() > -99999);
            transcript->setProperty("followTail", false);
            transcript->setProperty("contentY",
                                    transcript->property("contentY").toDouble() + markerY() + 9);
            QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
        }
        QTest::qWait(150);
        const qreal anchored = following ? 0 : markerY();
        // What the reader is shown: the line's position in each frame that goes to the screen.
        // The scene graph synchronizes with the GUI thread blocked, so reading it there is safe.
        QList<qreal> frames;
        bool recording = false;
        QObject::connect(
            view.quickView(), &QQuickWindow::beforeSynchronizing, view.quickView(),
            [&] {
                if (recording && !following)
                    frames.append(markerY());
            },
            Qt::DirectConnection);

        // Gaps between 4 ms ticks of the GUI thread: a stall shows up as one long gap.
        QElapsedTimer sinceTick;
        QList<qint64> gaps;
        QTimer ticker;
        ticker.setInterval(4);
        QObject::connect(&ticker, &QTimer::timeout, [&] { gaps.append(sinceTick.restart()); });
        const auto worstGap = [&](const QList<qint64>& values) {
            return values.isEmpty() ? 0 : *std::max_element(values.begin(), values.end());
        };
        QString current = source;
        const auto stream = [&](const int count) {
            for (int i = 0; i < count; ++i) {
                current += QStringLiteral("More streamed words %1.\n\n").arg(i);
                view.updateMessage(0, message(current));
                QTest::qWait(10);
            }
        };
        sinceTick.start();
        ticker.start();
        stream(25);
        const qint64 baseline = worstGap(gaps);
        gaps.clear();

        qreal drift = 0;
        QQuickItem* toDiagram = nullptr;
        QTRY_VERIFY((toDiagram = switchButton(body, "mermaidSwitchDiagram")) != nullptr);
        recording = true;
        QVERIFY(QMetaObject::invokeMethod(toDiagram, "clicked"));
        // Keep streaming through pending -> rendered, watching the reader's line the whole way.
        QElapsedTimer waited;
        waited.start();
        while (!body->property("text").toString().contains("![Mermaid diagram]") &&
               waited.elapsed() < 10000) {
            stream(1);
        }
        QVERIFY(body->property("text").toString().contains("![Mermaid diagram]"));
        for (int i = 0; i < 30; ++i) {
            stream(1);
        }
        ticker.stop();
        recording = false;
        for (const qreal frame : frames)
            drift = qMax(drift, qAbs(frame - anchored));
        if (!following)
            QVERIFY2(frames.size() > 10, "too few frames were presented to judge the switch");
        const qint64 duringSwitch = worstGap(gaps);
        QList<qint64> sorted = gaps;
        std::sort(sorted.begin(), sorted.end());
        const qint64 p99 =
            sorted.isEmpty() ? 0
                             : sorted[qMin<qsizetype>(sorted.size() - 1, sorted.size() * 99 / 100)];
        qInfo("%s: GUI gap baseline %lld ms, worst while switching %lld ms (p99 %lld ms); %lld "
              "frames, "
              "reader drift %.2f px",
              qPrintable(reading), baseline, duringSwitch, p99, qint64(frames.size()), drift);
        QVERIFY2(duringSwitch < 150, "switching to Mermaid stalled the GUI thread");
        if (following)
            QTRY_VERIFY(transcript->property("atYEnd").toBool());
        else
            QVERIFY2(drift < 1.5, "switching to Mermaid moved the line being read");
#endif
    }

    // Qt SVG ignores em offsets and nested <tspan>; each label row becomes its own <text>.
    void flattensLabelRowsForQtSvg() {
        const QByteArray svg =
            "<svg><style>#my-svg{font-size:13px;}</style>"
            "<text y=\"-10.1\" style=\"\"><tspan class=\"text-outer-tspan row\" x=\"0\" "
            "y=\"-0.1em\" dy=\"1.1em\"><tspan font-style=\"normal\" class=\"text-inner-tspan\" "
            "font-weight=\"normal\">Long</tspan><tspan font-style=\"normal\" "
            "class=\"text-inner-tspan\" font-weight=\"normal\"> label</tspan></tspan>"
            "<tspan class=\"text-outer-tspan row\" x=\"0\" y=\"1em\" dy=\"1.1em\"><tspan "
            "font-style=\"normal\" class=\"text-inner-tspan\" font-weight=\"normal\">wraps</tspan>"
            "</tspan></text><text x=\"4\">plain</text></svg>";
        const QByteArray flattened = ketplus::MermaidRenderer::flattenLabelRows(svg);
        QVERIFY2(flattened.contains("<text style=\"\" y=\"13.00\">Long label</text>"),
                 flattened.constData());
        QVERIFY(flattened.contains("<text style=\"\" y=\"27.30\">wraps</text>"));
        QVERIFY(flattened.contains("<text x=\"4\">plain</text>"));
        QVERIFY(!flattened.contains("tspan"));
    }

    void preservesSourceOnFailure() {
#ifdef Q_OS_WIN
        QSKIP("Test uses /usr/bin/false");
#else
        Environment environment;
        qputenv("KETPLUS_MMDC", "/usr/bin/false");
        ketplus::AgentWorkspaceView view;
        const auto source =
            QString("```mermaid\ninvalid %1\n```").arg(QUuid::createUuid().toString());
        QSignalSpy changed(&view, &ketplus::AgentWorkspaceView::mermaidChanged);
        // Not asked for: the source is just source and nothing is rendered.
        QCOMPARE(view.renderChatMarkdown(source, "failing"), source);
        view.setMermaidMode("failing", 0, true);
        // While rendering, the body holds the pending frame rather than the source.
        const auto pending = view.renderChatMarkdown(source, "failing");
        QVERIFY(pending.contains("mermaid-pending.svg"));
        QVERIFY(!pending.contains(source));
        QTRY_VERIFY(changed.count() > 0);
        const auto failed = view.renderChatMarkdown(source, "failing");
        QVERIFY(failed.contains(source));
        QVERIFY(failed.contains("preview failed"));
#endif
    }

    // Recreated transcript delegates can request a previously selected diagram while applying
    // their first document. An immediate cache failure must not be overwritten by Pending.
    void immediateFailureDoesNotLeaveRecreatedRowLoading() {
        Environment environment;
        // Discovery succeeds on every platform; the blocked cache prevents process launch.
        qputenv("KETPLUS_MMDC", QCoreApplication::applicationFilePath().toUtf8());
        const auto previousName = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName("mermaid-cache-failure-" +
                                             QUuid::createUuid().toString(QUuid::WithoutBraces));
        const auto restoreName =
            qScopeGuard([&] { QCoreApplication::setApplicationName(previousName); });
        const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QVERIFY(!cache.isEmpty());
        QCOMPARE(QFileInfo(cache).fileName(), QCoreApplication::applicationName());
        QVERIFY(!QFileInfo::exists(cache));
        QVERIFY(QDir().mkpath(cache));
        const auto cleanup = qScopeGuard([&] { QDir(cache).removeRecursively(); });
        // A regular file in place of the cache directory forces a synchronous request failure.
        QFile blocker(QDir(cache).filePath("mermaid"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();

        ketplus::AgentWorkspaceView view;
        view.resize(1100, 800);
        ketplus::ThemeManager theme(*qApp);
        theme.previewMode(ketplus::ThemeManager::Mode::Dark);
        view.applyTheme(theme.palette());
        view.show();
        const QString source = "```mermaid\nflowchart LR\nA --> B\n```";
        view.setMermaidMode("recreated", 0, true);
        view.resetMessages({QVariantMap{{"kind", "assistant"},
                                        {"body", source},
                                        {"meta", "Assistant"},
                                        {"messageKey", "recreated"}}});
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = findItem(view.quickView()->rootObject(), "messageBody")) != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(body->property("text").toString().contains("preview failed"),
                                 2000);
        // QQuickTextEdit serializes styled Markdown (e.g. medium-weight code) back
        // through QTextDocument. Verify readable/copyable source, not that serialization.
        auto* document = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
        QVERIFY(document && document->textDocument());
        const auto plainText = document->textDocument()->toPlainText();
        QVERIFY2(plainText.contains("flowchart LR\nA --> B"), qPrintable(plainText));
        QVERIFY(view.transcriptCopyText().contains(source));
        QVERIFY(!body->property("text").toString().contains("mermaid-pending.svg"));
        const auto capture = qEnvironmentVariable("KETPLUS_CHAT_MERMAID_CAPTURE");
        if (!capture.isEmpty()) {
            QTest::qWait(150);
            QVERIFY(view.quickView()->grabWindow().save(capture));
        }
    }

    void installsOnlyOnClickAndRetries() {
#ifdef Q_OS_WIN
        QSKIP("Fake npm requires a POSIX shell");
#else
        Environment environment;
        QTemporaryDir temp;
        const auto executable = [](const QString& path, const QByteArray& contents) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return false;
            if (file.write(contents) != contents.size())
                return false;
            file.close();
            return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        };
        const auto npm = temp.filePath("npm");
        const auto mmdc = temp.filePath("mmdc");
        QVERIFY(executable(npm, "#!/bin/sh\necho Permission denied\nexit 7\n"));
        qputenv("PATH", temp.path().toUtf8() + ":" + environment.path);
        qputenv("KETPLUS_MMDC", mmdc.toUtf8());
        ketplus::AgentWorkspaceView view;
        ketplus::ThemeManager theme(*qApp);
        view.applyTheme(theme.palette());
        view.resize(1100, 800);
        view.show();
        const auto source = QString("```mermaid\nflowchart LR\nA[%1] --> B\n```")
                                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        view.resetMessages({QVariantMap{{"kind", "assistant"},
                                        {"body", source},
                                        {"meta", "Assistant"},
                                        {"filePath", ""},
                                        {"messageKey", "install-answer"}}});
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = findItem(view.quickView()->rootObject(), "messageBody")) != nullptr);
        // Installing is offered only to a reader who asked for a diagram.
        QTest::qWait(300);
        QVERIFY(!findItem(body, "mermaidInstallButton"));
        QQuickItem* toDiagram = nullptr;
        QTRY_VERIFY((toDiagram = switchButton(body, "mermaidSwitchDiagram")) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(toDiagram, "clicked"));
        QQuickItem* button = nullptr;
        QTRY_VERIFY((button = findItem(body, "mermaidInstallButton")) != nullptr);
        QTRY_VERIFY(button->isVisible());
        auto* transcript = findItem(view.quickView()->rootObject(), "agentTranscript");
        QVERIFY(transcript);
        QTRY_VERIFY(transcript->opacity() > 0.9);
        auto* command = findItem(body, "mermaidInstallCommand");
        QVERIFY(command);
        QCOMPARE(command->property("text").toString(), "npm install -g @mermaid-js/mermaid-cli");
        QVERIFY(!view.findChild<QProcess*>("mermaidInstaller"));
        const auto capture = qEnvironmentVariable("KETPLUS_CHAT_INSTALL_CAPTURE");
        if (!capture.isEmpty()) {
            QTest::qWait(200);
            QVERIFY(view.quickView()->grabWindow().save(capture));
        }
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QTRY_VERIFY(view.mermaidInstallError().contains("Permission denied"));
        QVERIFY(!view.mermaidInstalling());
        QVERIFY(
            executable(mmdc + ".template",
                       "#!/bin/sh\nwhile [ \"$#\" -gt 0 ]; do\nif [ \"$1\" = \"-o\" ]; then "
                       "output=\"$2\"; shift 2; else shift; fi\ndone\nprintf '%s' '<svg "
                       "xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"100\"><rect "
                       "width=\"200\" height=\"100\" fill=\"green\"/></svg>' > \"$output\"\n"));
        QVERIFY(executable(
            npm, "#!/bin/sh\nprintf '%s\\n' \"$@\" >> \"$KETPLUS_MMDC.args\"\nsleep 0.2\ncp "
                 "\"$KETPLUS_MMDC.template\" \"$KETPLUS_MMDC\"\nchmod +x \"$KETPLUS_MMDC\"\n"));
        view.installMermaid();
        view.installMermaid();
        QVERIFY(view.mermaidInstalling());
        QTRY_VERIFY(body->property("text").toString().contains("![Mermaid diagram]"));
        QTRY_VERIFY(!findItem(body, "mermaidInstallOverlay"));
        QFile args(mmdc + ".args");
        QVERIFY(args.open(QIODevice::ReadOnly));
        QCOMPARE(args.readAll(), QByteArray("install\n-g\n@mermaid-js/mermaid-cli\n"));
#endif
    }

    void realMmdcSmoke_data() {
        QTest::addColumn<QString>("graph");
        QTest::addColumn<QString>("label");
        QTest::newRow("simple") << QStringLiteral(
                                       "flowchart LR\nA[Chat] --> B[mmdc]\nB --> C[Preview]")
                                << QStringLiteral("Chat");
        QTest::newRow("unicode-databases")
            << QStringLiteral("flowchart TB\n"
                              "Z[\"Danh tính · một tài khoản\"]\n"
                              "Z -->|SSO| A[\"Ứng dụng A\"]\n"
                              "Z -->|SSO| B[\"Ứng dụng B\"]\n"
                              "Z -->|SSO| C[\"Gateway → ứng dụng C\"]\n"
                              "Z --> I[(\"Database danh tính\")]\n"
                              "A --> AD[(\"Database A\")]\n"
                              "B --> BD[(\"Database B\")]\n"
                              "C --> CD[(\"Database theo tenant\")]\n"
                              "D[\"Daemon · giữ dữ liệu cũ\"] <--> A")
            << QStringLiteral("Danh tính");
    }

    void realMmdcSmoke() {
        if (!qEnvironmentVariableIsSet("KETPLUS_REAL_CHAT_MERMAID"))
            QSKIP("Enable real mmdc smoke test explicitly");
        QFETCH(QString, graph);
        QFETCH(QString, label);
        ketplus::AgentWorkspaceView view;
        QSignalSpy failed(view.findChild<ketplus::MermaidRenderer*>(),
                          &ketplus::MermaidRenderer::diagramFailed);
        QVERIFY(failed.isValid());
        ketplus::ThemeManager theme(*qApp);
        view.applyTheme(theme.palette());
        view.resize(1100, 800);
        view.show();
        const auto source = QString("Mermaid in chat\n\n```mermaid\n%%%% %1\n%2\n```\n\n"
                                    "Diagram rendered from the message.")
                                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces), graph);
        view.resetMessages({QVariantMap{{"kind", "assistant"},
                                        {"body", source},
                                        {"meta", "Assistant"},
                                        {"filePath", ""},
                                        {"messageKey", "real-smoke"}}});
        QQuickItem* switchBody = nullptr;
        QTRY_VERIFY((switchBody = findItem(view.quickView()->rootObject(), "messageBody")) !=
                    nullptr);
        QQuickItem* toDiagram = nullptr;
        QTRY_VERIFY((toDiagram = switchButton(switchBody, "mermaidSwitchDiagram")) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(toDiagram, "clicked"));
        QTRY_VERIFY_WITH_TIMEOUT(
            view.renderChatMarkdown(source, "real-smoke").contains("![Mermaid diagram]") ||
                !failed.isEmpty(),
            60000);
        const auto error = failed.isEmpty() ? QString{} : failed.first().at(1).toString();
        QVERIFY2(failed.isEmpty(), qPrintable(error));
        const auto match =
            QRegularExpression("<([^>]+)>").match(view.renderChatMarkdown(source, "real-smoke"));
        QVERIFY(match.hasMatch());
        QImageReader image(QUrl(match.captured(1)).toLocalFile());
        QCOMPARE(image.format(), QByteArray("svg"));
        // Labels are SVG text Qt draws, never HTML inside <foreignObject>, which it skips.
        QFile rendered(QUrl(match.captured(1)).toLocalFile());
        QVERIFY(rendered.open(QIODevice::ReadOnly));
        const QByteArray svg = rendered.readAll();
        QVERIFY(!svg.contains("<foreignObject"));
        QVERIFY(svg.contains(label.toUtf8()));
        QVERIFY(!image.read().isNull());
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = findItem(view.quickView()->rootObject(), "messageBody")) != nullptr);
        QTRY_VERIFY(body->property("text").toString().contains("![Mermaid diagram]"));
        const auto capture = qEnvironmentVariable("KETPLUS_CHAT_MERMAID_CAPTURE");
        if (!capture.isEmpty()) {
            QTest::qWait(300);
            QVERIFY(view.quickView()->grabWindow().save(capture));
        }
    }

    // A tall, wide diagram: the frame must grow to the natural height and keep it centred.
    void realLargeMmdcSmoke() {
        if (!qEnvironmentVariableIsSet("KETPLUS_REAL_CHAT_MERMAID"))
            QSKIP("Enable real mmdc smoke test explicitly");
        ketplus::AgentWorkspaceView view;
        ketplus::ThemeManager theme(*qApp);
        view.applyTheme(theme.palette());
        view.resize(1100, 1800);
        view.show();
        QString graph = "flowchart TD\n";
        for (int stage = 0; stage < 6; ++stage) {
            graph += QString("subgraph S%1[Stage %1]\n").arg(stage);
            for (int node = 0; node < 4; ++node)
                graph += QString("N%1_%2[Step %1.%2]\n").arg(stage).arg(node);
            graph += QString("N%1_0 --> N%1_1\nN%1_0 --> N%1_2\nN%1_1 --> N%1_3\nN%1_2 --> "
                             "N%1_3\nend\n")
                         .arg(stage);
            if (stage > 0)
                graph += QString("N%1_3 --> N%2_0\n").arg(stage - 1).arg(stage);
        }
        const auto source =
            QString("Large diagram\n\n```mermaid\n%1```\n\nAfter the diagram %2.")
                .arg(graph, QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
        view.resetMessages({QVariantMap{{"kind", "assistant"},
                                        {"body", source},
                                        {"meta", "Assistant"},
                                        {"filePath", ""},
                                        {"messageKey", "real-large"}}});
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = findItem(view.quickView()->rootObject(), "messageBody")) != nullptr);
        QQuickItem* toDiagram = nullptr;
        QTRY_VERIFY((toDiagram = switchButton(body, "mermaidSwitchDiagram")) != nullptr);
        const qreal sourceHeight = body->height();
        QVERIFY(QMetaObject::invokeMethod(toDiagram, "clicked"));
        QTRY_VERIFY_WITH_TIMEOUT(
            view.renderChatMarkdown(source, "real-large").contains("![Mermaid diagram]"), 90000);
        QTRY_VERIFY(body->property("text").toString().contains("![Mermaid diagram]"));
        QTRY_VERIFY_WITH_TIMEOUT(body->height() > sourceHeight, 5000);
        QTRY_VERIFY(!body->property("mermaidBlocks").toList().isEmpty());
        qInfo() << "large diagram: body height" << body->height() << "source height" << sourceHeight
                << "frame" << body->property("mermaidBlocks");
        const auto capture = qEnvironmentVariable("KETPLUS_CHAT_MERMAID_CAPTURE");
        if (!capture.isEmpty()) {
            QTest::qWait(300);
            QVERIFY(view.quickView()->grabWindow().save(capture));
        }
    }
};

QTEST_MAIN(ChatMermaidTest)
#include "ChatMermaidTest.moc"
