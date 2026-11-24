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
            {"cleanup_status", state == "needs_attention"                  ? "unverified"
                               : state == "completed" || state == "failed" ? "best_effort_complete"
                                                                           : "not_required"},
            {"exit_code", 0}};
}
QVariantMap access(const QString& scope = "owner:1") {
    return {{"scope", scope}, {"canRead", true}, {"canControl", true}, {"canResolve", true}};
}
} // namespace

void AgentWorkspaceViewTest::managedTaskCleanupRequiresUserConfirmation() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setActivePath(QDir::tempPath());
    view.setBackgroundTaskState(access());
    view.setBackgroundTasks({task("attention", "needs_attention")});
    view.resize(900, 760);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* root = view.quickView()->rootObject();
    auto* bar = findVisualItem(root, "backgroundTaskBar");
    QVERIFY(bar);
    QTRY_VERIFY(bar->isVisible());
    QVERIFY(bar->property("headline").toString().contains("cleanup needs attention"));
    auto* details = findVisualItem(root, "backgroundTaskDetails_attention");
    QVERIFY(details);
    QVERIFY(details->property("text").toString().contains("Root exit code: 0"));
    QVERIFY(details->property("text").toString().contains("cleanup remains unverified"));
    auto* row = findVisualItem(root, "backgroundTaskCard_attention");
    QVERIFY(row);
    auto* warning = findVisualItem(root, "backgroundTaskAttention_attention");
    QVERIFY(warning);
    QVERIFY(warning->isVisible());
    QVERIFY(!row->isVisible()); // A warning banner, never a success or spinner row.
    QVERIFY(QMetaObject::invokeMethod(row, "dismissRequested"));
    QVERIFY(bar->isVisible()); // An unresolved incident cannot be dismissed locally.

    QSignalSpy resolved(&view,
                        &ketplus::AgentWorkspaceView::backgroundTaskCleanupResolutionRequested);
    QSignalSpy stopped(&view, &ketplus::AgentWorkspaceView::backgroundTaskStopRequested);
    auto* resolve = findVisualItem(root, "resolveTaskCleanup_attention");
    QVERIFY(resolve);
    QVERIFY(QMetaObject::invokeMethod(resolve, "clicked"));
    auto* dialog = root->findChild<QObject*>("backgroundTaskCleanupDialog");
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->property("visible").toBool());
    QCOMPARE(resolved.count(), 0);
    auto newer = task("attention", "needs_attention");
    newer.insert("revision", 22);
    view.setBackgroundTasks({newer});
    // The decision keeps the revision shown when opening, even after an update.
    auto* confirm = root->findChild<QObject*>("confirmTaskCleanup");
    QVERIFY(confirm);
    QVERIFY(QMetaObject::invokeMethod(confirm, "clicked"));
    QCOMPARE(resolved.count(), 1);
    QCOMPARE(resolved.first().at(1).toLongLong(), 21);
    view.resolveBackgroundTaskCleanup("attention", 22);
    QCOMPARE(resolved.count(), 1); // No duplicate mutation while waiting.
    view.acceptBackgroundTaskResult("attention", "resolve_cleanup",
                                    {{"error", "Task revision changed; review it again."}});
    QTRY_VERIFY(findVisualItem(root, "backgroundTaskActionError_attention")
                    ->property("text")
                    .toString()
                    .contains("revision changed"));
    QCOMPARE(view.backgroundTasks().first().toMap().value("state").toString(), "needs_attention");

    auto* retry = findVisualItem(root, "retryTaskCleanup_attention");
    QVERIFY(retry);
    QVERIFY(QMetaObject::invokeMethod(retry, "clicked"));
    QCOMPARE(stopped.count(), 1);
    view.acceptBackgroundTaskResult("attention", "stop", {{"ok", true}});
    view.setAgentAvailable(false);
    QTRY_VERIFY(!retry->isEnabled());
    QTRY_VERIFY(!findVisualItem(root, "resolveTaskCleanup_attention")->isEnabled());
    view.stopBackgroundTask("attention");
    view.resolveBackgroundTaskCleanup("attention", 22);
    QCOMPARE(stopped.count(), 1);
    QCOMPARE(resolved.count(), 1);
    view.setAgentAvailable(true);
    QTRY_VERIFY(retry->isEnabled());
    auto readOnly = access();
    readOnly.insert("canControl", false);
    readOnly.insert("canResolve", false);
    view.setBackgroundTaskState(readOnly);
    QTRY_VERIFY(!retry->isEnabled());
    view.stopBackgroundTask("attention");
    QCOMPARE(stopped.count(), 1);
}

// ADR 0127: a turn that ended on a live job says so in its header, and the opened job says
// when it was created, its timeout and that the agent continues with its result.
void AgentWorkspaceViewTest::managedTaskWaitIsShown() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setBackgroundTaskState(access());
    const auto conversation = [](int attention) {
        return QVariantMap{{"id", "owner"},
                           {"title", "Picker check"},
                           {"active", true},
                           {"started", true},
                           {"status", "Completed"},
                           {"liveJobs", 1},
                           {"queuedJobs", 0},
                           {"attentionJobs", attention},
                           {"jobTitle", "Build and verify the background worker"}};
    };
    view.setConversations({conversation(0)});
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
    // KetPlusMock --screen=workspace/job-running --open=job (and job-attention).
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

    view.setConversations({conversation(1)});
    auto attention = task("job", "needs_attention");
    attention.insert("revision", 23);
    view.setBackgroundTasks({attention});
    QCOMPARE(badge->property("label").toString(), QStringLiteral("Needs attention"));
    QTRY_VERIFY(facts->property("rows").toList().last().toList().value(0).toString() == "Next");
    capture("job-attention");
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
    // The opened output, not the row, names the task and its cleanup assurance.
    auto* footer = findVisualItem(root, "backgroundTaskOutputFooter");
    QVERIFY(footer);
    QTRY_VERIFY(footer->isVisible());
    QCOMPARE(footer->property("text").toString(), "Task job · Best effort cleanup complete");
    view.setBackgroundTasks({task("job", "running")});
    QTRY_COMPARE(footer->property("text").toString(), "Task job · Best effort cleanup");
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
    QVariantList tasks{task("attention", "needs_attention")};
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
    QCOMPARE(view.backgroundTasks().size(), 25);
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
    // Under the banner that replaces an attention card, notes start at the banner's edge.
    QCOMPARE(
        findVisualItem(root, "backgroundTaskDetails_attention")->property("leftPadding").toReal(),
        0.0);
    QVERIFY(
        findVisualItem(root, "backgroundTaskDetails_running")->property("leftPadding").toReal() >
        0);
    auto* assurance = findVisualItem(root, "backgroundTaskAssurance");
    QVERIFY(assurance);
    QTRY_VERIFY(assurance->isVisible());
    QVERIFY(assurance->property("text").toString().contains("best effort"));
    QVERIFY(assurance->property("text").toString().contains("descendant"));
    QSignalSpy pages(&view, &ketplus::AgentWorkspaceView::backgroundTasksRequested);
    auto* more = findVisualItem(root, "backgroundTasksLoadMore");
    QVERIFY(more);
    QVERIFY(QMetaObject::invokeMethod(more, "clicked"));
    QCOMPARE(pages.count(), 1);
    QVERIFY(pages.first().at(0).toBool());
    QTRY_VERIFY(bar->width() > 0);
    auto* warning = findVisualItem(root, "backgroundTaskDetails_attention");
    QVERIFY(warning);
    QTRY_VERIFY(warning->height() > 0);
    QVERIFY(warning->width() <= bar->width() + 1);
    const auto barRight = bar->mapToScene(QPointF(bar->width(), 0)).x();
    QVERIFY2(barRight <= view.quickView()->width() + 1, qPrintable(QString::number(barRight)));
    QVERIFY(bar->height() < view.height());
    auto* scroll = findVisualItem(root, "backgroundTaskRows");
    auto* acknowledgement = findVisualItem(root, "resolveTaskCleanup_attention");
    QVERIFY(scroll);
    QVERIFY(acknowledgement);
    QTRY_VERIFY(acknowledgement->mapToScene(QPointF(0, acknowledgement->height())).y() <=
                scroll->mapToScene(QPointF(0, scroll->height())).y() + 1);
    const auto captures = qEnvironmentVariable("KETPLUS_BACKGROUND_CAPTURE_DIR");
    if (!captures.isEmpty()) {
        QVERIFY(QDir().mkpath(captures));
        QTest::qWait(100);
        const auto image = view.quickView()->grabWindow();
        QVERIFY(!image.isNull());
        QVERIFY(image.save(captures +
                           QString("/tasks-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
    }
    view.acceptBackgroundTaskResult("attention", "resolve_cleanup",
                                    {{"error", "Task changed. Review cleanup again."}});
    auto* failure = findVisualItem(root, "backgroundTaskActionError_attention");
    QVERIFY(failure);
    QTRY_VERIFY(failure->isVisible());
    QTRY_VERIFY(failure->mapToScene(QPointF(0, failure->height())).y() <=
                scroll->mapToScene(QPointF(0, scroll->height())).y() + 1);
    view.acceptBackgroundTaskResult("attention", "resolve_cleanup", {{"ok", true}});
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
    QCOMPARE(view.backgroundTasks().size(), 25);
    // Kernel-verified cleanup makes no best-effort claim.
    auto verified = task("verified", "running");
    verified.insert("containment_mode", "cgroup_v2");
    view.setBackgroundTasks({verified});
    QTRY_VERIFY(!assurance->isVisible());
}
