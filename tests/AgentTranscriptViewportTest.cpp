#include "AgentWorkspaceViewTest.h"
#include "agent/AgentMessageModel.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/TranscriptRowGeometry.h"

#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJSEngine>
#include <QJSValue>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QtTest>

using ketplus::AgentWorkspaceView;

namespace {
QVariantMap streamed(const QString& body, const QString& key = "first") {
    return {{"kind", "assistant"},
            {"body", body},
            {"meta", QStringLiteral("Codex · streaming")},
            {"messageKey", key}};
}

QQuickItem* bodyForKey(QQuickItem* item, const QString& key) {
    if (item->property("messageKey").toString() == key && item->property("index").isValid())
        return agent_workspace_test::findVisualItem(item, "messageBody");
    for (auto* child : item->childItems())
        if (auto* body = bodyForKey(child, key))
            return body;
    return nullptr;
}

QVariantMap activityRow(const QString& key, const QString& turn) {
    return {{"messageKey", key},     {"kind", "tool"}, {"category", "activity"},
            {"subtype", "fileRead"}, {"turnId", turn}, {"title", key},
            {"command", key},        {"body", key},    {"status", "completed"},
            {"running", false},      {"filePath", ""}};
}

QQuickItem* delegateForKey(QQuickItem* item, const QString& key) {
    if (item->property("messageKey").toString() == key && item->property("index").isValid())
        return item;
    for (auto* child : item->childItems())
        if (auto* delegate = delegateForKey(child, key))
            return delegate;
    return nullptr;
}
} // namespace

class AgentTranscriptViewportTest final : public QObject {
    Q_OBJECT
  private slots:
    void captureUsesStableInteriorRow() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        QVariantList rows;
        for (int index = 0; index < 80; ++index) {
            rows.append(streamed(QString("Row %1.\n\n").arg(index).repeated(1 + index % 6),
                                 QString::number(index)));
        }
        view.resetMessages(rows, "interior-anchor");
        auto* root = view.quickView()->rootObject();
        auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        transcript->setProperty("followTail", false);
        QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtIndex", Q_ARG(int, 30),
                                          Q_ARG(int, 0)));
        QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
        QTest::qWait(100);

        QVariant captured;
        QVERIFY(QMetaObject::invokeMethod(root, "captureTranscriptPosition",
                                          Q_RETURN_ARG(QVariant, captured)));
        const auto anchor = captured.toMap();
        auto* delegate = delegateForKey(transcript, anchor.value("key").toString());
        QVERIFY(delegate);
        const auto screenY = delegate->mapToItem(transcript, QPointF()).y();
        QVERIFY2(
            screenY >= 24 && screenY < transcript->height() - 24,
            qPrintable(QString("Expected an interior anchor; row starts at %1 px").arg(screenY)));
    }

    void captureUsesDisplayedOffsetDuringMutationCompensation() {
        QFile source(":/agent/TranscriptPosition.js");
        QVERIFY(source.open(QIODevice::ReadOnly));
        const auto script = QString::fromUtf8(source.readAll());
        QJSEngine engine;
        QVERIFY(!engine.evaluate(script.mid(script.indexOf('\n') + 1)).isError());
        const auto result = engine.evaluate(R"JS(
            (function() {
                const delegate = {index: 4, messageKey: "four", y: 1000,
                    mapToItem: function() { return {y: 140}; }, readingTextItem: null};
                const view = {height: 600, contentY: 1200, originY: 0, followTail: false,
                    contentItem: {children: [delegate]},
                    messageModel: {get: function() { return {messageKey: "four"}; }},
                    indexAt: function() { return 4; }, itemAtIndex: function() { return delegate; }};
                return capture(view);
            })()
        )JS");
        QVERIFY2(!result.isError(), qPrintable(result.toString()));
        QCOMPARE(result.property("key").toString(), QStringLiteral("four"));
        QCOMPARE(result.property("offset").toNumber(), -140.0);
    }

    // Replaces olderPagesRequireFreshInputRatherThanFastCompletionOrGeometry. The old
    // contract was "one page per physical gesture"; the prefetch contract is "one
    // in-flight request, re-evaluated after every completion, backed off after a
    // completion that added nothing until fresh input", with the reading row fixed.
    void olderPagesPrefetchByDistanceWithoutDuplicateRequests() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        QVariantList rows;
        for (int i = 0; i < 240; ++i)
            rows.append(streamed("Short historical message.", QString::number(i)));
        view.resetMessages(rows, "prefetch-distance");
        auto* root = view.quickView()->rootObject();
        auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        transcript->setProperty("idlePrefetchDone", true); // Isolate the distance rule.
        // The rule is about real distance. Rows the view has not measured yet
        // count as a placeholder height, so wait for measurements first.
        const auto* geometry = qvariant_cast<QObject*>(transcript->property("geometry"));
        QVERIFY(geometry);
        QTRY_COMPARE_WITH_TIMEOUT(geometry->property("measuredRowCount").toInt(),
                                  geometry->property("rowCount").toInt(), 10000);
        QSignalSpy requests(&view, &AgentWorkspaceView::olderMessagesRequested);
        // Completion echoes the request token, as AgentSessionManager does.
        const auto done = [&](QVariantMap state) {
            state.insert("requestToken", requests.last().at(2));
            return state;
        };
        view.setHistoryState({{"hasOlder", true}});
        const double threshold = transcript->property("prefetchThreshold").toDouble();
        QCOMPARE(threshold, qMax(3000.0, 6 * transcript->height()));
        QVERIFY(transcript->property("contentHeight").toDouble() > threshold + 2000);
        QTest::qWait(60);
        QCOMPARE(requests.size(), 0); // Following the tail is not reading history.

        const auto placeAbove = [&](double above) {
            transcript->setProperty("followTail", false);
            transcript->setProperty("contentY", transcript->property("originY").toDouble() + above);
            QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
        };
        const auto readingRow = [&] {
            QVariant captured;
            QMetaObject::invokeMethod(root, "captureTranscriptPosition",
                                      Q_RETURN_ARG(QVariant, captured));
            const QString key = captured.toMap().value("key").toString();
            auto* row = delegateForKey(transcript, key);
            return qMakePair(key, row ? row->mapToItem(transcript, QPointF()).y() : -1e9);
        };
        placeAbove(threshold + 400);
        QTest::qWait(60);
        QCOMPARE(requests.size(), 0);
        // Well before the top (the old rule needed contentY <= originY + 80).
        placeAbove(threshold - 300);
        QTRY_COMPARE(requests.size(), 1);
        QCOMPARE(requests.first().at(0).toDouble(), transcript->height());
        QVERIFY(requests.first().at(1).toDouble() > 0); // Rendered row height hint.
        for (int i = 0; i < 20; ++i) {
            QVERIFY(QMetaObject::invokeMethod(transcript, "requestOlderAtTop"));
            QVERIFY(QMetaObject::invokeMethod(transcript, "noteTranscriptInput"));
            QCoreApplication::processEvents();
        }
        QCOMPARE(requests.size(), 1); // In flight: no duplicates.
        view.setHistoryState({{"hasOlder", true}, {"loading", true}});
        QTest::qWait(20);
        QCOMPARE(requests.size(), 1);

        // A small prepend leaves the buffer short: the completion re-evaluates and
        // chains without new input, and the reading row does not move.
        const auto before = readingRow();
        QVERIFY(before.second > -1e9);
        view.prependMessages({streamed("Older prefix.", "older-0"), streamed("Older.", "older-1")});
        view.setHistoryState(done({{"hasOlder", true}}));
        QTRY_COMPARE(requests.size(), 2);
        auto* row = delegateForKey(transcript, before.first);
        QVERIFY(row);
        QVERIFY2(qAbs(row->mapToItem(transcript, QPointF()).y() - before.second) <= 2,
                 qPrintable(QString("Reading row moved from %1 to %2 px after prepend")
                                .arg(before.second)
                                .arg(row->mapToItem(transcript, QPointF()).y())));

        // Zero new rows: back off instead of storming, until fresh input.
        view.setHistoryState(done({{"hasOlder", true}}));
        QTest::qWait(80);
        QVERIFY(QMetaObject::invokeMethod(transcript, "requestOlderAtTop"));
        QTest::qWait(20);
        QCOMPARE(requests.size(), 2);
        QVERIFY(QMetaObject::invokeMethod(transcript, "noteTranscriptInput"));
        QTRY_COMPARE(requests.size(), 3);

        // Errors never request by themselves; the slim row shows retry only then.
        view.setHistoryState(done({{"hasOlder", true}, {"error", "Temporary failure"}}));
        QVERIFY(QMetaObject::invokeMethod(transcript, "noteTranscriptInput"));
        QTest::qWait(60);
        QCOMPARE(requests.size(), 3);
        auto* header = agent_workspace_test::findVisualItem(root, "olderHistoryHeader");
        auto* button = agent_workspace_test::findVisualItem(root, "loadOlderMessagesButton");
        auto* indicator = agent_workspace_test::findVisualItem(root, "olderHistoryIndicator");
        QVERIFY(header && button && indicator);
        QVERIFY(header->height() > 0 && header->height() <= 24);
        QVERIFY(button->isVisible());
        QVERIFY(button->height() <= 24);
        QVERIFY(!indicator->isVisible());
        QCOMPARE(button->property("text").toString(), "Retry loading history");
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QCOMPARE(requests.size(), 4);
        view.setHistoryState({{"hasOlder", true}, {"loading", true}});
        QVERIFY(header->height() <= 24);
        QVERIFY(indicator->isVisible());
        QVERIFY(!button->isVisible());
        view.setHistoryState(done({}));
        QVERIFY(QMetaObject::invokeMethod(transcript, "noteTranscriptInput"));
        QTest::qWait(60);
        QCOMPARE(requests.size(), 4); // No more older history.
        // The header stays as the mock's 24 px top padding, with nothing in it.
        QCOMPARE(header->height(), 24.0);
        QVERIFY(!indicator->isVisible());
        QVERIFY(!button->isVisible());
    }

    void idlePrefetchSkipsReaderAtNewestMessage() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        QVariantList rows;
        for (int i = 0; i < 40; ++i)
            rows.append(streamed(QString("Message %1.\n\n").arg(i), QString::number(i)));
        QSignalSpy requests(&view, &AgentWorkspaceView::olderMessagesRequested);
        view.resetMessages(rows, "idle-prefetch-tail");
        view.setHistoryState({{"hasOlder", true}});
        auto* transcript =
            view.quickView()->rootObject()->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        QVERIFY(transcript->property("followTail").toBool());
        // An older page could push the window over budget and trim the newest rows away.
        QTest::qWait(1500);
        QCOMPARE(requests.size(), 0);
    }

    void olderPageTakingOverGroupKeepsReaderNearBottom() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        // The window opens on the tail of a turn's activity group; the reader
        // is a few pixels above the bottom.
        QVariantList rows;
        for (int i = 0; i < 4; ++i)
            rows.append(activityRow(QString("c%1").arg(i), "turn-9"));
        for (int i = 0; i < 60; ++i)
            rows.append(streamed(QString("Answer %1.\n\n").arg(i).repeated(1 + i % 4),
                                 QString("a%1").arg(i)));
        // The newest answer is taller than the viewport; the reader is inside it.
        rows.append(streamed(QString("A long closing paragraph.\n\n").repeated(60), "giant"));
        view.resetMessages(rows, "older-page-regroup");
        auto* transcript =
            view.quickView()->rootObject()->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        auto* model = transcript->property("messageModel").value<ketplus::AgentMessageModel*>();
        QVERIFY(model);
        auto* geometry = model->geometry();
        transcript->setProperty("followTail", false);
        transcript->setProperty("contentY", transcript->property("contentHeight").toDouble() -
                                                transcript->height() - 13);
        QTest::qWait(50);
        QVERIFY(!transcript->property("followTail").toBool());
        const QString key = transcript->property("readingAnchor").toMap().value("key").toString();
        QVERIFY(!key.isEmpty());
        const auto screenTop = [&] {
            return geometry->top(geometry->rowForKey(key)) -
                   transcript->property("contentY").toDouble();
        };
        const double before = screenTop();

        // An older page whose last turn is the same one: its activity takes
        // over the group, so the model reports a removal and an insertion.
        QVariantList older;
        for (int i = 0; i < 70; ++i)
            older.append(i % 5 == 4 ? activityRow(QString("o%1").arg(i), "turn-9")
                                    : streamed(QString("Older %1.\n\n").arg(i).repeated(1 + i % 3),
                                               QString("o%1").arg(i)));
        older.append(activityRow("o-last", "turn-9"));
        view.prependMessages(older);
        const double settledNow = screenTop();
        QVERIFY2(qAbs(settledNow - before) <= 1.0,
                 qPrintable(
                     QString("reading row moved %1 px during the page").arg(settledNow - before)));
        QTest::qWait(200);
        QVERIFY2(qAbs(screenTop() - before) <= 1.0,
                 qPrintable(
                     QString("reading row moved %1 px after the page").arg(screenTop() - before)));
    }

    void boundedPageChangingBothEdgesKeepsReadingLine() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        QVariantList all;
        for (int i = 0; i < 600; ++i)
            all.append(
                streamed(QString("Row %1.\n\n").arg(i).repeated(1 + i % 4), QString("r%1").arg(i)));
        view.resetMessages(all.mid(200, 300), "bounded-edges");
        auto* transcript =
            view.quickView()->rootObject()->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        transcript->setProperty("idlePrefetchDone", true);
        auto* model = transcript->property("messageModel").value<ketplus::AgentMessageModel*>();
        QVERIFY(model);
        auto* geometry = model->geometry();
        transcript->setProperty("followTail", false);
        transcript->setProperty("contentY", geometry->top(geometry->rowForKey("r350")));
        QTest::qWait(100);
        const auto key = transcript->property("readingAnchor").toMap().value("key").toString();
        QVERIFY(!key.isEmpty());
        const auto screenTop = [&] {
            return geometry->top(geometry->rowForKey(key)) -
                   transcript->property("contentY").toDouble();
        };
        const double before = screenTop();
        for (const int first : {150, 220, 180}) {
            view.replaceMessageWindow(all.mid(first, 300));
            QTest::qWait(100);
            QVERIFY(geometry->rowForKey(key) >= 0);
            QVERIFY2(qAbs(screenTop() - before) <= 1.0,
                     qPrintable(QString("reader moved %1 px after window %2")
                                    .arg(screenTop() - before)
                                    .arg(first)));
        }
    }

    void underfilledTailLoadsOlderHistory() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        // The newest page is a few rows after a compaction: nothing to scroll,
        // so no reader input can ever ask for what is above it.
        const QVariantList rows{streamed("After compaction.", "0"), streamed("Short reply.", "1")};
        QSignalSpy requests(&view, &AgentWorkspaceView::olderMessagesRequested);
        view.resetMessages(rows, "underfilled-tail");
        view.setHistoryState({{"hasOlder", true}});
        auto* transcript =
            view.quickView()->rootObject()->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        QVERIFY(transcript->property("followTail").toBool());
        QTRY_COMPARE(requests.size(), 1);
    }

    void idlePrefetchLoadsOnePageWithoutMovingViewport() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        QVariantList rows;
        // Taller than the prefetch threshold above the reader, so only the idle pass loads.
        for (int i = 0; i < 40; ++i)
            rows.append(streamed(QString("Message %1.\n\n").arg(i).repeated(2 + i % 3),
                                 QString::number(i)));
        QSignalSpy requests(&view, &AgentWorkspaceView::olderMessagesRequested);
        view.resetMessages(rows, "idle-prefetch");
        auto* root = view.quickView()->rootObject();
        auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        // A reader who has left the newest message, still near it.
        transcript->setProperty("followTail", false);
        transcript->setProperty("idlePrefetchDone", false);
        view.setHistoryState({{"hasOlder", true}});
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 3000);
        const auto fromEnd = [&] {
            return transcript->property("originY").toDouble() +
                   transcript->property("contentHeight").toDouble() - transcript->height() -
                   transcript->property("contentY").toDouble();
        };
        QTRY_VERIFY(qAbs(fromEnd()) < 2);
        view.setHistoryState({{"hasOlder", true}, {"loading", true}});
        QVariantList older;
        for (int i = 0; i < 20; ++i)
            older.append(
                streamed(QString("Older %1.\n\n").arg(i).repeated(2), QString("older-%1").arg(i)));
        view.prependMessages(older);
        view.setHistoryState({{"hasOlder", true}, {"requestToken", requests.last().at(2)}});
        QTest::qWait(1200);
        QCOMPARE(requests.size(), 1); // Exactly one background page, no chain.
        QVERIFY2(qAbs(fromEnd()) < 2,
                 qPrintable(QString("Idle prefetch moved the viewport by %1 px").arg(fromEnd())));
    }

    void newerPagesAppendWhenSupportedAndFallBackToJump() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        QVariantList rows;
        for (int i = 0; i < 240; ++i)
            rows.append(streamed("Historical message.", QString::number(i)));
        view.resetMessages(rows, "prefetch-newer");
        auto* root = view.quickView()->rootObject();
        auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        QSignalSpy newer(&view, &AgentWorkspaceView::newerMessagesRequested);
        QSignalSpy latest(&view, &AgentWorkspaceView::latestMessagesRequested);
        QSignalSpy older(&view, &AgentWorkspaceView::olderMessagesRequested);
        const auto placeBelow = [&](double below) {
            transcript->setProperty("followTail", false);
            transcript->setProperty("contentY",
                                    transcript->property("originY").toDouble() +
                                        transcript->property("contentHeight").toDouble() -
                                        transcript->height() - below);
            QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
        };

        // Legacy daemon: no after_cursor, so the bottom is handled by Jump to latest.
        view.setHistoryState({{"hasNewer", true}, {"canLoadNewer", false}});
        placeBelow(400);
        QTest::qWait(80);
        QCOMPARE(newer.size(), 0);
        // The bottom of a trimmed window is not the live tail even when it cannot page.
        placeBelow(0);
        QTest::qWait(40);
        QVERIFY(!transcript->property("followTail").toBool());
        auto* jump = agent_workspace_test::findVisualItem(root, "jumpToLatestButton");
        QVERIFY(jump && jump->isVisible());
        QVERIFY(QMetaObject::invokeMethod(jump, "clicked"));
        QCOMPARE(latest.size(), 1);
        // A jump from a trimmed window settles over several frames; placing the view before
        // it finishes lets the jump pull it back to the bottom and prefetch newer rows.
        QTRY_VERIFY(!root->property("settlingTranscript").toBool());

        view.setHistoryState({{"hasNewer", true}, {"canLoadNewer", true}});
        placeBelow(transcript->property("prefetchThreshold").toDouble() + 400);
        QTest::qWait(60);
        QCOMPARE(newer.size(), 0);
        placeBelow(400);
        QTRY_COMPARE(newer.size(), 1);
        QVERIFY(!transcript->property("followTail").toBool()); // Not the live tail yet.
        // An unrelated state update (no matching token) neither completes the newer
        // request nor backs off either direction.
        view.setHistoryState({{"hasNewer", true}, {"canLoadNewer", true}, {"hasOlder", true}});
        QTest::qWait(40);
        QVERIFY(transcript->property("historyRequestPending").toBool());
        QCOMPARE(transcript->property("historyRequestDirection").toString(), "newer");
        QVERIFY(!transcript->property("olderPrefetchBackoff").toBool());
        QVERIFY(!transcript->property("newerPrefetchBackoff").toBool());
        view.setHistoryState({{"hasNewer", true}, {"canLoadNewer", true}, {"loading", true}});
        QVariant captured;
        QMetaObject::invokeMethod(root, "captureTranscriptPosition",
                                  Q_RETURN_ARG(QVariant, captured));
        const QString key = captured.toMap().value("key").toString();
        auto* row = delegateForKey(transcript, key);
        QVERIFY(row);
        const double y = row->mapToItem(transcript, QPointF()).y();
        // Newer page appended, oldest rows trimmed (RetainedEdge::Newest).
        QVariantList window = rows.mid(30);
        for (int i = 0; i < 30; ++i)
            window.append(streamed("Newer message.", QString("newer-%1").arg(i)));
        QSignalSpy counts(view.messageModel(), SIGNAL(countChanged()));
        view.replaceMessageWindow(window);
        QCOMPARE(counts.size(), 2); // One removal and one batched insertion.
        view.setHistoryState({{"hasOlder", true}, {"requestToken", newer.last().at(2)}});
        QTest::qWait(80);
        QCOMPARE(view.messageModel()->sourceCount(), 240);
        QCOMPARE(view.messageModel()->source(0).value("messageKey").toString(), "30");
        QCOMPARE(view.messageModel()->source(239).value("messageKey").toString(), "newer-29");
        row = delegateForKey(transcript, key);
        QVERIFY(row);
        QVERIFY2(qAbs(row->mapToItem(transcript, QPointF()).y() - y) <= 2,
                 qPrintable(QString("Reading row moved from %1 to %2 px after newer page")
                                .arg(y)
                                .arg(row->mapToItem(transcript, QPointF()).y())));
        QCOMPARE(newer.size(), 1);
        Q_UNUSED(older);
    }

    // Compact rows in a tall viewport: an older page that trims the newest rows leaves the
    // bottom buffer short, and a newer page that trims the oldest leaves the top short.
    // Without input the view must not ping-pong between the two directions.
    void trimmedPagesDoNotPingPongWithoutInput() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 1400);
        view.show();
        constexpr int total = 300;
        constexpr int windowRows = 60;
        constexpr int pageRows = 20;
        QVariantList history;
        for (int i = 0; i < total; ++i)
            history.append(streamed(QString("Compact row %1.").arg(i), QString::number(i)));
        int from = 120;
        view.resetMessages(history.mid(from, windowRows), "prefetch-ping-pong");
        auto* root = view.quickView()->rootObject();
        auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        transcript->setProperty("idlePrefetchDone", true);
        QSignalSpy older(&view, &AgentWorkspaceView::olderMessagesRequested);
        QSignalSpy newer(&view, &AgentWorkspaceView::newerMessagesRequested);
        const auto state = [&] {
            return QVariantMap{{"hasOlder", from > 0},
                               {"hasNewer", from + windowRows < total},
                               {"canLoadNewer", from + windowRows < total}};
        };
        view.setHistoryState(state());
        const double threshold = transcript->property("prefetchThreshold").toDouble();
        const auto* geometry = qvariant_cast<QObject*>(transcript->property("geometry"));
        QVERIFY(geometry);
        QTRY_COMPARE_WITH_TIMEOUT(geometry->property("measuredRowCount").toInt(),
                                  geometry->property("rowCount").toInt(), 10000);
        QVERIFY2(transcript->property("contentHeight").toDouble() < 2 * threshold,
                 qPrintable(QString("contentHeight %1 threshold %2")
                                .arg(transcript->property("contentHeight").toDouble())
                                .arg(threshold)));
        transcript->setProperty("followTail", false);
        transcript->setProperty(
            "contentY",
            transcript->property("originY").toDouble() +
                (transcript->property("contentHeight").toDouble() - transcript->height()) / 2);
        QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
        int served = 0;
        int directionChanges = 0;
        QString lastDirection;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 2500 && served < 60) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            if (older.size() + newer.size() <= served)
                continue;
            // Serve exactly one outstanding request (only one may be in flight).
            QVERIFY(older.size() + newer.size() == served + 1);
            const bool isOlder =
                transcript->property("historyRequestDirection").toString() == "older";
            const QVariant token = isOlder ? older.last().at(2) : newer.last().at(2);
            ++served;
            if (!lastDirection.isEmpty() && lastDirection != (isOlder ? "older" : "newer"))
                ++directionChanges;
            lastDirection = isOlder ? "older" : "newer";
            auto loading = state();
            loading.insert("loading", true);
            view.setHistoryState(loading);
            from = isOlder ? std::max(0, from - pageRows)
                           : std::min(total - windowRows, from + pageRows);
            view.replaceMessageWindow(history.mid(from, windowRows));
            auto completed = state();
            completed.insert("requestToken", token);
            view.setHistoryState(completed);
        }
        QVERIFY2(directionChanges <= 1,
                 qPrintable(QString("Prefetch alternated directions %1 times without input "
                                    "(older=%2 newer=%3)")
                                .arg(directionChanges)
                                .arg(older.size())
                                .arg(newer.size())));
        QVERIFY2(served <= total / pageRows,
                 qPrintable(QString("%1 requests without input").arg(served)));
        // Fresh input lifts the trim block again.
        QVERIFY(QMetaObject::invokeMethod(transcript, "noteTranscriptInput"));
        QVERIFY(!transcript->property("olderTrimBlocked").toBool());
        QVERIFY(!transcript->property("newerTrimBlocked").toBool());
    }

    void idlePrefetchWaitsForRunningTurn() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        QVariantList rows;
        for (int i = 0; i < 40; ++i)
            rows.append(streamed(QString("Message %1.\n\n").arg(i), QString::number(i)));
        QSignalSpy requests(&view, &AgentWorkspaceView::olderMessagesRequested);
        view.setSessionState("Streaming", "Working", true);
        view.resetMessages(rows, "idle-prefetch-running");
        view.setHistoryState({{"hasOlder", true}});
        auto* transcript =
            view.quickView()->rootObject()->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        QTest::qWait(1500);
        QCOMPARE(requests.size(), 0); // No prepend under a streaming turn.
        QVERIFY(!transcript->property("idlePrefetchDone").toBool());
        view.setSessionState("Ready", "Ready", false);
        transcript->setProperty("followTail", false); // Idle prefetch skips the newest message.
        QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 3000); // Re-armed after the turn.
    }

    void offscreenMarkdownKeepsGeometryAndFlushesNewestSource() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        auto* root = view.quickView()->rootObject();
        auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);

        const auto original = QString("Original **paragraph**.\n\n").repeated(8);
        QVariantList history;
        for (int i = 0; i < 8; ++i)
            history.append(streamed(original, QString::number(i)));
        view.resetMessages(history, "viewport");
        transcript->setProperty("cacheBuffer", 100000);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        QTest::qWait(100);
        transcript->setProperty("followTail", false);
        QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtBeginning"));
        QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = bodyForKey(root, "0")));
        QTRY_COMPARE(body->property("renderedSource").toString(), original);
        auto* initialLatest = agent_workspace_test::findVisualItem(root, "jumpToLatestButton");
        QVERIFY(initialLatest);
        QVERIFY(QMetaObject::invokeMethod(initialLatest, "clicked"));
        QTRY_VERIFY(!body->property("renderInViewport").toBool());
        QCOMPARE(body->property("renderedSource").toString(), original);
        auto* quickDocument = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
        QVERIFY(quickDocument);
        QSignalSpy documentChanges(quickDocument->textDocument(), &QTextDocument::contentsChanged);
        const auto height = body->height();
        QString updated;
        for (int i = 1; i <= 100; ++i) {
            updated = QString("Newest **paragraph**.\n\n").repeated(i);
            auto item = streamed(updated, "0");
            item["meta"] = "Codex";
            view.updateMessage(0, item);
        }
        QCOMPARE(view.messageModel()->get(0).value("body").toString(), updated);
        QCOMPARE(body->property("renderedSource").toString(), original);
        QCOMPARE(body->height(), height);
        QCOMPARE(documentChanges.size(), 0);

        transcript->setProperty("followTail", false);
        QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtBeginning"));
        QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
        QTRY_VERIFY2(body->property("renderInViewport").toBool(),
                     qPrintable(QStringLiteral("y=%1 height=%2 contentY=%3 viewHeight=%4 pinned=%5")
                                    .arg(body->parentItem()->parentItem()->y())
                                    .arg(body->parentItem()->parentItem()->height())
                                    .arg(transcript->property("contentY").toDouble())
                                    .arg(transcript->height())
                                    .arg(body->property("readerSourcePinned").toBool())));
        QTRY_COMPARE(body->property("renderedSource").toString(), updated);
        QVERIFY(documentChanges.size() > 0);
        QVERIFY(body->height() > height);
        QVERIFY(!transcript->property("followTail").toBool());
        QVERIFY(QMetaObject::invokeMethod(body, "selectAll"));
        QCOMPARE(body->property("selectedText").toString().count("Newest paragraph."), 100);
        QVERIFY(QMetaObject::invokeMethod(body, "deselect"));
        auto* latest = root->findChild<QQuickItem*>("jumpToLatestButton");
        QVERIFY(latest);
        QVERIFY(QMetaObject::invokeMethod(latest, "clicked"));
        QTRY_VERIFY(transcript->property("atYEnd").toBool());
    }

    void emptyStreamingTailAppliesBodyWithoutSwitching() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        view.setSessionState("Streaming", "Working", true);
        auto* root = view.quickView()->rootObject();
        auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);

        // First paint: streaming meta with an empty body — the failure mode
        // that used to freeze a blank document until conversation switch.
        view.resetMessages({streamed(QString(), "live")}, "empty-stream");
        QTRY_COMPARE(transcript->opacity(), 1.0);
        transcript->setProperty("followTail", true);
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = bodyForKey(root, "live")));
        QTRY_VERIFY(body->property("sourceInitialized").toBool());
        QCOMPARE(body->property("renderedSource").toString(), QString());

        QString current;
        for (int i = 1; i <= 8; ++i) {
            current += QStringLiteral("Streamed paragraph %1.\n\n").arg(i);
            view.updateMessage(0, streamed(current, "live"));
            QTRY_COMPARE(body->property("renderedSource").toString(), current);
        }
        QVERIFY(!body->property("readerSourcePinned").toBool());
        auto final = streamed(current, "live");
        final["meta"] = "Codex";
        view.updateMessage(0, final);
        view.setSessionState("Done", "Ready", false);
        QTRY_COMPARE(body->property("renderedSource").toString(), current);
    }

    void answerAfterCollapsedActivitiesUpdatesWithoutSwitching_data() {
        QTest::addColumn<int>("completion");
        QTest::newRow("streaming") << 0;
        QTest::newRow("completed") << 1;
        QTest::newRow("completed-with-pending-delta") << 2;
    }

    void answerAfterCollapsedActivitiesUpdatesWithoutSwitching() {
        QFETCH(int, completion);
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        QVariantList history;
        for (int i = 0; i < 4; ++i)
            history.append(QVariantMap{{"kind", "tool"},
                                       {"subtype", "fileRead"},
                                       {"turnId", "previous-turn"},
                                       {"messageKey", QString("read-%1").arg(i)},
                                       {"title", "Read source.cpp"},
                                       {"status", "completed"}});
        auto initialAnswer = streamed(QString(), "live");
        initialAnswer["turnId"] = "current-turn";
        history.append(initialAnswer);
        view.resetMessages(history, "collapsed-history");
        view.setSessionState("Current turn", "Working", true);
        auto* model = view.messageModel();
        const int sourceIndex = history.size() - 1;
        QCOMPARE(model->sourceCount(), history.size());
        QTRY_COMPARE(model->count(), 2); // One activity group, then the answer.
        QVERIFY(sourceIndex >= model->count());
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = bodyForKey(view.quickView()->rootObject(), "live")));
        QTRY_VERIFY(body->property("sourceInitialized").toBool());
        if (completion == 2)
            view.updateMessage(sourceIndex, streamed("Queued prefix", "live"));
        auto answer = streamed("The **answer** is visible.", "live");
        answer["turnId"] = "current-turn";
        if (completion != 0)
            answer["meta"] = "Codex";
        view.updateMessage(sourceIndex, answer);
        if (completion != 0)
            view.setSessionState("Current turn", "Ready", false);
        QTRY_COMPARE_WITH_TIMEOUT(model->source(sourceIndex).value("body"), answer.value("body"),
                                  1000);
        QTRY_COMPARE(body->property("renderedSource"), answer.value("body"));
        auto* document = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
        QVERIFY(document);
        QTRY_COMPARE(document->textDocument()->toPlainText(), "The answer is visible.");
        QTest::qWait(150); // A queued prefix must not overwrite the final body.
        QCOMPARE(model->source(sourceIndex).value("body"), answer.value("body"));
    }

    void warmSwitchKeepsMixedImageRowHeights_data() {
        QTest::addColumn<int>("attachments");
        QTest::newRow("single-image") << 1;
        QTest::newRow("thumbnail-grid") << 4;
    }

    void warmSwitchKeepsMixedImageRowHeights() {
        QFETCH(int, attachments);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVariantList rows;
        for (int i = 0; i < 120; ++i) {
            QImage image(i % 2 ? QSize(100, 900) : QSize(900, 100), QImage::Format_RGB32);
            image.fill(Qt::cyan);
            const auto path = directory.filePath(QString::number(i) + ".png");
            QVERIFY(image.save(path));
            QVariantList sources;
            for (int attachment = 0; attachment < attachments; ++attachment)
                sources.append(QUrl::fromLocalFile(path).toString());
            rows.append(QVariantMap{
                {"messageKey", QString("image-%1").arg(i)},
                {"kind", "assistant"},
                {"body",
                 QString(
                     "## Image dimensions vary\n\n- **Detail** and description with `code`.\n\n")
                     .repeated(12)},
                {"images", sources}});
        }
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        view.resetMessages(rows, "mixed-images");
        auto* transcript =
            view.quickView()->rootObject()->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        transcript->setProperty("cacheBuffer", 0);
        QTest::qWait(800);
        auto* geometry = view.messageModel()->geometry();
        QTRY_COMPARE_WITH_TIMEOUT(geometry->measuredRowCount(), geometry->rowCount(), 10000);
        QVERIFY(geometry->totalHeight() > 1000);
        transcript->setProperty("followTail", false);
        QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtIndex", Q_ARG(int, 55),
                                          Q_ARG(int, 0)));
        QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
        QTest::qWait(100);
        QVariant captured;
        QVERIFY(QMetaObject::invokeMethod(view.quickView()->rootObject(),
                                          "captureTranscriptPosition",
                                          Q_RETURN_ARG(QVariant, captured)));
        const auto key = captured.toMap().value("key").toString();
        auto* anchorRow = delegateForKey(transcript, key);
        QVERIFY(anchorRow);
        const auto screenY = anchorRow->mapToItem(transcript, QPointF()).y();
        const auto before = geometry->totalHeight();
        for (int cycle = 0; cycle < 4; ++cycle) {
            view.resetMessages({streamed("Other conversation.", "other")}, "other");
            QTest::qWait(60);
            view.resetMessages(rows, "mixed-images");
            double minimum = geometry->totalHeight();
            double maximumMotion = 0;
            bool anchorVisible = false;
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 350) {
                QCoreApplication::processEvents();
                minimum = std::min(minimum, geometry->totalHeight());
                if (auto* current = delegateForKey(transcript, key)) {
                    anchorVisible = true;
                    maximumMotion =
                        std::max(maximumMotion,
                                 qAbs(current->mapToItem(transcript, QPointF()).y() - screenY));
                }
                QTest::qWait(5);
            }
            QVERIFY(anchorVisible);
            QVERIFY2(maximumMotion < 2,
                     qPrintable(QString("Warm anchor moved %1 px").arg(maximumMotion)));
            QVERIFY2(
                qAbs(minimum - before) < 2,
                qPrintable(QString("Warm images shrank from %1 to %2").arg(before).arg(minimum)));
        }
    }

    void switchingReleasesHiddenDocumentsAndRestoresLatestSource() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        view.resetMessages({streamed("Old **body**.", "a")}, "a");
        auto* root = view.quickView()->rootObject();
        QQuickItem* body = nullptr;
        QTRY_VERIFY((body = bodyForKey(root, "a")));
        auto* model = view.messageModel();
        view.resetMessages({streamed("Other conversation.", "b")}, "b");
        QTRY_VERIFY(bodyForKey(root, "b"));
        QVERIFY(!bodyForKey(root, "a"));
        QVERIFY(view.messageModel() != model);
        QCOMPARE(view.benchmarkWarmTranscriptCount(), 1);
        view.applyTheme(agent_workspace_test::testPalette());
        const auto updated = streamed("Latest **body** with [link](src/main.cpp).", "a");
        view.resetMessages({updated}, "a");
        QCOMPARE(view.messageModel(), model);
        QTRY_VERIFY((body = bodyForKey(root, "a")));
        QTRY_COMPARE(body->property("renderedSource").toString(), updated.value("body").toString());
        QVERIFY(!bodyForKey(root, "b"));
        auto* document = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
        QVERIFY(document);
        QCOMPARE(document->textDocument()->toPlainText(), "Latest body with link.");
    }

    void partialBodyIsLiteralUntilItsPrefixIsAvailable() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        const QString source = "**Do not interpret a suffix**\n\n[link](src/main.cpp)\n\n"
                               "```cpp\nint answer = 42;\n```";
        auto item = streamed(source, "partial");
        item["meta"] = "Codex";
        item["partialBody"] = true;
        view.resetMessages({item}, "partial");
        QTest::qWait(100);
        auto* root = view.quickView()->rootObject();
        auto* body = bodyForKey(root, "partial");
        QVERIFY(body);
        auto* quickDocument = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
        QVERIFY(quickDocument);
        QVERIFY(body->property("renderedPartialBody").toBool());
        QCOMPARE(quickDocument->textDocument()->toPlainText(), source);
        QVERIFY(view.messageCodeBlocks(quickDocument).isEmpty());

        item["partialBody"] = false;
        view.updateMessage(0, item);
        QTRY_VERIFY(!body->property("renderedPartialBody").toBool());
        QCOMPARE(quickDocument->textDocument()->toPlainText(),
                 "Do not interpret a suffix\nlink\nint answer = 42;");
        QVERIFY(!view.messageCodeBlocks(quickDocument).isEmpty());
    }

    void assistantRowsDoNotComputeCommandFileOrUserMetrics() {
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        view.resetMessages({streamed(QString("Many **paragraphs**.\n\n").repeated(1000), "large")},
                           "gating");
        QTest::qWait(100);
        // The first block may be virtualized away; inspect a visible block.
        auto* body =
            agent_workspace_test::findVisualItem(view.quickView()->rootObject(), "messageBody");
        QVERIFY(body);
        auto* delegate = body->parentItem();
        while (delegate && !delegate->property("messageKey").isValid())
            delegate = delegate->parentItem();
        QVERIFY(delegate);
        QCOMPARE(delegate->property("commandText").toString(), "");
        for (const auto* property : {"commandLines", "fileLines"})
            QCOMPARE(
                qvariant_cast<QJSValue>(delegate->property(property)).property("length").toInt(),
                0);
        auto* metrics = delegate->findChild<QObject*>("messageBodyMetrics");
        QVERIFY(metrics);
        QCOMPARE(metrics->property("text").toString(), "");
        QCOMPARE(metrics->property("advanceWidth").toDouble(), 0.0);
    }

    void anchorRelocationIsBoundedAndRejectsAmbiguousMatches() {
        QFile source(":/agent/TranscriptPosition.js");
        QVERIFY(source.open(QIODevice::ReadOnly));
        const auto script = QString::fromUtf8(source.readAll());
        QJSEngine engine;
        QVERIFY(!engine.evaluate(script.mid(script.indexOf('\n') + 1)).isError());
        const auto result = engine.evaluate(R"JS(
            (function() {
                const sample = "A unique reading marker.";
                const old = "head\n" + sample + "\ntail";
                function check(value, prefix, partial, length, position) {
                    let maximumRead = 0;
                    const text = {length: value.length, renderedPrefixCharacters: prefix,
                        renderedPartialBody: partial,
                        getText: function(start, end) {
                            maximumRead = Math.max(maximumRead, end - start);
                            return value.slice(start, end);
                        }};
                    const saved = {position: position, sample: sample, length: length,
                        prefixCharacters: 0, partial: true};
                    return {position: matchingTextPosition(text, saved), maximumRead: maximumRead};
                }
                const prefix = "older ".repeat(100000);
                const literal = check(prefix + old, prefix.length, true, old.length, 5);
                const live = check(old + " appended", 0, true, old.length, 5);
                const rich = check("new heading\n" + old, 12, false, old.length, 5);
                const ambiguousText = sample + " between " + sample;
                const ambiguous = check(ambiguousText, 0, false, ambiguousText.length, 0);
                const distantText = "x".repeat(100000) + sample;
                const distant = check(distantText, 0, false, distantText.length, 0);
                return {literal: literal, live: live, rich: rich, ambiguous: ambiguous,
                    distant: distant, prefixLength: prefix.length};
            })()
        )JS");
        QVERIFY2(!result.isError(), qPrintable(result.toString()));
        QCOMPARE(result.property("literal").property("position").toInt(),
                 result.property("prefixLength").toInt() + 5);
        QCOMPARE(result.property("live").property("position").toInt(), 5);
        QCOMPARE(result.property("rich").property("position").toInt(), 17);
        QCOMPARE(result.property("ambiguous").property("position").toInt(), -1);
        QCOMPARE(result.property("distant").property("position").toInt(), -1);
        for (const auto* name : {"literal", "live", "rich", "ambiguous", "distant"})
            QVERIFY(result.property(name).property("maximumRead").toInt() <= 16384);
    }

    void sameItemHistoryPrefixKeepsVisibleTextAnchor_data() {
        QTest::addColumn<bool>("crlfPrefix");
        QTest::newRow("lf-prefix") << false;
        QTest::newRow("crlf-prefix") << true;
    }

    void sameItemHistoryPrefixKeepsVisibleTextAnchor() {
        QFETCH(bool, crlfPrefix);
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        view.setSessionState("Streaming", "Working", true);
        const QString marker = "Unique reading anchor remains on this screen.";
        QString source;
        for (int i = 0; i < 60; ++i)
            source +=
                i == 20 ? marker + "\n\n" : QString("Paragraph %1 has **formatting**.\n\n").arg(i);
        auto item = streamed(source, "history-item");
        item["meta"] = "Codex";
        item["partialBody"] = true;
        view.resetMessages({item}, "history-prefix");
        auto* root = view.quickView()->rootObject();
        auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        auto* body = bodyForKey(root, "history-item");
        QVERIFY(body);
        auto* quickDocument = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
        QVERIFY(quickDocument);
        const auto markerY = [&] {
            auto* document = quickDocument->textDocument();
            for (auto block = document->begin(); block.isValid(); block = block.next())
                if (block.text() == marker)
                    return body
                        ->mapToItem(
                            transcript,
                            QPointF(0, document->documentLayout()->blockBoundingRect(block).top()))
                        .y();
            return -99999.0;
        };
        QVERIFY(markerY() > -99999);
        transcript->setProperty("followTail", false);
        transcript->setProperty("contentY",
                                transcript->property("contentY").toDouble() + markerY() + 7);
        QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
        QTest::qWait(100);
        const auto before = markerY();
        QVERIFY(qAbs(before + 7) < 1);

        const auto prefix =
            QString(crlfPrefix ? "An older paragraph.\r\n\r\n" : "An older paragraph.\n\n")
                .repeated(40);
        const auto prefixCharacters = QString(prefix).replace("\r\n", "\n").size();
        item["body"] = prefix + source;
        item["bodyPrefixCharacters"] = prefixCharacters;
        view.updateMessage(0, item);
        QTRY_VERIFY(qAbs(markerY() - before) < 1);
        QVERIFY(!transcript->property("followTail").toBool());

        const QString liveSuffix = "Concurrent live output while loading older text.\n\n";
        item["body"] = prefix + source + liveSuffix;
        view.updateMessage(0, item);
        QTest::qWait(100);
        QCOMPARE(body->property("renderedSource").toString(), prefix + source);
        QVERIFY(qAbs(markerY() - before) < 1);

        const QString heading = "# Complete prefix\n\n";
        item["body"] = heading + prefix + source + liveSuffix;
        item["bodyPrefixCharacters"] = heading.size() + prefixCharacters;
        item["partialBody"] = false;
        view.updateMessage(0, item);
        QTRY_VERIFY(qAbs(markerY() - before) < 1);
        QVERIFY(!transcript->property("followTail").toBool());
    }

    void middleReaderRemainsStableThroughStreamAndCompletion_data() {
        QTest::addColumn<bool>("readingOlder");
        QTest::addColumn<bool>("scrollbarPressed");
        QTest::newRow("inside-streaming-message") << false << false;
        QTest::newRow("inside-older-message") << true << false;
        QTest::newRow("scrollbar-middle-message") << false << true;
    }

    void middleReaderRemainsStableThroughStreamAndCompletion() {
        QFETCH(bool, readingOlder);
        QFETCH(bool, scrollbarPressed);
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1000, 700);
        view.show();
        view.setSessionState("Streaming", "Working", true);
        const QString marker = "Reading this exact middle paragraph should stay still.";
        QString source;
        for (int i = 0; i < 60; ++i)
            source += i == 25 ? marker + "\n\n"
                              : QString("Paragraph %1 contains [a reference link][target] and "
                                        "enough text to exercise wrapping on a normal window.\n\n")
                                    .arg(i);
        QVariantList history{streamed(source, "reading")};
        if (readingOlder)
            history.append(streamed("Tail output.", "tail"));
        auto* root = view.quickView()->rootObject();
        auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
        QVERIFY(transcript);
        transcript->setProperty("cacheBuffer", 100000);
        view.resetMessages(history, "middle-reader");
        QTRY_COMPARE(transcript->opacity(), 1.0);
        auto* body = bodyForKey(root, "reading");
        QVERIFY(body);
        auto* quickDocument = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
        QVERIFY(quickDocument);
        const auto markerY = [&] {
            auto* document = quickDocument->textDocument();
            for (auto block = document->begin(); block.isValid(); block = block.next())
                if (block.text() == marker)
                    return body
                        ->mapToItem(
                            transcript,
                            QPointF(0, document->documentLayout()->blockBoundingRect(block).top()))
                        .y();
            return -99999.0;
        };
        QVERIFY(markerY() > -99999);
        transcript->setProperty("followTail", false);
        auto* scrollbar = qvariant_cast<QObject*>(transcript->property("verticalScrollBar"));
        QVERIFY(scrollbar);
        if (scrollbarPressed)
            QVERIFY(scrollbar->setProperty("pressed", true));
        transcript->setProperty("contentY",
                                transcript->property("contentY").toDouble() + markerY() + 9);
        if (!scrollbarPressed)
            QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
        QTest::qWait(100);
        const auto before = markerY();
        QVERIFY(qAbs(before + 9) < 1);
        QSignalSpy documentChanges(quickDocument->textDocument(), &QTextDocument::contentsChanged);
        const int row = readingOlder ? 1 : 0;
        const QString key = readingOlder ? "tail" : "reading";
        QString current = readingOlder ? "Tail output." : source;
        for (int i = 0; i < 25; ++i) {
            current += "More streamed words.\n\n";
            view.updateMessage(row, streamed(current, key));
            QTest::qWait(10);
            QVERIFY(qAbs(markerY() - before) < 1);
        }
        // This definition changes the interpretation of every prior reference.
        current += "\n[target]: https://example.test/documentation\n";
        auto final = streamed(current, key);
        final["meta"] = "Codex";
        view.updateMessage(row, final);
        view.setSessionState("Done", "Ready", false);
        if (scrollbarPressed)
            QVERIFY(scrollbar->setProperty("pressed", false));
        QTest::qWait(100);
        QCOMPARE(view.messageModel()->get(row).value("body").toString(), current);
        QVERIFY(qAbs(markerY() - before) < 1);
        QCOMPARE(documentChanges.size(), 0);
        QCOMPARE(body->property("renderedSource").toString(), source);
        QVERIFY(!transcript->property("followTail").toBool());

        auto* latest = root->findChild<QQuickItem*>("jumpToLatestButton");
        QVERIFY(latest);
        QVERIFY(QMetaObject::invokeMethod(latest, "clicked"));
        QTRY_VERIFY(transcript->property("atYEnd").toBool());
        auto* tail = bodyForKey(root, key);
        QVERIFY(tail);
        QTRY_COMPARE(tail->property("renderedSource").toString(), current);
    }
};

QTEST_MAIN(AgentTranscriptViewportTest)
#include "AgentTranscriptViewportTest.moc"
