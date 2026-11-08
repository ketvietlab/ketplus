#include "AgentWorkspaceViewTest.h"
#include "agent/AgentWorkspaceView.h"

#include <QDir>
#include <QFile>
#include <QQuickItem>
#include <QQuickView>
#include <QRegularExpression>
#include <QtTest>

using namespace agent_workspace_test;

void AgentWorkspaceViewTest::measurementTimerStopsAndRestartsForNewRows() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* transcript = root->findChild<QQuickItem*>(QStringLiteral("agentTranscript"));
    QVERIFY(transcript);
    auto* timer = transcript->findChild<QObject*>(QStringLiteral("transcriptMeasurementTimer"));
    QVERIFY(timer);
    QTRY_VERIFY(!timer->property("running").toBool());

    QVariantList rows;
    for (int i = 0; i < 150; ++i) {
        rows.append(QVariantMap{{"kind", "assistant"},
                                {"messageKey", QStringLiteral("idle-row-%1").arg(i)},
                                {"body", QStringLiteral("Row %1. ").arg(i).repeated(40)}});
    }
    view.resetMessages(rows, QStringLiteral("idle-measurement"));
    QTRY_VERIFY_WITH_TIMEOUT(!timer->property("running").toBool(), 10000);
    QCOMPARE(transcript->property("measurementPending").toBool(), false);

    view.resize(640, 640);
    QTRY_VERIFY_WITH_TIMEOUT(!timer->property("running").toBool(), 10000);
    // Pending work stays pending while hidden, then resumes when the window returns.
    view.hide();
    transcript->setProperty("measurementPending", true);
    QVERIFY(!timer->property("running").toBool());
    auto* growth = transcript->findChild<QObject*>(QStringLiteral("transcriptRowWindowGrowth"));
    auto* glide = transcript->findChild<QObject*>(QStringLiteral("transcriptWheelGlide"));
    QVERIFY(growth && glide);
    QVERIFY(!growth->property("running").toBool());
    QVERIFY(!glide->property("running").toBool());
    view.show();
    QTRY_VERIFY_WITH_TIMEOUT(!transcript->property("measurementPending").toBool(), 10000);
}

// #126 sites 5 and 6: both frame animations end on their own while the window stays visible.
void AgentWorkspaceViewTest::transcriptFrameAnimationsStopOnTheirOwn() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* transcript =
        view.quickView()->rootObject()->findChild<QQuickItem*>(QStringLiteral("agentTranscript"));
    QVERIFY(transcript);
    auto* growth = transcript->findChild<QObject*>(QStringLiteral("transcriptRowWindowGrowth"));
    auto* glide = transcript->findChild<QObject*>(QStringLiteral("transcriptWheelGlide"));
    QVERIFY(growth && glide);
    QVariantList rows;
    for (int i = 0; i < 150; ++i)
        rows.append(QVariantMap{{"kind", "assistant"},
                                {"messageKey", QStringLiteral("glide-row-%1").arg(i)},
                                {"body", QStringLiteral("Row %1. ").arg(i).repeated(40)}});
    view.resetMessages(rows, QStringLiteral("frame-animations"));
    QTRY_VERIFY_WITH_TIMEOUT(transcript->property("measurementPending").toBool() == false, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!growth->property("running").toBool(), 10000);
    QTRY_VERIFY(transcript->property("contentY").toReal() > 0); // opened at the newest row

    // One wheel notch towards older rows glides, then stops.
    const qreal before = transcript->property("contentY").toReal();
    QVERIFY(QMetaObject::invokeMethod(transcript, "glideWheel", Q_ARG(QVariant, 360.0)));
    QTRY_VERIFY(glide->property("running").toBool() ||
                transcript->property("contentY").toReal() < before);
    QTRY_VERIFY_WITH_TIMEOUT(!glide->property("running").toBool(), 5000);
    QVERIFY(transcript->property("contentY").toReal() < before);
    QCOMPARE(transcript->property("wheelGlideRemaining").toReal(), 0.0);
    // Growing the realized window after the scroll ends as well.
    QTRY_VERIFY_WITH_TIMEOUT(!growth->property("running").toBool(), 10000);
}

// #126: a minimized window holds no clock, like a hidden one, alone or inside a main window.
void AgentWorkspaceViewTest::minimizedWindowReleasesTheWorkingClock() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.setSessionState("Minimize trial", "Working…", true);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* icon = view.quickView()->rootObject()->findChild<QQuickItem*>("workingAnimationIcon");
    QVERIFY(icon);
    QTRY_VERIFY(icon->property("holdsClock").toBool());
    // Qt Widgets hides the embedded QQuickView while its top-level window is minimized.
    view.showMinimized();
    QTRY_VERIFY(!view.quickView()->isVisible());
    QTRY_VERIFY(!icon->property("holdsClock").toBool());
    view.showNormal();
    QTRY_VERIFY(icon->property("holdsClock").toBool());

    // Embedded in another top-level window, as in the app's main window.
    QWidget main;
    main.resize(900, 640);
    ketplus::AgentWorkspaceView embedded(&main);
    embedded.applyTheme(testPalette());
    embedded.resize(900, 640);
    embedded.setSessionState("Embedded trial", "Working…", true);
    main.show();
    QVERIFY(QTest::qWaitForWindowExposed(embedded.quickView()));
    auto* embeddedIcon =
        embedded.quickView()->rootObject()->findChild<QQuickItem*>("workingAnimationIcon");
    QVERIFY(embeddedIcon);
    QTRY_VERIFY(embeddedIcon->property("holdsClock").toBool());
    main.showMinimized();
    QTRY_VERIFY(!embedded.quickView()->isVisible());
    QTRY_VERIFY(!embeddedIcon->property("holdsClock").toBool());
    main.showNormal();
    QTRY_VERIFY(embeddedIcon->property("holdsClock").toBool());
}

void AgentWorkspaceViewTest::agentQmlUsesBoundedAnimationsAndKitPrimitives() {
    const QDir resources(QStringLiteral(":/agent"));
    const QStringList files = resources.entryList({QStringLiteral("*.qml")}, QDir::Files);
    QVERIFY(files.size() > 20);
    QVERIFY(!QFile::exists(QStringLiteral(":/agent/ChromeMetrics.js")));
    QVERIFY(!QFile::exists(QStringLiteral(":/agent/TintedIcon.qml")));
    QVERIFY(!QFile::exists(QStringLiteral(":/agent/KetToolTip.qml")));
    const QRegularExpression frames(QStringLiteral("FrameAnimation\\s*\\{\\s*id:\\s*(\\w+)"));
    int frameCount = 0;
    for (const QString& file : files) {
        QFile source(resources.filePath(file));
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(source.readAll());
        QVERIFY2(!text.contains(QStringLiteral("Animation.Infinite")), qPrintable(file));
        int matchedFrames = 0;
        auto matches = frames.globalMatch(text);
        while (matches.hasNext()) {
            const QString id = matches.next().captured(1);
            QVERIFY2(
                file == QStringLiteral("TranscriptView.qml") &&
                    (id == QStringLiteral("rowWindowGrowth") || id == QStringLiteral("wheelGlide")),
                qPrintable(file + ":" + id));
            ++frameCount;
            ++matchedFrames;
        }
        QCOMPARE(text.count(QRegularExpression(QStringLiteral("\\bFrameAnimation\\s*\\{"))),
                 matchedFrames);
    }
    QCOMPARE(frameCount, 2);
}
