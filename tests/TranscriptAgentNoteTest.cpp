#include "AgentWorkspaceViewTest.h"
#include "KetAppearance.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/DaemonAgentNotes.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/TranscriptNormalizer.h"
#include <QClipboard>
#include <QDir>

#include <QJsonArray>
#include <QQuickView>
#include <QtTest>

using namespace agent_workspace_test;

// The mock's "Message to @agent/8941  DS Icon · Claude · Replied" note, its text under it.
void AgentWorkspaceViewTest::sentAgentMessageReadsAsTheMockNote() {
    const auto rows = ketplus::agent_message::daemonWindowMessages(
        {{"events",
          QJsonArray{
              QJsonObject{{"cursor", 1},
                          {"turn_sequence", 1},
                          {"event_type", "agent_message.state"},
                          {"payload", QJsonObject{{"message_id", "q1"},
                                                  {"kind", "request"},
                                                  {"state", "sent"},
                                                  {"direction", "outgoing"},
                                                  {"peer_number", "8941"},
                                                  {"peer_title", "DS Icon"},
                                                  {"peer_provider", "claude"},
                                                  {"text", "Is the <b>spinner</b> fine?"}}}},
              QJsonObject{{"cursor", 2},
                          {"turn_sequence", 2},
                          {"event_type", "user.message"},
                          {"payload", QJsonObject{{"text", "Yes"},
                                                  {"origin", "agent"},
                                                  {"agent_messages",
                                                   QJsonArray{QJsonObject{{"message_id", "r1"},
                                                                          {"kind", "reply"},
                                                                          {"reply_to", "q1"},
                                                                          {"from_number", "8941"},
                                                                          {"body", "Yes"}}}}}}}}}});
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    view.resetMessages({rows.first()}, "agent-note");
    auto* root = view.quickView()->rootObject();
    QQuickItem* note = nullptr;
    QTRY_VERIFY((note = findVisibleItem(root, "transcriptAgentNote")) != nullptr);
    auto* text = findVisualItem(note, "transcriptAgentNoteText");
    QVERIFY(text);
    // The other agent's words are text, never markup.
    QCOMPARE(text->property("text").toString(),
             QStringLiteral("<b>Message to @agent/8941</b>&nbsp;&nbsp;DS Icon · Claude · "
                            "Replied"));
    auto* body = findVisualItem(note, "transcriptAgentNoteBody");
    QVERIFY(body);
    QCOMPARE(body->property("text").toString(), QStringLiteral("Is the <b>spinner</b> fine?"));
    QVERIFY(body->property("selectByMouse").toBool());
    // The note replaces the notice banner the row would otherwise draw.
    QVERIFY(!findVisibleItem(note->parentItem(), "transcriptNotice"));
}

void AgentWorkspaceViewTest::incomingAgentCardsClampAndExpand() {
    const QString longText =
        QStringLiteral("Synthetic digest line, selectable <b>plain text</b>.\n").repeated(24);
    const QList<QPair<QString, QString>> cases{{"agent", "request"}, {"agent", "reply"},
                                               {"agent", "notice"},  {"system", "notice"},
                                               {"team", "digest"},   {"agent", "system-wake"}};
    for (const auto& [origin, messageKind] : cases) {
        const QJsonArray senders =
            origin == "agent" && messageKind != "system-wake"
                ? QJsonArray{QJsonObject{{"from_number", "1234"},
                                         {"from_title", "Synthetic peer"},
                                         {"from_provider", "claude"},
                                         {"kind", messageKind}}}
                : QJsonArray{QJsonObject{
                      {"from_number", ""}, {"from_title", "Team Example"}, {"kind", "digest"}}};
        auto rows = ketplus::agent_message::daemonWindowMessages(
            {{"events",
              QJsonArray{QJsonObject{{"cursor", 1},
                                     {"turn_sequence", 1},
                                     {"event_type", "user.message"},
                                     {"payload", QJsonObject{{"origin", origin},
                                                             {"text", longText},
                                                             {"agent_messages", senders}}}}}}});
        QCOMPARE(rows.size(), 1);
        ketplus::AgentWorkspaceView view;
        view.applyTheme(testPalette());
        view.resize(900, 640);
        view.show();
        view.resetMessages(rows, "incoming-" + origin);
        QQuickItem* note = nullptr;
        QTRY_VERIFY(
            (note = findVisibleItem(view.quickView()->rootObject(), "transcriptAgentNote")));
        auto* surface = findVisibleItem(view.quickView()->rootObject(), "messageSurface");
        QVERIFY(surface);
        QCOMPARE(surface->property("color").value<QColor>().alpha(), 0);
        QCOMPARE(note->property("previewLines").toInt(), 4);
        QCOMPARE(note->property("iconName").toString(), QStringLiteral("arrow-down"));
        QVERIFY(note->property("label").toString().startsWith(
            origin == "agent" && messageKind != "system-wake" ? "From @agent/1234"
                                                              : "From Team Example"));
        auto* body = findVisualItem(note, "transcriptAgentNoteBody");
        auto* toggle = findVisibleItem(note, "transcriptAgentNoteExpand");
        QVERIFY(body && toggle);
        QCOMPARE(body->property("text").toString(), longText);
        QVERIFY(body->property("selectByMouse").toBool());
        view.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&view));
        view.quickView()->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(view.quickView()));
        QTest::mouseClick(view.quickView(), Qt::LeftButton, {},
                          body->mapToScene(QPointF(8, 8)).toPoint());
        body->forceActiveFocus();
        QTRY_VERIFY(body->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(body, "selectAll"));
        QCOMPARE(body->property("selectedText").toString(), longText);
        QKeyEvent shortcut(QEvent::ShortcutOverride, Qt::Key_C, Qt::ControlModifier);
        shortcut.ignore();
        QCoreApplication::sendEvent(body, &shortcut);
        QVERIFY(shortcut.isAccepted());
        QGuiApplication::clipboard()->setText("previous clipboard");
        QTest::keySequence(view.quickView(), QKeySequence::Copy);
        QCOMPARE(QGuiApplication::clipboard()->text(), longText);
        QVERIFY(QMetaObject::invokeMethod(body, "deselect"));
        QVERIFY(body->height() <= note->property("previewHeight").toReal() + 1);
        QTest::qWait(300); // Let the initial Latest positioning finish before measuring input.
        const auto collapsed = note->height();
        const auto top = note->mapToScene(QPointF()).y();
        QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
        QTRY_VERIFY(note->property("expanded").toBool());
        QTRY_VERIFY(note->height() > collapsed * 2);
        QTRY_VERIFY(qAbs(note->mapToScene(QPointF()).y() - top) < 2);
        QCOMPARE(toggle->property("text").toString(), QStringLiteral("Collapse"));
        QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
        QTRY_VERIFY(!note->property("expanded").toBool());
        QTRY_VERIFY(qAbs(note->height() - collapsed) < 1);
    }
    // Only owner input retains the right-aligned filled bubble.
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    for (const auto type : {"userMessage", "daemonUserMessage"}) {
        const auto owner = ketplus::ActivityNormalizer::normalize(
            {{"id", "owner"}, {"type", type}, {"text", "Synthetic owner input"}},
            ketplus::TranscriptPhase::Completed);
        QCOMPARE(owner.subtype, QString{});
        view.resetMessages({owner.toVariantMap()}, QStringLiteral("owner-") + type);
        QTRY_VERIFY(findVisibleItem(view.quickView()->rootObject(), "messageBody"));
        QVERIFY(!findVisibleItem(view.quickView()->rootObject(), "transcriptAgentNote"));
        auto* surface = findVisibleItem(view.quickView()->rootObject(), "messageSurface");
        QVERIFY(surface);
        QVERIFY(surface->property("color").value<QColor>().alpha() > 0);
        QVERIFY(surface->property("radius").toReal() > 0);
    }
}

// Optional deterministic captures; no daemon, provider, account or saved user preferences.
void AgentWorkspaceViewTest::agentCardEvidence() {
    const auto directory = qEnvironmentVariable("KETPLUS_AGENT_CARD_CAPTURE_DIR");
    if (directory.isEmpty())
        return;
    QVERIFY(QDir().mkpath(directory));
    const auto rows = ketplus::agent_message::daemonWindowMessages(
        {{"events",
          QJsonArray{QJsonObject{
              {"cursor", 1},
              {"turn_sequence", 1},
              {"event_type", "user.message"},
              {"payload",
               QJsonObject{
                   {"origin", "agent"},
                   {"text",
                    QStringLiteral("Synthetic team digest, no account data.\n").repeated(30)},
                   {"agent_messages", QJsonArray{QJsonObject{{"kind", "notice"},
                                                             {"from_number", "1234"},
                                                             {"from_title", "Example team"},
                                                             {"from_provider", "claude"}}}}}}}}}});
    for (const auto mode : {ket::Appearance::Mode::Light, ket::Appearance::Mode::Dark}) {
        ket::Appearance::instance().setMode(mode);
        ketplus::AgentWorkspaceView view;
        view.applyTheme(testPalette());
        view.resize(1000, 720);
        view.show();
        view.resetMessages(rows, "synthetic-card-evidence");
        QTRY_VERIFY(view.quickView()->rootObject());
        QTest::qWait(300);
        const auto name = mode == ket::Appearance::Mode::Dark ? "dark" : "light";
        QVERIFY(view.quickView()->grabWindow().save(directory + "/" + name + ".png"));
        auto incoming = rows.first().toMap();
        incoming.insert("body", QStringLiteral("Synthetic selectable digest line.\n").repeated(8));
        incoming.insert("messageKey", "paired-incoming");
        const auto outgoing = ketplus::agent_message::agentNote(
            {{"state", "sent"},
             {"message_id", "synthetic-outgoing"},
             {"direction", "outgoing"},
             {"kind", "notice"},
             {"peer_number", "5678"},
             {"peer_title", "Example peer"},
             {"peer_provider", "claude"},
             {"text", "Synthetic outgoing notice, no account data."}},
            "paired-outgoing");
        QCOMPARE(outgoing.value("subtype").toString(), QStringLiteral("agentNote"));
        view.resetMessages({outgoing, incoming}, "paired-card-evidence");
        QTest::qWait(300);
        QVERIFY(view.quickView()->grabWindow().save(directory + "/paired-" + name + ".png"));
        QQuickItem* toggle = nullptr;
        QTRY_VERIFY((
            toggle = findVisibleItem(view.quickView()->rootObject(), "transcriptAgentNoteExpand")));
        QVERIFY(QMetaObject::invokeMethod(toggle, "clicked"));
        QTest::qWait(300);
        QVERIFY(view.quickView()->grabWindow().save(directory + "/expanded-" + name + ".png"));
    }
}
