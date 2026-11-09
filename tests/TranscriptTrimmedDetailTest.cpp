#include "AgentWorkspaceViewTest.h"
#include "agent/AgentMessageFormatter.h"
#include "agent/AgentMessageModel.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/DaemonTranscriptNotices.h"

#include <QQuickView>
#include <QSignalSpy>
#include <QtTest>

using namespace agent_workspace_test;

void AgentWorkspaceViewTest::trimmedTurnOpensStorageSettings() {
    // A turn end as the daemon projects it once retention has reduced the turn's detail.
    auto old = ketplus::agent_message::daemonEventNotice(
        "turn.completed",
        {{"status", "completed"},
         {"detail_trimmed",
          QJsonObject{{"commands", 38}, {"tool_results", 12}, {"older_than_days", 30}}}},
        "daemon-notice:1");
    old.insert("turnId", "old-turn");
    auto recent = ketplus::agent_message::daemonEventNotice(
        "turn.completed", {{"status", "completed"}}, "daemon-notice:2");
    recent.insert("turnId", "recent-turn");
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1100, 900);
    view.show();
    view.resetMessages({old, recent}, "trimmed-detail");
    auto* root = view.quickView()->rootObject();
    QList<QQuickItem*> notes;
    const auto collect = [&] {
        notes.clear();
        const auto visit = [&](auto&& self, QQuickItem* item) -> void {
            if (!item->isVisible())
                return;
            if (item->objectName() == "transcriptTrimmedDetail")
                notes.append(item);
            for (auto* child : item->childItems())
                self(self, child);
        };
        visit(visit, root);
        return notes.size();
    };
    // Only the trimmed turn carries the note; the recent one still holds its detail.
    QTRY_COMPARE(collect(), 1);
    auto* text = findVisualItem(notes.first(), "transcriptTrimmedDetailText");
    QVERIFY(text);
    QCOMPARE(text->property("text").toString(),
             QStringLiteral("<b>Detail trimmed</b>&nbsp;&nbsp;This turn is older than 30 days: 38 "
                            "commands and 12 tool results were reduced to their summaries."));
    // Above the turn's end line, as in the mock.
    auto* notice = findVisualItem(notes.first()->parentItem(), "transcriptNotice");
    QVERIFY(notice);
    QVERIFY(notes.first()->mapToItem(root, QPointF(0, notes.first()->height())).y() <=
            notice->mapToItem(root, QPointF()).y());

    QSignalSpy settings(&view, &ketplus::AgentWorkspaceView::settingsRequested);
    auto* button = findVisualItem(notes.first(), "transcriptStorageSettings");
    QVERIFY(button && button->isVisible());
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QCOMPARE(settings.count(), 1);
    QCOMPARE(settings.constFirst().constFirst().toString(), QStringLiteral("storage"));
}

// The mock's compaction: a divider with its text and what it means under it, not a banner.
void AgentWorkspaceViewTest::compactionReadsAsAnEventLine() {
    auto row = ketplus::agent_message::activityMessage(
        QJsonObject{{"id", "compaction"}, {"type", "contextCompaction"}, {"status", "completed"}},
        true);
    row.insert("messageKey", "compaction");
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1100, 700);
    view.show();
    view.resetMessages({row}, "compaction");
    auto* root = view.quickView()->rootObject();
    QQuickItem* line = nullptr;
    QTRY_VERIFY((line = findVisibleItem(root, "transcriptEventLine")) != nullptr);
    QCOMPARE(findVisualItem(line, "transcriptEventDivider")->property("text").toString(),
             QStringLiteral("Context compacted"));
    QCOMPARE(findVisualItem(line, "transcriptEventDetail")->property("text").toString(),
             QStringLiteral("Older steps are summarised; the conversation continues."));
}
