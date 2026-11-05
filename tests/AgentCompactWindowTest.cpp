#include "AgentWorkspaceViewTest.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexAppServer.h"
#include <QApplication>
#include <QDir>
#include <QQuickView>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QtTest>

using namespace agent_workspace_test;

void AgentWorkspaceViewTest::manualCompactWindow() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.resize(1000, 900);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    view.quickView()->requestActivate();
    const QVariantMap saved{{"model", "opus"},
                            {"effort", "high"},
                            {"provider", "claude"},
                            {"permission", "workspace-write"},
                            {"compactWindow", 272000},
                            {"compactWindowSupported", true},
                            {"configurationWritable", true},
                            {"configurationRevision", 7}};
    view.setConversationConfiguration(saved);
    view.setModels({QVariantMap{
        {"model", "opus"},
        {"displayName", "Opus 5.5"},
        {"provider", "claude"},
        {"modelContextWindow", 1000000},
        {"permissions", ketplus::CodexAppServer::permissionProfiles()},
        {"defaultPermission", "workspace-write"},
        {"defaultReasoningEffort", "high"},
        {"supportedReasoningEfforts", QVariantList{QVariantMap{{"reasoningEffort", "high"}}}}}});
    auto* root = view.quickView()->rootObject();
    auto* popup = root->findChild<QObject*>("modelPopup");
    auto* opener = root->findChild<QQuickItem*>("modelButton");
    auto* compact = root->findChild<QObject*>("compactWindowControl");
    auto* apply = root->findChild<QQuickItem*>("applyModelButton");
    auto* cancel = root->findChild<QQuickItem*>("cancelModelButton");
    QVERIFY(popup && opener && compact && apply && cancel);
    QSignalSpy requests(&view, &ketplus::AgentWorkspaceView::conversationConfigurationRequested);
    const auto open = [&] { return QMetaObject::invokeMethod(opener, "clicked"); };
    QVERIFY(open());
    QTRY_COMPARE(compact->property("preset").toInt(), 272000);
    QCOMPARE(compact->property("capacity").toInt(), 1000000);
    compact->setProperty("preset", 500000);
    QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
    QCOMPARE(requests.size(), 0);
    QCOMPARE(view.conversationCompactWindow().toInt(), 272000);
    QVERIFY(open());
    QCOMPARE(compact->property("preset").toInt(), 272000);
    compact->setProperty("custom", true);
    for (const QString& invalid :
         {QString{}, QStringLiteral("99000"), QStringLiteral("1000001"), QStringLiteral("272k"),
          QStringLiteral("2.72e5"), QStringLiteral("272000.5")}) {
        compact->setProperty("customText", invalid);
        QTRY_VERIFY(!apply->isEnabled());
    }
    compact->setProperty("customText", "350000");
    QTRY_VERIFY(apply->isEnabled());
    auto peer = saved;
    peer["configurationRevision"] = 8;
    view.setConversationConfiguration(peer);
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.last()[4].toMap().value("expected_revision").toLongLong(), 7);
    QCOMPARE(requests.last()[4].toMap().value("compactWindow").toInt(), 350000);
    QVERIFY(view.modelConfigurationApplying());
    QVERIFY(!apply->isEnabled());
    view.finishModelConfiguration("Settings changed. Retry.");
    QVERIFY(popup->property("visible").toBool());
    QCOMPARE(compact->property("customText").toString(), QStringLiteral("350000"));
    QVERIFY(!view.modelConfigurationError().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QCOMPARE(requests.size(), 2);
    QCOMPARE(requests.last()[4].toMap().value("expected_revision").toLongLong(), 8);
    auto accepted = saved;
    accepted["compactWindow"] = 350000;
    view.setConversationConfiguration(accepted);
    view.finishModelConfiguration({});
    QTRY_VERIFY(!popup->property("visible").toBool());
    QCOMPARE(view.conversationCompactWindow().toInt(), 350000);
    QVERIFY(open());
    compact->setProperty("custom", false);
    compact->setProperty("preset", 0);
    QVERIFY(QMetaObject::invokeMethod(apply, "clicked"));
    QVERIFY(requests.last()[4].toMap().contains("compactWindow"));
    QVERIFY(requests.last()[4].toMap().value("compactWindow").isNull());
    view.finishModelConfiguration({});
    QVERIFY(open());
    view.setSessionState("Conversation", "Working", true);
    QTRY_VERIFY(apply->isEnabled());
    view.setSessionState("Conversation", "Ready", false);
    view.setBackgroundTasks({QVariantMap{
        {"task_id", "shell"}, {"status", "running"}, {"title", "Background fixture"}}});
    QTRY_VERIFY(apply->isEnabled());
    view.setBackgroundTasks({});
    auto offline = accepted;
    offline["configurationWritable"] = false;
    view.setConversationConfiguration(offline);
    QTRY_VERIFY(!apply->isEnabled());
    view.setConversationConfiguration(accepted);
    QTRY_VERIFY(apply->isEnabled());
    const auto capture = qEnvironmentVariable("KETPLUS_COMPACT_WINDOW_CAPTURE");
    if (!capture.isEmpty())
        qInfo() << "Compact window renderer" << QGuiApplication::platformName()
                << view.quickView()->rendererInterface()->graphicsApi() << "DPR"
                << view.quickView()->devicePixelRatio();
    const auto checkBounds = [&] {
        QVERIFY(popup->property("visible").toBool());
        const auto point = apply->mapToItem(root, QPointF(0, 0));
        QVERIFY(point.y() >= 0 && point.y() + apply->height() <= root->height());
        QVERIFY(popup->property("width").toDouble() <= root->width());
    };
    ketplus::ThemeManager theme(*qobject_cast<QApplication*>(QCoreApplication::instance()));
    const auto catalog = view.models();
    for (bool dark : {true, false}) {
        theme.previewMode(dark ? ketplus::ThemeManager::Mode::Dark
                               : ketplus::ThemeManager::Mode::Light);
        view.applyTheme(theme.palette());
        for (const auto size : {QSize(1000, 900), QSize(420, 480)}) {
            view.resize(size);
            QTest::qWait(100);
            const auto resizedFooter = apply->mapToItem(root, QPointF(0, 0));
            if (resizedFooter.y() < 0 || resizedFooter.y() + apply->height() > root->height())
                qWarning() << "resized" << size << "footer" << resizedFooter.y() << apply->height()
                           << "root" << root->height() << "popup" << popup->property("height");
            checkBounds();
            if (!capture.isEmpty())
                QVERIFY(view.quickView()->grabWindow().save(capture + (dark ? ".dark" : ".light") +
                                                            QString::number(size.width()) +
                                                            ".png"));
            for (const QString& state :
                 {QStringLiteral("default"), QStringLiteral("custom"), QStringLiteral("invalid"),
                  QStringLiteral("auto"), QStringLiteral("error"), QStringLiteral("conflict"),
                  QStringLiteral("denied"), QStringLiteral("offline"), QStringLiteral("running"),
                  QStringLiteral("background"), QStringLiteral("applying"),
                  QStringLiteral("loading"), QStringLiteral("unsupported"), QStringLiteral("empty"),
                  QStringLiteral("unknown-capacity")}) {
                view.finishModelConfiguration({});
                QVERIFY(QMetaObject::invokeMethod(popup, "close"));
                view.setModels(catalog);
                view.setConversationConfiguration(saved);
                view.setSessionState("Conversation", "Ready", false);
                view.setBackgroundTasks({});
                root->setProperty("modelsLoading", false);
                QVERIFY(open());
                if (state == "custom" || state == "invalid") {
                    compact->setProperty("custom", true);
                    compact->setProperty("customText", state == "invalid" ? "99000" : "350000");
                } else if (state == "auto") {
                    compact->setProperty("preset", 0);
                } else if (state == "running") {
                    view.setSessionState("Conversation", "Working", true);
                } else if (state == "background") {
                    view.setBackgroundTasks({QVariantMap{{"task_id", "shell"},
                                                         {"status", "running"},
                                                         {"title", "Background fixture"}}});
                } else if (state == "offline" || state == "unsupported") {
                    auto configuration = saved;
                    configuration[state == "offline" ? "configurationWritable"
                                                     : "compactWindowSupported"] = false;
                    view.setConversationConfiguration(configuration);
                } else if (state == "loading" || state == "empty") {
                    view.setModels({});
                    root->setProperty("modelsLoading", state == "loading");
                } else if (state == "unknown-capacity") {
                    auto model = catalog.first().toMap();
                    model.remove("modelContextWindow");
                    view.setModels({model});
                } else if (state == "applying" || state == "error" || state == "conflict" ||
                           state == "denied") {
                    view.applyModelConfiguration("opus", "high", "claude", {});
                    if (state != "applying")
                        view.finishModelConfiguration(
                            state == "conflict" ? "Settings changed on another device. Retry."
                            : state == "denied" ? "Permission denied. Ask the machine owner."
                                                : "Settings were not applied. Retry.");
                }
                QTest::qWait(50);
                const auto footerPoint = apply->mapToItem(root, QPointF(0, 0));
                if (footerPoint.y() < 0 || footerPoint.y() + apply->height() > root->height())
                    qWarning() << state << size << "footer" << footerPoint.y() << apply->height()
                               << "root" << root->height() << "popup" << popup->property("height");
                checkBounds();
                if (state == "invalid" || state == "offline" || state == "applying" ||
                    state == "loading")
                    QVERIFY(!apply->isEnabled());
                if (state == "running" || state == "background") {
                    QVERIFY(apply->isEnabled());
                    QVERIFY(!compact->property("blocked").toBool());
                    QCOMPARE(popup->property("lockedProvider").toString(), QString{});
                }
                if (!capture.isEmpty())
                    QVERIFY(view.quickView()->grabWindow().save(
                        capture + (dark ? ".dark" : ".light") + QString::number(size.width()) +
                        "." + state + ".png"));
            }
            view.finishModelConfiguration({});
            view.setModels(catalog);
            view.setConversationConfiguration(accepted);
            root->setProperty("modelsLoading", false);
            QVERIFY(QMetaObject::invokeMethod(popup, "close"));
            QVERIFY(open());
        }
    }
    theme.cancelPreview();
    view.resize(420, 480);
    compact->setProperty("custom", false);
    compact->setProperty("preset", 272000);
    auto* popupContent = popup->property("contentItem").value<QQuickItem*>();
    QVERIFY(popupContent);
    auto* presetChoice = findVisualItem(popupContent, QStringLiteral("compactWindow_272000"));
    auto* nextChoice = findVisualItem(popupContent, QStringLiteral("compactWindow_500000"));
    QVERIFY(presetChoice && nextChoice);
    QTest::qWait(100);
    view.quickView()->requestActivate();
    presetChoice->forceActiveFocus();
    QTRY_VERIFY(presetChoice->hasActiveFocus());
    QTest::keyClick(view.quickView(), Qt::Key_Right);
    QTRY_COMPARE(compact->property("preset").toInt(), 500000);
    const auto choiceVisible = [&] {
        const auto position = nextChoice->mapToItem(root, QPointF(0, 0));
        const auto footer = apply->mapToItem(root, QPointF(0, 0));
        return position.y() > 0 && position.y() + nextChoice->height() < footer.y();
    };
    QTRY_VERIFY(choiceVisible());
    view.resize(1000, 900);
    // Key events target the embedded Quick view; global OS activation belongs
    // to the parent app and cannot be required of a command-line test runner.
    nextChoice->forceActiveFocus();
    QTest::keyClick(view.quickView(), Qt::Key_Escape);
    QTRY_VERIFY(!popup->property("visible").toBool());
}
