#include "AgentWorkspaceViewTest.h"
#include "agent/AgentTranscriptPaging.h"
#include "agent/AgentWorkspaceView.h"
#include <QElapsedTimer>
#include <QFile>
#include <QPointer>
#include <QStyleHints>
#include <sys/stat.h>

#include <QAbstractTextDocumentLayout>
#include <QAnimationDriver>
#include <QImage>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QtTest>

using namespace agent_workspace_test;

namespace {

// Deterministic animation clock. Flickable's kinetic wheel motion (QQuickTimeLine)
// only advances when the test steps a frame, so the amount of physical motion
// between a wheel event and a model update no longer depends on wall-clock time.
class ManualAnimationDriver final : public QAnimationDriver {
  public:
    ManualAnimationDriver() { install(); }
    ~ManualAnimationDriver() override { uninstall(); }
    qint64 elapsed() const override { return elapsed_; }
    void step(qint64 milliseconds) {
        elapsed_ += milliseconds;
        advance();
    }

  private:
    qint64 elapsed_ = 0;
};

// One deterministic frame: advance animations by 16 ms, flush posted events,
// zero-interval timers and Qt.callLater, and run polish/layout synchronously.
void stepFrame(QQuickView* window, ManualAnimationDriver& driver) {
    driver.step(16);
    for (int pass = 0; pass < 3; ++pass) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        (void)window->grabWindow();
    }
    QCoreApplication::processEvents();
}

QQuickItem* delegateForKey(QQuickItem* item, const QString& key) {
    if (item->property("messageKey").toString() == key && item->property("index").isValid())
        return item;
    for (auto* child : item->childItems())
        if (auto* match = delegateForKey(child, key))
            return match;
    return nullptr;
}

QVariantMap captureAnchor(QQuickItem* root) {
    QVariant result;
    QMetaObject::invokeMethod(root, "captureTranscriptPosition", Q_RETURN_ARG(QVariant, result));
    return result.toMap();
}

QVariantList mixedHistory(int count) {
    QVariantList history;
    for (int index = 0; index < count; ++index) {
        history.append(QVariantMap{
            {"messageKey", QString::number(index)},
            {"kind", "assistant"},
            {"meta", "Codex"},
            {"body", QString("Message %1 with **bold** text and enough words to wrap when the "
                             "transcript column becomes narrower than usual.\n\n")
                         .arg(index)
                         .repeated(1 + (index % 7) * 3)}});
    }
    return history;
}

// Rows whose height follows the column width: long single-paragraph plain-text
// user bubbles alternating with short assistant replies.
QVariantList reflowHistory(int count) {
    QVariantList history;
    for (int index = 0; index < count; ++index) {
        const bool user = index % 2 == 0;
        history.append(QVariantMap{
            {"messageKey", QString::number(index)},
            {"kind", user ? "user" : "assistant"},
            {"meta", user ? "You" : "Codex"},
            {"body", user ? QString("User %1 asks a long question that keeps going so the bubble "
                                    "wraps across several lines. ")
                                .arg(index)
                                .repeated(2 + index % 5)
                          : QString("Reply %1.\n\n").arg(index)}});
    }
    return history;
}

// Park the reader in the middle of the transcript without physical input.
void readFromMiddle(QQuickItem* transcript) {
    transcript->setProperty("followTail", false);
    const double originY = transcript->property("originY").toDouble();
    const double contentHeight = transcript->property("contentHeight").toDouble();
    transcript->setProperty("contentY", originY + (contentHeight - transcript->height()) / 2);
    QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
}

} // namespace

void AgentWorkspaceViewTest::streamingTableKeepsReadingParagraph_data() {
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("offset");
    QTest::newRow("wide") << 1000 << 0;
    QTest::newRow("narrow") << 600 << 0;
    QTest::newRow("partial-line") << 1000 << 9;
}

void AgentWorkspaceViewTest::streamingTableKeepsReadingParagraph() {
    QFETCH(int, width);
    QFETCH(int, offset);
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(width, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QString body = "| Name | Description |\n| --- | --- |\n";
    for (int i = 0; i < 50; ++i)
        body +=
            QString("| Row %1 | A description with several words that can wrap to more lines. |\n")
                .arg(i);
    QVariantMap entry{{"messageKey", "table"}, {"kind", "assistant"}, {"body", body}};
    view.setSessionState("Streaming", "Working", true);
    view.resetMessages({entry}, "table-scroll");
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTest::qWait(100);
    auto* text = findVisualItem(root, "messageBody");
    QVERIFY(text);
    auto* quickDocument = qvariant_cast<QQuickTextDocument*>(text->property("textDocument"));
    QVERIFY(quickDocument);
    auto* document = quickDocument->textDocument();
    const auto markerY = [&] {
        for (auto block = document->begin(); block.isValid(); block = block.next())
            if (block.text() == "Row 20")
                return text
                    ->mapToItem(
                        transcript,
                        QPointF(0, document->documentLayout()->blockBoundingRect(block).top()))
                    .y();
        return -99999.0;
    };
    QVERIFY(markerY() != -99999);
    transcript->setProperty("followTail", false);
    transcript->setProperty("contentY",
                            transcript->property("contentY").toDouble() + markerY() + offset);
    QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
    QTest::qWait(100);
    const double before = markerY();
    QVERIFY(qAbs(before + offset) < 1);
    const QString capturePath =
        qEnvironmentVariable("KETPLUS_REFLOW_CAPTURE") + "." + QTest::currentDataTag();
    if (qEnvironmentVariableIsSet("KETPLUS_REFLOW_CAPTURE")) {
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        QVERIFY(view.quickView()->grabWindow().save(capturePath + ".before.png"));
    }
    body += "| A much longer name which changes the available width of all previous table cells | "
            "Short |\n";
    entry["body"] = body;
    view.updateMessage(0, entry);
    QTest::qWait(100);
    QVERIFY2(
        qAbs(markerY() - before) < 2,
        qPrintable(QString("Reading paragraph moved from %1 to %2 px").arg(before).arg(markerY())));
    QVERIFY(!transcript->property("followTail").toBool());
    if (qEnvironmentVariableIsSet("KETPLUS_REFLOW_CAPTURE"))
        QVERIFY(view.quickView()->grabWindow().save(capturePath + ".after.png"));

    // Opening a context panel also reflows the text within the same message.
    view.resize(width == 1000 ? 600 : 1000, 700);
    QTest::qWait(150);
    QVERIFY2(qAbs(markerY() - before) < 2,
             qPrintable(QString("Reading paragraph moved after resize from %1 to %2 px")
                            .arg(before)
                            .arg(markerY())));

    auto* latest = findVisualItem(root, "jumpToLatestButton");
    QVERIFY(latest);
    QVERIFY(QMetaObject::invokeMethod(latest, "clicked"));
    body += "| Another row | Keep following new output after Jump to latest. |\n";
    entry["body"] = body;
    view.updateMessage(0, entry);
    QTRY_VERIFY(transcript->property("followTail").toBool());
    QTRY_VERIFY(transcript->property("atYEnd").toBool());
}

void AgentWorkspaceViewTest::scrollingWhileStreamingKeepsReadingPosition_data() {
    QTest::addColumn<bool>("mixedHeights");
    QTest::addColumn<bool>("wheelPhases");
    QTest::addColumn<bool>("stream");
    QTest::newRow("uniform-touchpad") << false << true << true;
    QTest::newRow("mixed-touchpad") << true << true << true;
    QTest::newRow("mixed-mouse") << true << false << true;
    QTest::newRow("mixed-idle") << true << true << false;
}

void AgentWorkspaceViewTest::scrollingWhileStreamingKeepsReadingPosition() {
    QFETCH(bool, mixedHeights);
    QFETCH(bool, wheelPhases);
    QFETCH(bool, stream);
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QVariantList history;
    for (int index = 0; index < 120; ++index) {
        const int paragraphs = mixedHeights ? 1 + (index % 7) * 3 : 3;
        history.append(QVariantMap{
            {"messageKey", QString::number(index)},
            {"kind", "assistant"},
            {"meta", "Codex"},
            {"body",
             QString("Message %1 with **bold** text.\n\n").arg(index).repeated(paragraphs)}});
    }
    view.setSessionState("Streaming", "Working", true);
    view.resetMessages(history, "scrolling-stream");
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTest::qWait(100);

    // From here on, time only advances in explicit frames: kinetic mouse-wheel
    // motion, polish/layout, Qt.callLater and zero-delay refresh timers are all
    // driven by stepFrame(), and wheel timestamps follow the same clock.
    ManualAnimationDriver driver;
    const auto frame = [&] { stepFrame(view.quickView(), driver); };
    frame();
    const auto wheel = [&](int delta, Qt::ScrollPhase phase) {
        const auto position = transcript->mapToScene(QPointF(transcript->width() - 20, 100));
        QWheelEvent event(position, view.quickView()->mapToGlobal(position.toPoint()),
                          wheelPhases ? QPoint(0, delta) : QPoint(), QPoint(0, delta), Qt::NoButton,
                          Qt::NoModifier, wheelPhases ? phase : Qt::NoScrollPhase, false,
                          wheelPhases ? Qt::MouseEventSynthesizedBySystem
                                      : Qt::MouseEventNotSynthesized);
        event.setTimestamp(1000 + driver.elapsed());
        QCoreApplication::sendEvent(view.quickView(), &event);
    };
    if (wheelPhases)
        wheel(0, Qt::ScrollBegin);
    for (int step = 0; step < 100; ++step) {
        wheel(wheelPhases ? 96 : 120, Qt::ScrollUpdate);
        frame();
        QVERIFY(!transcript->property("followTail").toBool());
        const auto beforeDelta = captureAnchor(root);
        const QString key = beforeDelta.value("key").toString();
        auto* readingItem = delegateForKey(transcript, key);
        QVERIFY(readingItem);
        const double readingScreenY = readingItem->mapToItem(transcript, QPointF()).y();
        auto entry = view.messageModel()->get(view.messageModel()->count() - 1);
        entry["body"] = entry.value("body").toString() + "Extra streamed output.\n\n";
        if (stream) {
            view.updateMessage(view.messageModel()->count() - 1, entry);
            if (step % 3 == 0)
                view.appendMessage({{"messageKey", QString("new-%1").arg(step)},
                                    {"kind", "assistant"},
                                    {"body", "New streamed item.\n\n"}});
        }
        frame();
        readingItem = delegateForKey(transcript, key);
        QVERIFY(readingItem);
        const double displacement =
            readingItem->mapToItem(transcript, QPointF()).y() - readingScreenY;
        QVERIFY2(displacement >= -2 && displacement < 200,
                 qPrintable(QString("Step %1: reading row %2 moved %3 px during update")
                                .arg(step)
                                .arg(key)
                                .arg(displacement)));
    }
    if (wheelPhases)
        wheel(0, Qt::ScrollEnd);
    // Notches glide, and a notched wheel ends at a wall-clock pause in its clicks.
    for (int settle = 0; settle < 2000 && transcript->property("scrolling").toBool(); ++settle) {
        QTest::qWait(1);
        frame();
    }
    QVERIFY(!transcript->property("scrolling").toBool());
    for (int settle = 0; settle < 10; ++settle)
        frame();
    const auto anchor = captureAnchor(root);
    QVERIFY(anchor.value("index").toInt() > 0);
    QVERIFY(anchor.value("index").toInt() < 118);
    for (int step = 0; step < 30; ++step) {
        auto entry = view.messageModel()->get(119);
        entry["body"] = entry.value("body").toString() + "More output.\n\n";
        view.updateMessage(119, entry);
        frame();
        const auto actual = captureAnchor(root);
        QCOMPARE(actual.value("key"), anchor.value("key"));
        QVERIFY2(qAbs(actual.value("offset").toDouble() - anchor.value("offset").toDouble()) < 2,
                 qPrintable(QString("Offset changed from %1 to %2")
                                .arg(anchor.value("offset").toDouble())
                                .arg(actual.value("offset").toDouble())));
        QVERIFY(!transcript->property("followTail").toBool());
    }
}

void AgentWorkspaceViewTest::boundedWheelStabilizationPreservesContinuedAndReverseInput() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QVariantList history;
    for (int index = 0; index < 120; ++index) {
        history.append(QVariantMap{
            {"messageKey", QString::number(index)},
            {"kind", "assistant"},
            {"meta", "Codex"},
            {"body", QString("Message %1.\n\n").arg(index).repeated(1 + (index % 7) * 3)}});
    }
    view.resetMessages(history, "wheel-direction");
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTest::qWait(100);
    // Rows the view has not measured count as a placeholder height; measuring
    // them above the reader moves contentY without moving the reader.
    const auto* geometry = qvariant_cast<QObject*>(transcript->property("geometry"));
    QVERIFY(geometry);
    QTRY_COMPARE_WITH_TIMEOUT(geometry->property("measuredRowCount").toInt(),
                              geometry->property("rowCount").toInt(), 10000);
    ManualAnimationDriver driver;
    const auto frame = [&] { stepFrame(view.quickView(), driver); };
    frame();
    const auto wheel = [&](int delta, Qt::ScrollPhase phase, bool waitForFrame = true) {
        const auto position = transcript->mapToScene(QPointF(transcript->width() - 20, 100));
        QWheelEvent event(position, view.quickView()->mapToGlobal(position.toPoint()),
                          QPoint(0, delta), QPoint(0, delta), Qt::NoButton, Qt::NoModifier, phase,
                          false, Qt::MouseEventSynthesizedBySystem);
        event.setTimestamp(1000 + driver.elapsed());
        QCoreApplication::sendEvent(view.quickView(), &event);
        if (waitForFrame)
            frame();
    };

    wheel(0, Qt::ScrollBegin);
    const auto tailY = transcript->property("contentY").toDouble();
    // Flickable consumes the first phased updates of a gesture before moving;
    // start the gesture so the next event's motion is observable on its own.
    for (int warmup = 0; warmup < 3; ++warmup)
        wheel(24, Qt::ScrollUpdate);
    // The heuristic motion budget no longer exists: physical motion is credited
    // to the anchor exactly, so a single large event must move its full distance
    // (the old contract asserted transcriptWheelMotionBudget >= 360 instead).
    const auto beforeLargeEvent = transcript->property("contentY").toDouble();
    wheel(360, Qt::ScrollUpdate);
    const auto afterLargeEvent = transcript->property("contentY").toDouble();
    QVERIFY2(beforeLargeEvent - afterLargeEvent >= 300,
             qPrintable(QString("A single 360 px physical wheel event moved only %1 px")
                            .arg(beforeLargeEvent - afterLargeEvent)));

    for (int step = 0; step < 20; ++step)
        wheel(96, Qt::ScrollUpdate);
    const auto olderY = transcript->property("contentY").toDouble();
    QVERIFY2(olderY < tailY - 160,
             qPrintable(QString("Continued upward input moved only %1 px").arg(tailY - olderY)));
    for (int step = 0; step < 20; ++step)
        wheel(-96, Qt::ScrollUpdate);
    wheel(0, Qt::ScrollEnd);
    const auto newerY = transcript->property("contentY").toDouble();
    QVERIFY2(newerY > olderY + 160,
             qPrintable(QString("Reverse input moved only %1 px").arg(newerY - olderY)));
}

// A notch is applied by the view itself: it must travel its whole distance, but
// over several frames rather than as one jump, and show the scrollbar meanwhile.
void AgentWorkspaceViewTest::notchedWheelGlidesItsFullDistance() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QVariantList history;
    for (int index = 0; index < 120; ++index) {
        history.append(QVariantMap{
            {"messageKey", QString::number(index)},
            {"kind", "assistant"},
            {"meta", "Codex"},
            {"body", QString("Message %1.\n\n").arg(index).repeated(1 + (index % 7) * 3)}});
    }
    view.resetMessages(history, "notched-wheel");
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    const auto* geometry = qvariant_cast<QObject*>(transcript->property("geometry"));
    QVERIFY(geometry);
    QTRY_COMPARE_WITH_TIMEOUT(geometry->property("measuredRowCount").toInt(),
                              geometry->property("rowCount").toInt(), 10000);
    ManualAnimationDriver driver;
    const auto frame = [&] { stepFrame(view.quickView(), driver); };
    QMetaObject::invokeMethod(root, "setBenchmarkScrollProgress", Q_ARG(QVariant, 0.5));
    for (int settle = 0; settle < 30; ++settle)
        frame();
    auto* scrollBar = transcript->property("verticalScrollBar").value<QQuickItem*>();
    QVERIFY(scrollBar);
    QCOMPARE(scrollBar->opacity(), 0.0);

    const double distance = QGuiApplication::styleHints()->wheelScrollLines() * 20;
    QPointer<QQuickItem> marker = delegateForKey(root, captureAnchor(root).value("key").toString());
    QVERIFY(marker);
    // Row measurements can rebase contentY without moving the reader's text.
    // Measure the retained row on screen so the oracle survives font metric changes.
    const double markerStartY = marker->mapToScene(QPointF()).y();
    const auto position = transcript->mapToScene(QPointF(transcript->width() - 20, 100));
    QWheelEvent event(position, view.quickView()->mapToGlobal(position.toPoint()), QPoint(),
                      QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(view.quickView(), &event);
    // The glide starts on the frame after the notch arrives.
    frame();
    frame();
    QVERIFY(marker);
    const double firstStep = marker->mapToScene(QPointF()).y() - markerStartY;
    QVERIFY2(
        firstStep > 0 && firstStep < distance * 0.9,
        qPrintable(QString("First glide step moved %1 of %2 px").arg(firstStep).arg(distance)));
    // The scrollbar fades in while the glide runs.
    QVERIFY(scrollBar->opacity() > 0);

    for (int glide = 0; glide < 60; ++glide)
        frame();
    QVERIFY(marker);
    const double moved = marker->mapToScene(QPointF()).y() - markerStartY;
    QVERIFY2(qAbs(moved - distance) < 1,
             qPrintable(QString("A notch moved %1 px, expected %2").arg(moved).arg(distance)));
    // The end of a notched wheel is a pause in its clicks, timed on the wall
    // clock; the scrollbar then fades on animation frames.
    for (int idle = 0; idle < 200 && scrollBar->opacity() > 0; ++idle) {
        QTest::qWait(5);
        frame();
    }
    QVERIFY(!transcript->property("scrolling").toBool());
    QCOMPARE(scrollBar->opacity(), 0.0);
    QVERIFY(!transcript->property("followTail").toBool());
}

// A history page arriving above the reader must not rebuild the rows on screen.
// A prepend reports its removal and insertion separately; the row window used to
// be recomputed between the two against half-updated geometry, came out empty,
// and destroyed every delegate only to create them again.
void AgentWorkspaceViewTest::prependKeepsVisibleDelegates() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    const auto page = [](int from, int to) {
        QVariantList rows;
        for (int index = from; index < to; ++index)
            rows.append(QVariantMap{
                {"messageKey", QString("m%1").arg(index)},
                {"kind", index % 3 == 0 ? "user" : "assistant"},
                {"meta", "Codex"},
                {"body", QString("Row %1 with **bold** text.\n\n").arg(index).repeated(1 + index % 4)}});
        return rows;
    };
    view.resetMessages(page(40, 80), "prepend-delegates");
    view.setHistoryState({{"hasOlder", true}});
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    transcript->setProperty("idlePrefetchDone", true);
    const auto* geometry = qvariant_cast<QObject*>(transcript->property("geometry"));
    QVERIFY(geometry);
    QTRY_COMPARE_WITH_TIMEOUT(geometry->property("measuredRowCount").toInt(),
                              geometry->property("rowCount").toInt(), 10000);
    // Read near the top, where a page's rows fall inside the realized margin.
    transcript->setProperty("followTail", false);
    transcript->setProperty("contentY", 200.0);
    QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
    QTest::qWait(50);

    QHash<QString, QPointer<QQuickItem>> before;
    for (int index = 40; index < 80; ++index) {
        const QString key = QString("m%1").arg(index);
        if (auto* item = delegateForKey(transcript, key)) {
            const double y = item->mapToItem(transcript, QPointF()).y();
            if (y + item->height() > 0 && y < transcript->height())
                before.insert(key, item);
        }
    }
    QVERIFY(!before.isEmpty());
    view.prependMessages(page(20, 40));
    QTest::qWait(50);
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        QVERIFY2(it.value(), qPrintable(QString("Delegate for %1 was destroyed").arg(it.key())));
        QVERIFY2(delegateForKey(transcript, it.key()) == it.value().data(),
                 qPrintable(QString("Delegate for %1 was rebuilt").arg(it.key())));
    }
}

// A page window that drops rows above the reader (a newer page trimming the
// oldest rows) shifts every remaining row up by the removed height. The reading
// row must stay where it was on screen, whether the reader is idle or a wheel
// glide is running.
void AgentWorkspaceViewTest::windowTrimAboveKeepsReadingRow_data() {
    QTest::addColumn<bool>("gliding");
    QTest::newRow("idle") << false;
    QTest::newRow("wheel-glide") << true;
}

void AgentWorkspaceViewTest::windowTrimAboveKeepsReadingRow() {
    QFETCH(bool, gliding);
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    const auto page = [](int from, int to) {
        QVariantList rows;
        for (int index = from; index < to; ++index)
            rows.append(QVariantMap{
                {"messageKey", QString("t%1").arg(index)},
                {"kind", index % 3 == 0 ? "user" : "assistant"},
                {"meta", "Codex"},
                {"body",
                 QString("Row %1 with **bold** text.\n\n").arg(index).repeated(1 + index % 5)}});
        return rows;
    };
    view.resetMessages(page(0, 80), "trim-above");
    view.setHistoryState({{"hasOlder", true}, {"hasNewer", true}});
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    transcript->setProperty("idlePrefetchDone", true);
    const auto* geometry = qvariant_cast<QObject*>(transcript->property("geometry"));
    QVERIFY(geometry);
    QTRY_COMPARE_WITH_TIMEOUT(geometry->property("measuredRowCount").toInt(),
                              geometry->property("rowCount").toInt(), 10000);
    ManualAnimationDriver driver;
    const auto frame = [&] { stepFrame(view.quickView(), driver); };
    QMetaObject::invokeMethod(root, "setBenchmarkScrollProgress", Q_ARG(QVariant, 0.6));
    for (int settle = 0; settle < 30; ++settle)
        frame();
    if (gliding) {
        const auto position = transcript->mapToScene(QPointF(transcript->width() - 20, 100));
        QWheelEvent event(position, view.quickView()->mapToGlobal(position.toPoint()), QPoint(),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(view.quickView(), &event);
        frame();
        frame();
    }
    // The row nearest the middle of the viewport is the one being read.
    QString readingKey;
    double readingY = 0;
    double best = 1e9;
    for (int index = 20; index < 80; ++index) {
        const QString key = QString("t%1").arg(index);
        if (auto* item = delegateForKey(transcript, key)) {
            const double y = item->mapToItem(transcript, QPointF()).y();
            const double distance = qAbs(y + item->height() / 2 - transcript->height() / 2);
            if (y > 0 && y + item->height() < transcript->height() && distance < best) {
                best = distance;
                readingKey = key;
                readingY = y;
            }
        }
    }
    QVERIFY(!readingKey.isEmpty());
    const double glideBefore = transcript->property("contentY").toDouble();
    frame();
    const double glideStep = transcript->property("contentY").toDouble() - glideBefore;
    if (auto* item = delegateForKey(transcript, readingKey))
        readingY = item->mapToItem(transcript, QPointF()).y();

    view.replaceMessageWindow(page(20, 80));
    frame();
    auto* item = delegateForKey(transcript, readingKey);
    QVERIFY2(item, qPrintable(readingKey + " was not realized after the trim"));
    const double after = item->mapToItem(transcript, QPointF()).y();
    // A running glide may advance by at most one more of its own steps.
    const double allowance = gliding ? qAbs(glideStep) * 1.5 + 2 : 1;
    QVERIFY2(qAbs(after - readingY) <= allowance,
             qPrintable(QString("%1 moved from %2 to %3 on screen (allowed %4)")
                            .arg(readingKey)
                            .arg(readingY)
                            .arg(after)
                            .arg(allowance)));
}

// Maximizing the window changes the column width, which invalidates every
// cached height. A row whose own height the new width happens not to change
// reports nothing, and must still keep its real height rather than fall back to
// the provisional one -- otherwise the rows below are drawn over it.
void AgentWorkspaceViewTest::resizeKeepsRowsFromOverlapping() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QVariantList history;
    for (int index = 0; index < 60; ++index) {
        // Short lines stay one line at any width; code blocks do not wrap. Both
        // keep their height across the resize while being far from 120 px.
        const QString body = index % 3 == 0
                                 ? QString("```\n%1\n```").arg(QString("line\n").repeated(12 + index % 5))
                                 : QString("Short reply %1.").arg(index);
        history.append(QVariantMap{{"messageKey", QString("r%1").arg(index)},
                                   {"kind", index % 4 == 1 ? "user" : "assistant"},
                                   {"meta", "Codex"},
                                   {"body", body}});
    }
    view.resetMessages(history, "resize-overlap");
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTest::qWait(200);

    const auto* geometry = qvariant_cast<QObject*>(transcript->property("geometry"));
    QVERIFY(geometry);
    // Checked on the very first frame presented at each size, before any queued
    // work gets a turn: a reader sees that frame too.
    const auto rowsOverlap = [&](const QString& stage) -> QString {
        QList<QPair<double, QQuickItem*>> rows;
        for (int index = 0; index < 60; ++index)
            if (auto* item = delegateForKey(transcript, QString("r%1").arg(index)))
                if (item->isVisible())
                    rows.append({item->mapToItem(transcript, QPointF()).y(), item});
        if (rows.size() < 3)
            return QString("%1: only %2 rows realized").arg(stage).arg(rows.size());
        std::sort(rows.begin(), rows.end(),
                  [](const auto& left, const auto& right) { return left.first < right.first; });
        for (int row = 0; row + 1 < rows.size(); ++row) {
            const double bottom = rows.at(row).first + rows.at(row).second->height();
            if (bottom > rows.at(row + 1).first + 1)
                return QString("%1: row %2 (height %3) overlaps the next row by %4 px")
                           .arg(stage, rows.at(row).second->property("messageKey").toString())
                           .arg(rows.at(row).second->height())
                           .arg(bottom - rows.at(row + 1).first);
        }
        return {};
    };
    // Every frame the window presents (checked after polish, as it is synced to
    // be drawn) while it is being resized, the way a
    // maximize animates through sizes, must show no overlap.
    QStringList frameFailures;
    int framesChecked = 0;
    const auto connection = QObject::connect(
        view.quickView(), &QQuickWindow::beforeSynchronizing, view.quickView(), [&] {
            ++framesChecked;
            const QString failure = rowsOverlap(
                QString("frame at %1x%2").arg(view.quickView()->width()).arg(view.quickView()->height()));
            if (!failure.isEmpty() && frameFailures.size() < 5)
                frameFailures << failure;
        });
    const auto sweep = [&](QSize from, QSize to) {
        for (int step = 1; step <= 24; ++step) {
            view.resize(from + (to - from) * step / 24);
            QTest::qWait(8);
        }
        QTest::qWait(150);
    };
    sweep({1000, 700}, {1900, 1150});
    sweep({1900, 1150}, {1000, 700});
    sweep({1000, 700}, {760, 900});
    QObject::disconnect(connection);
    QVERIFY(framesChecked > 20);
    QVERIFY2(frameFailures.isEmpty(), qPrintable(frameFailures.join("; ")));
    const QString settled = rowsOverlap("settled");
    QVERIFY2(settled.isEmpty(), qPrintable(settled));
}

void AgentWorkspaceViewTest::widthReflowKeepsReadingAnchor() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    view.resetMessages(reflowHistory(120), "width-reflow");
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTest::qWait(100);
    ManualAnimationDriver driver;
    const auto frame = [&] { stepFrame(view.quickView(), driver); };
    readFromMiddle(transcript);
    for (int settle = 0; settle < 5; ++settle)
        frame();

    const auto anchor = captureAnchor(root);
    const QString key = anchor.value("key").toString();
    QVERIFY(!key.isEmpty());
    auto* row = delegateForKey(transcript, key);
    QVERIFY(row);
    const double screenY = row->mapToItem(transcript, QPointF()).y();
    const double heightBefore = transcript->property("contentHeight").toDouble();

    view.resize(640, 700);
    for (int settle = 0; settle < 10; ++settle)
        frame();

    QVERIFY2(qAbs(transcript->property("contentHeight").toDouble() - heightBefore) > 1,
             "Narrowing the transcript must re-wrap rows for this test to be meaningful");
    row = delegateForKey(transcript, key);
    QVERIFY2(row, "The anchored row must remain materialized after reflow");
    const double after = row->mapToItem(transcript, QPointF()).y();
    QVERIFY2(qAbs(after - screenY) < 2,
             qPrintable(QString("Anchored row %1 moved from %2 to %3 px after width change")
                            .arg(key)
                            .arg(screenY)
                            .arg(after)));
    QVERIFY(!transcript->property("followTail").toBool());

    // Widening back must also keep the same row in place.
    view.resize(1000, 700);
    for (int settle = 0; settle < 10; ++settle)
        frame();
    row = delegateForKey(transcript, key);
    QVERIFY(row);
    QVERIFY2(qAbs(row->mapToItem(transcript, QPointF()).y() - screenY) < 2,
             "Anchored row moved after widening");
}

void AgentWorkspaceViewTest::imageAttachmentReservesHeight() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath("wide.png");
    QImage image(800, 400, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    QVERIFY(image.save(path));
    const QUrl source = QUrl::fromLocalFile(path);

    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    QCOMPARE(view.messageImageSize(source), QSize(800, 400));
    QVERIFY(!view.messageImageSize(QUrl::fromLocalFile(directory.filePath("missing.png"))).isValid());
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    view.resetMessages({QVariantMap{{"messageKey", "with-image"},
                                    {"kind", "user"},
                                    {"body", "Look at this"},
                                    {"images", QVariantList{source.toString()}}}},
                       "image-reserve");
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_VERIFY(findVisualItem(transcript, "messageAttachmentFrame"));
    auto* frame = findVisualItem(transcript, "messageAttachmentFrame");
    auto* attachment = findVisualItem(frame, "messageAttachmentImage");
    QVERIFY(attachment);
    QTRY_VERIFY(frame->width() > 0);
    // The approved landscape thumbnail reserves 120:78 geometry before decoding.
    constexpr double thumbnailAspect = 78.0 / 120.0;
    const double reserved = frame->width() * thumbnailAspect;
    QVERIFY2(qAbs(frame->height() - reserved) < 0.5,
             qPrintable(QString("Reserved %1 px, expected %2 px (image status %3)")
                            .arg(frame->height())
                            .arg(reserved)
                            .arg(attachment->property("status").toInt())));
    // Width may still settle with the bubble layout; the invariant is that the
    // frame never shows a placeholder height that differs from the reservation
    // for its current width, including across the Loading -> Ready transition.
    QStringList violations;
    const auto checkReservation = [&] {
        if (!frame->isVisible() || frame->width() <= 0)
            return;
        const double expected = frame->width() * thumbnailAspect;
        if (qAbs(frame->height() - expected) >= 0.5)
            violations << QString("%1x%2 (status %3)")
                              .arg(frame->width())
                              .arg(frame->height())
                              .arg(attachment->property("status").toInt());
    };
    // Queued: evaluate after the width -> height binding chain has settled.
    connect(frame, &QQuickItem::heightChanged, this, checkReservation, Qt::QueuedConnection);
    connect(frame, &QQuickItem::widthChanged, this, checkReservation, Qt::QueuedConnection);
    connect(attachment, &QQuickItem::stateChanged, this, checkReservation, Qt::QueuedConnection);
    QTRY_COMPARE(attachment->property("status").toInt(), 1); // Image.Ready
    QTest::qWait(50);
    checkReservation();
    disconnect(frame, nullptr, this, nullptr);
    QVERIFY2(violations.isEmpty(), qPrintable(violations.join(", ")));
    QVERIFY(qAbs(frame->height() - frame->width() * thumbnailAspect) < 0.5);
}

void AgentWorkspaceViewTest::messageImageSizeRejectsNonRegularFiles() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ketplus::AgentWorkspaceView view;

    // A FIFO would block QImageReader forever; it must be rejected unopened.
    const QString fifo = directory.filePath("pipe.png");
    QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
    QElapsedTimer timer;
    timer.start();
    QVERIFY(!view.messageImageSize(QUrl::fromLocalFile(fifo)).isValid());
    const QString link = directory.filePath("link.png");
    QVERIFY(QFile::link(fifo, link));
    QVERIFY(!view.messageImageSize(QUrl::fromLocalFile(link)).isValid());
    QVERIFY(timer.elapsed() < 1000);
    QVERIFY(!view.messageImageSize(QUrl(QStringLiteral("https://example.com/a.png"))).isValid());

    // Missing files are negative-cached: creating the file later does not
    // cause a reopen for the same source.
    const QString missing = directory.filePath("later.png");
    const QUrl missingSource = QUrl::fromLocalFile(missing);
    QVERIFY(!view.messageImageSize(missingSource).isValid());
    QImage image(10, 20, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(missing));
    QVERIFY(!view.messageImageSize(missingSource).isValid());
    // A different source still reads normally.
    QCOMPARE(view.messageImageSize(QUrl::fromLocalFile(directory.filePath("./later.png"))), QSize(10, 20));
}

void AgentWorkspaceViewTest::programmaticScrollAfterWheelIsNotUndone() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    view.resetMessages(mixedHistory(120), "wheel-then-programmatic");
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTest::qWait(100);
    ManualAnimationDriver driver;
    const auto frame = [&] { stepFrame(view.quickView(), driver); };
    frame();
    const auto wheel = [&](int delta, Qt::ScrollPhase phase) {
        const auto position = transcript->mapToScene(QPointF(transcript->width() - 20, 100));
        QWheelEvent event(position, view.quickView()->mapToGlobal(position.toPoint()),
                          QPoint(0, delta), QPoint(0, delta), Qt::NoButton, Qt::NoModifier, phase,
                          false, Qt::MouseEventSynthesizedBySystem);
        event.setTimestamp(1000 + driver.elapsed());
        QCoreApplication::sendEvent(view.quickView(), &event);
        frame();
    };
    wheel(0, Qt::ScrollBegin);
    for (int step = 0; step < 10; ++step)
        wheel(96, Qt::ScrollUpdate);
    wheel(0, Qt::ScrollEnd);
    for (int settle = 0; settle < 2000 && transcript->property("moving").toBool(); ++settle)
        frame();
    for (int settle = 0; settle < 5; ++settle)
        frame();
    auto* wheelHandler = root->property("transcriptWheel").value<QObject*>();
    QVERIFY(wheelHandler);
    QCOMPARE(wheelHandler->property("direction").toInt(), 0);

    const double wheelY = transcript->property("contentY").toDouble();
    QMetaObject::invokeMethod(root, "setBenchmarkScrollProgress", Q_ARG(QVariant, 0.1));
    frame();
    const auto anchor = captureAnchor(root);
    const QString key = anchor.value("key").toString();
    QVERIFY(!key.isEmpty());
    auto* row = delegateForKey(transcript, key);
    QVERIFY(row);
    const double screenY = row->mapToItem(transcript, QPointF()).y();
    for (int settle = 0; settle < 10; ++settle)
        frame();
    QTest::qWait(50);
    const double after = transcript->property("contentY").toDouble();
    QVERIFY2(qAbs(after - wheelY) > 1000,
             qPrintable(QString("Programmatic scroll was undone back to the wheel position %1 (now %2)")
                            .arg(wheelY)
                            .arg(after)));
    row = delegateForKey(transcript, key);
    QVERIFY2(row, "The programmatic reading row must remain materialized");
    QVERIFY2(qAbs(row->mapToItem(transcript, QPointF()).y() - screenY) < 2,
             qPrintable(QString("Programmatic reading row %1 moved from %2 to %3 px")
                            .arg(key)
                            .arg(screenY)
                            .arg(row->mapToItem(transcript, QPointF()).y())));
    QVERIFY(!transcript->property("followTail").toBool());
}

void AgentWorkspaceViewTest::streamingRefreshDuringWheelKeepsPosition() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    view.setSessionState("Streaming", "Working", true);
    view.resetMessages(mixedHistory(120), "refresh-during-wheel");
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTest::qWait(100);
    ManualAnimationDriver driver;
    const auto frame = [&] { stepFrame(view.quickView(), driver); };
    frame();
    const auto wheel = [&](int delta, Qt::ScrollPhase phase) {
        const auto position = transcript->mapToScene(QPointF(transcript->width() - 20, 100));
        QWheelEvent event(position, view.quickView()->mapToGlobal(position.toPoint()),
                          QPoint(0, delta), QPoint(0, delta), Qt::NoButton, Qt::NoModifier, phase,
                          false, Qt::MouseEventSynthesizedBySystem);
        event.setTimestamp(1000 + driver.elapsed());
        QCoreApplication::sendEvent(view.quickView(), &event);
    };
    wheel(0, Qt::ScrollBegin);
    for (int step = 0; step < 40; ++step) {
        wheel(96, Qt::ScrollUpdate);
        frame();
    }
    // One more update without releasing the gesture.
    wheel(24, Qt::ScrollUpdate);
    // The row above the reader has to be on screen to re-render at all:
    // offscreen rows defer their refresh by design. Row heights depend on the
    // fonts the platform resolves, so keep scrolling up in small steps, within
    // the same gesture, until one is.
    QVariantMap anchor;
    int anchorIndex = -1;
    QString aboveKey;
    QQuickItem* above = nullptr;
    QQuickItem* reading = nullptr;
    for (int nudge = 0; nudge < 200; ++nudge) {
        anchor = captureAnchor(root);
        anchorIndex = anchor.value("index").toInt();
        aboveKey = anchorIndex > 0
                       ? view.messageModel()->get(anchorIndex - 1).value("messageKey").toString()
                       : QString();
        above = aboveKey.isEmpty() ? nullptr : delegateForKey(transcript, aboveKey);
        reading = delegateForKey(transcript, anchor.value("key").toString());
        if (above && reading && above->mapToItem(transcript, QPointF(0, above->height())).y() > 0)
            break;
        wheel(24, Qt::ScrollUpdate);
        frame();
    }
    QVariant active;
    QVERIFY(QMetaObject::invokeMethod(root, "transcriptInputActive", Q_RETURN_ARG(QVariant, active)));
    QVERIFY2(active.toBool(), "The touchpad gesture must still be in flight");
    QVERIFY(anchorIndex > 0);
    QVERIFY(above);
    QVERIFY(reading);
    QVERIFY2(above->mapToItem(transcript, QPointF(0, above->height())).y() > 0,
             "A row above the reader must be on screen");
    const double aboveHeight = above->height();
    const double readingY = reading->mapToItem(transcript, QPointF()).y();
    const int capturesBefore = root->property("transcriptRenderAnchorCaptures").toInt();

    // A streamed revision of the row directly above the reader: its deferred
    // Markdown refresh changes geometry above the anchor mid-gesture.
    auto entry = view.messageModel()->get(anchorIndex - 1);
    entry["body"] = entry.value("body").toString()
                    + QString("Streamed paragraph inserted above the reader.\n\n").repeated(6);
    view.updateMessage(anchorIndex - 1, entry);
    for (int settle = 0; settle < 3; ++settle) {
        // Keep the gesture alive with zero-distance updates (no motion).
        wheel(0, Qt::ScrollUpdate);
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        (void)view.quickView()->grabWindow();
    }

    above = delegateForKey(transcript, aboveKey);
    reading = delegateForKey(transcript, anchor.value("key").toString());
    QVERIFY(above);
    QVERIFY(reading);
    QVERIFY2(above->height() > aboveHeight + 20,
             "The row above the reader must have re-rendered during the gesture");
    QCOMPARE(root->property("transcriptRenderAnchorCaptures").toInt(), capturesBefore);
    const double after = reading->mapToItem(transcript, QPointF()).y();
    QVERIFY2(qAbs(after - readingY) < 2,
             qPrintable(QString("Reading row jumped from %1 to %2 px during wheel")
                            .arg(readingY)
                            .arg(after)));
    wheel(0, Qt::ScrollEnd);
    for (int settle = 0; settle < 2000 && transcript->property("moving").toBool(); ++settle)
        frame();
    QVERIFY(!transcript->property("followTail").toBool());
}

void AgentWorkspaceViewTest::measuredHeightsAreReusedPerWidth() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    view.resetMessages(mixedHistory(30), "measured-heights");
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTest::qWait(100);

    const QString key = QStringLiteral("27"); // 1 + 6*3 paragraphs, wraps
    const auto body = [&]() -> QQuickItem* {
        auto* row = delegateForKey(transcript, key);
        return row ? findVisualItem(row, "messageBody") : nullptr;
    };
    QTRY_VERIFY(body() && body()->property("sourceInitialized").toBool());
    const auto measured = [&](QQuickItem* text) {
        QVariant result;
        auto* row = delegateForKey(transcript, key);
        QMetaObject::invokeMethod(
            transcript, "measuredBodyHeight", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, key),
            Q_ARG(QVariant, row->property("body").toString().size()), Q_ARG(QVariant, text->width()));
        return result.toDouble();
    };

    auto* text = body();
    QTRY_VERIFY(qAbs(measured(text) - text->height()) < 0.5);
    // An unrendered/recycled instance of this row would use the measurement,
    // not the glyph-width guess.
    QVERIFY2(qAbs(text->property("estimatedUnrenderedHeight").toDouble() - text->height()) < 0.5,
             qPrintable(QString("Estimate %1 does not reuse measured height %2")
                            .arg(text->property("estimatedUnrenderedHeight").toDouble())
                            .arg(text->height())));
    const double wideHeight = text->height();

    // A body revision is not served a stale measurement.
    QVariant stale;
    QMetaObject::invokeMethod(transcript, "measuredBodyHeight", Q_RETURN_ARG(QVariant, stale),
                              Q_ARG(QVariant, key), Q_ARG(QVariant, 1), Q_ARG(QVariant, text->width()));
    QCOMPARE(stale.toDouble(), -1.0);

    // A width change invalidates the bucket: the old-width measurement is no
    // longer served, and the row re-measures at its new width.
    Q_UNUSED(wideHeight);
    const double wideWidth = text->width();
    const int bodyLength = delegateForKey(transcript, key)->property("body").toString().size();
    view.resize(640, 700);
    QTRY_VERIFY(body() && qAbs(body()->width() - wideWidth) > 1);
    QVariant oldBucket;
    QMetaObject::invokeMethod(transcript, "measuredBodyHeight", Q_RETURN_ARG(QVariant, oldBucket),
                              Q_ARG(QVariant, key), Q_ARG(QVariant, bodyLength),
                              Q_ARG(QVariant, wideWidth));
    QCOMPARE(oldBucket.toDouble(), -1.0);
    text = body();
    QTRY_VERIFY(qAbs(measured(text) - text->height()) < 0.5);
    QVERIFY(qAbs(text->property("estimatedUnrenderedHeight").toDouble() - text->height()) < 0.5);
}

// Flicks upward through a 60-turn history whose older pages arrive five frames
// after each request. Asserts the prefetch contract and prints the measurements
// (set KETPLUS_PREFETCH_BASELINE=1 to only measure, e.g. against the old QML).
void AgentWorkspaceViewTest::historyPrefetchChainsThroughLongHistoryWithMomentum() {
    const bool baseline = qEnvironmentVariableIsSet("KETPLUS_PREFETCH_BASELINE");
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    constexpr int turns = 60;
    const auto turnRows = [](int turn) {
        const QString turnId = QString("turn:%1").arg(turn);
        return QVariantList{
            QVariantMap{{"messageKey", QString("t%1-u").arg(turn)},
                        {"kind", "user"},
                        {"meta", "You"},
                        {"turnId", turnId},
                        {"body", QString("Question %1 about the transcript.").arg(turn)}},
            QVariantMap{
                {"messageKey", QString("t%1-a").arg(turn)},
                {"kind", "assistant"},
                {"meta", "Codex"},
                {"turnId", turnId},
                {"body", QString("Answer %1 with **bold** text and enough words to wrap.\n\n")
                             .arg(turn)
                             .repeated(1 + (turn % 4) * 2)}}};
    };
    int loadedFrom = turns - 10;
    QVariantList initial;
    for (int turn = loadedFrom; turn < turns; ++turn)
        initial += turnRows(turn);
    view.resetMessages(initial, "prefetch-chain");
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    transcript->setProperty("idlePrefetchDone", true); // Measure scrolling only.
    QTest::qWait(100);
    QSignalSpy requests(&view, &ketplus::AgentWorkspaceView::olderMessagesRequested);
    view.setHistoryState({{"hasOlder", true}});

    ManualAnimationDriver driver;
    int frameNumber = 0;
    const auto frame = [&] {
        stepFrame(view.quickView(), driver);
        ++frameNumber;
    };
    const auto wheel = [&](int delta, Qt::ScrollPhase phase) {
        const auto position = transcript->mapToScene(QPointF(transcript->width() - 20, 100));
        QWheelEvent event(position, view.quickView()->mapToGlobal(position.toPoint()),
                          QPoint(0, delta), QPoint(0, delta), Qt::NoButton, Qt::NoModifier, phase,
                          false, Qt::MouseEventSynthesizedBySystem);
        event.setTimestamp(1000 + driver.elapsed());
        QCoreApplication::sendEvent(view.quickView(), &event);
    };
    frame();
    const QString firstOlderKey = QString("t%1-a").arg(loadedFrom - 1);
    const int startFrame = frameNumber;
    int firstVisibleFrame = -1;
    int handled = 0;
    int dueFrame = -1;
    int pageTurns = 0;
    int hardStops = 0;
    int prependsWhileMoving = 0;
    int momentumKept = 0;
    double maximumDrift = 0;
    QStringList driftLog;
    const auto checkVisible = [&] {
        if (firstVisibleFrame >= 0)
            return;
        auto* row = delegateForKey(transcript, firstOlderKey);
        if (!row)
            return;
        const double y = row->mapToItem(transcript, QPointF()).y();
        if (y + row->height() > 0 && y < transcript->height())
            firstVisibleFrame = frameNumber;
    };
    const auto serve = [&] {
        if (dueFrame < 0 && requests.size() > handled) {
            const auto arguments = requests.at(handled++);
            pageTurns = ketplus::transcript_paging::adaptiveTurnLimit(
                arguments.value(0).toDouble(), arguments.value(1).toDouble(), 2.0, 20);
            dueFrame = frameNumber + 5; // Simulated daemon/IPC latency.
            view.setHistoryState({{"hasOlder", true}, {"loading", true}});
        }
        if (dueFrame < 0 || frameNumber < dueFrame)
            return;
        dueFrame = -1;
        const int from = std::max(0, loadedFrom - pageTurns);
        QVariantList page;
        for (int turn = from; turn < loadedFrom; ++turn)
            page += turnRows(turn);
        const auto anchor = captureAnchor(root);
        auto* row = delegateForKey(transcript, anchor.value("key").toString());
        const double before = row ? row->mapToItem(transcript, QPointF()).y() : 0;
        const bool moving = transcript->property("moving").toBool();
        const bool traceMomentum = qEnvironmentVariableIsSet("KETPLUS_PREFETCH_TRACE");
        const auto momentumState = [&](const char* stage) {
            if (traceMomentum)
                qInfo().noquote()
                    << QString(
                           "MOMENTUM %1 moving=%2 flicking=%3 contentY=%4 originY=%5 vVelocity=%6")
                           .arg(stage)
                           .arg(transcript->property("moving").toBool())
                           .arg(transcript->property("flicking").toBool())
                           .arg(transcript->property("contentY").toDouble())
                           .arg(transcript->property("originY").toDouble())
                           .arg(transcript->property("verticalVelocity").toDouble());
        };
        momentumState("before");
        view.prependMessages(page);
        momentumState("after-insert");
        loadedFrom = from;
        // Settle layout without advancing animation time.
        for (int pass = 0; pass < 3; ++pass) {
            QCoreApplication::sendPostedEvents();
            QCoreApplication::processEvents();
            (void)view.quickView()->grabWindow();
        }
        momentumState("after-settle");
        row = delegateForKey(transcript, anchor.value("key").toString());
        if (row) {
            const double drift = qAbs(row->mapToItem(transcript, QPointF()).y() - before);
            maximumDrift = std::max(maximumDrift, drift);
            driftLog << QString::number(drift, 'f', 2);
        }
        view.setHistoryState(QVariantMap{{"hasOlder", loadedFrom > 0},
                                         {"requestToken", requests.at(handled - 1).value(2)}});
        if (moving) {
            ++prependsWhileMoving;
            const double y = transcript->property("contentY").toDouble();
            momentumState("after-state");
            // The view restarts a fling it had to interrupt, and a restarted
            // fling makes its first step on the frame after it starts.
            frame();
            frame();
            momentumState("after-frame");
            if (transcript->property("moving").toBool() &&
                qAbs(transcript->property("contentY").toDouble() - y) > 0.01)
                ++momentumKept;
        }
    };
    const auto atTop = [&] {
        return transcript->property("contentY").toDouble() -
                   transcript->property("originY").toDouble() <
               1;
    };
    for (int flick = 0; flick < 400 && !(loadedFrom == 0 && dueFrame < 0 && atTop()); ++flick) {
        // A short physical wheel gesture followed by a kinetic flick. The flick is
        // started programmatically because offscreen wheel phases leave no momentum.
        wheel(0, Qt::ScrollBegin);
        for (int step = 0; step < 4; ++step) {
            wheel(96, Qt::ScrollUpdate);
            frame();
            serve();
            checkVisible();
        }
        wheel(0, Qt::ScrollEnd);
        frame();
        transcript->setProperty("followTail", false);
        QVERIFY(
            QMetaObject::invokeMethod(transcript, "flick", Q_ARG(qreal, 0), Q_ARG(qreal, 2500)));
        for (int settle = 0; settle < 600 && transcript->property("moving").toBool(); ++settle) {
            frame();
            serve();
            checkVisible();
        }
        if (atTop() && (loadedFrom > 0 || dueFrame >= 0))
            ++hardStops;
        for (int wait = 0; wait < 8 && (dueFrame >= 0 || requests.size() > handled); ++wait) {
            frame();
            serve();
            checkVisible();
        }
    }
    const int loadedTurns = turns - 10 - loadedFrom;
    qInfo().noquote() << QString("PREFETCH_METRICS mode=%1 firstOlderVisibleMs=%2 hardStops=%3 "
                                 "requests=%4 turnsLoaded=%5 requestsPer50Turns=%6 maxDriftPx=%7 "
                                 "prependsWhileMoving=%8 momentumKept=%9 drifts=[%10]")
                             .arg(baseline ? "baseline" : "prefetch")
                             .arg(firstVisibleFrame < 0 ? -1
                                                        : (firstVisibleFrame - startFrame) * 16)
                             .arg(hardStops)
                             .arg(requests.size())
                             .arg(loadedTurns)
                             .arg(loadedTurns > 0 ? 50.0 * requests.size() / loadedTurns : 0, 0,
                                  'f', 1)
                             .arg(maximumDrift, 0, 'f', 2)
                             .arg(prependsWhileMoving)
                             .arg(momentumKept)
                             .arg(driftLog.join(','));
    if (baseline)
        return;
    QCOMPARE(loadedFrom, 0); // Chained prepends reached the true top.
    QVERIFY(atTop());
    QCOMPARE(hardStops, 0); // No hard stop while older history remained.
    QVERIFY2(maximumDrift <= 2,
             qPrintable(QString("Reading row drifted up to %1 px on prepend").arg(maximumDrift)));
    QVERIFY(firstVisibleFrame >= 0);
    QVERIFY(prependsWhileMoving > 0);
    QCOMPARE(momentumKept, prependsWhileMoving); // Kinetic scrolling continued across prepends.
    QVERIFY(requests.size() < 50 * 50 / std::max(loadedTurns, 1));
}

// The jump button belongs to the viewport, and a jump from far above lands on
// the newest message without the reader seeing the transcript travel there.
void AgentWorkspaceViewTest::jumpToLatestFromFarAboveShowsNoScroll() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QVariantList history;
    for (int index = 0; index < 160; ++index)
        history.append(QVariantMap{
            {"messageKey", QString("j%1").arg(index)},
            {"kind", index % 2 ? "assistant" : "user"},
            {"meta", "Codex"},
            // An image reference keeps the worker from measuring the row, so its
            // height is only known once a delegate has laid it out, as with
            // much real content.
            {"body", QString("Reply %1 with a few words.\n\n").arg(index).repeated(1 + index % 4) +
                         "![diagram](missing.png)\n\n" +
                         QString("Closing line %1.\n\n").arg(index).repeated(index % 5)}});
    view.resetMessages(history, "jump-far");
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTest::qWait(200);
    QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtBeginning"));
    QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
    QTest::qWait(150);
    QTRY_VERIFY(!transcript->property("followTail").toBool());

    auto* latest = findVisualItem(root, "jumpToLatestButton");
    QVERIFY(latest);
    QTRY_VERIFY(latest->isVisible());
    // On screen at the bottom right of the viewport, whatever is scrolled.
    const auto onScreen = [&] {
        const QPointF corner = latest->mapToItem(transcript, QPointF(latest->width(), latest->height()));
        return corner.y() <= transcript->height() + 0.5 && corner.y() >= latest->height() &&
               corner.x() <= transcript->width() + 0.5;
    };
    QVERIFY(onScreen());
    transcript->setProperty("contentY", transcript->property("contentY").toDouble() + 3000);
    QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
    QTest::qWait(50);
    QVERIFY(onScreen());
    QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtBeginning"));
    QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
    QTest::qWait(150);
    // The newest rows arrive while the reader is far above, so no delegate has
    // ever laid them out.
    for (int index = 160; index < 200; ++index)
        view.appendMessage(QVariantMap{
            {"messageKey", QString("j%1").arg(index)},
            {"kind", "assistant"},
            {"meta", "Codex"},
            {"body", QString("Later reply %1.\n\n").arg(index).repeated(1 + index % 6) +
                         "![diagram](missing.png)\n\n"}});
    QTest::qWait(50);

    // Every frame shown during the jump is either where the reader was or the
    // bottom; nothing in between is ever on screen.
    const double start = transcript->property("contentY").toDouble();
    QStringList travelled;
    double shownNewestY = -1e9;
    const auto connection = QObject::connect(
        view.quickView(), &QQuickWindow::beforeSynchronizing, view.quickView(),
        [&] {
            if (transcript->opacity() < 1.0)
                return;
            const double y = transcript->property("contentY").toDouble();
            const double end = std::max(
                0.0, transcript->property("contentHeight").toDouble() - transcript->height());
            if (qAbs(y - start) > 1 && qAbs(y - end) > 1)
                travelled.append(QString("%1/%2").arg(y).arg(end));
            // Once the bottom is on screen, the rows there hold still.
            if (qAbs(y - start) <= 1)
                return;
            auto* newest = delegateForKey(transcript, "j199");
            if (!newest || !newest->isVisible()) {
                travelled.append("newest row not on screen");
                return;
            }
            const double newestY = newest->mapToItem(transcript, QPointF()).y();
            if (shownNewestY < -1e8)
                shownNewestY = newestY;
            else if (qAbs(newestY - shownNewestY) > 1)
                travelled.append(QString("newest row moved %1 -> %2").arg(shownNewestY).arg(newestY));
        });
    view.quickView()->requestActivate();
    // Cmd+Down on macOS. A test window is not always given focus; the shortcut
    // then cannot fire, and the jump it triggers is invoked directly.
    if (QTest::qWaitForWindowActive(view.quickView()))
        QTest::keyClick(view.quickView(), Qt::Key_Down, Qt::ControlModifier);
    else
        QVERIFY(QMetaObject::invokeMethod(root, "jumpToLatest"));
    QTRY_VERIFY(transcript->property("followTail").toBool());
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTest::qWait(300);
    QObject::disconnect(connection);
    QVERIFY2(travelled.isEmpty(),
             qPrintable("Frames shown between start and bottom: " + travelled.join(", ")));
    QVariant belowValue;
    QVERIFY(QMetaObject::invokeMethod(transcript, "contentBelow", Q_RETURN_ARG(QVariant, belowValue)));
    const double below = belowValue.toDouble();
    QVERIFY2(below < 1, qPrintable(QString("%1 px left below after the jump").arg(below)));
}

// The position indicator shows where the reader is in the whole conversation,
// so loading a page of older history above them does not move it.
void AgentWorkspaceViewTest::positionIndicatorIgnoresLoadedPages() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    // Turns 0-129 hold events 1-260, two per turn; turns 100-129 are loaded.
    const auto rowsFor = [](int firstTurn, int lastTurn) {
        QVariantList rows;
        for (int turn = firstTurn; turn <= lastTurn; ++turn)
            for (int part = 0; part < 2; ++part)
                rows.append(QVariantMap{
                    {"messageKey", QString("t%1-%2").arg(turn).arg(part)},
                    {"turnId", QString("daemon-turn:%1").arg(turn)},
                    {"kind", part ? "assistant" : "user"},
                    {"meta", "Codex"},
                    {"body", QString("Turn %1 part %2.\n\n").arg(turn).arg(part).repeated(1 + turn % 3)}});
        return rows;
    };
    const auto stateFor = [](int firstTurn) {
        QVariantMap cursors;
        for (int turn = firstTurn; turn <= 129; ++turn)
            cursors.insert(QString("daemon-turn:%1").arg(turn), QVariantList{2 * turn + 1, 2 * turn + 2});
        return QVariantMap{{"hasOlder", firstTurn > 0}, {"hasNewer", false},
                           {"turnCursors", cursors}, {"headCursor", 260}};
    };
    view.setHistoryState(stateFor(100));
    view.resetMessages(rowsFor(100, 129), "position-indicator");
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    transcript->setProperty("idlePrefetchDone", true);
    QTest::qWait(200);
    const auto position = [&] {
        QMetaObject::invokeMethod(transcript, "updateConversationPosition");
        return transcript->property("conversationPosition").toDouble();
    };
    // At the newest message the indicator is at the bottom.
    QTRY_VERIFY(qAbs(position() - 1.0) < 0.001);

    transcript->setProperty("followTail", false);
    transcript->setProperty("contentY", transcript->property("contentHeight").toDouble() / 2);
    QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
    QTest::qWait(150);
    const double before = position();
    // Reading around turn 115 of 130: well inside the conversation, and nowhere
    // near the middle of the loaded rows' own range.
    QVERIFY2(before > 0.75 && before < 0.95, qPrintable(QString("position %1").arg(before)));

    view.setHistoryState(stateFor(80));
    view.prependMessages(rowsFor(80, 99));
    QTest::qWait(200);
    const double after = position();
    QVERIFY2(qAbs(after - before) < 0.01,
             qPrintable(QString("indicator moved from %1 to %2 on a page load").arg(before).arg(after)));
    auto* thumb = findVisualItem(root, "transcriptPositionThumb");
    QVERIFY(thumb);
    QCOMPARE(thumb->height(), 40.0);
}
