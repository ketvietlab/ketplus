#include "AgentWorkspaceViewTest.h"
#include "KetAppearance.h"
#include "agent/AgentWorkspaceView.h"

#include <QDateTime>
#include <QDir>
#include <QFont>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QtTest>

using namespace agent_workspace_test;

namespace {
QVariantMap task(const QString& id, const QString& state) {
    return {{"task_id", id},
            {"schema_version", 1},
            {"kind", "process"},
            {"title", "Build and verify the background worker"},
            {"state", state},
            {"revision", 21},
            {"containment_mode", "native_best_effort"},
            {"cleanup_status",
             state == "completed" || state == "failed" ? "best_effort_complete" : "not_required"},
            {"exit_code", 0}};
}
QVariantMap access(const QString& scope = "owner:1") {
    return {{"scope", scope}, {"canRead", true}, {"canControl", true}};
}
} // namespace

// The daemon settles cleanup itself: whatever state a task is in, the user is shown no cleanup
// warning, no acknowledgement control and no assurance line.
void AgentWorkspaceViewTest::managedTaskNeverShowsCleanupToTheUser() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setActivePath(QDir::tempPath());
    view.setBackgroundTaskState(access());
    view.setBackgroundTasks(
        {task("running", "running"), task("ended", "interrupted"), task("done", "completed")});
    view.resize(900, 760);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* root = view.quickView()->rootObject();
    auto* bar = findVisualItem(root, "backgroundTaskBar");
    QVERIFY(bar);
    QTRY_VERIFY(bar->isVisible());
    QVERIFY(!bar->property("headline").toString().contains("cleanup", Qt::CaseInsensitive));
    QVERIFY(!bar->property("headline").toString().contains("attention", Qt::CaseInsensitive));
    for (const auto* name : {"backgroundTaskAssurance", "backgroundTaskAttention_running",
                             "resolveTaskCleanup_running", "retryTaskCleanup_running",
                             "backgroundTaskAttention_ended", "resolveTaskCleanup_ended"})
        QVERIFY2(!findVisualItem(root, name), name);
    QVERIFY(!root->findChild<QObject*>("backgroundTaskCleanupDialog"));
    QVERIFY(!root->findChild<QObject*>("confirmTaskCleanup"));
    for (const auto* id : {"running", "ended", "done"}) {
        auto* details = findVisualItem(root, QByteArray("backgroundTaskDetails_") + id);
        QVERIFY(details);
        QVERIFY(!details->property("text").toString().contains("cleanup", Qt::CaseInsensitive));
    }
    QVERIFY(findVisualItem(root, "backgroundTaskCard_running")->isVisible());
    QVERIFY(QMetaObject::invokeMethod(findVisualItem(root, "backgroundTaskCard_ended"),
                                      "dismissRequested"));
    QTRY_VERIFY(!findVisualItem(root, "backgroundTaskCard_ended"));
    // A record an earlier daemon left in the retired state is read as a plain ended task.
    view.setBackgroundTasks({task("legacy", "needs_attention")});
    QTRY_VERIFY(findVisualItem(root, "backgroundTaskCard_legacy"));
    QVERIFY(!findVisualItem(root, "backgroundTaskAttention_legacy"));
    QVERIFY(!findVisualItem(root, "resolveTaskCleanup_legacy"));
}

// ADR 0127: a turn that ended on a live job says so in its header, and the opened job says
// when it was created, its timeout and that the agent continues with its result.
void AgentWorkspaceViewTest::managedTaskWaitIsShown() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setBackgroundTaskState(access());
    const auto conversation = []() {
        return QVariantMap{
            {"id", "owner"},         {"title", "Picker check"},
            {"active", true},        {"started", true},
            {"status", "Completed"}, {"liveJobs", 1},
            {"queuedJobs", 0},       {"jobTitle", "Build and verify the background worker"}};
    };
    view.setConversations({conversation()});
    auto running = task("job", "running");
    running.insert("created_at", QDateTime::currentMSecsSinceEpoch() - 194000);
    running.insert("timeout_ms", 30 * 60 * 1000);
    view.setBackgroundTasks({running});
    auto* root = view.quickView()->rootObject();
    auto* badge = root->findChild<QObject*>("conversationStatusBadge");
    QVERIFY(badge);
    QCOMPARE(badge->property("label").toString(), QStringLiteral("Waiting on job"));

    view.requestBackgroundTaskOutput("job");
    auto* facts = root->findChild<QQuickItem*>("backgroundTaskFacts");
    QVERIFY(facts);
    QTRY_VERIFY(facts->isVisible());
    const auto rows = facts->property("rows").toList();
    QStringList names;
    for (const auto& row : rows)
        names << row.toList().value(0).toString();
    QCOMPARE(names, (QStringList{"Created", "Timeout", "When it ends"}));
    QVERIFY(rows.at(0).toList().value(1).toString().startsWith(QStringLiteral("3m 1")));
    QCOMPARE(rows.at(1).toList().value(1).toString(), QStringLiteral("30m"));
    // KETPLUS_JOB_WAIT_CAPTURE_DIR saves the header and the opened job, to set beside
    // KetPlusMock --screen=workspace/job-running --open=job.
    const auto captures = qEnvironmentVariable("KETPLUS_JOB_WAIT_CAPTURE_DIR");
    const auto capture = [&](const QString& name) {
        if (captures.isEmpty())
            return;
        if (!view.isVisible()) {
            view.setProductSidebarVisible(false);
            view.resize(760, 820);
            view.show();
            QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        }
        QTest::qWait(100);
        QVERIFY(QDir().mkpath(captures));
        QVERIFY(view.quickView()->grabWindow().save(captures + "/" + name + ".png"));
    };
    capture("job-waiting");

    // A job stopping toward cancellation wakes no one (ADR 0127): the pane says so.
    auto cancelling = task("job", "stopping");
    cancelling.insert("revision", 22);
    cancelling.insert("terminal_target", "cancelled");
    view.setBackgroundTasks({cancelling});
    QTRY_COMPARE(facts->property("rows").toList().last().toList().value(1).toString(),
                 QStringLiteral("It ends as cancelled; the agent is not woken"));
}

void AgentWorkspaceViewTest::managedTaskOutputFencesRequestsAndKeepsCursors() {
    ketplus::AgentWorkspaceView view;
    view.setAgentAvailable(true);
    view.setBackgroundTaskState(access());
    view.setBackgroundTasks({task("job", "running")});
    QSignalSpy reads(&view, &ketplus::AgentWorkspaceView::backgroundTaskOutputRequested);
    view.requestBackgroundTaskOutput("job");
    QCOMPARE(reads.count(), 1);
    const auto old = reads.last().at(2).toULongLong();
    view.requestBackgroundTaskOutput("job");
    QCOMPARE(reads.count(), 1);
    view.closeBackgroundTaskOutput();
    view.requestBackgroundTaskOutput("job");
    const auto current = reads.last().at(2).toULongLong();
    view.acceptBackgroundTaskResult("job", "output",
                                    {{"request", QString::number(old)}, {"output", "stale"}});
    QVERIFY(!view.backgroundTaskOutput().contains("output"));
    const QString cursor = "opaque/+cursor==";
    view.acceptBackgroundTaskResult("job", "output",
                                    {{"request", QString::number(current)},
                                     {"output", "<b>plain data</b>"},
                                     {"next_cursor", cursor}});
    view.requestBackgroundTaskOutput("job");
    QCOMPARE(reads.last().at(1).toString(), cursor);
    view.acceptBackgroundTaskResult("job", "output",
                                    {{"request", QString::number(reads.last().at(2).toULongLong())},
                                     {"output", "\nsecond page"},
                                     {"next_cursor", "next"},
                                     {"truncated", true}});
    QVERIFY(view.backgroundTaskOutput().value("output").toString().contains("<b>plain data</b>"));
    QVERIFY(view.backgroundTaskOutput().value("output").toString().contains("Output gap:"));
    QVERIFY(view.backgroundTaskOutput().value("output").toString().endsWith("second page"));
    QVERIFY(view.backgroundTaskOutput().value("truncated").toBool());
    view.requestBackgroundTaskOutput("job");
    view.acceptBackgroundTaskResult("job", "output", {{"error", "Permission denied"}});
    QVERIFY(view.backgroundTaskOutput().value("output").toString().contains("second page"));
    view.setAgentAvailable(false);
    const auto before = reads.count();
    view.requestBackgroundTaskOutput("job");
    QCOMPARE(reads.count(), before);
    auto* outputText =
        view.quickView()->rootObject()->findChild<QObject*>("backgroundTaskOutputText");
    QVERIFY(outputText);
    QCOMPARE(outputText->property("textFormat").toInt(),
             0); // Explicit plain text, including markup-like output.
    view.setBackgroundTaskState(access("other-owner:2"));
    QVERIFY(view.backgroundTaskOutput().isEmpty());
    view.acceptBackgroundTaskResult("job", "output", {{"output", "wrong conversation"}});
    QVERIFY(view.backgroundTaskOutput().isEmpty());
}

void AgentWorkspaceViewTest::managedTaskOutputRetriesAfterReadError() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setBackgroundTaskState(access());
    view.setBackgroundTasks({task("job", "completed")});
    view.setProductSidebarVisible(false);
    view.resize(900, 760);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    QSignalSpy reads(&view, &ketplus::AgentWorkspaceView::backgroundTaskOutputRequested);
    view.requestBackgroundTaskOutput("job");
    QCOMPARE(reads.count(), 1);
    view.acceptBackgroundTaskResult("job", "output", {{"error", "Temporary read failure"}});
    auto* root = view.quickView()->rootObject();
    auto* retry = findVisualItem(root, "backgroundTaskOutputRetry");
    QVERIFY(retry);
    QTRY_VERIFY(retry->isVisible());
    QTRY_VERIFY(retry->isEnabled());

    auto denied = access();
    denied.insert("canRead", false);
    view.setBackgroundTaskState(denied);
    QTRY_VERIFY(!retry->isEnabled());
    view.requestBackgroundTaskOutput("job");
    QCOMPARE(reads.count(), 1);
    auto disconnected = access();
    disconnected.insert("connected", false);
    view.setBackgroundTaskState(disconnected);
    QTRY_VERIFY(!retry->isEnabled());
    view.requestBackgroundTaskOutput("job");
    QCOMPARE(reads.count(), 1);
    view.setBackgroundTaskState(access());
    view.setAgentAvailable(false);
    QTRY_VERIFY(!retry->isEnabled());
    view.requestBackgroundTaskOutput("job");
    QCOMPARE(reads.count(), 1);
    view.setAgentAvailable(true);
    QTRY_VERIFY(retry->isEnabled());

    QTest::mouseClick(view.quickView(), Qt::LeftButton, Qt::NoModifier,
                      retry->mapToScene(retry->boundingRect().center()).toPoint());
    QTRY_COMPARE(reads.count(), 2);
    QTRY_VERIFY(!retry->isEnabled());
    view.requestBackgroundTaskOutput("job");
    QCOMPARE(reads.count(), 2); // The retry remains fenced while the read is pending.
    view.acceptBackgroundTaskResult("job", "output",
                                    {{"request", QString::number(reads.last().at(2).toULongLong())},
                                     {"output", "Recovered output"}});
    QTRY_VERIFY(!retry->isVisible());
    QVERIFY(view.backgroundTaskOutput().value("error").toString().isEmpty());
    QCOMPARE(view.backgroundTaskOutput().value("output").toString(), "Recovered output");
    auto* output = root->findChild<QObject*>("backgroundTaskOutputText");
    QVERIFY(output);
    QTRY_COMPARE(output->property("text").toString(), "Recovered output");
    // The opened output, not the row, names the task.
    auto* footer = findVisualItem(root, "backgroundTaskOutputFooter");
    QVERIFY(footer);
    QTRY_VERIFY(footer->isVisible());
    QCOMPARE(footer->property("text").toString(), "Task job");
    view.setBackgroundTasks({task("job", "running")});
    QTRY_COMPARE(footer->property("text").toString(), "Task job");
}

void AgentWorkspaceViewTest::managedTaskStatesRender_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<int>("width");
    for (bool dark : {false, true})
        for (int width : {390, 900})
            QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light").arg(width)))
                << dark << width;
}

void AgentWorkspaceViewTest::managedTaskStatesRender() {
    QFETCH(bool, dark);
    QFETCH(int, width);
    ket::Appearance::instance().setMode(dark ? ket::Appearance::Mode::Dark
                                             : ket::Appearance::Mode::Light);
    ket::Appearance::instance().setZoom(width == 390 ? 1.25 : 1.0);
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setActivePath(QDir::tempPath());
    auto page = access();
    page.insert("nextCursor", "opaque-registry-page");
    view.setBackgroundTaskState(page);
    QVariantList tasks;
    for (const auto& state : {"queued", "starting", "running", "stopping", "completed", "failed",
                              "cancelled", "timed_out", "interrupted"}) {
        auto record = task(state, state);
        record.insert("completion",
                      QVariantMap{{"status", QString(state) == "completed" ? "delivered"
                                             : QString(state) == "failed"  ? "suppressed"
                                                                           : "pending"},
                                  {"reason", "owner_busy"}});
        tasks.append(record);
    }
    for (int i = 0; i < 15; ++i)
        tasks.append(task(QString("queued-%1").arg(i), "queued"));
    view.setBackgroundTasks(tasks);
    view.setProductSidebarVisible(false);
    view.resize(width, 820);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* root = view.quickView()->rootObject();
    auto* bar = findVisualItem(root, "backgroundTaskBar");
    QVERIFY(bar);
    QTRY_VERIFY(bar->isVisible());
    QTRY_VERIFY(findVisualItem(root, "backgroundTaskCard_queued-14"));
    QCOMPARE(view.backgroundTasks().size(), 24);
    for (const auto& value : tasks) {
        const auto id = value.toMap().value("task_id").toString();
        auto* card = findVisualItem(root, "backgroundTaskCard_" + id);
        QVERIFY(card);
        QVERIFY2(!card->property("meta").toString().contains("undefined"), qPrintable(id));
        // Under a card only notes about that task, set like its meta line: no task id and
        // no machine-wide assurance repeated per row.
        auto* details = findVisualItem(root, "backgroundTaskDetails_" + id);
        QVERIFY(details);
        const auto text = details->property("text").toString();
        QVERIFY2(!text.contains("Task " + id) && !text.contains("Native macOS"), qPrintable(id));
        const auto recipe = card->property("recipe").toMap();
        QVERIFY2(qAbs(details->property("font").value<QFont>().pixelSize() -
                      recipe.value("metaPixels").toReal()) < 1.0,
                 qPrintable(id));
    }
    auto* quiet = findVisualItem(root, "backgroundTaskDetails_queued-0");
    QVERIFY(quiet);
    QVERIFY(!quiet->isVisible());
    // A failed card's meta is red; its follow-up note is not an error.
    QCOMPARE(findVisualItem(root, "backgroundTaskDetails_failed")->property("color"),
             findVisualItem(root, "backgroundTaskDetails_completed")->property("color"));
    QVERIFY(
        findVisualItem(root, "backgroundTaskDetails_running")->property("leftPadding").toReal() >
        0);
    QSignalSpy pages(&view, &ketplus::AgentWorkspaceView::backgroundTasksRequested);
    auto* more = findVisualItem(root, "backgroundTasksLoadMore");
    QVERIFY(more);
    QVERIFY(QMetaObject::invokeMethod(more, "clicked"));
    QCOMPARE(pages.count(), 1);
    QVERIFY(pages.first().at(0).toBool());
    QTRY_VERIFY(bar->width() > 0);
    const auto barRight = bar->mapToScene(QPointF(bar->width(), 0)).x();
    QVERIFY2(barRight <= view.quickView()->width() + 1, qPrintable(QString::number(barRight)));
    QVERIFY(bar->height() < view.height());
    auto* scroll = findVisualItem(root, "backgroundTaskRows");
    QVERIFY(scroll);
    const auto captures = qEnvironmentVariable("KETPLUS_BACKGROUND_CAPTURE_DIR");
    if (!captures.isEmpty()) {
        QVERIFY(QDir().mkpath(captures));
        QTest::qWait(100);
        const auto image = view.quickView()->grabWindow();
        QVERIFY(!image.isNull());
        QVERIFY(image.save(captures +
                           QString("/tasks-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
    }
    view.acceptBackgroundTaskResult("running", "stop", {{"error", "Task changed. Try again."}});
    auto* failure = findVisualItem(root, "backgroundTaskActionError_running");
    QVERIFY(failure);
    QTRY_VERIFY(failure->isVisible());
    QTRY_VERIFY(failure->mapToScene(QPointF(0, failure->height())).y() <=
                scroll->mapToScene(QPointF(0, scroll->height())).y() + 1);
    view.acceptBackgroundTaskResult("running", "stop", {{"ok", true}});
    QPointer<QQuickItem> focused = findVisualItem(root, "backgroundTaskCard_queued");
    QVERIFY(focused);
    view.quickView()->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(view.quickView()));
    focused->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(focused && focused->hasActiveFocus());
    QTRY_VERIFY(focused->mapToScene(QPointF(0, focused->height())).y() <=
                scroll->mapToScene(QPointF(0, scroll->height())).y() + 1);
    page.insert("loading", true);
    view.setBackgroundTaskState(page);
    QTRY_VERIFY(focused && focused->hasActiveFocus());
    QCOMPARE(findVisualItem(root, "backgroundTaskCard_queued"), focused.data());
    page.insert("loading", false);
    view.setBackgroundTaskState(page);
    ket::Appearance::instance().setMode(dark ? ket::Appearance::Mode::Light
                                             : ket::Appearance::Mode::Dark);
    ket::Appearance::instance().setZoom(width == 390 ? 1.0 : 1.25);
    view.applyTheme(testPalette());
    QTRY_VERIFY(bar->mapToScene(QPointF(bar->width(), 0)).x() <= view.quickView()->width() + 1);
    QCOMPARE(view.backgroundTasks().size(), 24);
}
