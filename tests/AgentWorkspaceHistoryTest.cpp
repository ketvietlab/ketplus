#include "AgentWorkspaceViewTest.h"
#include "KetProfile.h"
#include "agent/AgentMessageFormatter.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentTranscriptWindowBudget.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexAppServer.h"
#include "agent/McpToolResultMedia.h"
#include "agent/TranscriptNormalizer.h"
#include "agent/TranscriptPresentationGrouper.h"
#include "ui/Theme.h"

#include <QAbstractItemModelTester>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextImageFormat>
#include <QtTest>

using namespace agent_workspace_test;

void AgentWorkspaceViewTest::webSearchLabelsAndAnimation() {
    QTemporaryDir directory;
    const auto scriptPath = directory.filePath("web-search.py");
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(#!/usr/bin/env python3
import json,sys
for line in sys.stdin:
    r=json.loads(line)
    if "id" not in r: continue
    m=r.get("method")
    result={}
    if m in ("thread/start","thread/resume"): result={"thread":{"id":"web-thread","turns":[]}}
    elif m=="turn/start": result={"turn":{"id":"web-turn"}}
    elif m=="thread/turns/list": result={"data":[{"id":"web-turn","status":"completed","items":[
        {"id":"search","type":"webSearch","query":"Qt documentation"}]}],"nextCursor":None}
    print(json.dumps({"id":r["id"],"result":result}),flush=True)
)PY");
    script.close();
    QVERIFY(QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));
    ketplus::CodexAppServer server(scriptPath);
    ketplus::AgentSessionManager manager(&server);
    const auto path = directory.path();
    manager.submitPrompt(path, "Find documentation");
    QTRY_COMPARE(manager.status(path), QStringLiteral("Thinking…"));
    const QString query =
        "https://code.qt.io/cgit/qt/qtbase.git/tree/src/gui/text/qtextmarkdownimporter.cpp?h=6.11";
    QJsonObject item{{"id", "search"}, {"type", "webSearch"}, {"query", query}};
    const auto notify = [&](const QString& method) {
        server.notificationReceived(
            method, {{"threadId", "web-thread"}, {"turnId", "web-turn"}, {"item", item}});
    };
    notify("item/started");
    auto entry = manager.messages(path).last().toMap();
    QCOMPARE(entry.value("meta").toString(), QStringLiteral("Search web · running"));
    QCOMPARE(entry.value("body").toString(), query);
    QCOMPARE(entry.value("subtype").toString(), "webSearch");
    QVERIFY(entry.value("running").toBool());
    const auto key = entry.value("messageKey");
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(600, 640);
    view.show();
    view.resetMessages({entry}, "web-search");
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    auto* shine = findVisualItem(root, "toolActivityShine");
    QVERIFY(shine && shine->isVisible());
    auto* activityTitle = findVisualItem(root, "transcriptActivityTitle");
    QVERIFY(activityTitle && activityTitle->isVisible());
    QCOMPARE(activityTitle->property("text").toString(), QStringLiteral("Searching the web"));
    auto* activityRow = findVisualItem(root, "transcriptActivityRow");
    QVERIFY(activityRow);
    QVERIFY(QMetaObject::invokeMethod(activityRow, "toggleDetails"));
    auto* activityDetail = findVisualItem(root, "transcriptActivityDetailText");
    QVERIFY(activityDetail && activityDetail->isVisible());
    QCOMPARE(activityDetail->property("text").toString(), query);
    view.quickView()->requestActivate();
    QTest::qWait(50);
    activityRow->forceActiveFocus();
    QTRY_VERIFY(activityRow->hasActiveFocus());
    QTest::keyClick(view.quickView(), Qt::Key_Left);
    QTRY_VERIFY(!activityRow->property("expanded").toBool());
    QCOMPARE(activityDetail->property("text").toString(), QString{});
    QTest::keyClick(view.quickView(), Qt::Key_Right);
    QTRY_VERIFY(activityRow->property("expanded").toBool());
    QCOMPARE(activityDetail->property("text").toString(), query);
    auto* copyDetails = findVisualItem(root, "copyActivityDetails");
    QVERIFY(copyDetails);
    QVERIFY(QMetaObject::invokeMethod(copyDetails, "clicked"));
    QCOMPARE(QGuiApplication::clipboard()->text(), query);
    const auto capturePath = qEnvironmentVariable("KETPLUS_WEB_CAPTURE");
    if (!capturePath.isEmpty()) {
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        QVERIFY(view.quickView()->grabWindow().save(capturePath + ".running.png"));
    }
    notify("item/completed");
    entry = manager.messages(path).last().toMap();
    QCOMPARE(entry.value("meta").toString(), QStringLiteral("Search web · completed"));
    QCOMPARE(entry.value("messageKey"), key);
    QVERIFY(!entry.value("running").toBool());
    view.updateMessage(0, entry);
    QTRY_VERIFY(!shine->isVisible());
    if (!capturePath.isEmpty())
        QVERIFY(view.quickView()->grabWindow().save(capturePath + ".completed.png"));
    item["id"] = "canceled-search";
    notify("item/started");
    server.notificationReceived(
        "turn/completed", {{"threadId", "web-thread"},
                           {"turn", QJsonObject{{"id", "web-turn"}, {"status", "interrupted"}}}});
    entry = manager.messages(path).last().toMap();
    QVERIFY(!entry.value("running").toBool());
    QCOMPARE(entry.value("meta").toString(), QStringLiteral("Search web · stopped"));
    item = {{"id", "other-tool"}, {"type", "dynamicToolCall"}, {"tool", "custom"}};
    notify("item/completed");
    QCOMPARE(manager.messages(path).last().toMap().value("eventType").toString(),
             QStringLiteral("tool.dynamic"));
    QCOMPARE(manager.messages(path).last().toMap().value("title").toString(),
             QStringLiteral("Used custom"));
    ketplus::AgentSessionManager restored(&server);
    restored.activate(path);
    QTRY_COMPARE(restored.messages(path).size(), 1);
    QTRY_VERIFY(!restored.historyState(path).value(QStringLiteral("loading")).toBool());
    entry = restored.messages(path).first().toMap();
    QCOMPARE(entry.value("meta").toString(), QStringLiteral("Search web · completed"));
    QCOMPARE(entry.value("subtype").toString(), "webSearch");
    QVERIFY(!entry.value("running").toBool());
}

void AgentWorkspaceViewTest::messageModelUpdatesIncrementally() {
    ketplus::AgentMessageModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    model.reset(
        {QVariantMap{{"messageKey", "current"}, {"body", "before"}, {"kind", "assistant"}}});
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    model.set(0, {{"body", "after"}, {"kind", "assistant"}});
    QCOMPARE(changed.size(), 1);
    const auto roles = changed.first().at(2).value<QList<int>>();
    QVERIFY(roles.size() >= 1);
    QVERIFY(roles.contains(model.roleNames().key(QByteArray("body"))));
    model.prepend({QVariantMap{{"messageKey", "older"}, {"body", "old"}}});
    QCOMPARE(inserted.size(), 1);
    QCOMPARE(reset.size(), 0);
    QCOMPARE(model.indexOfKey("current"), 1);
    QCOMPARE(model.get(1).value("body").toString(), "after");
    model.set(1, model.get(1));
    QCOMPARE(changed.size(), 1);
}

void AgentWorkspaceViewTest::expiredQuestionsLeaveNoTranscriptRow() {
    ketplus::AgentMessageModel model;
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QVariantMap question{
        {"messageKey", "question"}, {"kind", "tool"}, {"subtype", "question"}, {"running", true}};
    const QVariantMap after{{"messageKey", "after"}, {"kind", "assistant"}, {"body", "Continue"}};
    model.reset({question, after});
    QCOMPARE(model.count(), 2);
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    question.insert("running", false);
    question.insert("status", "expired");
    model.set(0, question);
    QCOMPARE(model.count(), 1);
    QCOMPARE(model.get(0).value("messageKey").toString(), "after");
    QCOMPARE(model.geometry()->rowCount(), 1);
    QCOMPARE(removed.size(), 1);
    QCOMPARE(resets.size(), 0);
    QCOMPARE(model.sourceCount(), 2);
    model.reset({question, after}); // History/cache hydration uses the same visibility rule.
    QCOMPARE(model.count(), 1);
    QVariantMap answered = question;
    answered.insert("messageKey", "answered");
    answered.insert("status", "resolved");
    answered.insert("questionAnswers", QVariantMap{{"choice", "Yes"}});
    QVariantMap approval = question;
    approval.insert("messageKey", "approval");
    approval.insert("subtype", "approval");
    model.prepend({answered, approval});
    QCOMPARE(model.count(), 3);
    QCOMPARE(model.indexOfKey("answered"), 0);
    QCOMPARE(model.indexOfKey("approval"), 1);
    QCOMPARE(model.indexOfKey("question"), -1);
}

void AgentWorkspaceViewTest::appendDuringGiantRenderKeepsRows() {
    const QString paragraph = QStringLiteral("A paragraph of a very long answer.\n\n");
    QString body = paragraph.repeated(600 * 1024 / paragraph.size());
    ketplus::AgentMessageModel model;
    model.reset({QVariantMap{{"messageKey", "before"}, {"kind", "user"}, {"body", "Go"}},
                 QVariantMap{{"messageKey", "giant"}, {"kind", "assistant"}, {"body", body}}});
    QTRY_VERIFY_WITH_TIMEOUT(!model.materializing(), 5000);
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);

    // The giant row re-renders on a worker while new rows arrive after it and
    // an older page arrives before it. Every other row stays where it is: a
    // reset would blank the transcript until the worker finished.
    body += paragraph;
    auto growing = model.source(1);
    growing.insert("body", body);
    model.set(1, growing);
    QVERIFY(model.materializing());
    model.append(QVariantMap{{"messageKey", "tool"}, {"kind", "tool"}, {"body", "ls"}});
    model.prepend({QVariantMap{{"messageKey", "older"}, {"kind", "user"}, {"body", "Earlier"}}});
    QCOMPARE(resets.size(), 0);
    QCOMPARE(model.indexOfKey("older"), 0);
    QTRY_VERIFY_WITH_TIMEOUT(model.deferredOutstanding() == 0 && !model.materializing(), 10000);
    QCOMPARE(resets.size(), 0);
    QString rendered;
    for (int row = 0; row < model.count(); ++row)
        if (model.get(row).value("messageKey").toString().startsWith(QStringLiteral("giant")))
            rendered += model.get(row).value("body").toString();
    QCOMPARE(rendered, body);
    QCOMPARE(model.get(model.count() - 1).value("messageKey").toString(), QStringLiteral("tool"));
}

void AgentWorkspaceViewTest::chunksPathologicalAssistantMessages() {
    const QString paragraph = QStringLiteral(
        "## Result\n\nA [link](https://example.com) and **bold text** remain Markdown.\n\n");
    QString source;
    source.reserve(8 * 1024 * 1024 + paragraph.size());
    while (source.size() < 8 * 1024 * 1024)
        source += paragraph;

    ketplus::AgentMessageModel model;
    model.reset({QVariantMap{
        {"messageKey", "giant"}, {"kind", "assistant"}, {"body", source}, {"meta", "Codex"}}});
    QVERIFY(model.materializing());
    QCOMPARE(model.sourceCount(), 1);
    QCOMPARE(model.source(0).value("body").toString(), source);
    QTRY_VERIFY_WITH_TIMEOUT(!model.materializing(), 5000);
    QVERIFY(model.count() > 80);
    QCOMPARE(model.get(0).value("messageKey").toString(), QStringLiteral("giant"));
    QVERIFY(!model.get(0).value("chunkContinuation").toBool());
    for (int row = 0; row < model.count(); ++row) {
        const auto block = model.get(row);
        QVERIFY2(block.value("body").toString().size() <= 16 * 1024,
                 qPrintable(QStringLiteral("row %1 retained %2 characters")
                                .arg(row)
                                .arg(block.value("body").toString().size())));
        if (row > 0)
            QVERIFY(block.value("chunkContinuation").toBool());
        QVERIFY(!block.value("partialBody").toBool());
    }
    QVERIFY(model.get(0).value("body").toString().contains(QStringLiteral("[link]")));
    QSignalSpy giantReset(&model, &QAbstractItemModel::modelReset);
    auto growing = model.source(0);
    growing.insert("body", source + paragraph);
    model.set(0, growing);
    QVERIFY(model.materializing());
    QTRY_VERIFY_WITH_TIMEOUT(!model.materializing(), 5000);
    QCOMPARE(giantReset.size(), 0);
    QCOMPARE(model.source(0).value("body").toString(), source + paragraph);

    // Direct model updates intentionally bypass AgentWorkspaceView's display
    // timer. Even an adversarial producer may retain at most one active job
    // and one replaceable latest request; the terminal body remains lossless.
    QString terminalBody = source + paragraph;
    terminalBody.reserve(terminalBody.size() + 16 * 1024);
    for (int delta = 0; delta < 1'000; ++delta) {
        terminalBody += QStringLiteral("delta:%1\n").arg(delta);
        auto deltaItem = model.source(0);
        deltaItem.insert("body", terminalBody);
        deltaItem.insert("partialBody", delta != 999);
        deltaItem.insert("meta", delta == 999 ? QStringLiteral("Codex")
                                              : QStringLiteral("Codex · streaming"));
        model.set(0, deltaItem);
        QVERIFY2(model.deferredOutstanding() <= 2,
                 qPrintable(QStringLiteral("unbounded deferred work at delta %1").arg(delta)));
    }
    QCOMPARE(model.source(0).value("body").toString(), terminalBody);
    QVERIFY(!model.source(0).value("partialBody").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(model.deferredOutstanding() == 0 && !model.materializing(), 10000);
    QCOMPARE(model.maximumDeferredOutstanding(), 2);
    QCOMPARE(giantReset.size(), 0);
    QString renderedTerminalBody;
    for (int row = 0; row < model.count(); ++row)
        renderedTerminalBody += model.get(row).value("body").toString();
    QCOMPARE(renderedTerminalBody, terminalBody);
    QVERIFY(!model.get(0).value("partialBody").toBool());
    QCOMPARE(model.get(0).value("meta").toString(), QStringLiteral("Codex"));

    ketplus::AgentMessageModel interleaved;
    QString bodyA = paragraph.repeated(9'000);
    QString bodyB = paragraph.repeated(9'000);
    interleaved.reset(
        {QVariantMap{{"messageKey", "giant-a"}, {"kind", "assistant"}, {"body", bodyA}},
         QVariantMap{{"messageKey", "giant-b"}, {"kind", "assistant"}, {"body", bodyB}}});
    QTRY_VERIFY_WITH_TIMEOUT(!interleaved.materializing(), 5000);
    QSignalSpy interleavedReset(&interleaved, &QAbstractItemModel::modelReset);
    for (int delta = 0; delta < 200; ++delta) {
        bodyA += QStringLiteral("a:%1\n").arg(delta);
        auto a = interleaved.source(0);
        a.insert("body", bodyA);
        interleaved.set(0, a);
        bodyB += QStringLiteral("b:%1\n").arg(delta);
        auto b = interleaved.source(1);
        b.insert("body", bodyB);
        interleaved.set(1, b);
        QVERIFY(interleaved.deferredOutstanding() <= 2);
    }
    QTRY_VERIFY_WITH_TIMEOUT(interleaved.deferredOutstanding() == 0 && !interleaved.materializing(),
                             10000);
    QCOMPARE(interleavedReset.size(), 0);
    QStringList renderedSources;
    for (int row = 0; row < interleaved.count(); ++row) {
        const auto block = interleaved.get(row);
        if (!block.value("chunkContinuation").toBool())
            renderedSources.append(QString{});
        QVERIFY(!renderedSources.isEmpty());
        renderedSources.last() += block.value("body").toString();
    }
    QCOMPARE(renderedSources, QStringList({bodyA, bodyB}));

    // A small status/tool update also advances the model generation. It must
    // not strand an in-flight giant row at its previous rendered revision.
    ketplus::AgentMessageModel mixedRows;
    QString mixedGiant = source;
    mixedRows.reset(
        {QVariantMap{{"messageKey", "mixed-giant"}, {"kind", "assistant"}, {"body", mixedGiant}},
         QVariantMap{{"messageKey", "mixed-status"}, {"kind", "status"}, {"body", "old"}}});
    QTRY_VERIFY_WITH_TIMEOUT(!mixedRows.materializing(), 5000);
    mixedGiant += paragraph;
    auto giantUpdate = mixedRows.source(0);
    giantUpdate.insert("body", mixedGiant);
    mixedRows.set(0, giantUpdate);
    QVERIFY(mixedRows.deferredOutstanding() > 0);
    auto statusUpdate = mixedRows.source(1);
    statusUpdate.insert("body", "new");
    mixedRows.set(1, statusUpdate);
    QVERIFY(mixedRows.materializing());
    QTRY_VERIFY_WITH_TIMEOUT(mixedRows.deferredOutstanding() == 0 && !mixedRows.materializing(),
                             10000);
    QString mixedRendered;
    for (int row = 0; row < mixedRows.count(); ++row) {
        const auto block = mixedRows.get(row);
        if (block.value("messageKey").toString().startsWith(QStringLiteral("mixed-giant")))
            mixedRendered += block.value("body").toString();
    }
    QCOMPARE(mixedRendered, mixedGiant);
    QCOMPARE(mixedRows.source(1).value("body").toString(), QStringLiteral("new"));

    ketplus::AgentMessageModel unsafeMarkdown;
    const QString unbroken = QString(200 * 1024, u'x');
    unsafeMarkdown.reset(
        {QVariantMap{{"messageKey", "unbroken"}, {"kind", "assistant"}, {"body", unbroken}}});
    QTRY_VERIFY_WITH_TIMEOUT(!unsafeMarkdown.materializing(), 5000);
    QVERIFY(unsafeMarkdown.count() > 1);
    for (int row = 0; row < unsafeMarkdown.count(); ++row)
        QVERIFY(unsafeMarkdown.get(row).value("partialBody").toBool());

    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 700);
    view.show();
    view.resetMessages(
        {QVariantMap{
            {"messageKey", "giant"}, {"kind", "assistant"}, {"body", source}, {"meta", "Codex"}}},
        "giant-window");
    QVERIFY(view.messageModel()->materializing());
    QTRY_VERIFY_WITH_TIMEOUT(view.messageModel()->count() > 80, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(view.benchmarkUsableViewport(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.messageModel()->materializing(), 5000);
    QQuickItem* renderedBody = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        (renderedBody = findVisualItem(view.quickView()->rootObject(), "messageBody")) != nullptr,
        5000);
    QVERIFY(renderedBody->property("text").toString().size() <= 16 * 1024);

    ketplus::AgentMessageModel fenced;
    fenced.reset({QVariantMap{
        {"messageKey", "stale"}, {"kind", "assistant"}, {"body", source}, {"meta", "Codex"}}});
    QVERIFY(fenced.materializing());
    fenced.reset({QVariantMap{
        {"messageKey", "newest"}, {"kind", "assistant"}, {"body", "Newest snapshot"}}});
    QVERIFY(!fenced.materializing());
    QTest::qWait(100);
    QCOMPARE(fenced.count(), 1);
    QCOMPARE(fenced.get(0).value("messageKey").toString(), QStringLiteral("newest"));

    QVector<QVariantMap> rows;
    rows.reserve(10'000);
    for (int index = 0; index < 10'000; ++index) {
        rows.append(QVariantMap{{"messageKey", QString::number(index)},
                                {"turnId", QStringLiteral("turn:%1").arg(index / 2)},
                                {"body", QString(2048, u'x')}});
    }
    const auto newest =
        ketplus::transcript_window::trim(rows, ketplus::transcript_window::RetainedEdge::Newest);
    QVERIFY(newest.changed);
    QVERIFY(newest.removedOlder);
    QVERIFY(rows.size() <= ketplus::transcript_window::maximumMessages);
    QCOMPARE(rows.constLast().value("messageKey").toString(), QStringLiteral("9999"));

    QVector<QVariantMap> olderRows;
    olderRows.reserve(1'000);
    for (int index = 0; index < 1'000; ++index) {
        olderRows.append(QVariantMap{{"messageKey", QString::number(index)},
                                     {"turnId", QStringLiteral("turn:%1").arg(index / 2)},
                                     {"body", QString(1024, u'y')}});
    }
    const auto oldest = ketplus::transcript_window::trim(
        olderRows, ketplus::transcript_window::RetainedEdge::Oldest);
    QVERIFY(oldest.changed);
    QVERIFY(oldest.removedNewer);
    QVERIFY(olderRows.size() <= ketplus::transcript_window::maximumMessages);
    QCOMPARE(olderRows.constFirst().value("messageKey").toString(), QStringLiteral("0"));

    QVector<QVariantMap> pathologicalTurn;
    pathologicalTurn.reserve(1'000);
    for (int index = 0; index < 1'000; ++index) {
        pathologicalTurn.append(QVariantMap{{"messageKey", QStringLiteral("single:%1").arg(index)},
                                            {"turnId", "one-pathological-turn"},
                                            {"body", QString(128, u'z')}});
    }
    const auto hardBound = ketplus::transcript_window::trim(
        pathologicalTurn, ketplus::transcript_window::RetainedEdge::Newest);
    QVERIFY(hardBound.changed);
    QVERIFY(hardBound.removedOlder);
    QCOMPARE(pathologicalTurn.size(), ketplus::transcript_window::maximumMessages);
    QCOMPARE(pathologicalTurn.constLast().value("messageKey").toString(),
             QStringLiteral("single:999"));
}

void AgentWorkspaceViewTest::transcriptModelDeduplicatesPagesAndBoundsPayloads() {
    ketplus::AgentMessageModel model;
    model.reset({QVariantMap{{"messageKey", "current"}, {"body", "current"}}});
    model.prepend({QVariantMap{{"messageKey", "older"}, {"body", "older"}},
                   QVariantMap{{"messageKey", "current"}, {"body", "duplicate"}},
                   QVariantMap{{"messageKey", "older"}, {"body", "duplicate older"}}});
    QCOMPARE(model.count(), 2);
    QCOMPARE(model.get(0).value("messageKey").toString(), QStringLiteral("older"));
    QCOMPARE(model.get(1).value("body").toString(), QStringLiteral("current"));

    const QString largeOutput(96 * 1024, QLatin1Char('x'));
    model.append(QVariantMap{{"messageKey", "large"},
                             {"details", QVariantMap{{"output", largeOutput}}},
                             {"sanitizedPayload", QVariantMap{{"result", largeOutput}}}});
    const QVariantMap bounded = model.get(2);
    QVERIFY(!bounded.value("detailsRef").toString().isEmpty());
    QVERIFY(!bounded.value("payloadRef").toString().isEmpty());
    QVERIFY(bounded.value("detailsData").toString().size() < 256);
    QCOMPARE(QJsonDocument::fromJson(
                 model.resolvePayload(bounded.value("detailsRef").toString()).toUtf8())
                 .object()
                 .value("output")
                 .toString()
                 .size(),
             largeOutput.size());
    const QString firstReference = bounded.value("detailsRef").toString();
    for (int index = 0; index < 80; ++index) {
        model.append(QVariantMap{
            {"messageKey", QStringLiteral("large-%1").arg(index)},
            {"details", QVariantMap{{"output", largeOutput + QString::number(index)}}}});
    }
    QVERIFY(model.retainedPayloadCount() <= 64);
    QVERIFY(model.retainedPayloadBytes() <= 4 * 1024 * 1024);
    QVERIFY(!QJsonDocument::fromJson(model.resolvePayload(firstReference).toUtf8())
                 .object()
                 .value("available")
                 .toBool());
    model.setPayloadSource([&](const QString& key, const QString& kind) -> QVariant {
        if (key == "large" && kind == "details")
            return QVariantMap{{"output", largeOutput}};
        return {};
    });
    QCOMPARE(QJsonDocument::fromJson(model.resolvePayload(firstReference).toUtf8())
                 .object()
                 .value("output")
                 .toString(),
             largeOutput);
    QVERIFY(model.retainedPayloadBytes() <= 4 * 1024 * 1024);

    const QString giantOutput(5 * 1024 * 1024, QLatin1Char('g'));
    model.reset(
        {QVariantMap{{"messageKey", "giant"}, {"details", QVariantMap{{"output", giantOutput}}}}});
    const auto giantReference = model.get(0).value("detailsRef").toString();
    QVERIFY(!giantReference.isEmpty());
    model.setPayloadSource([&](const QString& key, const QString& kind) -> QVariant {
        if (key == "giant" && kind == "details")
            return QVariantMap{{"output", giantOutput}};
        return {};
    });
    QCOMPARE(QJsonDocument::fromJson(model.resolvePayload(giantReference).toUtf8())
                 .object()
                 .value("output")
                 .toString(),
             giantOutput);
    QCOMPARE(model.retainedPayloadBytes(), 0);
    // Stale/cross-conversation data must not be substituted for the same key.
    model.setPayloadSource([](const QString&, const QString&) -> QVariant {
        return QVariantMap{{"output", "different conversation"}};
    });
    QVERIFY(!QJsonDocument::fromJson(model.resolvePayload(giantReference).toUtf8())
                 .object()
                 .value("available")
                 .toBool());
    model.clear();
    QVERIFY(!QJsonDocument::fromJson(model.resolvePayload(firstReference).toUtf8())
                 .object()
                 .value("available")
                 .toBool());
}

void AgentWorkspaceViewTest::presentationGroupingIsDerivedAndDeterministic() {
    const QString handoff =
        QStringLiteral("# Context\nMany instructions and previous results.\n"
                       "User request: Thực hiện harden mock UI của KétPlus Desktop, sử dụng Atlas\n"
                       "<ketplus_response_preferences>More instructions");
    const QString suggested = ketplus::agent_message::firstLine(handoff);
    QVERIFY(suggested.startsWith(QStringLiteral("Thực hiện harden mock UI")));
    QVERIFY(suggested.size() <= 56);
    QVERIFY(!suggested.contains(QStringLiteral("response_preferences")));
    QCOMPARE(ketplus::agent_message::firstLine(QStringLiteral("Fix the diff gutter")),
             QStringLiteral("Fix the diff gutter"));
    const QVariantList canonical{
        QVariantMap{{"messageKey", "one"},
                    {"turnId", "turn"},
                    {"eventType", "image.view"},
                    {"images", QVariantList{"one.png"}},
                    {"details", QVariantMap{{"itemIds", QVariantList{"one"}}}}},
        QVariantMap{{"messageKey", "two"},
                    {"turnId", "turn"},
                    {"eventType", "image.view"},
                    {"images", QVariantList{"two.png"}},
                    {"details", QVariantMap{{"itemIds", QVariantList{"two"}}}}}};
    const QVariantList untouched = canonical;
    const QVariantList first = ketplus::TranscriptPresentationGrouper::group(canonical);
    const QVariantList second = ketplus::TranscriptPresentationGrouper::group(canonical);
    QCOMPARE(canonical, untouched);
    QCOMPARE(first, second);
    QCOMPARE(first.size(), 1);
    QCOMPARE(first.first().toMap().value("images").toList().size(), 2);
}

void AgentWorkspaceViewTest::coalescesStreamingDeltas() {
    QTemporaryDir directory;
    const QString scriptPath = directory.filePath(QStringLiteral("coalesce.py"));
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(#!/usr/bin/env python3
import json,sys
for line in sys.stdin:
    request=json.loads(line)
    if "id" not in request: continue
    method=request.get("method")
    result={}
    if method=="thread/start": result={"thread":{"id":"coalesce-thread","turns":[]}}
    elif method=="turn/start": result={"turn":{"id":"coalesce-turn"}}
    print(json.dumps({"id":request["id"],"result":result}),flush=True)
)PY");
    script.close();
    QVERIFY(QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));
    ketplus::CodexAppServer server(scriptPath);
    ketplus::AgentSessionManager manager(&server);
    manager.activate(directory.path());
    manager.submitPrompt(directory.path(), QStringLiteral("Stream"));
    QTRY_COMPARE(manager.status(directory.path()), QStringLiteral("Thinking…"));
    server.notificationReceived("turn/started", {{"threadId", "coalesce-thread"},
                                                 {"turn", QJsonObject{{"id", "coalesce-turn"}}}});
    server.notificationReceived("item/started", {{"threadId", "coalesce-thread"},
                                                 {"turnId", "coalesce-turn"},
                                                 {"item", QJsonObject{{"id", "answer"},
                                                                      {"type", "agentMessage"},
                                                                      {"phase", "final_answer"},
                                                                      {"text", ""}}}});
    QSignalSpy updates(&manager, &ketplus::AgentSessionManager::messageUpdated);
    QString expected;
    for (int index = 0; index < 100; ++index) {
        expected += QLatin1Char('x');
        server.notificationReceived("item/agentMessage/delta", {{"threadId", "coalesce-thread"},
                                                                {"turnId", "coalesce-turn"},
                                                                {"itemId", "answer"},
                                                                {"delta", "x"}});
    }
    QCOMPARE(updates.size(), 0);
    QTRY_COMPARE(manager.messages(directory.path()).last().toMap().value("body").toString(),
                 expected);
    QVERIFY(updates.size() <= 2);

    server.notificationReceived("item/agentMessage/delta", {{"threadId", "coalesce-thread"},
                                                            {"turnId", "coalesce-turn"},
                                                            {"itemId", "answer"},
                                                            {"delta", "!"}});
    server.notificationReceived("item/completed",
                                {{"threadId", "coalesce-thread"},
                                 {"turnId", "coalesce-turn"},
                                 {"item", QJsonObject{{"id", "answer"},
                                                      {"type", "agentMessage"},
                                                      {"phase", "final_answer"},
                                                      {"text", expected + "!"}}}});
    QCOMPARE(manager.messages(directory.path()).last().toMap().value("body").toString(),
             expected + "!");
    server.notificationReceived("turn/completed", {{"threadId", "coalesce-thread"},
                                                   {"turn", QJsonObject{{"id", "coalesce-turn"},
                                                                        {"status", "completed"}}}});
    server.notificationReceived("item/agentMessage/delta", {{"threadId", "coalesce-thread"},
                                                            {"turnId", "coalesce-turn"},
                                                            {"itemId", "answer"},
                                                            {"delta", "late"}});
    QTest::qWait(32);
    QCOMPARE(manager.messages(directory.path()).last().toMap().value("body").toString(),
             expected + "!");
}

void AgentWorkspaceViewTest::normalizesTranscriptV2() {
    const QJsonArray items{
        QJsonObject{
            {"id", "user"},
            {"type", "userMessage"},
            {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "Fix the transcript"}}}}},
        QJsonObject{{"id", "commentary"},
                    {"type", "agentMessage"},
                    {"phase", "commentary"},
                    {"text", "I am checking the repository."}},
        QJsonObject{{"id", "command"},
                    {"type", "commandExecution"},
                    {"command", "curl --token super-secret https://example.test"},
                    {"cwd", "/tmp/worktree"},
                    {"status", "completed"},
                    {"exitCode", 0},
                    {"durationMs", 42},
                    {"commandActions", QJsonArray{}}},
        QJsonObject{{"id", "image"}, {"type", "imageView"}, {"path", "/tmp/screenshot.png"}},
        QJsonObject{{"id", "image-2"}, {"type", "imageView"}, {"path", "/tmp/screenshot-2.png"}},
        QJsonObject{{"id", "image-3"}, {"type", "imageView"}, {"path", "/tmp/screenshot-3.png"}},
        QJsonObject{{"id", "image-4"}, {"type", "imageView"}, {"path", "/tmp/screenshot-4.png"}},
        QJsonObject{
            {"id", "browser"},
            {"type", "dynamicToolCall"},
            {"tool", "open_website"},
            {"arguments",
             QJsonObject{{"url", "https://example.test/page?token=secret#code=oauth-code"}}}},
        QJsonObject{{"id", "future"},
                    {"type", "futureToolV2"},
                    {"password", "must-not-leak"},
                    {"value", "kept"}},
        QJsonObject{{"id", "final"},
                    {"type", "agentMessage"},
                    {"phase", "final_answer"},
                    {"text", "Transcript updated."}}};
    QSet<QString> turns;
    const QVariantList entries = ketplus::agent_message::historyMessages(
        QJsonArray{QJsonObject{{"id", "turn"}, {"status", "completed"}, {"items", items}}}, turns);

    QCOMPARE(entries.size(), 7);
    QCOMPARE(entries.first().toMap().value("category").toString(), QStringLiteral("user"));
    QCOMPARE(entries.at(1).toMap().value("eventType").toString(),
             QStringLiteral("assistant.commentary"));
    const QVariantMap command = entries.at(2).toMap();
    QCOMPARE(command.value("eventType").toString(), QStringLiteral("command.run"));
    QVERIFY(command.value("title").toString().startsWith(QStringLiteral("Ran: curl")));
    QVERIFY(!command.value("command").toString().contains(QStringLiteral("super-secret")));
    QCOMPARE(command.value("cwd").toString(), QStringLiteral("/tmp/worktree"));
    QCOMPARE(command.value("exitCode").toInt(), 0);
    const QVariantMap image = entries.at(3).toMap();
    QCOMPARE(image.value("eventType").toString(), QStringLiteral("image.view"));
    QCOMPARE(image.value("images").toList().size(), 4);
    QCOMPARE(image.value("title").toString(), QStringLiteral("Viewed 4 images"));
    const QVariantMap browser = entries.at(4).toMap();
    QCOMPARE(browser.value("eventType").toString(), QStringLiteral("browser.open"));
    QCOMPARE(browser.value("title").toString(), QStringLiteral("Opened website · example.test"));
    QVERIFY(!browser.value("target").toString().contains(QStringLiteral("secret")));
    QVERIFY(!browser.value("target").toString().contains(QStringLiteral("oauth-code")));
    const QVariantMap fallback = entries.at(5).toMap();
    QCOMPARE(fallback.value("eventType").toString(), QStringLiteral("unknown"));
    QCOMPARE(fallback.value("subtype").toString(), QStringLiteral("generic"));
    const QVariantMap payload = fallback.value("sanitizedPayload").toMap();
    QCOMPARE(payload.value("password").toString(), QStringLiteral("[redacted]"));
    QCOMPARE(payload.value("value").toString(), QStringLiteral("kept"));
    QCOMPARE(entries.last().toMap().value("category").toString(), QStringLiteral("final"));
    QCOMPARE(entries.last().toMap().value("body").toString(),
             QStringLiteral("Transcript updated."));
    const QString copied = ketplus::agent_message::transcriptText(entries);
    QVERIFY(copied.contains(QStringLiteral("I am checking")));
    QVERIFY(copied.contains(QStringLiteral("Ran: curl")));

    QSet<QString> legacyTurns;
    const QVariantList legacy = ketplus::agent_message::historyMessages(
        QJsonArray{QJsonObject{
            {"id", "legacy"},
            {"items",
             QJsonArray{
                 QJsonObject{{"id", "progress"}, {"type", "agentMessage"}, {"text", "Checking..."}},
                 QJsonObject{{"id", "answer"}, {"type", "agentMessage"}, {"text", "Done."}}}}}},
        legacyTurns);
    QCOMPARE(legacy.size(), 2);
    QCOMPARE(legacy.first().toMap().value("body").toString(), QStringLiteral("Checking..."));
    QCOMPARE(legacy.last().toMap().value("body").toString(), QStringLiteral("Done."));
}

void AgentWorkspaceViewTest::semanticCommandLabelsStayExpandable() {
    const auto build = ketplus::agent_message::activityMessage(
        QJsonObject{{"id", "build"},
                    {"type", "commandExecution"},
                    {"command", "cmake --build build/dev --target ketplus"},
                    {"status", "completed"}},
        true);
    QCOMPARE(build.value("title").toString(), QStringLiteral("Run build: ketplus"));
    QCOMPARE(build.value("subtype").toString(), QStringLiteral("command"));

    const auto read = ketplus::agent_message::activityMessage(
        QJsonObject{
            {"id", "read"},
            {"type", "commandExecution"},
            {"command", "/bin/zsh -lc \"sed -n '1,240p' modules/ai/src/CursorAdapter.cpp\""},
            {"cwd", "/project"},
            {"status", "completed"}},
        true);
    QCOMPARE(read.value("subtype").toString(), QStringLiteral("fileRead"));
    QCOMPARE(read.value("title").toString(), QStringLiteral("Read: CursorAdapter.cpp L1–240"));
    QVERIFY(read.value("target").toString().isEmpty());
    QCOMPARE(read.value("command").toString(),
             QStringLiteral("/bin/zsh -lc \"sed -n '1,240p' modules/ai/src/CursorAdapter.cpp\""));
    // The row opens the file it read, at the first line shown.
    QCOMPARE(read.value("fileLocation").toString(),
             QStringLiteral("/project/modules/ai/src/CursorAdapter.cpp"));
    const auto readAfterCd = ketplus::agent_message::activityMessage(
        QJsonObject{{"id", "read-cd"},
                    {"type", "commandExecution"},
                    {"command", "cd /repo/app && sed -n '40,73p' docs/benchmarks.md"},
                    {"cwd", "/session"},
                    {"status", "completed"}},
        true);
    QCOMPARE(readAfterCd.value("fileLocation").toString(),
             QStringLiteral("/repo/app/docs/benchmarks.md:40"));
    const auto readInWrapper = ketplus::agent_message::activityMessage(
        QJsonObject{{"id", "read-wrapped"},
                    {"type", "commandExecution"},
                    {"command", "/bin/zsh -lc \"cd 'pkg' && cat tsconfig.build.json\""},
                    {"cwd", "/repo"},
                    {"status", "completed"}},
        true);
    QCOMPARE(readInWrapper.value("fileLocation").toString(),
             QStringLiteral("/repo/pkg/tsconfig.build.json"));

    const auto search = ketplus::agent_message::activityMessage(
        QJsonObject{{"id", "search"},
                    {"type", "commandExecution"},
                    {"command", "rg -n -C 5 'stopRequested' tests/AgentWorkspaceTest.cpp"},
                    {"status", "completed"}},
        true);
    QCOMPARE(search.value("subtype").toString(), QStringLiteral("codeSearch"));
    QVERIFY(search.value("fileLocation").toString().isEmpty());
    QCOMPARE(search.value("title").toString(),
             QStringLiteral("Searched code: “stopRequested” in AgentWorkspaceTest.cpp"));

    ketplus::AgentMessageModel model;
    QVariantMap readRow = read;
    QVariantMap searchRow = search;
    readRow.insert("turnId", "turn");
    searchRow.insert("turnId", "turn");
    model.reset({readRow, searchRow});
    // The two steps read as one line; each keeps its exact command when expanded.
    QCOMPARE(model.get(0).value("groupSummary").toString(),
             QStringLiteral("Explored a file, searched code"));
    model.setGroupExpanded(model.get(0).value("groupKey").toString(), true);
    QCOMPARE(model.get(0).value("command").toString(), read.value("command").toString());
    QCOMPARE(model.get(1).value("command").toString(), search.value("command").toString());
}

void AgentWorkspaceViewTest::userMessageShowsTheImagesItWasSentWith() {
    QTemporaryDir media;
    QVERIFY(media.isValid());
    QImage image(3, 2, QImage::Format_RGB32);
    image.fill(Qt::blue);
    const QString staging = media.filePath(QStringLiteral("staging.png"));
    QVERIFY(image.save(staging, "PNG"));
    QFile file(staging);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    const QString hex =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    QVERIFY(QFile::rename(staging, media.filePath(hex + QStringLiteral(".png"))));
    const QJsonArray blocks{
        QJsonObject{{"type", "image"},
                    {"media_ref", "sha256:" + hex},
                    {"mime", "image/png"},
                    {"bytes", bytes.size()},
                    {"name", "shot.png"}},
        // Pruned from the store: the message still shows its other images.
        QJsonObject{{"type", "image"},
                    {"media_ref", "sha256:" + QString(64, QLatin1Char('0'))},
                    {"mime", "image/png"},
                    {"bytes", 12}},
        QJsonObject{{"type", "image"}, {"media_ref", "sha256:../../etc"}, {"mime", "image/png"}}};
    const QVariantList images = ketplus::mcp_media::messageImages(blocks, media.path());
    QCOMPARE(images.size(), 1);
    QCOMPARE(images.first().toString(),
             QUrl::fromLocalFile(QDir(media.path()).absoluteFilePath(hex + QStringLiteral(".png")))
                 .toString());
    QVERIFY(ketplus::mcp_media::messageImages({}, media.path()).isEmpty());
}

void AgentWorkspaceViewTest::localImageActivityRendersRetainedMediaAfterSourceCleanup() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray previousState = qgetenv("KETPLUS_DAEMON_STATE");
    const auto restore = qScopeGuard([previousState] {
        if (previousState.isNull())
            qunsetenv("KETPLUS_DAEMON_STATE");
        else
            qputenv("KETPLUS_DAEMON_STATE", previousState);
    });
    qputenv("KETPLUS_DAEMON_STATE", directory.filePath("journal.sqlite3").toUtf8());
    QVERIFY(QDir().mkpath(directory.filePath("media")));
    QImage image(6, 4, QImage::Format_RGB32);
    image.fill(Qt::blue);
    const QString path = directory.filePath("browser capture.png");
    QVERIFY(image.save(path, "PNG"));
    QFile original(path);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const QByteArray bytes = original.readAll();
    original.close();
    const QString hex =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const QString retained = directory.filePath("media/" + hex + ".png");
    QVERIFY(QFile::rename(path, retained));
    const QJsonObject block{
        {"type", "image"}, {"media_ref", "sha256:" + hex}, {"mime", "image/png"}};
    for (const QString& type : {QStringLiteral("imageView"), QStringLiteral("imageGeneration")}) {
        const QString key = type == "imageView" ? "path" : "savedPath";
        const auto entry = ketplus::agent_message::activityMessage(
            {{"id", "image"},
             {"type", type},
             {key, path},
             {"result", QJsonObject{{"content", QJsonArray{block}}}}},
            true);
        QCOMPARE(entry.value("images").toList(),
                 QVariantList{QUrl::fromLocalFile(retained).toString()});
        QCOMPARE(entry.value("filePath").toString(), retained);
        QCOMPARE(entry.value("target").toString(), QUrl::fromLocalFile(retained).toString());
        ketplus::AgentWorkspaceView view;
        view.applyTheme(testPalette());
        view.resize(900, 640);
        view.show();
        view.resetMessages({entry});
        QQuickItem* attachment = nullptr;
        QTRY_VERIFY((attachment = findVisualItem(view.quickView()->rootObject(),
                                                 "messageAttachmentImage")) != nullptr);
        QTRY_COMPARE(attachment->property("status").toInt(), 1); // Image.Ready
        auto* row = findVisualItem(view.quickView()->rootObject(), "transcriptActivityRow");
        QVERIFY(row);
        if (!row->property("expanded").toBool())
            QVERIFY(QMetaObject::invokeMethod(row, "toggleDetails"));
        auto* resource = findVisualItem(view.quickView()->rootObject(), "openActivityResource");
        QTRY_VERIFY(resource && resource->isVisible());
        QCOMPARE(resource->property("resource").toString(),
                 QUrl::fromLocalFile(retained).toString());
    }
}

void AgentWorkspaceViewTest::normalizesTypedTranscriptCorpus() {
    using ketplus::ActivityNormalizer;
    using ketplus::TranscriptCategory;
    using ketplus::TranscriptPhase;

    const ActivityNormalizer::Context context{QStringLiteral("session"), 7, QStringLiteral("turn"),
                                              10, 2};
    const QList<QPair<QJsonObject, QString>> corpus{
        {{QJsonObject{{"id", "user"},
                      {"type", "userMessage"},
                      {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "Prompt"}}}}}},
         QStringLiteral("user.message")},
        {{QJsonObject{{"id", "command"},
                      {"type", "commandExecution"},
                      {"command", "pwd"},
                      {"status", "completed"}}},
         QStringLiteral("command.run")},
        {{QJsonObject{{"id", "files"}, {"type", "fileChange"}, {"changes", QJsonArray{}}}},
         QStringLiteral("file.change")},
        {{QJsonObject{{"id", "search"}, {"type", "webSearch"}, {"query", "Qt"}}},
         QStringLiteral("search.web")},
        {{QJsonObject{{"id", "image"}, {"type", "imageView"}, {"path", "/tmp/image.png"}}},
         QStringLiteral("image.view")},
        {{QJsonObject{{"id", "generation"},
                      {"type", "imageGeneration"},
                      {"savedPath", "/tmp/generated.png"}}},
         QStringLiteral("image.generate")},
        {{QJsonObject{
             {"id", "mcp"}, {"type", "mcpToolCall"}, {"server", "local"}, {"tool", "inspect"}}},
         QStringLiteral("integration.mcp")},
        {{QJsonObject{{"id", "dynamic"}, {"type", "dynamicToolCall"}, {"tool", "custom"}}},
         QStringLiteral("tool.dynamic")},
        {{QJsonObject{{"id", "collab"}, {"type", "collabAgentToolCall"}}},
         QStringLiteral("integration.collaboration")},
        {{QJsonObject{{"id", "subagent"}, {"type", "subAgentActivity"}}},
         QStringLiteral("integration.subagent")},
        {{QJsonObject{{"id", "sleep"}, {"type", "sleep"}}}, QStringLiteral("activity.wait")},
        {{QJsonObject{{"id", "compact"}, {"type", "contextCompaction"}}},
         QStringLiteral("system.compaction")},
        {{QJsonObject{{"id", "future"}, {"type", "futureItem"}}}, QStringLiteral("unknown")},
    };

    for (const auto& [item, expectedType] : corpus) {
        const auto event = ActivityNormalizer::normalize(item, TranscriptPhase::Completed, context);
        QCOMPARE(event.eventType, expectedType);
        QVERIFY(event.visible);
        QCOMPARE(event.identity.attempt, 2);
        QCOMPARE(event.identity.generation, quint64(7));
        QVERIFY(!event.identity.stableKey().isEmpty());
        QCOMPARE(event.toVariantMap().value(QStringLiteral("messageKey")).toString(),
                 event.identity.stableKey());
    }

    // Daemon rows pass through the normalizer: a read keeps the file it opens and a command
    // keeps its description.
    const auto read =
        ActivityNormalizer::normalize(
            QJsonObject{{"id", "read"},
                        {"type", "commandExecution"},
                        {"command", "/bin/zsh -lc \"rg -n 'x' a.cpp | head\nsed -n "
                                    "'1,25p' tests/AgentWorkspaceRenderingTest.cpp\""},
                        {"cwd", "/project"},
                        {"description", "Read the rendering test"},
                        {"status", "completed"}},
            TranscriptPhase::Completed, context)
            .toVariantMap();
    QCOMPARE(read.value("fileLocation").toString(),
             QStringLiteral("/project/tests/AgentWorkspaceRenderingTest.cpp"));
    QCOMPARE(read.value("commandDescription").toString(),
             QStringLiteral("Read the rendering test"));

    const auto commentary = ActivityNormalizer::normalize(QJsonObject{{"id", "commentary"},
                                                                      {"type", "agentMessage"},
                                                                      {"phase", "commentary"},
                                                                      {"text", "Hidden progress"}},
                                                          TranscriptPhase::Completed, context);
    QCOMPARE(commentary.eventType, QStringLiteral("assistant.commentary"));
    QVERIFY(commentary.visible);

    const auto reasoning = ActivityNormalizer::normalize(
        QJsonObject{{"id", "reasoning"}, {"type", "reasoning"}, {"text", "Hidden"}},
        TranscriptPhase::Completed, context);
    QCOMPARE(reasoning.eventType, QStringLiteral("assistant.reasoning"));
    // The mock's "Thought" step: it shows that the agent thought, never what it thought.
    QVERIFY(reasoning.visible);
    const QVariantMap thought = reasoning.toVariantMap();
    QCOMPARE(thought.value("title").toString(), QStringLiteral("Thought"));
    QCOMPARE(thought.value("subtype").toString(), QStringLiteral("thought"));
    QVERIFY(!QString::fromUtf8(QJsonDocument::fromVariant(thought).toJson()).contains("Hidden"));

    const auto registered = ActivityNormalizer::registeredRawTypes();
    for (const auto& [item, expectedType] : corpus) {
        Q_UNUSED(expectedType);
        const QString rawType = item.value(QStringLiteral("type")).toString();
        if (rawType != QStringLiteral("futureItem"))
            QVERIFY2(registered.contains(rawType), qPrintable(rawType));
    }

    auto changedGeneration = context;
    ++changedGeneration.generation;
    const auto first =
        ActivityNormalizer::normalize(corpus.first().first, TranscriptPhase::Completed, context);
    const auto second = ActivityNormalizer::normalize(
        corpus.first().first, TranscriptPhase::Completed, changedGeneration);
    QVERIFY(first.identity.stableKey() != second.identity.stableKey());
}

void AgentWorkspaceViewTest::mergesLiveAndReplayTranscriptLifecycle() {
    using ketplus::ActivityNormalizer;
    using ketplus::TranscriptLifecycleMerger;
    using ketplus::TranscriptPhase;

    const ActivityNormalizer::Context context{QStringLiteral("session"), 1, QStringLiteral("turn"),
                                              0, 1};
    TranscriptLifecycleMerger live;
    live.apply(ActivityNormalizer::normalize(
        QJsonObject{
            {"id", "answer"}, {"type", "agentMessage"}, {"phase", "final_answer"}, {"text", ""}},
        TranscriptPhase::Started, context));
    live.apply(ActivityNormalizer::normalize(QJsonObject{{"id", "answer"},
                                                         {"type", "agentMessage"},
                                                         {"phase", "final_answer"},
                                                         {"text", "Hello "}},
                                             TranscriptPhase::Delta, context));
    live.apply(ActivityNormalizer::normalize(QJsonObject{{"id", "answer"},
                                                         {"type", "agentMessage"},
                                                         {"phase", "final_answer"},
                                                         {"text", "world"}},
                                             TranscriptPhase::Delta, context));
    live.apply(ActivityNormalizer::normalize(QJsonObject{{"id", "answer"},
                                                         {"type", "agentMessage"},
                                                         {"phase", "final_answer"},
                                                         {"text", "Hello world"}},
                                             TranscriptPhase::Completed, context));

    TranscriptLifecycleMerger replay;
    replay.apply(ActivityNormalizer::normalize(QJsonObject{{"id", "answer"},
                                                           {"type", "agentMessage"},
                                                           {"phase", "final_answer"},
                                                           {"text", "Hello world"}},
                                               TranscriptPhase::Completed, context));

    QCOMPARE(live.variantEvents(), replay.variantEvents());

    TranscriptLifecycleMerger attempts;
    attempts.apply(ActivityNormalizer::normalize(QJsonObject{{"id", "command"},
                                                             {"type", "commandExecution"},
                                                             {"command", "false"},
                                                             {"status", "failed"}},
                                                 TranscriptPhase::Failed, context));
    auto retryContext = context;
    retryContext.attempt = 2;
    retryContext.sequence = 1;
    attempts.apply(ActivityNormalizer::normalize(QJsonObject{{"id", "command"},
                                                             {"type", "commandExecution"},
                                                             {"command", "true"},
                                                             {"status", "completed"}},
                                                 TranscriptPhase::Completed, retryContext));
    QCOMPARE(attempts.events().size(), 2);
    QCOMPARE(attempts.events().first().phase, TranscriptPhase::Failed);
    QCOMPARE(attempts.events().last().identity.attempt, 2);
}

void AgentWorkspaceViewTest::sanitizesTranscriptPayloads() {
    using ketplus::TranscriptSanitizer;

    const QJsonObject payload{
        {"authorization", "Bearer top-secret"},
        {"nested", QJsonObject{{"refresh-token", "refresh-secret"},
                               {"url", "https://user:pass@example.test/"
                                       "callback?token=query-secret&ok=yes#code=oauth-secret"}}},
        {"command", "curl --api-key command-secret -H 'Authorization: Bearer bearer-secret'"}};
    const QJsonObject safe = TranscriptSanitizer::json(payload).toObject();
    QCOMPARE(safe.value(QStringLiteral("authorization")).toString(), QStringLiteral("[redacted]"));
    QCOMPARE(safe.value(QStringLiteral("nested"))
                 .toObject()
                 .value(QStringLiteral("refresh-token"))
                 .toString(),
             QStringLiteral("[redacted]"));
    const QString serialized = QString::fromUtf8(QJsonDocument(safe).toJson());
    QVERIFY(!serialized.contains(QStringLiteral("top-secret")));
    QVERIFY(!serialized.contains(QStringLiteral("refresh-secret")));
    QVERIFY(!serialized.contains(QStringLiteral("query-secret")));
    QVERIFY(!serialized.contains(QStringLiteral("oauth-secret")));
    QVERIFY(!serialized.contains(QStringLiteral("command-secret")));
    QVERIFY(!serialized.contains(QStringLiteral("bearer-secret")));
    QVERIFY(serialized.contains(QStringLiteral("ok")));
    QVERIFY(serialized.contains(QStringLiteral("yes")));

    // Embedded URLs keep their scheme from the first letter, and a long run of scheme
    // characters without "://" is scanned once rather than from every position.
    const QString label =
        TranscriptSanitizer::text(QStringLiteral("Ran: 12git+ssh://host/repo?token=leak x"));
    QVERIFY2(label.startsWith(QStringLiteral("Ran: 12git+ssh://host/repo?token=")),
             qPrintable(label));
    QVERIFY2(!label.contains(QStringLiteral("leak")), qPrintable(label));
    QVERIFY(label.endsWith(QStringLiteral(" x")));
    const QString output = QString(1024 * 1024, QLatin1Char('m')) + QStringLiteral(" done");
    QCOMPARE(TranscriptSanitizer::text(output), output);
}

void AgentWorkspaceViewTest::coversVersionedTranscriptCorpus() {
    using ketplus::ActivityNormalizer;
    using ketplus::TranscriptPhase;

    const QString corpusPath = QFINDTESTDATA("fixtures/transcript-v2/canonical-items.json");
    QVERIFY2(!corpusPath.isEmpty(), "The versioned transcript corpus is missing");
    QFile corpusFile(corpusPath);
    QVERIFY(corpusFile.open(QIODevice::ReadOnly));
    const QJsonObject corpus = QJsonDocument::fromJson(corpusFile.readAll()).object();
    QCOMPARE(corpus.value(QStringLiteral("version")).toInt(), 1);

    const auto registeredItems = ActivityNormalizer::registeredRawTypes();
    int sequence = 0;
    ketplus::TranscriptLifecycleMerger liveCorpus;
    ketplus::TranscriptLifecycleMerger replayCorpus;
    for (const auto& value : corpus.value(QStringLiteral("cases")).toArray()) {
        const QJsonObject fixture = value.toObject();
        const QJsonObject item = fixture.value(QStringLiteral("item")).toObject();
        const QString rawType = item.value(QStringLiteral("type")).toString();
        const ActivityNormalizer::Context context{QStringLiteral("corpus"), 1,
                                                  QStringLiteral("turn"), sequence++, 1};
        const auto event = ActivityNormalizer::normalize(item, TranscriptPhase::Completed, context);
        QCOMPARE(event.eventType, fixture.value(QStringLiteral("expectedEventType")).toString());
        QCOMPARE(event.visible, fixture.value(QStringLiteral("visible")).toBool(true));
        if (!fixture.value(QStringLiteral("genericPolicy")).toBool())
            QVERIFY2(registeredItems.contains(rawType), qPrintable(rawType));
        QVERIFY(!event.identity.stableKey().isEmpty());
        const QString payload = QString::fromUtf8(
            QJsonDocument::fromVariant(event.sanitizedPayload).toJson(QJsonDocument::Compact));
        QVERIFY2(!payload.contains(QStringLiteral("CORPUS_SECRET")), qPrintable(rawType));
        liveCorpus.apply(ActivityNormalizer::normalize(item, TranscriptPhase::Started, context));
        liveCorpus.apply(event);
        replayCorpus.apply(event);
    }
    QCOMPARE(liveCorpus.variantEvents(), replayCorpus.variantEvents());

    const QString inventoryPath = QFINDTESTDATA("fixtures/transcript-v2/observed-inventory.json");
    QVERIFY2(!inventoryPath.isEmpty(), "The observed protocol inventory is missing");
    QFile inventoryFile(inventoryPath);
    QVERIFY(inventoryFile.open(QIODevice::ReadOnly));
    const QJsonObject inventory = QJsonDocument::fromJson(inventoryFile.readAll()).object();
    const auto registeredProtocol = ActivityNormalizer::registeredProtocolEventTypes();
    for (const auto& value : inventory.value(QStringLiteral("protocolEvents")).toArray()) {
        const QString type = value.toObject().value(QStringLiteral("type")).toString();
        QVERIFY2(registeredProtocol.contains(type), qPrintable(type));
        QVERIFY(ActivityNormalizer::protocolPolicy(type) !=
                ActivityNormalizer::ProtocolEventPolicy::Generic);
    }
    QCOMPARE(ActivityNormalizer::protocolPolicy(QStringLiteral("future.protocol.event")),
             ActivityNormalizer::ProtocolEventPolicy::Generic);

    const QString protocolPath = QFINDTESTDATA("fixtures/transcript-v2/protocol-events.json");
    QVERIFY2(!protocolPath.isEmpty(), "The protocol policy corpus is missing");
    QFile protocolFile(protocolPath);
    QVERIFY(protocolFile.open(QIODevice::ReadOnly));
    const auto protocolCases = QJsonDocument::fromJson(protocolFile.readAll())
                                   .object()
                                   .value(QStringLiteral("cases"))
                                   .toArray();
    const auto policyName = [](const ActivityNormalizer::ProtocolEventPolicy policy) {
        switch (policy) {
        case ActivityNormalizer::ProtocolEventPolicy::NormalizeItem:
            return QStringLiteral("normalize-item");
        case ActivityNormalizer::ProtocolEventPolicy::MergeDelta:
            return QStringLiteral("merge-delta");
        case ActivityNormalizer::ProtocolEventPolicy::Approval:
            return QStringLiteral("approval");
        case ActivityNormalizer::ProtocolEventPolicy::Error:
            return QStringLiteral("error");
        case ActivityNormalizer::ProtocolEventPolicy::Metadata:
            return QStringLiteral("metadata");
        case ActivityNormalizer::ProtocolEventPolicy::Generic:
            return QStringLiteral("generic");
        }
        return QStringLiteral("generic");
    };
    for (const auto& value : protocolCases) {
        const auto fixture = value.toObject();
        const QString type = fixture.value(QStringLiteral("type")).toString();
        QCOMPARE(policyName(ActivityNormalizer::protocolPolicy(type)),
                 fixture.value(QStringLiteral("expectedPolicy")).toString());
        const QString safePayload =
            QString::fromUtf8(QJsonDocument(ketplus::TranscriptSanitizer::json(
                                                fixture.value(QStringLiteral("payload")))
                                                .toObject())
                                  .toJson(QJsonDocument::Compact));
        QVERIFY(!safePayload.contains(QStringLiteral("CORPUS_SECRET")));
    }
}

void AgentWorkspaceViewTest::streamsLiveCommentary() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString scriptPath = directory.filePath(QStringLiteral("phase-server.py"));
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(#!/usr/bin/env python3
import json,sys
for line in sys.stdin:
    request=json.loads(line)
    if "id" not in request: continue
    method=request.get("method")
    result={}
    if method=="thread/start": result={"thread":{"id":"phase-thread","turns":[]}}
    elif method=="turn/start": result={"turn":{"id":"phase-turn"}}
    print(json.dumps({"id":request["id"],"result":result}),flush=True)
)PY");
    script.close();
    QVERIFY(QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));
    ketplus::CodexAppServer server(scriptPath);
    ketplus::AgentSessionManager manager(&server);
    const QString path = directory.path();
    manager.submitPrompt(path, QStringLiteral("Implement transcript"));
    QTRY_COMPARE(manager.status(path), QStringLiteral("Thinking…"));
    server.notificationReceived("turn/started", {{"threadId", "phase-thread"},
                                                 {"turn", QJsonObject{{"id", "phase-turn"}}}});
    const auto notifyItem = [&](const QString& method, const QJsonObject& item) {
        server.notificationReceived(
            method, {{"threadId", "phase-thread"}, {"turnId", "phase-turn"}, {"item", item}});
    };
    notifyItem("item/started", QJsonObject{{"id", "progress"},
                                           {"type", "agentMessage"},
                                           {"phase", "commentary"},
                                           {"text", ""}});
    server.notificationReceived("item/agentMessage/delta", {{"threadId", "phase-thread"},
                                                            {"turnId", "phase-turn"},
                                                            {"itemId", "progress"},
                                                            {"delta", "I am checking files."}});
    QTRY_COMPARE(manager.messages(path).last().toMap().value("body").toString(),
                 QStringLiteral("I am checking files."));
    notifyItem("item/completed", QJsonObject{{"id", "progress"},
                                             {"type", "agentMessage"},
                                             {"phase", "commentary"},
                                             {"text", "I am checking files."}});
    QCOMPARE(manager.messages(path).size(), 2);
    QCOMPARE(manager.messages(path).last().toMap().value("body").toString(),
             QStringLiteral("I am checking files."));
    QCOMPARE(manager.messages(path).last().toMap().value("eventType").toString(),
             QStringLiteral("assistant.commentary"));

    const QJsonObject command{
        {"id", "command"}, {"type", "commandExecution"},     {"command", "ls /tmp"},
        {"cwd", path},     {"commandActions", QJsonArray{}}, {"status", "completed"}};
    notifyItem("item/started", command);
    QCOMPARE(manager.messages(path).size(), 3);
    QCOMPARE(manager.messages(path).last().toMap().value("title").toString(),
             QStringLiteral("Running: ls /tmp"));
    notifyItem("item/completed", command);
    QCOMPARE(manager.messages(path).last().toMap().value("title").toString(),
             QStringLiteral("Ran: ls /tmp"));

    const auto imageItem = [](const QString& id, const QString& path) {
        return QJsonObject{{"id", id}, {"type", "imageView"}, {"path", path}};
    };
    notifyItem("item/started", imageItem("image-a", "/tmp/a.png"));
    notifyItem("item/completed", imageItem("image-a", "/tmp/a.png"));
    notifyItem("item/started", imageItem("image-b", "/tmp/b.png"));
    notifyItem("item/completed", imageItem("image-b", "/tmp/b.png"));
    QCOMPARE(manager.messages(path).size(), 4);
    QCOMPARE(manager.messages(path).last().toMap().value("title").toString(),
             QStringLiteral("Viewed 2 images"));
    QCOMPARE(manager.messages(path).last().toMap().value("images").toList().size(), 2);

    notifyItem("item/started", QJsonObject{{"id", "answer"},
                                           {"type", "agentMessage"},
                                           {"phase", "final_answer"},
                                           {"text", ""}});
    server.notificationReceived("item/agentMessage/delta", {{"threadId", "phase-thread"},
                                                            {"turnId", "phase-turn"},
                                                            {"itemId", "answer"},
                                                            {"delta", "Finished."}});
    QCOMPARE(manager.messages(path).size(), 5);
    QTRY_COMPARE(manager.messages(path).last().toMap().value("body").toString(),
                 QStringLiteral("Finished."));
    notifyItem("item/completed", QJsonObject{{"id", "answer"},
                                             {"type", "agentMessage"},
                                             {"phase", "final_answer"},
                                             {"text", "Finished."}});
    server.notificationReceived(
        "turn/completed", {{"threadId", "phase-thread"},
                           {"turn", QJsonObject{{"id", "phase-turn"}, {"status", "completed"}}}});
    QCOMPARE(manager.messages(path).size(), 5);
}

void AgentWorkspaceViewTest::paginatesConversationHistory() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto scriptPath = directory.filePath("pages.py");
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(#!/usr/bin/env python3
import json,sys,time,os
base=os.path.dirname(__file__)
def turn(i):
    return {"id":str(i),"status":"completed","items":[
        {"id":"u"+str(i),"type":"userMessage","content":[{"type":"text","text":"Prompt "+str(i)}]},
        {"id":"a"+str(i),"type":"agentMessage","text":"Answer "+str(i)}]}
for line in sys.stdin:
    r=json.loads(line); m=r.get("method"); p=r.get("params",{})
    if "id" not in r: continue
    def send(result): print(json.dumps({"id":r["id"],"result":result}),flush=True)
    with open(os.path.join(base,"calls.jsonl"),"a") as f: f.write(json.dumps(r)+"\n")
    if m=="initialize": send({})
    elif m=="thread/start": send({"thread":{"id":"paged","turns":[]}})
    elif m=="thread/resume": send({"thread":{"id":"paged","turns":[]}})
    elif m=="turn/start":
        send({"turn":{"id":"initial"}})
        print(json.dumps({"method":"turn/completed","params":{"threadId":"paged","turn":{"id":"initial","status":"completed"}}}),flush=True)
    elif m=="thread/turns/list":
        time.sleep(0.05)
        cursor=p.get("cursor")
        if os.path.exists(os.path.join(base,"legacy")):
            print(json.dumps({"id":r["id"],"error":{"code":-32601,"message":"Method not found"}}),flush=True)
        elif cursor=="24" and not os.path.exists(os.path.join(base,"retried")):
            open(os.path.join(base,"retried"),"w").close()
            print(json.dumps({"id":r["id"],"error":{"code":-1,"message":"Temporary history failure"}}),flush=True)
        else:
            top=44 if cursor is None else int(cursor)
            low=max(0,top-19)
            data=[turn(i) for i in range(top,low-1,-1)]
            if cursor is not None: data.insert(0,turn(top+1))
            send({"data":data,"nextCursor":str(low-1) if low else None})
    elif m=="thread/read": send({"thread":{"turns":[turn(i) for i in range(45)]}})
)PY");
    script.close();
    QVERIFY(QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));
    ketplus::CodexAppServer server(scriptPath);
    const auto path = directory.path();
    {
        ketplus::AgentSessionManager seed(&server);
        seed.submitPrompt(path, "Seed persisted thread");
        QTRY_VERIFY(!seed.isRunning(path));
    }
    ketplus::AgentSessionManager manager(&server);
    QSignalSpy prepended(&manager, &ketplus::AgentSessionManager::messagesPrepended);
    QSignalSpy copied(&manager, &ketplus::AgentSessionManager::conversationCopyReady);
    manager.activate(path);
    QTRY_COMPARE(manager.messages(path).size(), 40);
    QCOMPARE(manager.messages(path).first().toMap().value("body").toString(), "Prompt 25");
    QVERIFY(manager.historyState(path).value("hasOlder").toBool());
    const auto id = manager.activeConversationId(path);
    manager.loadOlderMessages(path);
    manager.loadOlderMessages(path);
    QTRY_VERIFY(!manager.historyState(path).value("error").toString().isEmpty());
    QCOMPARE(manager.messages(path).size(), 40);
    manager.copyConversation(path);
    manager.cancelConversationCopy(path);
    QTest::qWait(150);
    QCOMPARE(copied.size(), 0);
    manager.copyConversation(path);
    QTRY_COMPARE(copied.size(), 1);
    const auto text = copied.first().at(2).toString();
    QCOMPARE(text.count("Codex:\n"), 45);
    QVERIFY(text.startsWith("You:\nPrompt 0"));
    QVERIFY(text.endsWith("Answer 44"));
    QCOMPARE(manager.messages(path).size(), 40);
    // Completing a page in the background must not insert into the newly selected conversation.
    prepended.clear();
    manager.loadOlderMessages(path);
    manager.newSession(path);
    QTest::qWait(150);
    QCOMPARE(manager.messages(path).size(), 0);
    QCOMPARE(prepended.size(), 0);
    manager.selectConversation(path, id);
    QTRY_COMPARE_WITH_TIMEOUT(manager.messages(path).size(), 80, 3000);
    // Live item indexes must move with a prepend; subsequent deltas update the same row.
    server.notificationReceived("turn/started",
                                {{"threadId", "paged"}, {"turn", QJsonObject{{"id", "live"}}}});
    server.notificationReceived("item/started", {{"threadId", "paged"},
                                                 {"turnId", "live"},
                                                 {"item", QJsonObject{{"id", "stream"},
                                                                      {"type", "agentMessage"},
                                                                      {"phase", "final_answer"},
                                                                      {"text", ""}}}});
    server.notificationReceived("item/agentMessage/delta",
                                {{"threadId", "paged"}, {"itemId", "stream"}, {"delta", "Live"}});
    const auto liveKey = manager.messages(path).last().toMap().value("messageKey");
    manager.loadOlderMessages(path);
    QTRY_COMPARE(manager.messages(path).size(), 91);
    server.notificationReceived(
        "item/agentMessage/delta",
        {{"threadId", "paged"}, {"itemId", "stream"}, {"delta", " output"}});
    QTRY_COMPARE(manager.messages(path).last().toMap().value("body").toString(), "Live output");
    QCOMPARE(manager.messages(path).last().toMap().value("messageKey"), liveKey);
    QVERIFY(!manager.historyState(path).value("hasOlder").toBool());
    QFile calls(directory.filePath("calls.jsonl"));
    QVERIFY(calls.open(QIODevice::ReadOnly));
    int resumeCount = 0;
    while (!calls.atEnd()) {
        const auto call = QJsonDocument::fromJson(calls.readLine()).object();
        const auto params = call.value("params").toObject();
        if (call.value("method") == "thread/resume") {
            ++resumeCount;
            QVERIFY(params.value("excludeTurns").toBool());
        } else if (call.value("method") == "thread/turns/list") {
            QCOMPARE(params.value("limit").toInt(), 20);
            QCOMPARE(params.value("itemsView").toString(), "full");
            QCOMPARE(params.value("sortDirection").toString(), "desc");
        }
    }
    QCOMPARE(resumeCount, 1);
    QFile legacy(directory.filePath("legacy"));
    QVERIFY(legacy.open(QIODevice::WriteOnly));
    legacy.close();
    ketplus::AgentSessionManager restored(&server);
    restored.activate(path);
    QTRY_COMPARE(restored.messages(path).size(), 90);
    QVERIFY(!restored.historyState(path).value("hasOlder").toBool());
}

void AgentWorkspaceViewTest::prependingAndStreamingKeepMessageAnchor_data() {
    QTest::addColumn<int>("width");
    QTest::newRow("wide") << 1000;
    QTest::newRow("narrow") << 600;
}

void AgentWorkspaceViewTest::prependingAndStreamingKeepMessageAnchor() {
    QFETCH(int, width);
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(width, 700);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    const auto waitForFrame = [&] {
        // A fresh spy prevents queued render-thread loop exits from ending the
        // next wait before its frame arrives.
        QSignalSpy presented(view.quickView(), &QQuickWindow::frameSwapped);
        view.quickView()->update();
        if (!presented.wait(5000))
            return false;
        // frameSwapped arrives from the render thread. Its event loop can also
        // process the next layout before returning, leaving GUI state between
        // polish and anchor restoration. Grab synchronously to sample a fully
        // synchronized scene rather than that intermediate layout.
        return !view.quickView()->grabWindow().isNull();
    };
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QVariantList messages;
    for (int i = 0; i < 60; ++i)
        messages.append(QVariantMap{
            {"messageKey", QString::number(i)},
            {"kind", "assistant"},
            {"body", QStringLiteral("Message %1\n\nText to read.\n\nLast paragraph.").arg(i)},
            {"meta", "Codex"}});
    view.resetMessages(messages, "anchor");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    transcript->setProperty("followTail", false);
    QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtIndex", Q_ARG(int, 30),
                                      Q_ARG(int, 0)));
    QVERIFY(waitForFrame());
    QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
    const auto capture = [&] {
        QVariant result;
        QMetaObject::invokeMethod(root, "captureTranscriptPosition",
                                  Q_RETURN_ARG(QVariant, result));
        return result.toMap();
    };
    const auto anchor = capture();
    QVERIFY(!anchor.value("key").toString().isEmpty());
    const auto capturePath =
        qEnvironmentVariable("KETPLUS_PAGINATION_CAPTURE") + "." + QTest::currentDataTag();
    QImage reference;
    QRect readingRegion;
    if (qEnvironmentVariableIsSet("KETPLUS_PAGINATION_CAPTURE")) {
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        const auto frame = view.quickView()->grabWindow();
        QVERIFY(!frame.isNull());
        const auto origin = transcript->mapToScene(QPointF(24, 24));
        const auto scale = frame.devicePixelRatio();
        readingRegion = QRect(qRound(origin.x() * scale), qRound(origin.y() * scale),
                              qRound((transcript->width() - 48) * scale), qRound(120 * scale));
        reference = frame.copy(readingRegion);
        QVERIFY(frame.save(capturePath + ".before.png"));
    }
    QVariantList prefix;
    for (int i = 0; i < 20; ++i)
        prefix.append(QVariantMap{{"messageKey", "older-" + QString::number(i)},
                                  {"kind", "assistant"},
                                  {"body", QString("Long old paragraph.\n\n").repeated(i + 1)}});
    view.prependMessages(prefix);
    for (int frame = 0; frame < 20; ++frame) {
        auto entry =
            view.messageModel()->get(49); // A cached delegate immediately above the reader.
        entry["body"] = entry.value("body").toString() + "\n\nAdditional streamed text.";
        view.updateMessage(49, entry);
        // Anchor restoration runs during polish; elapsed time is not evidence
        // that a frame has been rendered on a busy CI worker.
        QVERIFY(waitForFrame());
        const auto actual = capture();
        QCOMPARE(actual.value("key"), anchor.value("key"));
        QVERIFY2(qAbs(actual.value("offset").toDouble() - anchor.value("offset").toDouble()) < 0.01,
                 qPrintable(QString("Frame %1: expected %2; actual %3")
                                .arg(frame)
                                .arg(QString::fromUtf8(QJsonDocument::fromVariant(anchor).toJson(
                                    QJsonDocument::Compact)))
                                .arg(QString::fromUtf8(QJsonDocument::fromVariant(actual).toJson(
                                    QJsonDocument::Compact)))));
        QVERIFY(!transcript->property("followTail").toBool());
        QCOMPARE(transcript->opacity(), 1.0);
        if (!reference.isNull()) {
            const auto frameImage = view.quickView()->grabWindow();
            const auto reading = frameImage.copy(readingRegion);
            int maximumDifference = 0;
            for (int y = 0; y < reading.height(); ++y) {
                for (int x = 0; x < reading.width(); ++x) {
                    const auto actual = reading.pixelColor(x, y);
                    const auto expected = reference.pixelColor(x, y);
                    maximumDifference =
                        qMax(maximumDifference, qMax(qAbs(actual.red() - expected.red()),
                                                     qMax(qAbs(actual.green() - expected.green()),
                                                          qAbs(actual.blue() - expected.blue()))));
                }
            }
            if (maximumDifference > 2) {
                frameImage.save(capturePath + ".changed.png");
                qWarning() << "Frame" << frame << "anchor" << capture();
            }
            QVERIFY2(maximumDifference <= 2,
                     qPrintable(QString("Reading pixels changed: %1").arg(maximumDifference)));
            if (frame == 19)
                QVERIFY(frameImage.save(capturePath + ".after.png"));
        }
    }
    QVariantList boundedWindow;
    for (int index = 0; index < view.messageModel()->sourceCount() - 10; ++index)
        boundedWindow.append(view.messageModel()->source(index));
    view.replaceMessageWindow(boundedWindow);
    QVERIFY(waitForFrame());
    const auto afterEviction = capture();
    QCOMPARE(afterEviction.value("key"), anchor.value("key"));
    QVERIFY(qAbs(afterEviction.value("offset").toDouble() - anchor.value("offset").toDouble()) <
            0.01);
    QVERIFY(!transcript->property("followTail").toBool());

    // The 56 px "Load older messages" bar is now a slim (<= 24 px) edge row: reaching
    // the top requests a page by distance, a spinner shows while it loads, and a
    // retry control appears only on error.
    QSignalSpy requested(&view, &ketplus::AgentWorkspaceView::olderMessagesRequested);
    view.setHistoryState({{"hasOlder", true}});
    QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtBeginning"));
    QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
    QTRY_COMPARE(requested.size(), 1);
    auto* header = findVisualItem(root, "olderHistoryHeader");
    auto* olderButton = findVisualItem(root, "loadOlderMessagesButton");
    auto* indicator = findVisualItem(root, "olderHistoryIndicator");
    QVERIFY(header && olderButton && indicator);
    QVERIFY(header->height() > 0 && header->height() <= 24);
    QVERIFY(!olderButton->isVisible());
    if (!reference.isNull())
        QVERIFY(view.quickView()->grabWindow().save(capturePath + ".history.png"));
    view.setHistoryState({{"hasOlder", true}, {"loading", true}});
    QVERIFY(indicator->isVisible());
    QVERIFY(!olderButton->isVisible());
    QVERIFY(header->height() <= 24);
    view.setHistoryState({{"error", "Temporary failure"}});
    QVERIFY(olderButton->isVisible() && olderButton->isEnabled());
    QVERIFY(olderButton->width() <= transcript->width());
    QVERIFY(header->height() <= 24);
    QCOMPARE(olderButton->property("text").toString(), "Retry loading history");
    QVERIFY(QMetaObject::invokeMethod(olderButton, "clicked"));
    QCOMPARE(requested.size(), 2);
}

void AgentWorkspaceViewTest::wheelStopsTailFollowing() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1400, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QVariantMap entry{{"kind", "assistant"},
                      {"body", QString("Text **in progress**.\n\n").repeated(100)}};
    view.resetMessages({entry}, "wheel");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTest::qWait(100);
    auto* messageSurface = findVisualItem(root, QStringLiteral("messageSurface"));
    QVERIFY(messageSurface);
    QVERIFY(transcript->width() > 760);
    const auto position = transcript->mapToScene(QPointF(transcript->width() - 20, 100));
    QVERIFY(position.x() > messageSurface->mapToScene(QPointF(messageSurface->width(), 0)).x());
    QWheelEvent wheel(position, view.quickView()->mapToGlobal(position.toPoint()), QPoint(0, 80),
                      QPoint(0, 240), Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(view.quickView(), &wheel);
    QVERIFY(!transcript->property("followTail").toBool());
    QCOMPARE(transcript->property("scrollState").toString(), QStringLiteral("ReadingHistory"));
    QTest::qWait(40);
    QWheelEvent end(position, view.quickView()->mapToGlobal(position.toPoint()), {}, {},
                    Qt::NoButton, Qt::NoModifier, Qt::ScrollEnd, false);
    QCoreApplication::sendEvent(view.quickView(), &end);
    QTRY_VERIFY(!transcript->property("moving").toBool());
    QTest::qWait(150);
    QVERIFY(!transcript->property("followTail").toBool());
    const auto y = transcript->property("contentY").toDouble();
    for (int i = 0; i < 8; ++i) {
        entry["body"] = entry.value("body").toString() + "Extra output.\n\n";
        view.updateMessage(0, entry);
        QTest::qWait(30);
        QVERIFY(!transcript->property("followTail").toBool());
        QVERIFY(qAbs(transcript->property("contentY").toDouble() - y) < 1);
    }
}

void AgentWorkspaceViewTest::steersRunningTurn() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString scriptPath = directory.filePath("steering-server.py");
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(#!/usr/bin/env python3
import sys,json,os,time
for line in sys.stdin:
    request=json.loads(line)
    method=request.get("method")
    p=request.get("params",{})
    with open(os.path.join(os.path.dirname(__file__),"calls.jsonl"),"a") as log:
        log.write(json.dumps(request)+"\n")
    def send(x): print(json.dumps(x),flush=True)
    def result(x): send({"id":request["id"],"result":x})
    if method=="initialize": result({})
    elif method=="thread/start": result({"thread":{"id":"steering-thread","turns":[]}})
    elif method=="turn/start":
        result({"turn":{"id":"running-turn"}})
        send({"method":"turn/started","params":{"threadId":"steering-thread","turn":{"id":"running-turn"}}})
    elif method=="turn/steer":
        time.sleep(0.05)
        text=p["input"][0]["text"]
        if text=="finish first":
            send({"method":"turn/completed","params":{"threadId":"steering-thread","turn":{"id":"running-turn","status":"completed"}}})
        if text in ("reject","finish first"):
            send({"id":request["id"],"error":{"code":-32600,"message":"Turn does not accept steering"}})
        else:
            result({"turnId":"running-turn"})
            send({"method":"item/completed","params":{"threadId":"steering-thread","item":{"id":"steered-user","type":"userMessage","content":p["input"]}}})
)PY");
    script.close();
    QVERIFY(QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));
    ketplus::CodexAppServer server(scriptPath);
    ketplus::AgentSessionManager manager(&server);
    QSignalSpy finished(&manager, &ketplus::AgentSessionManager::steeringFinished);
    manager.submitPrompt(directory.path(), "Original task");
    manager.steerPrompt(directory.path(), "Before ready");
    QCOMPARE(finished.size(), 1);
    QVERIFY(!finished.last().last().toBool());
    QTRY_COMPARE(manager.status(directory.path()), QStringLiteral("Working…"));
    manager.setConversationConfiguration(directory.path(), "next-model", "high", "read-only",
                                         "cursor");
    const auto nextConfiguration = manager.conversationConfiguration(directory.path());
    QCOMPARE(nextConfiguration.value("model").toString(), QStringLiteral("next-model"));
    QCOMPARE(nextConfiguration.value("effort").toString(), QStringLiteral("high"));
    QCOMPARE(nextConfiguration.value("permission").toString(), QStringLiteral("read-only"));
    QCOMPARE(nextConfiguration.value("provider").toString(), QStringLiteral("cursor"));
    QCOMPARE(manager.conversations(directory.path()).first().toMap().value("provider").toString(),
             QStringLiteral("cursor"));
    const QString originalId =
        manager.conversations(directory.path()).first().toMap().value("id").toString();
    // The transcript shows what the user typed, not the skill text wrapped around it.
    manager.steerPrompt(directory.path(),
                        "## skill\nLong skill text\n\nUser steering request:\n"
                        "Focus on tests",
                        {}, "  Focus on tests  ");
    manager.newSession(directory.path());
    QTRY_COMPARE(finished.size(), 2);
    QVERIFY(finished.last().last().toBool());
    QCOMPARE(finished.last().at(1).toString(), originalId);
    QVERIFY(manager.messages(directory.path()).isEmpty());
    manager.selectConversation(directory.path(), originalId);
    QCOMPARE(manager.messages(directory.path()).size(), 2);
    QCOMPARE(manager.messages(directory.path()).last().toMap().value("body").toString(),
             QStringLiteral("Focus on tests"));
    QVERIFY(manager.isRunning(directory.path()));
    manager.steerPrompt(directory.path(), "reject");
    QTRY_COMPARE(finished.size(), 3);
    QVERIFY(!finished.last().last().toBool());
    QVERIFY(manager.isRunning(directory.path()));
    QCOMPARE(manager.messages(directory.path()).size(), 2);
    manager.steerPrompt(directory.path(), "finish first");
    QTRY_COMPARE(finished.size(), 4);
    QVERIFY(!finished.last().last().toBool());
    QVERIFY(!manager.isRunning(directory.path()));
    manager.steerPrompt(directory.path(), "Too late");
    QCOMPARE(finished.size(), 5);
    QVERIFY(!finished.last().last().toBool());
    QFile log(directory.filePath("calls.jsonl"));
    QVERIFY(log.open(QIODevice::ReadOnly));
    int starts = 0;
    int steers = 0;
    while (!log.atEnd()) {
        const QJsonObject call = QJsonDocument::fromJson(log.readLine()).object();
        const QString method = call.value("method").toString();
        if (method == "turn/start")
            ++starts;
        if (method == "turn/steer") {
            ++steers;
            const QJsonObject params = call.value("params").toObject();
            QCOMPARE(params.value("threadId").toString(), QStringLiteral("steering-thread"));
            QCOMPARE(params.value("expectedTurnId").toString(), QStringLiteral("running-turn"));
            QCOMPARE(params.keys().size(), 3);
        }
    }
    QCOMPARE(starts, 1);
    QCOMPARE(steers, 3);
}

void AgentWorkspaceViewTest::keepsSteeringDrafts() {
    QTemporaryDir directory;
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setActivePath(directory.path());
    const QString path = view.activePath();
    view.setAgentAvailable(true);
    const auto select = [&view](const QString& id) {
        view.setConversations({QVariantMap{{"id", id},
                                           {"title", id},
                                           {"active", true},
                                           {"status", "Working"},
                                           {"running", true}}});
        view.setSessionState(id, "Working", true);
    };
    select("first");
    view.resize(1000, 700);
    view.show();
    QTest::qWait(100);
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* prompt = root->findChild<QQuickItem*>("agentPrompt");
    auto* send = root->findChild<QQuickItem*>("sendButton");
    auto* stop = root->findChild<QQuickItem*>("stopButton");
    QVERIFY(prompt && send && stop);
    QVERIFY(send->width() >= 28 && send->height() >= 28);
    QVERIFY(stop->width() >= 28 && stop->height() >= 28);
    QVERIFY(prompt->isEnabled());
    for (const auto* name : {"attachButton", "permissionButton", "modelButton"}) {
        auto* button = root->findChild<QQuickItem*>(QString::fromLatin1(name));
        QVERIFY(button);
        QVERIFY(button->isEnabled());
        QVERIFY(button->height() >= 28);
    }
    auto* attachGlyph = root->findChild<QQuickItem*>("attachGlyph");
    QVERIFY(attachGlyph);
    QCOMPARE(attachGlyph->size(), QSizeF(ket::Profile::current().composerAttachIconSize,
                                         ket::Profile::current().composerAttachIconSize));
    QSignalSpy configuration(&view,
                             &ketplus::AgentWorkspaceView::conversationConfigurationRequested);
    view.updateConversationConfiguration("next-model", "high", "read-only");
    QCOMPARE(configuration.count(), 1);
    QImage attachment(8, 8, QImage::Format_RGB32);
    attachment.fill(Qt::red);
    const auto attachmentPath = directory.filePath("running.png");
    QVERIFY(attachment.save(attachmentPath));
    view.addAttachments({attachmentPath});
    QCOMPARE(view.attachments().size(), 1);
    view.removeAttachment(0);
    QVERIFY(stop->isVisible());
    QSignalSpy stopRequested(&view, &ketplus::AgentWorkspaceView::stopRequested);
    QVERIFY(QMetaObject::invokeMethod(stop, "clicked"));
    QCOMPARE(stopRequested.count(), 1);
    QSignalSpy submitted(&view, &ketplus::AgentWorkspaceView::steeringPromptSubmitted);
    prompt->setProperty("text", "Focus on tests");
    QVERIFY(send->isVisible());
    QVERIFY(!stop->isVisible());
    QVERIFY(QMetaObject::invokeMethod(root, "submitCurrentPrompt"));
    QVERIFY(view.steeringPending());
    QVERIFY(!send->isEnabled());
    QVERIFY(stop->isEnabled());
    QVERIFY(prompt->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(root, "submitCurrentPrompt"));
    QCOMPARE(submitted.count(), 1);
    view.finishSteering(path, "first", "Focus on tests", false);
    QVERIFY(!view.steeringPending());
    QCOMPARE(prompt->property("text").toString(), QStringLiteral("Focus on tests"));
    QVERIFY(QMetaObject::invokeMethod(root, "submitCurrentPrompt"));
    prompt->setProperty("text", "Newer draft");
    view.finishSteering(path, "first", "Focus on tests", true);
    QCOMPARE(prompt->property("text").toString(), QStringLiteral("Newer draft"));
    QVERIFY(QMetaObject::invokeMethod(root, "submitCurrentPrompt"));
    select("second");
    prompt->setProperty("text", "Another conversation draft");
    view.finishSteering(path, "first", "Newer draft", true);
    QCOMPARE(prompt->property("text").toString(), QStringLiteral("Another conversation draft"));
    select("first");
    QCOMPARE(prompt->property("text").toString(), QString());
    prompt->setProperty("text", "Final input");
    QVERIFY(QMetaObject::invokeMethod(root, "submitCurrentPrompt"));
    view.finishSteering(path, "first", "Final input", true);
    QCOMPARE(prompt->property("text").toString(), QString());
    QVERIFY(!send->isVisible());
    QVERIFY(stop->isVisible());
    QVERIFY(!send->isEnabled());
    view.addAttachments({attachmentPath});
    QVERIFY(send->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(root, "submitCurrentPrompt"));
    QCOMPARE(submitted.last().at(0).toString(), QString());
    QCOMPARE(submitted.last().at(1).toList().size(), 1);
    view.finishSteering(path, "first", "", true);
    QVERIFY(view.attachments().isEmpty());
    // Images go with the steer; a refused steer keeps them for another try.
    view.addAttachments({attachmentPath});
    prompt->setProperty("text", "Match this screenshot");
    QVERIFY(send->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(root, "submitCurrentPrompt"));
    QVERIFY(view.steeringPending());
    QCOMPARE(submitted.last().at(1).toList().size(), 1);
    QCOMPARE(submitted.last().at(1).toList().first().toMap().value("path").toString(),
             attachmentPath);
    view.finishSteering(path, "first", "Match this screenshot", false);
    QCOMPARE(view.attachments().size(), 1);
    QVERIFY(QMetaObject::invokeMethod(root, "submitCurrentPrompt"));
    view.finishSteering(path, "first", "Match this screenshot", true);
    QVERIFY(view.attachments().isEmpty());
    QCOMPARE(prompt->property("text").toString(), QString());
    prompt->setProperty("text", "Focus on the failing tests first.");
    const QString capture = qEnvironmentVariable("KETPLUS_AGENT_CAPTURE_PATH");
    if (!capture.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(view.quickView()->grabWindow().save(capture + ".steering.png"));
        view.resize(640, 600);
        QTest::qWait(100);
        auto* composer = root->findChild<QQuickItem*>("agentComposer");
        QVERIFY(composer);
        QVERIFY(send->mapToItem(composer, QPointF(send->width(), send->height())).x() <=
                composer->width());
        QVERIFY(stop->mapToItem(composer, QPointF(0, 0)).x() >= 0);
        QVERIFY(view.quickView()->grabWindow().save(capture + ".steering-narrow.png"));
    }
}

void AgentWorkspaceViewTest::renamesAndRestoresConversations() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ketplus::AgentWorkspaceView view;
    QSignalSpy renameRequested(&view, &ketplus::AgentWorkspaceView::conversationRenameRequested);
    view.requestRenameConversation(QStringLiteral("target"));
    QCOMPARE(renameRequested.count(), 1);
    QCOMPARE(renameRequested.first().first().toString(), QStringLiteral("target"));
    view.requestRenameConversation({});
    QCOMPARE(renameRequested.count(), 1);
    QSignalSpy archiveRequested(&view, &ketplus::AgentWorkspaceView::conversationArchiveRequested);
    view.requestArchiveConversation(QStringLiteral("target"));
    QCOMPARE(archiveRequested.count(), 1);
    view.requestArchiveConversation({});
    QCOMPARE(archiveRequested.count(), 1);
    QSignalSpy deleteRequested(&view, &ketplus::AgentWorkspaceView::conversationDeleteRequested);
    view.requestDeleteConversation(QStringLiteral("target"));
    QCOMPARE(deleteRequested.count(), 1);
    view.requestDeleteConversation({});
    QCOMPARE(deleteRequested.count(), 1);

    QString firstId;
    QString secondId;
    {
        ketplus::AgentSessionManager manager;
        manager.activate(directory.path());
        firstId = manager.conversations(directory.path()).first().toMap().value("id").toString();
        manager.newSession(directory.path());
        secondId = manager.conversations(directory.path()).first().toMap().value("id").toString();
        QSignalSpy stateChanged(&manager, &ketplus::AgentSessionManager::sessionStateChanged);
        manager.renameConversation(directory.path(), firstId, QStringLiteral("  Earlier chat  "));
        QCOMPARE(stateChanged.count(), 0);
        QCOMPARE(manager.conversations(directory.path()).last().toMap().value("title").toString(),
                 QStringLiteral("Earlier chat"));
        QCOMPARE(manager.conversations(directory.path()).first().toMap().value("active").toBool(),
                 true);
        manager.renameConversation(directory.path(), secondId, QStringLiteral("Current chat"));
        QCOMPARE(stateChanged.count(), 1);
        QCOMPARE(manager.title(directory.path()), QStringLiteral("Current chat"));
        manager.renameConversation(directory.path(), secondId, QStringLiteral(" \n "));
        manager.renameConversation(directory.path(), QStringLiteral("missing"),
                                   QStringLiteral("Wrong"));
        QCOMPARE(stateChanged.count(), 1);
        QCOMPARE(manager.title(directory.path()), QStringLiteral("Current chat"));
    }
    ketplus::AgentSessionManager restored;
    restored.activate(directory.path());
    QCOMPARE(restored.title(directory.path()), QStringLiteral("Current chat"));
    QCOMPARE(restored.conversations(directory.path()).first().toMap().value("id").toString(),
             secondId);
    restored.selectConversation(directory.path(), firstId);
    QCOMPARE(restored.title(directory.path()), QStringLiteral("Earlier chat"));
    QCOMPARE(restored.conversations(directory.path()).first().toMap().value("id").toString(),
             secondId);
    QVERIFY(restored.archiveConversation(directory.path(), firstId));
    QCOMPARE(restored.conversations(directory.path()).size(), 1);
    QCOMPARE(restored.archivedConversations().size(), 1);
    QVERIFY(!restored.archiveConversation(directory.path(), firstId));
    QVERIFY(restored.restoreConversation(directory.path(), firstId));
    QCOMPARE(restored.conversations(directory.path()).size(), 2);
    QCOMPARE(restored.archivedConversations().size(), 0);
    QVERIFY(restored.deleteConversation(directory.path(), secondId));
    QCOMPARE(restored.conversations(directory.path()).size(), 1);
    QCOMPARE(restored.activeConversationId(directory.path()), firstId);
}

void AgentWorkspaceViewTest::persistsConversationConfiguration() {
    QSettings().remove(QStringLiteral("agent/lastConversationConfiguration"));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QString firstId;
    QString secondId;
    {
        ketplus::AgentSessionManager manager;
        manager.activate(directory.path());
        firstId = manager.activeConversationId(directory.path());
        manager.setConversationConfiguration(
            directory.path(), QStringLiteral("gpt-first"), QStringLiteral("high"),
            QStringLiteral("danger-full-access"), QStringLiteral("claude"));
        QVariantMap firstConfig = manager.conversationConfiguration(directory.path());
        QCOMPARE(firstConfig.value(QStringLiteral("model")).toString(),
                 QStringLiteral("gpt-first"));
        QCOMPARE(firstConfig.value(QStringLiteral("effort")).toString(), QStringLiteral("high"));
        QCOMPARE(firstConfig.value(QStringLiteral("permission")).toString(),
                 QStringLiteral("danger-full-access"));
        QCOMPARE(firstConfig.value(QStringLiteral("provider")).toString(),
                 QStringLiteral("claude"));

        manager.newSession(directory.path());
        secondId = manager.activeConversationId(directory.path());
        QVariantMap copiedConfig = manager.conversationConfiguration(directory.path());
        QCOMPARE(copiedConfig.value(QStringLiteral("model")).toString(),
                 QStringLiteral("gpt-first"));
        QCOMPARE(copiedConfig.value(QStringLiteral("effort")).toString(), QStringLiteral("high"));
        QCOMPARE(copiedConfig.value(QStringLiteral("permission")).toString(),
                 QStringLiteral("danger-full-access"));
        QCOMPARE(copiedConfig.value(QStringLiteral("provider")).toString(),
                 QStringLiteral("claude"));

        manager.setConversationConfiguration(directory.path(), QStringLiteral("gpt-second"),
                                             QStringLiteral("low"), QStringLiteral("read-only"),
                                             QStringLiteral("cursor"));
        manager.selectConversation(directory.path(), firstId);
        firstConfig = manager.conversationConfiguration(directory.path());
        QCOMPARE(firstConfig.value(QStringLiteral("model")).toString(),
                 QStringLiteral("gpt-first"));
        QCOMPARE(firstConfig.value(QStringLiteral("effort")).toString(), QStringLiteral("high"));
        QCOMPARE(firstConfig.value(QStringLiteral("permission")).toString(),
                 QStringLiteral("danger-full-access"));
        QCOMPARE(firstConfig.value(QStringLiteral("provider")).toString(),
                 QStringLiteral("claude"));
        manager.selectConversation(directory.path(), secondId);
    }

    ketplus::AgentSessionManager restored;
    restored.activate(directory.path());
    QCOMPARE(restored.activeConversationId(directory.path()), secondId);
    QVariantMap restoredConfig = restored.conversationConfiguration(directory.path());
    QCOMPARE(restoredConfig.value(QStringLiteral("model")).toString(),
             QStringLiteral("gpt-second"));
    QCOMPARE(restoredConfig.value(QStringLiteral("effort")).toString(), QStringLiteral("low"));
    QCOMPARE(restoredConfig.value(QStringLiteral("permission")).toString(),
             QStringLiteral("read-only"));
    QCOMPARE(restoredConfig.value(QStringLiteral("provider")).toString(), QStringLiteral("cursor"));
    restored.selectConversation(directory.path(), firstId);
    restoredConfig = restored.conversationConfiguration(directory.path());
    QCOMPARE(restoredConfig.value(QStringLiteral("model")).toString(), QStringLiteral("gpt-first"));
    QCOMPARE(restoredConfig.value(QStringLiteral("effort")).toString(), QStringLiteral("high"));
    QCOMPARE(restoredConfig.value(QStringLiteral("permission")).toString(),
             QStringLiteral("danger-full-access"));
    QCOMPARE(restoredConfig.value(QStringLiteral("provider")).toString(), QStringLiteral("claude"));

    QTemporaryDir other;
    QVERIFY(other.isValid());
    ketplus::AgentSessionManager seeded;
    seeded.activate(other.path());
    const QVariantMap lastUsed = seeded.conversationConfiguration(other.path());
    QCOMPARE(lastUsed.value(QStringLiteral("model")).toString(), QStringLiteral("gpt-second"));
    QCOMPARE(lastUsed.value(QStringLiteral("provider")).toString(), QStringLiteral("cursor"));
    restored.selectConversation(directory.path(), firstId);
    restoredConfig = restored.conversationConfiguration(directory.path());
    QCOMPARE(restoredConfig.value(QStringLiteral("model")).toString(), QStringLiteral("gpt-first"));
    QCOMPARE(restoredConfig.value(QStringLiteral("provider")).toString(), QStringLiteral("claude"));
    QSettings().remove(QStringLiteral("agent/lastConversationConfiguration"));
}

void AgentWorkspaceViewTest::reportsConversationContextWindow() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString scriptPath = directory.filePath(QStringLiteral("context-server.py"));
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(#!/usr/bin/env python3
import json
import sys

def send(value):
    print(json.dumps(value), flush=True)

for line in sys.stdin:
    request = json.loads(line)
    method = request.get("method")
    request_id = request.get("id")
    params = request.get("params", {})
    if method == "initialize":
        send({"id": request_id, "result": {}})
    elif method == "thread/start":
        send({"id": request_id, "result": {"thread": {"id": "context-thread", "turns": []}}})
    elif method == "turn/start":
        thread = params.get("threadId", "context-thread")
        send({"id": request_id, "result": {"turn": {"id": "context-turn"}}})
        send({"method": "turn/started", "params": {"threadId": thread, "turn": {"id": "context-turn"}}})
        send({"method": "thread/tokenUsage/updated", "params": {
            "threadId": thread,
            "tokenUsage": {
                "last": {"totalTokens": 5168, "inputTokens": 5152, "outputTokens": 16},
                "total": {"totalTokens": 90000},
                "modelContextWindow": 258400
            }
        }})
        send({"method": "item/agentMessage/delta", "params": {"threadId": thread, "itemId": "a1", "delta": "done"}})
        send({"method": "turn/completed", "params": {"threadId": thread, "turn": {"id": "context-turn", "status": "completed"}}})
    else:
        send({"id": request_id, "result": {}})
)PY");
    script.close();
    QVERIFY(QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));

    QString conversationId;
    {
        ketplus::CodexAppServer server(scriptPath);
        ketplus::AgentSessionManager manager(&server);
        QSignalSpy snapshot(&manager, &ketplus::AgentSessionManager::usageSnapshotChanged);
        manager.submitPrompt(directory.path(), QStringLiteral("Count tokens"));
        QTRY_VERIFY(!manager.isRunning(directory.path()));
        conversationId = manager.activeConversationId(directory.path());
        QTRY_VERIFY(snapshot.count() >= 1);
        const QVariantMap context =
            manager.usageSnapshot(directory.path()).value(QStringLiteral("context")).toMap();
        QVERIFY(context.value(QStringLiteral("available")).toBool());
        QCOMPARE(context.value(QStringLiteral("usedTokens")).toLongLong(), 5168);
        QCOMPARE(context.value(QStringLiteral("limitTokens")).toLongLong(), 258400);
        QCOMPARE(
            manager.usageSnapshot(directory.path()).value(QStringLiteral("provider")).toString(),
            QStringLiteral("codex"));
        manager.newSession(directory.path());
        const QVariantMap empty =
            manager.usageSnapshot(directory.path()).value(QStringLiteral("context")).toMap();
        QVERIFY(!empty.value(QStringLiteral("available")).toBool());
        manager.selectConversation(directory.path(), conversationId);
        const QVariantMap restoredLive =
            manager.usageSnapshot(directory.path()).value(QStringLiteral("context")).toMap();
        QCOMPARE(restoredLive.value(QStringLiteral("usedTokens")).toLongLong(), 5168);
    }

    ketplus::AgentSessionManager restored;
    restored.activate(directory.path());
    restored.selectConversation(directory.path(), conversationId);
    const QVariantMap restoredContext =
        restored.usageSnapshot(directory.path()).value(QStringLiteral("context")).toMap();
    QVERIFY(restoredContext.value(QStringLiteral("available")).toBool());
    QCOMPARE(restoredContext.value(QStringLiteral("usedTokens")).toLongLong(), 5168);
    QCOMPARE(restoredContext.value(QStringLiteral("limitTokens")).toLongLong(), 258400);
}

// Plan windows are readable only while a provider turn is alive, so the last reading
// has to survive until the next turn — and stop being shown once its window resets.
void AgentWorkspaceViewTest::reportsPlanLimitsAndDropsWindowsThatAlreadyReset() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString scriptPath = directory.filePath(QStringLiteral("limits-server.py"));
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly));
    const qint64 future = QDateTime::currentSecsSinceEpoch() + 3600;
    const qint64 past = QDateTime::currentSecsSinceEpoch() - 3600;
    script.write(QStringLiteral(R"PY(#!/usr/bin/env python3
import json
import sys

def send(value):
    print(json.dumps(value), flush=True)

for line in sys.stdin:
    request = json.loads(line)
    method = request.get("method")
    request_id = request.get("id")
    params = request.get("params", {})
    if method == "initialize":
        send({"id": request_id, "result": {}})
    elif method == "thread/start":
        send({"id": request_id, "result": {"thread": {"id": "limits-thread", "turns": []}}})
    elif method == "turn/start":
        thread = params.get("threadId", "limits-thread")
        send({"id": request_id, "result": {"turn": {"id": "limits-turn"}}})
        send({"method": "turn/started", "params": {"threadId": thread, "turn": {"id": "limits-turn"}}})
        send({"method": "thread/sessionUsage/updated", "params": {"threadId": thread,
            "usage": {"mode": "runtime", "budgetUsd": 0, "estimatedCostUsd": 12.5, "inputTokens": 20}}})
        send({"method": "thread/planLimits/updated", "params": {
            "threadId": thread,
            "limits": [
                {"id": "five_hour", "label": "Session", "usedPercent": 41.5, "resetsAt": %1,
                 "available": True},
                {"id": "seven_day", "label": "Weekly", "usedPercent": 12, "resetsAt": %2,
                 "available": True}
            ]
        }})
        send({"method": "turn/completed", "params": {"threadId": thread, "turn": {"id": "limits-turn", "status": "completed"}}})
    else:
        send({"id": request_id, "result": {}})
)PY")
                     .arg(future)
                     .arg(past)
                     .toUtf8());
    script.close();
    QVERIFY(QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));

    QString conversationId;
    {
        ketplus::CodexAppServer server(scriptPath);
        ketplus::AgentSessionManager manager(&server);
        manager.submitPrompt(directory.path(), QStringLiteral("Read my plan"));
        QTRY_VERIFY(!manager.isRunning(directory.path()));
        conversationId = manager.activeConversationId(directory.path());
        const QVariantList limits =
            manager.usageSnapshot(directory.path()).value(QStringLiteral("limits")).toList();
        // The weekly row's window has already reset; its percentage describes a period
        // that is over, so only the live one is offered.
        QCOMPARE(limits.size(), 1);
        QCOMPARE(limits.first().toMap().value(QStringLiteral("id")).toString(),
                 QStringLiteral("five_hour"));
        QCOMPARE(limits.first().toMap().value(QStringLiteral("usedPercent")).toDouble(), 41.5);
    }

    ketplus::AgentSessionManager restored;
    restored.activate(directory.path());
    restored.selectConversation(directory.path(), conversationId);
    const QVariantList restoredLimits =
        restored.usageSnapshot(directory.path()).value(QStringLiteral("limits")).toList();
    QCOMPARE(restored.usageSnapshot(directory.path())
                 .value("session")
                 .toMap()
                 .value("estimatedCostUsd")
                 .toDouble(),
             12.5);
    QCOMPARE(restoredLimits.size(), 1);
    QCOMPARE(restoredLimits.first().toMap().value(QStringLiteral("id")).toString(),
             QStringLiteral("five_hour"));
}

void AgentWorkspaceViewTest::hydratesSavedConversationOnSelection() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString scriptPath = directory.filePath(QStringLiteral("history-server.py"));
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(#!/usr/bin/env python3
import json
import sys

def send(value):
    print(json.dumps(value), flush=True)

for line in sys.stdin:
    request = json.loads(line)
    method = request.get("method")
    request_id = request.get("id")
    params = request.get("params", {})
    if method == "initialize":
        send({"id": request_id, "result": {}})
    elif method == "thread/start":
        send({"id": request_id, "result": {"thread": {"id": "saved-thread", "turns": []}}})
    elif method == "turn/start":
        send({"id": request_id, "result": {"turn": {"id": "saved-turn"}}})
        send({"method": "turn/completed", "params": {"threadId": "saved-thread", "turn": {"id": "saved-turn", "status": "completed"}}})
    elif method == "thread/resume":
        send({"id": request_id, "result": {"thread": {"id": params.get("threadId", "saved-thread"), "turns": [
            {"items": [
                {"id": "user-1", "type": "userMessage", "content": [{"type": "text", "text": "Saved user prompt"}]},
                {"id": "agent-1", "type": "agentMessage", "text": "Saved assistant response"}
            ]}
        ]}}})
)PY");
    script.close();
    QVERIFY(QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));

    QString savedConversationId;
    {
        ketplus::CodexAppServer server(scriptPath);
        ketplus::AgentSessionManager manager(&server);
        manager.submitPrompt(directory.path(), QStringLiteral("Original prompt"));
        QTRY_VERIFY(!manager.isRunning(directory.path()));
        savedConversationId = manager.activeConversationId(directory.path());
        manager.newSession(directory.path());
        QVERIFY(manager.messages(directory.path()).isEmpty());
    }

    ketplus::CodexAppServer server(scriptPath);
    ketplus::AgentSessionManager manager(&server);
    manager.activate(directory.path());
    QVERIFY(manager.messages(directory.path()).isEmpty());
    manager.selectConversation(directory.path(), savedConversationId);
    QTRY_COMPARE_WITH_TIMEOUT(manager.messages(directory.path()).size(), 2, 5000);
    QCOMPARE(
        manager.messages(directory.path()).at(0).toMap().value(QStringLiteral("body")).toString(),
        QStringLiteral("Saved user prompt"));
    QCOMPARE(
        manager.messages(directory.path()).at(1).toMap().value(QStringLiteral("body")).toString(),
        QStringLiteral("Saved assistant response"));
}
