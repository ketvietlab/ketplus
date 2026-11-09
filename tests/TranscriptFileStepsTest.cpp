#include "AgentWorkspaceViewTest.h"
#include "agent/AgentMessageFormatter.h"
#include "agent/AgentMessageModel.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/TranscriptNormalizer.h"

#include <QJsonArray>
#include <QQuickView>
#include <QStringList>
#include <QtTest>

using namespace agent_workspace_test;

namespace {

QVariantMap fileEdit(const QString& id, const QJsonArray& changes) {
    auto row = ketplus::agent_message::fileChangeMessage(
        {{"id", id}, {"type", "fileChange"}, {"status", "completed"}, {"changes", changes}}, true);
    row.insert("messageKey", id);
    row.insert("turnId", "diff-turn");
    return row;
}

QJsonObject change(const QString& path, const QString& type, const QString& diff = {}) {
    QJsonObject entry{{"path", path}, {"kind", QJsonObject{{"type", type}}}};
    if (!diff.isEmpty())
        entry.insert("diff", diff);
    return entry;
}

void collectVisible(QQuickItem* item, const QString& name, QList<QQuickItem*>& found) {
    if (!item->isVisible())
        return;
    if (item->objectName() == name)
        found.append(item);
    for (auto* child : item->childItems())
        collectVisible(child, name, found);
}

QList<QQuickItem*> visibleItems(QQuickItem* root, const QString& name) {
    QList<QQuickItem*> found;
    collectVisible(root, name, found);
    return found;
}

QStringList lineKinds(QQuickItem* diff) {
    QStringList kinds;
    auto* lines = findVisualItem(diff, "transcriptFileDiffLines");
    for (const auto& line : lines->property("lines").toList())
        kinds.append(line.toMap().value("kind").toString());
    return kinds;
}

} // namespace

void AgentWorkspaceViewTest::fileStepsOpenTheirDiffInline() {
    QStringList longFile;
    for (int line = 0; line < 450; ++line)
        longFile.append(QStringLiteral("line %1").arg(line));
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1100, 900);
    view.show();
    view.resetMessages(
        {fileEdit("single", {change("/worktree/README.md", "update", "@@ -1 +1 @@\n-old\n+new\n")}),
         fileEdit("several",
                  {change("/worktree/src/Edited.cpp", "update", "@@ -1 +1,2 @@\n-a\n+b\n+c\n"),
                   change("/worktree/src/New.h", "add", "first\nsecond\n"),
                   change("/worktree/src/Long.txt", "add", longFile.join('\n')),
                   change("/worktree/src/Old.cpp", "delete")})},
        "inline-diffs");
    auto* model = view.messageModel();
    QTRY_COMPARE(model->count(), 1);
    model->setGroupExpanded(model->get(0).value("groupKey").toString(), true);
    QTRY_COMPARE(model->count(), 2);
    auto* root = view.quickView()->rootObject();
    QList<QQuickItem*> steps;
    QTRY_VERIFY((steps = visibleItems(root, "transcriptFileStep")).size() == 4);
    QVERIFY(visibleItems(root, "transcriptFileDiff").isEmpty());
    QVERIFY(steps.at(0)->property("expandable").toBool());
    // A step without a diff has nothing to open below it.
    QVERIFY(!steps.at(3)->property("expandable").toBool());
    QVERIFY(!findVisualItem(steps.at(3), "transcriptFileStepChevron")->isVisible());

    // Opening a step draws its hunk below it instead of asking for the viewer.
    QSignalSpy viewer(&view, &ketplus::AgentWorkspaceView::fileDiffRequested);
    QVERIFY(QMetaObject::invokeMethod(steps.at(0), "clicked"));
    QList<QQuickItem*> diffs;
    QTRY_VERIFY((diffs = visibleItems(root, "transcriptFileDiff")).size() == 1);
    QCOMPARE(lineKinds(diffs.at(0)), (QStringList{"hunk", "remove", "add", "add"}));
    QTRY_VERIFY(diffs.at(0)->mapToItem(root, QPointF()).y() >=
                steps.at(0)->mapToItem(root, QPointF(0, steps.at(0)->height())).y());
    // The open diff pushes the next file down instead of drawing over it.
    QTRY_VERIFY(steps.at(1)->mapToItem(root, QPointF()).y() >=
                diffs.at(0)->mapToItem(root, QPointF(0, diffs.at(0)->height())).y());
    QCOMPARE(viewer.count(), 0);
    QVERIFY(steps.at(0)->property("open").toBool());

    // A created file's diff is its content, every line added.
    QVERIFY(QMetaObject::invokeMethod(steps.at(1), "clicked"));
    QTRY_VERIFY((diffs = visibleItems(root, "transcriptFileDiff")).size() == 2);
    QCOMPARE(lineKinds(diffs.at(1)), (QStringList{"add", "add"}));

    // A long diff stops at its bound and offers the full diff in the viewer.
    QVERIFY(QMetaObject::invokeMethod(steps.at(2), "clicked"));
    QTRY_VERIFY((diffs = visibleItems(root, "transcriptFileDiff")).size() == 3);
    QCOMPARE(lineKinds(diffs.at(2)).size(), 400);
    auto* more = findVisualItem(diffs.at(2), "transcriptFullDiff");
    QVERIFY(more && more->isVisible());
    QCOMPARE(more->property("text").toString(),
             QStringLiteral("50 more lines · Open the full diff"));
    QVERIFY(QMetaObject::invokeMethod(more, "clicked"));
    QCOMPARE(viewer.count(), 1);
    QCOMPARE(viewer.constLast().constFirst().toString(), QStringLiteral("/worktree/src/Long.txt"));
    // A whole diff still offers the viewer.
    auto* viewerButton = findVisualItem(diffs.at(0), "transcriptFullDiff");
    QVERIFY(viewerButton->isVisible());
    QCOMPARE(viewerButton->property("text").toString(), QStringLiteral("Open in the diff viewer"));
    QVERIFY(QMetaObject::invokeMethod(viewerButton, "clicked"));
    QCOMPARE(viewer.count(), 2);
    QCOMPARE(viewer.constLast().constFirst().toString(),
             QStringLiteral("/worktree/src/Edited.cpp"));

    // Clicking again closes it; the open diffs stay with the row.
    QVERIFY(QMetaObject::invokeMethod(steps.at(0), "clicked"));
    QTRY_COMPARE(visibleItems(root, "transcriptFileDiff").size(), 2);
    QVERIFY(!steps.at(0)->property("open").toBool());

    // A one-file edit opens on its diff instead of a one-row card.
    QQuickItem* single = nullptr;
    for (auto* row : visibleItems(root, "transcriptActivityRow")) {
        if (row->property("messageKey").toString() == QStringLiteral("single"))
            single = row;
    }
    QVERIFY(single);
    QVERIFY(QMetaObject::invokeMethod(single, "toggleDetails"));
    QTRY_VERIFY(!visibleItems(single, "transcriptFileDiff").isEmpty());
    QCOMPARE(lineKinds(visibleItems(single, "transcriptFileDiff").constFirst()),
             (QStringList{"hunk", "remove", "add"}));
    QVERIFY(visibleItems(single, "changedFileRow").isEmpty());
    // Without the card, its line keeps the file actions.
    auto* header = findVisibleItem(single, "transcriptActivityHeader");
    QVERIFY(header);
    QTest::mouseClick(view.quickView(), Qt::RightButton, Qt::NoModifier,
                      header->mapToScene(QPointF(30, header->height() / 2)).toPoint());
    auto* menu = single->findChild<QObject*>("changedFileContextMenu");
    QVERIFY(menu);
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(menu->findChild<QObject*>("copyChangedFilePathAction")->property("enabled").toBool());
    QVERIFY(menu->findChild<QObject*>("openChangedFileAction"));
    QMetaObject::invokeMethod(menu, "close");
}

void AgentWorkspaceViewTest::claudeEditOpensItsDiffInline() {
    // Claude's Edit, through the normalizer and the model as a session delivers it.
    const QJsonObject item{{"id", "claude-edit"},
                           {"type", "dynamicToolCall"},
                           {"tool", "Edit"},
                           {"status", "completed"},
                           {"arguments", QJsonObject{{"file_path", "/worktree/src/Claude.cpp"},
                                                     {"old_string", "before"},
                                                     {"new_string", "after"}}}};
    auto row = ketplus::ActivityNormalizer::normalize(item, ketplus::TranscriptPhase::Completed)
                   .toVariantMap();
    row.insert("messageKey", "claude-edit");
    row.insert("turnId", "claude-turn");
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1100, 900);
    view.show();
    view.resetMessages({row}, "claude-inline-diff");
    auto* root = view.quickView()->rootObject();
    QQuickItem* edit = nullptr;
    QTRY_VERIFY((edit = findVisibleItem(root, "transcriptActivityRow")) != nullptr);
    QCOMPARE(edit->property("subtype").toString(), QStringLiteral("fileChange"));
    QVERIFY(QMetaObject::invokeMethod(edit, "toggleDetails"));
    QTRY_VERIFY(!visibleItems(edit, "transcriptFileDiff").isEmpty());
    QCOMPARE(lineKinds(visibleItems(edit, "transcriptFileDiff").constFirst()),
             (QStringList{"hunk", "remove", "add"}));
}
