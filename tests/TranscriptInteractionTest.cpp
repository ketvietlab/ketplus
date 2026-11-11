#include "AgentWorkspaceViewTest.h"
#include "agent/AgentMessageFormatter.h"
#include "agent/AgentMessageModel.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/TranscriptNormalizer.h"

#include <QAccessible>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QPainter>
#include <QQuickView>
#include <QTemporaryDir>
#include <QtTest>

using namespace agent_workspace_test;

void AgentWorkspaceViewTest::browserEvidenceStaysVisible_data() {
    QTest::addColumn<QString>("tool");
    for (const char* tool : {"browser_capture", "visual_compare"})
        QTest::newRow(tool) << QString::fromLatin1(tool);
}

void AgentWorkspaceViewTest::browserEvidenceStaysVisible() {
    QFETCH(QString, tool);
    QImage image(640, 320, QImage::Format_RGB32);
    image.fill(QColor("#f4f6fa"));
    QPainter painter(&image);
    painter.fillRect(24, 24, 592, 52, QColor("#c9dcff"));
    painter.fillRect(24, 96, 280, 200, QColor("#c5eee3"));
    painter.fillRect(324, 96, 292, 200, QColor("#e0dbff"));
    painter.setPen(QColor("#172c4a"));
    painter.drawText(QRect(36, 24, 560, 52), Qt::AlignVCenter, "Browser screenshot evidence");
    painter.end();
    QByteArray png;
    QBuffer buffer(&png);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&buffer, "PNG"));
    QJsonObject item{
        {"id", "browser-evidence"},
        {"type", "mcpToolCall"},
        {"server", "ketplus_browser"},
        {"tool", tool},
        {"status", "completed"},
        {"arguments", QJsonObject{{"sessionId", "internal-session-id"}}},
        {"result", QJsonObject{{"content", QJsonArray{QJsonObject{
                                               {"type", "image"},
                                               {"data", QString::fromLatin1(png.toBase64())},
                                               {"mimeType", "image/png"}}}}}}};
    auto running = ketplus::agent_message::activityMessage(item, false);
    auto completed = ketplus::agent_message::activityMessage(item, true);
    running.insert("messageKey", "browser-evidence");
    completed.insert("messageKey", "browser-evidence");
    running.insert("turnId", "browser-turn");
    completed.insert("turnId", "browser-turn");
    const auto close = [](const QString& id) {
        auto row = ketplus::agent_message::activityMessage(
            {{"id", id},
             {"type", "mcpToolCall"},
             {"server", "ketplus_browser"},
             {"tool", "browser_close_session"},
             {"status", "completed"},
             {"arguments", QJsonObject{{"sessionId", "internal-session-id"}}}},
            true);
        row.insert("messageKey", id);
        row.insert("turnId", "browser-turn");
        return row;
    };
    const auto before = close("before"), after = close("after");
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1100, 900);
    view.show();
    view.resetMessages({before, running, after}, "browser-evidence");
    // Capture/compare remain visible; session housekeeping is only in the source transcript.
    QTRY_COMPARE(view.messageModel()->rowCount(), 1);
    view.updateMessage(1, completed);
    QTRY_COMPARE(view.messageModel()->rowCount(), 1);
    auto* root = view.quickView()->rootObject();
    const auto evidenceRow = [&]() -> QQuickItem* {
        const auto find = [&](auto&& self, QQuickItem* parent) -> QQuickItem* {
            if (parent->objectName() == "transcriptActivityRow" &&
                parent->property("messageKey").toString() ==
                    completed.value("messageKey").toString())
                return parent;
            for (auto* child : parent->childItems())
                if (auto* found = self(self, child))
                    return found;
            return nullptr;
        };
        return find(find, root);
    };
    QQuickItem* row = nullptr;
    QTRY_VERIFY((row = evidenceRow()));
    auto* gallery = findVisualItem(row, "transcriptActivityGallery");
    QVERIFY(gallery);
    QTRY_VERIFY(gallery->isVisible() && gallery->height() > 0);
    QVERIFY(!row->property("expanded").toBool());
    QVERIFY(!row->property("hasDetails").toBool());
    QVERIFY(QMetaObject::invokeMethod(row, "toggleDetails"));
    QVERIFY(!row->property("expanded").toBool());
    QVERIFY(!findVisualItem(row, "transcriptActivityDetails")->isVisible());
    auto* thumbnail = findVisualItem(gallery, "messageAttachmentImage");
    QVERIFY(thumbnail);
    QTRY_COMPARE(thumbnail->property("status").toInt(), 1);
    const auto capture = qEnvironmentVariable("KETPLUS_BROWSER_TRANSCRIPT_CAPTURE");
    if (!capture.isEmpty()) {
        QTest::qWait(150);
        QVERIFY(view.quickView()->grabWindow().save(capture + "-" + tool + ".png"));
    }
    auto* frame = findVisualItem(gallery, "messageAttachmentFrame");
    QVERIFY(frame);
    QCOMPARE(frame->width(), 120.0);
    QCOMPARE(frame->height(), 78.0);
    view.quickView()->requestActivate();
    QTest::qWait(50);
    frame->forceActiveFocus();
    QTest::keyClick(view.quickView(), Qt::Key_Return);
    auto* preview = root->findChild<QObject*>("imagePreview");
    QVERIFY(preview);
    QTRY_VERIFY(preview->property("visible").toBool());
    QTest::keyClick(view.quickView(), Qt::Key_Escape);
    // Replayed history keeps screenshots outside collapsed activity groups too.
    view.resetMessages({before, completed, after}, "browser-replay");
    QTRY_COMPARE(view.messageModel()->rowCount(), 1);
    QTRY_VERIFY((row = evidenceRow()));
    QTRY_VERIFY(findVisualItem(row, "transcriptActivityGallery")->isVisible());
    // Missing media keeps a visible placeholder rather than silently disappearing.
    completed.insert("images", QVariantList{"ketplus-media-unavailable:0"});
    view.updateMessage(1, completed);
    QTRY_VERIFY((row = evidenceRow()));
    QQuickItem* placeholder = nullptr;
    QTRY_VERIFY((placeholder = findVisualItem(row, "messageAttachmentUnavailable")) &&
                placeholder->isVisible());
    QCOMPARE(placeholder->property("text").toString(), "Screenshot unavailable");
    // Failed tools show a bounded human-readable error, never the JSON receipt.
    item.insert("result", QJsonObject{{"isError", true},
                                      {"structuredContent",
                                       QJsonObject{{"code", "ORIGIN_DENIED"},
                                                   {"message", "This origin is not allowed."}}}});
    view.resetMessages({ketplus::agent_message::activityMessage(item, true)}, "browser-error");
    QTRY_VERIFY((row = findVisualItem(root, "transcriptActivityRow")));
    QQuickItem* error = nullptr;
    QTRY_VERIFY((error = findVisualItem(row, "transcriptBrowserError")) && error->isVisible());
    QCOMPARE(error->property("text").toString(), "This origin is not allowed.");
    QVERIFY(!findVisualItem(row, "transcriptActivityDetails")->isVisible());
}

void AgentWorkspaceViewTest::browserStepsCollapseAroundEvidence_data() {
    QTest::addColumn<int>("width");
    QTest::newRow("wide") << 1100;
    QTest::newRow("narrow") << 520;
}

void AgentWorkspaceViewTest::fileChangesStayCompactInsideExpandedGroup() {
    const auto edit = [](const QString& id, const QString& path) {
        auto row = ketplus::agent_message::fileChangeMessage(
            {{"id", id},
             {"type", "fileChange"},
             {"status", "completed"},
             {"changes",
              QJsonArray{QJsonObject{{"path", path}, {"kind", QJsonObject{{"type", "update"}}}}}}},
            true);
        row.insert("messageKey", id);
        row.insert("turnId", "edit-turn");
        return row;
    };
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1100, 900);
    view.show();
    view.resetMessages(
        {edit("first", "/worktree/src/First.cpp"), edit("second", "/worktree/tests/Second.cpp")},
        "compact-edits");
    auto* model = view.messageModel();
    QTRY_COMPARE(model->count(), 1);
    QCOMPARE(model->get(0).value("groupSummary").toString(), "Edited 2 files");
    model->setGroupExpanded(model->get(0).value("groupKey").toString(), true);
    QTRY_COMPARE(model->count(), 2);
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QList<QQuickItem*> rows;
    const auto collect = [&](const auto& self, QQuickItem* item) -> void {
        if (item->objectName() == "transcriptActivityRow")
            rows.append(item);
        for (auto* child : item->childItems())
            self(self, child);
    };
    QTRY_VERIFY(([&] {
        rows.clear();
        collect(collect, root);
        return rows.size() == 2;
    })());
    QCOMPARE(rows.at(0)->property("displayTitle").toString(), "Edited First.cpp");
    QCOMPARE(rows.at(1)->property("displayTitle").toString(), "Edited Second.cpp");
    for (auto* row : rows) {
        QVERIFY(!row->property("expanded").toBool());
        QVERIFY(!findVisualItem(row, "transcriptActivityDetails")->isVisible());
    }
    QVERIFY(QMetaObject::invokeMethod(rows.at(0), "toggleDetails"));
    QTRY_VERIFY(findVisualItem(rows.at(0), "transcriptActivityDetails")->isVisible());
    QVERIFY(!findVisualItem(rows.at(1), "transcriptActivityDetails")->isVisible());

    // One edit of several files lists each file as its own step, as the mock does.
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString deep = QStringLiteral("src/a-rather-long-module-directory/with-nested-feature-"
                                        "folders/and-one-more-level-of-depth");
    QVERIFY(QDir(files.path()).mkpath(deep));
    const QString edited = files.filePath(deep + "/Edited.cpp");
    const QString created = files.filePath("src/New.h");
    for (const auto& path : {edited, created}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    auto several = ketplus::agent_message::fileChangeMessage(
        {{"id", "several"},
         {"type", "fileChange"},
         {"status", "completed"},
         {"changes",
          QJsonArray{QJsonObject{{"path", edited},
                                 {"kind", QJsonObject{{"type", "update"}}},
                                 {"diff", "@@ -1 +1,2 @@\n-a\n+b\n+c\n"}},
                     QJsonObject{{"path", created}, {"kind", QJsonObject{{"type", "add"}}}},
                     QJsonObject{{"path", files.filePath("src/Old.cpp")},
                                 {"kind", QJsonObject{{"type", "delete"}}}}}}},
        true);
    several.insert("messageKey", "several");
    several.insert("turnId", "edit-turn");
    ketplus::AgentWorkspaceView listing;
    listing.applyTheme(testPalette());
    listing.resize(800, 700);
    listing.show();
    listing.resetMessages({edit("single", files.filePath("README.md")), several}, "several-edits");
    auto* listed = listing.messageModel();
    QTRY_COMPARE(listed->count(), 1);
    listed->setGroupExpanded(listed->get(0).value("groupKey").toString(), true);
    QTRY_COMPARE(listed->count(), 2);
    root = listing.quickView()->rootObject();
    QList<QQuickItem*> steps;
    const auto collectSteps = [&](const auto& self, QQuickItem* item) -> void {
        if (!item->isVisible())
            return;
        if (item->objectName() == "transcriptFileStep")
            steps.append(item);
        for (auto* child : item->childItems())
            self(self, child);
    };
    QTRY_VERIFY(([&] {
        steps.clear();
        collectSteps(collectSteps, root);
        return steps.size() == 3;
    })());
    // The single-file edit keeps its one header line.
    QVERIFY(findVisualItem(root, "transcriptActivityHeader"));
    QCOMPARE(steps.at(0)->property("text").toString(), QStringLiteral("Edited"));
    QVERIFY(steps.at(0)->property("detail").toString().contains("Edited.cpp"));
    QVERIFY(steps.at(0)->property("detail").toString().contains("+2 −1"));
    QCOMPARE(steps.at(1)->property("text").toString(), QStringLiteral("Created"));
    QCOMPARE(steps.at(2)->property("text").toString(), QStringLiteral("Deleted"));
    QCOMPARE(steps.at(2)->property("iconName").toString(), QStringLiteral("trash"));
    QTRY_VERIFY(steps.at(2)->property("missing").toBool());
    QVERIFY(!steps.at(0)->property("missing").toBool());

    // A long path elides inside its step instead of running past it.
    auto* detail = findVisualItem(steps.at(0), "transcriptFileStepDetail");
    QVERIFY(detail);
    QCOMPARE(detail->property("text").toString(), steps.at(0)->property("detail").toString());
    QVERIFY(detail->property("truncated").toBool());
    QVERIFY(detail->mapToItem(steps.at(0), QPointF(detail->width(), 0)).x() <=
            steps.at(0)->width());

    // A step without a diff opens the file's diff in the viewer.
    QSignalSpy diffs(&listing, &ketplus::AgentWorkspaceView::fileDiffRequested);
    QVERIFY(QMetaObject::invokeMethod(steps.at(1), "clicked"));
    QCOMPARE(diffs.size(), 1);
    QCOMPARE(diffs.constFirst().constFirst().toString(), created);

    // Each step keeps the card's file actions; a missing file can still be copied.
    const auto menuOf = [&](QQuickItem* fileStep) {
        QTest::mouseClick(listing.quickView(), Qt::RightButton, Qt::NoModifier,
                          fileStep->mapToScene(QPointF(30, fileStep->height() / 2)).toPoint());
        auto* menu = fileStep->findChild<QObject*>("changedFileContextMenu");
        return menu;
    };
    auto* deletedMenu = menuOf(steps.at(2));
    QVERIFY(deletedMenu);
    QTRY_VERIFY(deletedMenu->property("visible").toBool());
    auto* copyAction = deletedMenu->findChild<QObject*>("copyChangedFilePathAction");
    QVERIFY(copyAction && copyAction->property("enabled").toBool());
    QVERIFY(
        !deletedMenu->findChild<QObject*>("revealChangedFileAction")->property("enabled").toBool());
    QVERIFY(
        !deletedMenu->findChild<QObject*>("openChangedFileAction")->property("enabled").toBool());
    QGuiApplication::clipboard()->clear();
    QVERIFY(QMetaObject::invokeMethod(copyAction, "triggered"));
    QVERIFY(QGuiApplication::clipboard()->text().endsWith(QStringLiteral("/src/Old.cpp")));
    QMetaObject::invokeMethod(deletedMenu, "close");
    QTRY_VERIFY(!deletedMenu->property("visible").toBool());
    QCOMPARE(diffs.size(), 1);

    auto* editedMenu = menuOf(steps.at(0));
    QVERIFY(editedMenu);
    QTRY_VERIFY(editedMenu->property("visible").toBool());
    QVERIFY(
        editedMenu->findChild<QObject*>("revealChangedFileAction")->property("enabled").toBool());
    auto* openAction = editedMenu->findChild<QObject*>("openChangedFileAction");
    QVERIFY(openAction->property("enabled").toBool());
    QSignalSpy openedFile(&listing, &ketplus::AgentWorkspaceView::fileOpenRequested);
    QVERIFY(QMetaObject::invokeMethod(openAction, "triggered"));
    QCOMPARE(openedFile.count(), 1);
    QMetaObject::invokeMethod(editedMenu, "close");
    // The right button opens the menu, not the diff.
    QCOMPARE(diffs.size(), 1);
}

void AgentWorkspaceViewTest::activityIconsFollowTheMock_data() {
    QTest::addColumn<QString>("subtype");
    QTest::addColumn<QString>("change");
    QTest::addColumn<QString>("icon");
    // mockups/qt TranscriptActivityItem.qml: command terminal, read file-text, web and browser
    // globe, mcp package, generate and thinking sparkles, tool sliders, skill layers, agent send.
    QTest::newRow("command") << "command" << "" << "terminal";
    QTest::newRow("read") << "fileRead" << "" << "file-text";
    QTest::newRow("code search") << "codeSearch" << "" << "search";
    QTest::newRow("listing") << "fileSearch" << "listing" << "view-list";
    QTest::newRow("file search") << "fileSearch" << "" << "search";
    QTest::newRow("web search") << "webSearch" << "" << "globe";
    QTest::newRow("web fetch") << "webFetch" << "" << "globe";
    QTest::newRow("browser") << "ketplusBrowser" << "" << "globe";
    QTest::newRow("generated image") << "imageGeneration" << "" << "sparkles";
    QTest::newRow("reasoning") << "reasoning" << "" << "sparkles";
    QTest::newRow("plan") << "plan" << "" << "map";
    QTest::newRow("monitor") << "monitor" << "" << "clock";
    QTest::newRow("skill") << "skill" << "" << "layers";
    QTest::newRow("tools loaded") << "toolsLoaded" << "" << "sliders";
    QTest::newRow("task stopped") << "taskStopped" << "" << "terminal";
    QTest::newRow("message to an agent") << "collaboration" << "" << "send";
    QTest::newRow("subagent") << "subAgent" << "" << "bot";
    QTest::newRow("mcp") << "integration" << "" << "package";
    QTest::newRow("unknown") << "somethingNew" << "" << "sliders";
    QTest::newRow("edited") << "fileChange" << "update" << "edit";
    QTest::newRow("created") << "fileChange" << "add" << "file-code";
    QTest::newRow("deleted") << "fileChange" << "delete" << "trash";
}

void AgentWorkspaceViewTest::activityIconsFollowTheMock() {
    QFETCH(QString, subtype);
    QFETCH(QString, change);
    QFETCH(QString, icon);
    QVariantMap row{{"messageKey", "step"}, {"kind", "tool"},         {"category", "activity"},
                    {"subtype", subtype},   {"title", "Did a thing"}, {"turnId", "icons"},
                    {"status", "completed"}};
    if (change == QStringLiteral("listing")) {
        row.insert("title", "Listed files: src");
    } else if (!change.isEmpty()) {
        row = ketplus::agent_message::fileChangeMessage(
            {{"id", "step"},
             {"type", "fileChange"},
             {"status", "completed"},
             {"changes", QJsonArray{QJsonObject{{"path", "/worktree/src/File.cpp"},
                                                {"kind", QJsonObject{{"type", change}}}}}}},
            true);
        row.insert("messageKey", "step");
        row.insert("turnId", "icons");
    }
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    view.resetMessages({row}, "icons:" + subtype + change);
    auto* root = view.quickView()->rootObject();
    QQuickItem* drawn = nullptr;
    QTRY_VERIFY((drawn = findVisibleItem(root, "transcriptActivityIcon")) != nullptr);
    QCOMPARE(drawn->property("name").toString(), icon);
}

void AgentWorkspaceViewTest::browserStepsReadAsTheMock_data() {
    QTest::addColumn<QString>("tool");
    QTest::addColumn<QJsonObject>("arguments");
    QTest::addColumn<QJsonObject>("result");
    QTest::addColumn<QString>("completed");
    QTest::addColumn<QString>("running");
    const auto target = [](const QString& name) {
        return QJsonObject{
            {"structuredContent",
             QJsonObject{{"outcome", "applied"},
                         {"target", QJsonObject{{"role", "button"}, {"name", name}}}}}};
    };
    const auto act = [](const QJsonObject& action) {
        return QJsonObject{{"sessionId", "s1"}, {"action", action}};
    };
    QTest::newRow("open") << "browser_open"
                          << QJsonObject{{"url", "http://localhost:5173/sidebar?token=hidden"}}
                          << QJsonObject{} << "Opened: localhost:5173/sidebar"
                          << "Opening: localhost:5173/sidebar";
    QTest::newRow("open nowhere") << "browser_open" << QJsonObject{} << QJsonObject{}
                                  << "Opened a page" << "Opening a page";
    QTest::newRow("click") << "browser_act"
                           << act({{"type", "click"}, {"target", QJsonObject{{"nodeRef", "n4"}}}})
                           << target("Review the startup flow")
                           << "Clicked: “Review the startup flow”" << "Clicking";
    // A filled value is never shown, only the field it went into.
    QTest::newRow("fill") << "browser_act"
                          << act({{"type", "fill"},
                                  {"target", QJsonObject{{"nodeRef", "n2"}}},
                                  {"value", "typed-value"}})
                          << target("Email") << "Filled in: “Email”" << "Filling in";
    QTest::newRow("press") << "browser_act"
                           << act({{"type", "press"},
                                   {"target", QJsonObject{{"nodeRef", "n2"}}},
                                   {"key", "Enter"}})
                           << target("Search") << "Pressed a key: “Search”"
                           << "Pressing a key";
    QTest::newRow("navigate") << "browser_act"
                              << act({{"type", "navigate"},
                                      {"url", "http://localhost:5173/settings/storage"}})
                              << QJsonObject{} << "Opened: localhost:5173/settings/storage"
                              << "Opening: localhost:5173/settings/storage";
    QTest::newRow("history") << "browser_act" << act({{"type", "history"}, {"direction", "back"}})
                             << QJsonObject{} << "Went back" << "Going back";
    QTest::newRow("unchecked") << "browser_act"
                               << act({{"type", "check"},
                                       {"target", QJsonObject{{"nodeRef", "n7"}}},
                                       {"checked", false}})
                               << target("Dark mode") << "Unchecked: “Dark mode”"
                               << "Unchecking";
    QTest::newRow("wait text") << "browser_wait"
                               << QJsonObject{{"condition",
                                               QJsonObject{{"type", "text"}, {"text", "Working"}}}}
                               << QJsonObject{} << "Waited for: text “Working”"
                               << "Waiting for: text “Working”";
    QTest::newRow("wait load") << "browser_wait"
                               << QJsonObject{{"condition", QJsonObject{{"type", "load"}}}}
                               << QJsonObject{} << "Waited for: the page to load"
                               << "Waiting for: the page to load";
    // A provider that keeps only the text block still reads the observation.
    QTest::newRow("observe")
        << "browser_observe" << QJsonObject{{"sessionId", "s1"}}
        << QJsonObject{{"content",
                        QJsonArray{QJsonObject{
                            {"type", "text"},
                            {"text",
                             QString::fromUtf8(
                                 QJsonDocument(
                                     QJsonObject{{"page", QJsonObject{{"title", "Storage"}}},
                                                 {"visibleNodeCount", 142}})
                                     .toJson(QJsonDocument::Compact))}}}}}
        << "Observed the page: Storage · 142 nodes" << "Observing the page";
    // A secret spelled with JSON escapes inside the text block is withheld as in the object.
    QTest::newRow("escaped secret")
        << "browser_act" << act({{"type", "click"}, {"target", QJsonObject{{"nodeRef", "n4"}}}})
        << QJsonObject{{"content",
                        QJsonArray{QJsonObject{{"type", "text"},
                                               {"text", "{\"target\":{\"role\":\"button\",\"name\":"
                                                        "\"token\\u003dSYNTHETIC_SECRET\"}}"}}}}}
        << "Clicked: “token=[redacted]”" << "Clicking";
    QTest::newRow("plain secret") << "browser_act"
                                  << act({{"type", "click"},
                                          {"target", QJsonObject{{"nodeRef", "n4"}}}})
                                  << target("token=SYNTHETIC_SECRET")
                                  << "Clicked: “token=[redacted]”" << "Clicking";
    QTest::newRow("compare") << "visual_compare" << QJsonObject{}
                             << QJsonObject{{"structuredContent",
                                             QJsonObject{{"raster",
                                                          QJsonObject{{"mismatchRatio", 0.004}}}}}}
                             << "Compared with the reference: 0.4% of pixels differ"
                             << "Comparing with the reference";
    QTest::newRow("identical") << "visual_compare" << QJsonObject{}
                               << QJsonObject{{"structuredContent",
                                               QJsonObject{
                                                   {"raster", QJsonObject{{"mismatchRatio", 0}}}}}}
                               << "Compared with the reference: no pixels differ"
                               << "Comparing with the reference";
    QTest::newRow("other") << "browser_pages" << QJsonObject{} << QJsonObject{}
                           << "Used ketplus_browser · pages" << "Using ketplus_browser · pages";
}

void AgentWorkspaceViewTest::browserStepsReadAsTheMock() {
    QFETCH(QString, tool);
    QFETCH(QJsonObject, arguments);
    QFETCH(QJsonObject, result);
    QFETCH(QString, completed);
    QFETCH(QString, running);
    const QJsonObject item{{"id", "step"}, {"type", "mcpToolCall"},  {"server", "ketplus_browser"},
                           {"tool", tool}, {"arguments", arguments}, {"result", result}};
    const auto done = ketplus::agent_message::activityMessage(item, true);
    QCOMPARE(done.value("title").toString(), completed);
    QVERIFY(!done.value("title").toString().contains("hidden"));
    QVERIFY(!done.value("title").toString().contains("typed-value"));
    QVERIFY(!done.value("title").toString().contains("SYNTHETIC_SECRET"));
    QCOMPARE(ketplus::agent_message::activityMessage(item, false).value("title").toString(),
             running);
}

void AgentWorkspaceViewTest::browserStepsCollapseAroundEvidence() {
    QFETCH(int, width);
    QTemporaryDir media;
    QVERIFY(media.isValid());
    QImage image(640, 320, QImage::Format_RGB32);
    image.fill(QColor("#c9dcff"));
    const QString imagePath = media.filePath("evidence.png");
    QVERIFY(image.save(imagePath));
    const auto step = [](const QString& id, const QString& tool) {
        auto row = ketplus::agent_message::activityMessage(
            {{"id", id}, {"type", "mcpToolCall"}, {"server", "ketplus_browser"}, {"tool", tool}},
            true);
        row.insert("messageKey", id);
        row.insert("turnId", "browser-turn");
        return row;
    };
    QVariantList rows;
    auto open = step("open", "browser_open");
    open.insert("target", "https://example.test/page");
    rows.append(open);
    for (const char* tool :
         {"browser_observe", "browser_inspect", "browser_network", "browser_logs"})
        rows.append(step(QString::fromLatin1(tool), tool));
    auto capture = step("capture", "browser_capture");
    capture.insert("images", QVariantList{QUrl::fromLocalFile(imagePath).toString()});
    rows.append(capture);
    auto compare = step("compare", "visual_compare");
    compare.insert("images", capture.value("images"));
    rows.append(compare);
    // Session housekeeping ends the browser work and is not shown.
    rows.append(step("browser_close_page", "browser_close_page"));

    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(width, 900);
    view.show();
    view.resetMessages(rows, "browser-groups");
    auto* model = view.messageModel();
    // As in the mock, the capture and the comparison are steps of the browser group: the
    // closed group is one line, and opening it shows each step with its image.
    QTRY_COMPARE(model->rowCount(), 1);
    QCOMPARE(model->get(0).value("messageKey").toString(), "open");
    QCOMPARE(model->sourceCount(), rows.size());
    QCOMPARE(model->get(0).value("groupSummary").toString(),
             QStringLiteral("Used ketplus_browser · 7 steps"));
    QVERIFY(view.transcriptCopyText().contains("Observed the page"));
    QVERIFY(view.transcriptCopyText().contains("Inspected the page"));
    auto* root = view.quickView()->rootObject();
    const auto galleries = [&]() {
        const auto count = [](auto&& self, QQuickItem* parent) -> int {
            int found = parent->objectName() == "transcriptActivityGallery" &&
                                parent->isVisible() && parent->height() > 0
                            ? 1
                            : 0;
            for (auto* child : parent->childItems())
                found += self(self, child);
            return found;
        };
        return count(count, root);
    };
    QTRY_COMPARE(galleries(), 0);
    // Session housekeeping stays hidden when the group opens; the images show under their steps.
    model->setGroupExpanded("open", true);
    QTRY_COMPARE(model->rowCount(), 7);
    for (int row = 0; row < model->rowCount(); ++row)
        QVERIFY(model->get(row).value("messageKey").toString() != "browser_close_page");
    QTRY_VERIFY(findVisibleItem(root, "transcriptBrowserOpen"));
    auto* browserOpen = findVisibleItem(root, "transcriptBrowserOpen");
    QCOMPARE(browserOpen->property("text").toString(), QStringLiteral("Open"));
    QTRY_COMPARE(galleries(), 2);
    const QString capturePrefix = qEnvironmentVariable("KETPLUS_BROWSER_GROUP_CAPTURE");
    if (!capturePrefix.isEmpty()) {
        QTest::qWait(150);
        QVERIFY(
            view.quickView()->grabWindow().save(capturePrefix + QString::number(width) + ".png"));
    }
    model->setGroupExpanded("open", false);
    QTRY_COMPARE(model->rowCount(), 1);
    QTRY_COMPARE(galleries(), 0);
    // Restored history uses the same projection while retaining all source rows.
    view.resetMessages(rows, "browser-groups-replay");
    QTRY_COMPARE(model->rowCount(), 1);
    QCOMPARE(model->sourceCount(), rows.size());
}

void AgentWorkspaceViewTest::transcriptArtifactSafetyAndExport() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1100, 900);
    view.show();
    QSignalSpy opened(&view, &ketplus::AgentWorkspaceView::fileOpenRequested);
    auto artifact =
        ketplus::agent_message::activityMessage({{"type", "artifact"},
                                                 {"id", "report"},
                                                 {"path", "/project/report.md"},
                                                 {"authorization", "secret-credential"}},
                                                true);
    view.resetMessages({artifact}, "artifacts");
    auto* root = view.quickView()->rootObject();
    QQuickItem* row = nullptr;
    QTRY_VERIFY((row = findVisualItem(root, "transcriptActivityRow")));
    QCOMPARE(opened.size(), 0);
    QVERIFY(QMetaObject::invokeMethod(row, "toggleDetails"));
    auto* button = findVisualItem(row, "openActivityResource");
    QVERIFY(button && button->isVisible());
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QCOMPARE(opened.size(), 1);
    QCOMPARE(opened.last().first().toString(), QStringLiteral("/project/report.md"));
    const auto text = view.transcriptCopyText();
    QVERIFY(text.contains("/project/report.md"));
    QVERIFY(!text.contains("secret-credential"));

    auto unsafe = ketplus::agent_message::activityMessage(
        {{"type", "artifact"}, {"id", "unsafe"}, {"url", "javascript:alert(1)"}}, true);
    view.resetMessages({unsafe}, "unsafe");
    QTRY_VERIFY((row = findVisualItem(root, "transcriptActivityRow")));
    QVERIFY(QMetaObject::invokeMethod(row, "toggleDetails"));
    button = findVisualItem(row, "openActivityResource");
    QVERIFY(button && !button->isVisible());
    QCOMPARE(opened.size(), 1);

    const QVariantMap question{
        {"kind", "tool"},
        {"subtype", "question"},
        {"questions", QVariantList{QVariantMap{
                          {"id", "secret"}, {"question", "Credential?"}, {"isSecret", true}}}},
        {"questionAnswers", QVariantMap{{"secret", "must-not-export"}}}};
    const auto exported = ketplus::agent_message::transcriptText({question});
    QVERIFY(exported.contains("Credential?"));
    QVERIFY(exported.contains("[redacted]"));
    QVERIFY(!exported.contains("must-not-export"));
}

void AgentWorkspaceViewTest::transcriptAtlasLayout() {
    ketplus::AgentWorkspaceView view;
    auto palette = testPalette();
    palette.dark = false;
    palette.pageBackground = "#ffffff";
    palette.textMain = "#24272b";
    palette.textSecondary = "#52565c";
    palette.textMuted = "#74777b";
    palette.surfaceRaised = "#ffffff";
    palette.accent = "#5264cc";
    palette.accentSubtle = "#eef0fa";
    palette.border = "#e1e2e5";
    view.applyTheme(palette);
    view.resize(1226, 956);
    view.show();
    const auto activity = [](const QString& id, const QString& subtype, const QString& title,
                             const QString& target = QString()) {
        return QVariantMap{{"messageKey", id},       {"kind", "tool"},
                           {"category", "activity"}, {"subtype", subtype},
                           {"title", title},         {"target", target},
                           {"turnId", "mock-turn"},  {"status", "completed"}};
    };
    auto command = activity("command", "command", "Ran: cmake --build build/dev");
    command.insert("command", "cmake --build build/dev");
    command.insert("details", QVariantMap{{"output", "Build succeeded"}});
    view.resetMessages(
        {QVariantMap{{"messageKey", "user"},
                     {"kind", "user"},
                     {"body", "Refactor the transcript so it shows exact actions and the final "
                              "answer without intermediate reasoning."}},
         activity("read-1", "fileRead", "Read AgentWorkspace.qml",
                  "modules/ai/src/agent/qml/AgentWorkspace.qml"),
         activity("read-2", "fileRead", "Read AgentSessionManager.cpp",
                  "modules/ai/src/agent/AgentSessionManager.cpp"),
         activity("read-3", "fileRead", "Read CodexAdapter.cpp",
                  "apps/daemon/src/providers/CodexAdapter.cpp"),
         command,
         activity("search", "codeSearch", "Searched code for transcript event types",
                  "7 matching event families"),
         activity("browser", "browserNavigate", "Opened Qt Quick ListView documentation",
                  "https://doc.qt.io/qt-6/qml-qtquick-listview.html"),
         QVariantMap{
             {"messageKey", "answer"},
             {"kind", "assistant"},
             {"category", "final"},
             {"body",
              "### Transcript activity is explicit and easier to scan.\n\nThe conversation now "
              "keeps the result separate from the work performed:\n\n- **Exact file and command "
              "actions** remain visible and expandable.\n- **Images, browser visits, searches, and "
              "app usage** have dedicated activity treatments.\n- **Unknown future events** fall "
              "back to a safe generic row instead of disappearing."}}},
        "atlas-layout");
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* transcript = findVisualItem(root, "agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTRY_COMPARE(view.messageModel()->count(), 3);
    QCOMPARE(view.messageModel()->get(1).value("groupSummary").toString(),
             QString("Explored 3 files, ran a command, searched code, used the browser"));
    QVERIFY(view.messageModel()->get(2).value("groupRole").toString().isEmpty());
    const auto capture = qEnvironmentVariable("KETPLUS_ATLAS_TRANSCRIPT_CAPTURE");
    if (!capture.isEmpty()) {
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        QTest::qWait(150);
        QVERIFY(view.quickView()->grabWindow().save(capture));
    }
}

void AgentWorkspaceViewTest::transcriptRendererInteractionMatrix_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark") << true;
    QTest::newRow("light") << false;
}

void AgentWorkspaceViewTest::transcriptRendererInteractionMatrix() {
    QFETCH(bool, dark);
    QAccessible::setActive(true);
    auto palette = testPalette();
    if (!dark) {
        palette.dark = false;
        palette.pageBackground = "#fafafa";
        palette.appBackground = "#f5f5f5";
        palette.textMain = "#242424";
        palette.textSecondary = "#525252";
        palette.textMuted = "#6b6b6b";
        palette.surfaceRaised = "#ffffff";
        palette.interactiveBackground = "#f0f0f0";
    }
    ketplus::AgentWorkspaceView view;
    view.applyTheme(palette);
    view.resize(1200, 900);
    view.show();
    const auto capturePrefix = qEnvironmentVariable("KETPLUS_RENDERER_CAPTURE");
    if (!capturePrefix.isEmpty()) {
        view.raise();
        view.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    }
    const auto captureState = [&](const QString& state) {
        if (capturePrefix.isEmpty())
            return;
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        auto* transcript = findVisualItem(view.quickView()->rootObject(), "agentTranscript");
        QVERIFY(transcript);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        QTest::qWait(60);
        QVERIFY(view.quickView()->grabWindow().save(capturePrefix + "." + QTest::currentDataTag() +
                                                    "." + state + ".png"));
    };
    const QString exactCommand = "printf '" + QString(2000, QLatin1Char('x')) + "'";
    const QVariantMap command{
        {"kind", "tool"},
        {"subtype", "command"},
        {"messageKey", "command-attempt-1"},
        {"title", "Ran: printf"},
        {"command", exactCommand},
        {"cwd", "/project"},
        {"status", "failed"},
        {"exitCode", 1},
        {"durationMs", 42},
        {"details",
         QVariantMap{{"output", "Failure detail\n" + QString(40000, QLatin1Char('a'))}}}};
    view.resetMessages({command}, "scope-a");
    auto* root = view.quickView()->rootObject();
    QQuickItem* row = nullptr;
    QTRY_VERIFY((row = findVisualItem(root, "transcriptActivityRow")));
    QTRY_VERIFY(!row->property("expanded").toBool());
    QVERIFY(QMetaObject::invokeMethod(row, "toggleDetails"));
    QTRY_VERIFY(row->property("expanded").toBool());
    auto* title = findVisualItem(row, "transcriptActivityTitle");
    QVERIFY(title);
    // The mock's step line: "Ran" and the command beside it, the failure on its own line.
    QCOMPARE(title->property("text").toString(), QStringLiteral("Ran"));
    auto* detail = findVisualItem(row, "transcriptActivityDetail");
    QVERIFY(detail);
    QCOMPARE(detail->property("text").toString(), exactCommand);
    auto* status = findVisualItem(row, "transcriptActivityStatus");
    QVERIFY(status && status->isVisible());
    QCOMPARE(status->property("text").toString(), QStringLiteral("Failed · exit code 1 · 42 ms"));
    auto* accessible = QAccessible::queryAccessibleInterface(row);
    QVERIFY(accessible);
    QCOMPARE(accessible->role(), QAccessible::Button);
    QVERIFY(accessible->text(QAccessible::Name).contains("Failed"));
    auto* details = findVisualItem(row, "transcriptActivityDetailText");
    QVERIFY(details);
    QVERIFY(!details->property("text").toString().contains("Failure detail"));
    QVERIFY(!details->property("text").toString().contains(exactCommand));
    QVERIFY(details->property("text").toString().contains("exit: 1"));
    QVERIFY(details->property("text").toString().size() < 20000);
    auto* input = findVisualItem(row, "transcriptCommandInput");
    auto* showMore = findVisualItem(row, "transcriptCommandShowMore");
    QVERIFY(input && showMore);
    QCOMPARE(input->property("text").toString(), exactCommand);
    QVERIFY(showMore->isVisible());
    const qreal collapsedHeight = input->height();
    const qreal inputTop = input->mapToScene({}).y();
    QVERIFY(collapsedHeight < 40);
    QCOMPARE(input->y(), showMore->y());
    captureState("command-preview");
    QVERIFY(QMetaObject::invokeMethod(showMore, "clicked"));
    QTRY_VERIFY(input->height() > collapsedHeight);
    QVERIFY(qAbs(input->mapToScene({}).y() - inputTop) < 1.0);
    QCOMPARE(showMore->property("text").toString(), QStringLiteral("Show less"));
    captureState("command-expanded");
    auto* output = findVisualItem(row, "transcriptCommandOutput");
    QTRY_VERIFY(output && output->isVisible());
    QVERIFY(output->height() <= 300);
    QVERIFY(QMetaObject::invokeMethod(showMore, "clicked"));
    QTRY_COMPARE(input->height(), collapsedHeight);
    auto* copy = findVisualItem(row, "copyActivityDetails");
    QVERIFY(copy);
    QVERIFY(QMetaObject::invokeMethod(copy, "clicked"));
    QCOMPARE(QGuiApplication::clipboard()->text(), exactCommand);
    QVERIFY(QMetaObject::invokeMethod(row, "toggleDetails"));
    QTRY_VERIFY(!row->property("expanded").toBool());
    view.resetMessages({QVariantMap{{"kind", "assistant"}, {"body", "Another conversation"}}},
                       "scope-b");
    QTest::qWait(30);
    view.resetMessages({command}, "scope-a");
    QTRY_VERIFY((row = findVisualItem(root, "transcriptActivityRow")));
    QTest::qWait(30);
    QVERIFY(!row->property("expanded").toBool());
    captureState("command-collapsed");

    const QStringList subtypes{"fileRead",        "fileSearch",  "codeSearch",
                               "browserNavigate", "integration", "futureTool"};
    for (const auto& subtype : subtypes) {
        QVariantMap item{{"kind", "tool"},
                         {"subtype", subtype},
                         {"messageKey", subtype},
                         {"title", "Activity " + subtype},
                         {"target", "example target"},
                         {"status", "completed"},
                         {"sanitizedPayload", QVariantMap{{"result", "safe result"}}}};
        view.resetMessages({item}, "matrix");
        QTRY_VERIFY((row = findVisualItem(root, "transcriptActivityRow")));
        QTRY_COMPARE(row->property("subtype").toString(), subtype);
        QVERIFY(QMetaObject::invokeMethod(row, "toggleDetails"));
        details = findVisualItem(row, "transcriptActivityDetailText");
        QTRY_VERIFY(details->property("text").toString().contains("safe result"));
        captureState(subtype);
    }
    view.resetMessages(
        {QVariantMap{{"kind", "tool"},
                     {"subtype", "imageView"},
                     {"messageKey", "broken-image"},
                     {"title", "Viewed an image"},
                     {"images", QVariantList{"file:///missing-transcript-test-image.png"}}}},
        "broken");
    QQuickItem* frame = nullptr;
    QTRY_VERIFY((frame = findVisualItem(root, "messageAttachmentFrame")));
    auto* image = findVisualItem(frame, "messageAttachmentImage");
    QTRY_COMPARE(image->property("status").toInt(), 3);
    QVERIFY(frame->isVisible());
    QVERIFY(frame->height() > 0);
    const auto capture = qEnvironmentVariable("KETPLUS_RENDERER_CAPTURE");
    if (!capture.isEmpty()) {
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        auto* transcript = findVisualItem(root, "agentTranscript");
        QVERIFY(transcript);
        QTRY_COMPARE(transcript->opacity(), 1.0);
        QTest::qWait(60);
        QVERIFY(
            view.quickView()->grabWindow().save(capture + "." + QTest::currentDataTag() + ".png"));
    }
}

void AgentWorkspaceViewTest::activityOperationSemantics() {
    const QMap<QString, QString> controlTypes{
        {"functions.wait", "wait"},        {"monitor", "monitor"},      {"resume_agent", "wake"},
        {"ScheduleWakeup", "wake"},        {"EnterPlanMode", "plan"},   {"Skill", "skill"},
        {"ToolSearch", "toolsLoaded"},     {"TaskStop", "taskStopped"}, {"Agent", "subAgent"},
        {"functions.update_plan", "plan"}, {"WebFetch", "webFetch"},    {"WebSearch", "webSearch"},
        {"open_page", "webOpen"},          {"find_in_page", "webFind"}};
    for (auto it = controlTypes.cbegin(); it != controlTypes.cend(); ++it) {
        for (const auto& type : {"mcpToolCall", "dynamicToolCall"}) {
            const QJsonObject item{{"type", type},
                                   {"tool", it.key()},
                                   {"id", "step"},
                                   {"arguments", QJsonObject{{"query", "test"}}}};
            const auto started = ketplus::agent_message::activityMessage(item, false);
            const auto completed = ketplus::agent_message::activityMessage(item, true);
            QCOMPARE(started.value("subtype").toString(), it.value());
            QCOMPARE(started.value("status").toString(), QString("running"));
            QCOMPARE(completed.value("status").toString(), QString("completed"));
            QVERIFY(started.value("title") != completed.value("title"));
            if (it.key() == "ScheduleWakeup") {
                QCOMPARE(started.value("title").toString(), QString("Scheduling wake-up"));
                QCOMPARE(completed.value("title").toString(), QString("Scheduled wake-up"));
            } else if (it.key() == "EnterPlanMode") {
                QCOMPARE(started.value("title").toString(), QString("Entering plan mode"));
                QCOMPARE(completed.value("title").toString(), QString("Entered plan mode"));
            }
            QVERIFY(!completed.value("sanitizedPayload").toMap().isEmpty());
        }
    }

    const QMap<QString, QString> searchTypes{
        {"read_file", "fileRead"}, {"search_files", "fileSearch"}, {"search_code", "codeSearch"}};
    for (auto it = searchTypes.cbegin(); it != searchTypes.cend(); ++it) {
        const auto result = ketplus::agent_message::activityMessage(
            {{"type", "mcpToolCall"},
             {"tool", it.key()},
             {"id", it.key()},
             {"arguments", QJsonObject{{"path", "/project/a.cpp"}, {"query", "pattern"}}},
             {"result", QJsonObject{{"isError", true}, {"content", "Failure"}}}},
            true);
        QCOMPARE(result.value("subtype").toString(), it.value());
        QCOMPARE(result.value("status").toString(), QStringLiteral("failed"));
        QVERIFY(!result.value("target").toString().isEmpty());
    }
    const auto imageResult = ketplus::agent_message::activityMessage(
        {{"type", "mcpToolCall"},
         {"tool", "view_image"},
         {"id", "view"},
         {"arguments", QJsonObject{{"path", "/project/capture.png"}}}},
        true);
    QCOMPARE(imageResult.value("images").toList().size(), 1);
    const auto website = ketplus::agent_message::activityMessage(
        {{"type", "mcpToolCall"},
         {"server", "browser"},
         {"tool", "open"},
         {"arguments", QJsonObject{{"url", "https://user:secret@example.com/page?token=hidden"},
                                   {"pageTitle", "Example page"}}}},
        true);
    QVERIFY(website.value("title").toString().contains("Example page · example.com"));
    QVERIFY(!website.value("title").toString().contains("hidden"));
    QVERIFY(!website.value("target").toString().contains("hidden"));
    const QStringList operations{"open", "navigate", "click", "download", "screenshot"};
    for (const auto& operation : operations) {
        const QJsonObject item{
            {"id", operation}, {"type", "mcpToolCall"}, {"server", "browser"}, {"tool", operation}};
        const auto running = ketplus::agent_message::activityMessage(item, false);
        const auto completed = ketplus::agent_message::activityMessage(item, true);
        QCOMPARE(running.value("eventType").toString(), "browser." + operation);
        QCOMPARE(completed.value("eventType"), running.value("eventType"));
        QVERIFY(running.value("running").toBool());
        QVERIFY(!completed.value("running").toBool());
    }
    {
        // ketplus_browser (ADR 0008): inline screenshot -> owner-only media file, no base64 in
        // rows.
        QImage pixel(2, 2, QImage::Format_ARGB32);
        pixel.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(pixel.save(&buffer, "PNG"));
        const QString base64 = QString::fromLatin1(png.toBase64());
        const QJsonObject capture{
            {"id", "capture"},
            {"type", "mcpToolCall"},
            {"server", "ketplus_browser"},
            {"tool", "browser_capture"},
            {"status", "completed"},
            {"arguments", QJsonObject{{"sessionId", "s1"}, {"pageId", "p1"}, {"fullPage", false}}},
            {"result",
             QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "{}"}},
                                                QJsonObject{{"type", "image"},
                                                            {"data", base64},
                                                            {"mimeType", "image/png"}},
                                                QJsonObject{{"type", "image"},
                                                            {"data", "not base64!"},
                                                            {"mimeType", "image/png"}}}}}}};
        const QJsonObject subagent{{"id", "sub"}, {"type", "subAgentActivity"}};
        QCOMPARE(ketplus::agent_message::activityMessage(subagent, true).value("title").toString(),
                 QStringLiteral("Ran a subagent"));
        QCOMPARE(ketplus::agent_message::activityMessage(subagent, false).value("title").toString(),
                 QStringLiteral("Running a subagent"));
        const auto running = ketplus::agent_message::activityMessage(capture, false);
        QCOMPARE(running.value("eventType").toString(), QStringLiteral("browser.screenshot"));
        QCOMPARE(running.value("title").toString(), QStringLiteral("Capturing"));
        QVERIFY(running.value("images").toList().isEmpty());
        const auto done = ketplus::agent_message::activityMessage(capture, true);
        QCOMPARE(done.value("title").toString(), QStringLiteral("Captured: 2 × 2"));
        QCOMPARE(done.value("status").toString(), QStringLiteral("completed"));
        QCOMPARE(done.value("summary").toString(), QStringLiteral("fullPage=false"));
        const auto images = done.value("images").toList();
        QCOMPARE(images.size(), 1);
        const QString file = QUrl(images.first().toString()).toLocalFile();
        QVERIFY(QFileInfo::exists(file));
        QCOMPARE(QFile(file).size(), png.size());
        const QByteArray payload =
            QJsonDocument::fromVariant(done.value("sanitizedPayload")).toJson();
        QVERIFY(!payload.contains(base64.left(32).toLatin1()));
        QVERIFY(payload.contains("[inline image omitted]"));
        const QByteArray resource =
            QJsonDocument(ketplus::TranscriptSanitizer::json(
                              QJsonObject{{"type", "resource"},
                                          {"resource", QJsonObject{{"uri", "file:///a.png"},
                                                                   {"mimeType", "image/png"},
                                                                   {"blob", base64}}}})
                              .toObject())
                .toJson();
        QVERIFY(!resource.contains(base64.left(32).toLatin1()));
        QVERIFY(resource.contains("[inline resource omitted]"));
        const auto event =
            ketplus::ActivityNormalizer::normalize(capture, ketplus::TranscriptPhase::Completed);
        QVERIFY(!QJsonDocument::fromVariant(event.sanitizedPayload)
                     .toJson()
                     .contains(base64.left(32).toLatin1()));
        const auto act = ketplus::agent_message::activityMessage(
            {{"id", "act"},
             {"type", "mcpToolCall"},
             {"server", "ketplus_browser"},
             {"tool", "browser_open"},
             {"arguments",
              QJsonObject{{"sessionId", "s1"}, {"url", "http://127.0.0.1:5173/?token=hidden"}}},
             {"result", QJsonObject{{"isError", true}, {"content", QJsonArray{}}}}},
            true);
        QCOMPARE(act.value("eventType").toString(), QStringLiteral("browser.open"));
        QCOMPARE(act.value("status").toString(), QStringLiteral("failed"));
        QVERIFY(!act.value("summary").toString().contains("hidden"));
        QVERIFY(!act.value("target").toString().contains("hidden"));
    }
    const auto unknown = ketplus::agent_message::activityMessage(
        {{"id", "unknown"}, {"type", "mcpToolCall"}, {"server", "desktop"}, {"tool", "screenshot"}},
        true);
    QCOMPARE(unknown.value("subtype").toString(), QStringLiteral("integration"));
    const auto file = ketplus::agent_message::fileChangeMessage(
        {{"id", "files"},
         {"type", "fileChange"},
         {"status", "completed"},
         {"changes", QJsonArray{QJsonObject{{"path", "created.txt"},
                                            {"kind", QJsonObject{{"type", "add"}}},
                                            {"diff", "new\n"}},
                                QJsonObject{{"path", "deleted.txt"},
                                            {"kind", QJsonObject{{"type", "delete"}}},
                                            {"diff", "old\n"}},
                                QJsonObject{{"path", "before.txt"},
                                            {"kind", QJsonObject{{"type", "update"},
                                                                 {"move_path", "after.txt"}}}}}}},
        true);
    const auto changes = file.value("fileChanges").toList();
    QCOMPARE(changes.size(), 3);
    QCOMPARE(changes[0].toMap().value("operation").toString(), QStringLiteral("Created"));
    QCOMPARE(changes[1].toMap().value("operation").toString(), QStringLiteral("Deleted"));
    QCOMPARE(changes[2].toMap().value("operation").toString(), QStringLiteral("Moved"));
    QCOMPARE(changes[2].toMap().value("destination").toString(), QStringLiteral("after.txt"));
}

void AgentWorkspaceViewTest::activityGalleryLoadsInBatches() {
    QTemporaryDir directory;
    QImage source(1600, 1200, QImage::Format_RGB32);
    source.fill(Qt::green);
    const auto path = directory.filePath("image.png");
    QVERIFY(source.save(path));
    QVariantList images;
    for (int i = 0; i < 50; ++i)
        images.append(QUrl::fromLocalFile(path).toString());
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1100, 900);
    view.show();
    view.resetMessages({QVariantMap{{"kind", "tool"},
                                    {"subtype", "imageView"},
                                    {"messageKey", "gallery"},
                                    {"title", "Viewed 50 images"},
                                    {"images", images},
                                    {"status", "completed"}}},
                       "gallery");
    auto* root = view.quickView()->rootObject();
    QQuickItem* gallery = nullptr;
    QTRY_VERIFY((gallery = findVisualItem(root, "transcriptActivityGallery")));
    const auto count = [&] {
        int result = 0;
        for (auto* child : gallery->childItems())
            if (child->objectName() == "messageAttachmentFrame")
                ++result;
        return result;
    };
    QTRY_COMPARE(count(), 3);
    auto* image = findVisualItem(gallery, "messageAttachmentImage");
    QVERIFY(image);
    QTRY_COMPARE(image->property("status").toInt(), 1);
    QVERIFY(image->property("sourceSize").toSize().width() <= 240);
    auto* more = findVisualItem(gallery, "expandMessageImages");
    QVERIFY(more);
    QCOMPARE(more->property("text").toString(), QString("+47"));
    const auto capture = qEnvironmentVariable("KETPLUS_GALLERY_CAPTURE");
    if (!capture.isEmpty()) {
        QTest::qWait(150);
        QVERIFY(view.quickView()->grabWindow().save(capture + "-collapsed.png"));
    }
    view.quickView()->requestActivate();
    QTest::qWait(50);
    more->forceActiveFocus();
    QTRY_VERIFY(more->hasActiveFocus());
    QTest::keyClick(view.quickView(), Qt::Key_Space);
    QTRY_COMPARE(count(), 50);
    QTRY_VERIFY(!more->isVisible());
    QVERIFY(gallery->property("columns").toInt() > 1);
    QVERIFY(gallery->height() > 88);
    if (!capture.isEmpty()) {
        QTest::qWait(150);
        QVERIFY(view.quickView()->grabWindow().save(capture + "-expanded.png"));
    }
    auto* row = findVisualItem(root, "transcriptActivityRow");
    QVERIFY(row);
    QVERIFY(QMetaObject::invokeMethod(row, "toggleDetails"));
    QTRY_COMPARE(count(), 50); // evidence stays visible when the receipt is collapsed
    QVERIFY(gallery->isVisible());
    QVERIFY(QMetaObject::invokeMethod(row, "toggleDetails"));
    QTRY_COMPARE(count(), 50);
    auto* frame = findVisualItem(gallery, "messageAttachmentFrame");
    QVERIFY(frame);
    view.quickView()->requestActivate();
    QTest::qWait(50);
    frame->forceActiveFocus();
    QTRY_VERIFY(frame->hasActiveFocus());
    QTest::keyClick(view.quickView(), Qt::Key_Return);
    auto* popup = root->findChild<QObject*>("imagePreview");
    QVERIFY(popup);
    QTRY_VERIFY(popup->property("visible").toBool());
    QCOMPARE(popup->property("imageSource").toUrl().toLocalFile(), path);
    QTest::keyClick(view.quickView(), Qt::Key_Escape);
    QTRY_VERIFY(!popup->property("visible").toBool());
}

// As on the phone, a command the agent described reads as two lines inside its group:
// what it is for, then the command itself.
void AgentWorkspaceViewTest::describedCommandsReadAboveTheirCommand() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1100, 700);
    view.show();
    const QList<QPair<QString, QString>> commands{
        {"Inspect the runner volumes",
         "ssh -o ConnectTimeout=10 -i ~/.ssh/ketviet runner docker inspect runner"},
        {"Copy the new runner config",
         "scp -q -i ~/.ssh/ketviet /tmp/runner.new runner:/etc/runner.json"},
        {"Check the dry-run job logs",
         "kubectl logs job/yolo-care-automation-dryrun-illon | grep -v page"},
    };
    QVariantList rows{QVariantMap{
        {"messageKey", "user"}, {"kind", "user"}, {"body", "Why did the dry run fail?"}}};
    int index = 0;
    for (const auto& [description, text] : commands) {
        auto row = ketplus::agent_message::activityMessage(
            QJsonObject{{"id", QStringLiteral("c%1").arg(++index)},
                        {"type", "commandExecution"},
                        {"command", text},
                        {"description", description},
                        {"status", "completed"}},
            true);
        row.insert("messageKey", QStringLiteral("c%1").arg(index));
        row.insert("turnId", "turn");
        rows.append(row);
    }
    view.resetMessages(rows, "described-commands");
    QTRY_COMPARE(view.messageModel()->count(), 2);
    QCOMPARE(view.messageModel()->get(1).value("groupSummary").toString(),
             QStringLiteral("Ran 3 commands"));
    view.messageModel()->setGroupExpanded(view.messageModel()->get(1).value("groupKey").toString(),
                                          true);
    QTRY_COMPARE(view.messageModel()->count(), 4);
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QQuickItem* title = nullptr;
    QTRY_VERIFY((title = findVisualItem(root, "transcriptActivityTitle")));
    QVERIFY(!findVisualItem(root, "transcriptActivityCommandLine"));
    auto* row = findVisualItem(root, "transcriptActivityRow");
    QVERIFY(row);
    QVERIFY(QMetaObject::invokeMethod(row, "toggleDetails"));
    QQuickItem* input = nullptr;
    QTRY_VERIFY((input = findVisualItem(row, "transcriptCommandInput")) && input->isVisible());
    title = findVisualItem(row, "transcriptActivityTitle");
    QVERIFY(title);
    QVERIFY(!findVisualItem(root, "transcriptActivityCommandLine"));
    const QSet<QString> descriptions{commands[0].first, commands[1].first, commands[2].first};
    QVERIFY(descriptions.contains(title->property("text").toString()));
    QCOMPARE(input->property("text").toString(), commands[0].second);
    const auto capture = qEnvironmentVariable("KETPLUS_COMMAND_ROWS_CAPTURE");
    if (!capture.isEmpty()) {
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        QTest::qWait(150);
        QVERIFY(view.quickView()->grabWindow().save(capture));
    }
}

void AgentWorkspaceViewTest::readRowTitleOpensTheFileAtItsLine() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1100, 700);
    view.show();
    auto read = ketplus::agent_message::activityMessage(
        QJsonObject{{"id", "read"},
                    {"type", "commandExecution"},
                    {"command", "cd /repo && sed -n '40,73p' docs/benchmarks.md"},
                    {"cwd", "/session"},
                    {"aggregatedOutput", "## Benchmarks"},
                    {"status", "completed"}},
        true);
    read.insert("messageKey", "read");
    view.resetMessages(
        {QVariantMap{{"messageKey", "user"}, {"kind", "user"}, {"body", "Check it"}}, read},
        "read-opens-file");
    QTRY_COMPARE(view.messageModel()->count(), 2);
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QQuickItem* title = nullptr;
    QQuickItem* chevron = nullptr;
    QTRY_VERIFY((title = findVisualItem(root, "transcriptActivityTitle")) &&
                (chevron = findVisualItem(root, "transcriptActivityChevron")) &&
                chevron->isVisible() && title->width() > 0);
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    QSignalSpy opened(&view, &ketplus::AgentWorkspaceView::fileOpenRequested);
    QTest::mouseClick(view.quickView(), Qt::LeftButton, Qt::NoModifier,
                      title->mapToScene(QPointF(8, title->height() / 2)).toPoint());
    QTRY_COMPARE(opened.count(), 1);
    QCOMPARE(opened.at(0).at(0).toString(), QStringLiteral("/repo/docs/benchmarks.md:40"));

    // The chevron still shows what the command printed instead of opening the file.
    auto* row = title->parentItem()->parentItem();
    QVERIFY(row && !row->property("expanded").toBool());
    QTest::mouseClick(
        view.quickView(), Qt::LeftButton, Qt::NoModifier,
        chevron->mapToScene(QPointF(chevron->width() / 2, chevron->height() / 2)).toPoint());
    QTRY_VERIFY(row->property("expanded").toBool());
    QCOMPARE(opened.count(), 1);
}

void AgentWorkspaceViewTest::retainedCommandNoticeRemainsVisible() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 700);
    view.show();
    const QString output =
        QStringLiteral("[Output trimmed · original size 60000 bytes · history retention]\n"
                       "first lines\n[Middle output removed]\nlast lines");
    auto command =
        ketplus::agent_message::activityMessage(QJsonObject{{"id", "retained"},
                                                            {"type", "commandExecution"},
                                                            {"command", "synthetic-command"},
                                                            {"status", "completed"},
                                                            {"aggregatedOutput", output},
                                                            {"exitCode", 0}},
                                                true);
    command.insert("messageKey", "retained");
    view.resetMessages(
        {QVariantMap{{"messageKey", "user"}, {"kind", "user"}, {"body", "History"}}, command},
        "retained-command");
    QTRY_COMPARE(view.messageModel()->count(), 2);
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QQuickItem* row = nullptr;
    QTRY_VERIFY((row = findVisualItem(root, "transcriptActivityRow")));
    QVERIFY(QMetaObject::invokeMethod(row, "toggleDetails"));
    QQuickItem* rendered = nullptr;
    QTRY_VERIFY((rendered = findVisualItem(row, "transcriptCommandOutput")) &&
                rendered->isVisible());
    const auto noticeIsRendered = [&] {
        for (auto* child : rendered->findChildren<QQuickItem*>())
            if (child->property("text").toString() == output && child->isVisible() &&
                child->width() > 0 && child->height() > 0)
                return true;
        return false;
    };
    QTRY_VERIFY(noticeIsRendered());
    QVERIFY(rendered->width() > 0 && rendered->height() > 0);
}

void AgentWorkspaceViewTest::retainedToolTextNoticeRemainsVisible_data() {
    QTest::addColumn<QString>("type");
    QTest::addColumn<QString>("server");
    QTest::addColumn<bool>("arguments");
    QTest::newRow("structured") << QString("mcpToolCall") << QString("structured") << false;
    QTest::newRow("structured-browser")
        << QString("mcpToolCall") << QString("ketplus_browser") << false;
    QTest::newRow("structured-arguments")
        << QString("dynamicToolCall") << QString("synthetic") << true;
    QTest::newRow("mcp") << QString("mcpToolCall") << QString("synthetic") << false;
    QTest::newRow("browser") << QString("mcpToolCall") << QString("ketplus_browser") << false;
    QTest::newRow("dynamic") << QString("dynamicToolCall") << QString("synthetic") << false;
    QTest::newRow("arguments-mcp") << QString("mcpToolCall") << QString("synthetic") << true;
    QTest::newRow("arguments-browser")
        << QString("mcpToolCall") << QString("ketplus_browser") << true;
    QTest::newRow("arguments-dynamic")
        << QString("dynamicToolCall") << QString("synthetic") << true;
}

// #128: a compacted command or picture says what went, how large it was and how old.
void AgentWorkspaceViewTest::compactedItemsSayWhatWasTrimmed() {
    const auto marker = [](const QString& policy, qint64 bytes, int days) {
        const auto at = QDateTime::fromString("2026-11-10T08:00:00.000Z", Qt::ISODateWithMs);
        return QJsonObject{{"policy", policy},
                           {"original_bytes", bytes},
                           {"at", at.toString(Qt::ISODateWithMs)},
                           {"cutoff", at.addDays(-days).toString(Qt::ISODateWithMs)}};
    };
    const auto command = ketplus::agent_message::activityMessage(
        QJsonObject{{"id", "old-command"},
                    {"type", "commandExecution"},
                    {"command", "make test"},
                    {"status", "completed"},
                    {"aggregatedOutput", "head…tail"},
                    {"compacted", marker("command-output-v1", 1258291, 30)}},
        true);
    QCOMPARE(command.value("title").toString(),
             QString("Ran: make test · Output trimmed · 1.2 MB · older than 30 days"));
    // Size pressure trims with the current time as the cutoff: no age to state.
    const auto image = ketplus::agent_message::activityMessage(
        QJsonObject{{"id", "old-image"},
                    {"type", "imageGeneration"},
                    {"status", "completed"},
                    {"savedPath", "/missing/kite.png"},
                    {"compacted", marker("image-preview-v1", 40 * 1024, 0)}},
        true);
    QCOMPARE(image.value("title").toString(),
             QString("Generated an image · Full image trimmed · 40 KB"));
    const auto untouched =
        ketplus::agent_message::activityMessage(QJsonObject{{"id", "new-command"},
                                                            {"type", "commandExecution"},
                                                            {"command", "make test"},
                                                            {"status", "completed"}},
                                                true);
    QCOMPARE(untouched.value("title").toString(), QString("Ran: make test"));
}

void AgentWorkspaceViewTest::retainedToolTextNoticeRemainsVisible() {
    QFETCH(QString, type);
    QFETCH(QString, server);
    QFETCH(bool, arguments);
    QJsonObject marker{{"policy", "tool-result-text-v1"}, {"original_bytes", 9000}};
    if (arguments)
        marker.insert("arguments", QJsonObject{{"policy", "tool-arguments-v1"}});
    const bool structured = QByteArray(QTest::currentDataTag()).startsWith("structured");
    if (structured)
        marker.insert("structured_result", QJsonObject{{"policy", "tool-structured-result-v1"}});
    const QString notice =
        structured  ? (arguments ? "Result and arguments trimmed" : "Result details trimmed")
        : arguments ? "Text and arguments trimmed"
                    : "Text trimmed";
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 700);
    view.show();
    auto tool = ketplus::agent_message::activityMessage(
        QJsonObject{
            {"id", "retained-tool"},
            {"type", type},
            {"server", server},
            {"tool", "browser_open"},
            {"status", "completed"},
            {"arguments", QJsonObject{}},
            {"result",
             QJsonObject{{"content",
                          QJsonArray{QJsonObject{
                              {"type", "text"},
                              {"text", "[Tool result trimmed · history retention]\npreview"}}}}}},
            {"compacted", marker}},
        true);
    tool.insert("messageKey", "retained-tool");
    view.resetMessages(
        {QVariantMap{{"messageKey", "user"}, {"kind", "user"}, {"body", "History"}}, tool},
        "retained-tool");
    QTRY_COMPARE(view.messageModel()->count(), 2);
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QQuickItem* title = nullptr;
    QTRY_VERIFY((title = findVisualItem(root, "transcriptActivityTitle")) && title->isVisible());
    QVERIFY(title->property("text").toString().contains(notice));
    QVERIFY(title->width() > 0 && title->height() > 0);
    auto* row = findVisualItem(root, "transcriptActivityRow");
    QVERIFY(row);
    auto* accessible = QAccessible::queryAccessibleInterface(row);
    QVERIFY(accessible);
    QVERIFY(accessible->text(QAccessible::Name).contains(notice));
}

void AgentWorkspaceViewTest::retainedFileDiffNoticeRemainsVisible() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 700);
    view.show();
    auto file = ketplus::agent_message::fileChangeMessage(
        {{"id", "retained-file"},
         {"type", "fileChange"},
         {"status", "completed"},
         {"compacted", QJsonObject{{"policy", "file-diff-v1"}}},
         {"changes", QJsonArray{QJsonObject{
                         {"path", "src/Old.cpp"},
                         {"kind", QJsonObject{{"type", "update"}, {"move_path", "src/New.cpp"}}},
                         {"diff_excerpt", "@@ -1 +1 @@\n+partial\n"},
                         {"compacted", QJsonObject{{"policy", "file-diff-v1"},
                                                   {"stats", QJsonObject{{"known", true},
                                                                         {"added", 5000},
                                                                         {"removed", 4500}}}}}}}}},
        true);
    const auto stats = file.value("fileChanges").toList().first().toMap();
    QCOMPARE(stats.value("added").toInt(), 5000);
    QCOMPARE(stats.value("removed").toInt(), 4500);
    QVERIFY(stats.value("known").toBool());
    QCOMPARE(stats.value("operation").toString(), "Moved");
    QCOMPARE(stats.value("destination").toString(), "src/New.cpp");
    file.insert("messageKey", "retained-file");
    view.resetMessages(
        {QVariantMap{{"messageKey", "user"}, {"kind", "user"}, {"body", "History"}}, file},
        "retained-file");
    QTRY_COMPARE(view.messageModel()->count(), 2);
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QQuickItem* title = nullptr;
    QTRY_VERIFY((title = findVisualItem(root, "transcriptActivityTitle")) && title->isVisible());
    auto* row = findVisualItem(root, "transcriptActivityRow");
    QVERIFY(row);
    auto* detail = findVisualItem(root, "transcriptActivityDetail");
    QVERIFY(detail);
    for (bool compact : {false, true}) {
        row->setProperty("compact", compact);
        // The mock's step: "Moved", then both ends with the counts and the trimmed notice.
        QTRY_COMPARE(title->property("text").toString(), QStringLiteral("Moved"));
        QCOMPARE(detail->property("text").toString(),
                 QString::fromUtf8("src/Old.cpp → src/New.cpp  +5000 −4500 · Diff trimmed"));
        QVERIFY(title->width() > 0 && title->height() > 0);
        auto* accessible = QAccessible::queryAccessibleInterface(row);
        QVERIFY(accessible);
        QVERIFY(accessible->text(QAccessible::Name).contains("Diff trimmed"));
    }
}
