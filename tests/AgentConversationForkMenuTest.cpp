#include "AgentWorkspaceViewTest.h"
#include "agent/AgentWorkspaceView.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QQuickView>
#include <QSignalSpy>
#include <QtTest>

using namespace agent_workspace_test;

namespace {

QVariantMap conversation(const bool forkable, const QString& number = "8941") {
    return {{"id", "source"},  {"title", "Source"},    {"provider", "codex"},
            {"active", true},  {"running", true},      {"status", "Working"},
            {"started", true}, {"forkable", forkable}, {"number", number}};
}

} // namespace

void AgentWorkspaceViewTest::conversationMenusOfferFork() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setConversations({conversation(true)});
    view.resetMessages({QVariantMap{{QStringLiteral("kind"), QStringLiteral("user")},
                                    {QStringLiteral("body"), QStringLiteral("Explain the tree")},
                                    {QStringLiteral("meta"), QStringLiteral("You")}},
                        QVariantMap{{QStringLiteral("kind"), QStringLiteral("assistant")},
                                    {QStringLiteral("body"), QStringLiteral("It has two files.")},
                                    {QStringLiteral("meta"), QStringLiteral("Codex")}}});
    view.resize(1000, 700);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    QSignalSpy forks(&view, &ketplus::AgentWorkspaceView::conversationForkRequested);
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);

    // Sidebar: the conversation name's menu.
    QQuickItem* row = nullptr;
    QTRY_VERIFY((row = findVisualItem(root, QStringLiteral("conversationRow_source"))));
    auto* sidebarMenu = qvariant_cast<QObject*>(row->property("contextMenu"));
    QVERIFY(sidebarMenu);
    auto* sidebarFork = sidebarMenu->findChild<QObject*>("forkConversationMenuItem");
    QVERIFY(sidebarFork);
    QCOMPARE(sidebarFork->property("text").toString(), QStringLiteral("Fork conversation"));
    QVERIFY(sidebarFork->property("enabled").toBool());
    // As in the mock, the menu copies the reference other conversations message.
    auto* reference = sidebarMenu->findChild<QObject*>("copyConversationReferenceMenuItem");
    QVERIFY(reference);
    QCOMPARE(reference->property("shortcut").toString(), QStringLiteral("@agent/8941"));
    QVERIFY(QMetaObject::invokeMethod(reference, "triggered"));
    QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("@agent/8941"));
    QVERIFY(QMetaObject::invokeMethod(sidebarFork, "triggered"));
    QTRY_COMPARE(forks.size(), 1);
    QCOMPARE(forks.constFirst().constFirst().toString(), QStringLiteral("source"));

    // Transcript: a right-click on a message opens the conversation menu.
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QTRY_COMPARE(transcript->property("count").toInt(), 2);
    auto* body = findVisualItem(transcript, QStringLiteral("messageBody"));
    QVERIFY(body);
    QTRY_VERIFY(body->height() > 0);
    auto* transcriptMenu = transcript->findChild<QObject*>("transcriptContextMenu");
    QVERIFY(transcriptMenu);
    QVERIFY(!transcriptMenu->property("visible").toBool());
    QTest::mouseClick(view.quickView(), Qt::RightButton, Qt::NoModifier,
                      body->mapToScene(QPointF(10, body->height() / 2)).toPoint());
    QTRY_VERIFY(transcriptMenu->property("visible").toBool());
    auto* transcriptFork = qvariant_cast<QObject*>(transcriptMenu->property("forkItem"));
    QVERIFY(transcriptFork);
    QCOMPARE(transcriptFork->property("text").toString(), QStringLiteral("Fork this conversation"));
    QVERIFY(transcriptFork->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(transcriptFork, "triggered"));
    QTRY_COMPARE(forks.size(), 2);
    QCOMPARE(forks.last().constFirst().toString(), QStringLiteral("source"));
    QVERIFY(QMetaObject::invokeMethod(transcriptMenu, "close"));
    QTRY_VERIFY(!transcriptMenu->property("visible").toBool());

    // A conversation that cannot be forked keeps both items visible but disabled.
    view.setConversations({conversation(false)});
    QTRY_VERIFY(!transcriptFork->property("enabled").toBool());
    QTRY_VERIFY((row = findVisualItem(root, QStringLiteral("conversationRow_source"))));
    sidebarMenu = qvariant_cast<QObject*>(row->property("contextMenu"));
    QVERIFY(sidebarMenu);
    sidebarFork = sidebarMenu->findChild<QObject*>("forkConversationMenuItem");
    QVERIFY(sidebarFork);
    QTRY_VERIFY(!sidebarFork->property("enabled").toBool());
}

void AgentWorkspaceViewTest::hiddenConversationReferenceTakesNoKeyFocus() {
    // A conversation without a number has no reference; the menu keys step over its item.
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setConversations({conversation(true, {})});
    view.resize(1000, 700);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    view.quickView()->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(view.quickView()));
    QQuickItem* row = nullptr;
    QTRY_VERIFY((row = findVisualItem(view.quickView()->rootObject(),
                                      QStringLiteral("conversationRow_source"))));
    auto* menu = qvariant_cast<QObject*>(row->property("contextMenu"));
    QVERIFY(menu);
    auto* reference = menu->findChild<QObject*>("copyConversationReferenceMenuItem");
    QVERIFY(reference);
    QVERIFY(!reference->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(menu, "open"));
    QTRY_VERIFY(menu->property("visible").toBool());
    menu->setProperty("currentIndex", 0);
    auto* rename =
        qobject_cast<QQuickItem*>(menu->findChild<QObject*>("renameConversationMenuItem"));
    QVERIFY(rename);
    rename->forceActiveFocus();
    QTRY_VERIFY(rename->hasActiveFocus());
    for (int step = 0; step < 3; ++step)
        QTest::keyClick(view.quickView(), Qt::Key_Down);
    auto* archive = menu->findChild<QObject*>("archiveConversationMenuItem");
    QTRY_VERIFY(archive->property("highlighted").toBool());
    QVERIFY(!reference->property("activeFocus").toBool());
    QTest::keyClick(view.quickView(), Qt::Key_Up);
    QTRY_VERIFY(menu->findChild<QObject*>("changeConversationWorktreeMenuItem")
                    ->property("highlighted")
                    .toBool());
    QMetaObject::invokeMethod(menu, "close");
}
