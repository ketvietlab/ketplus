#include "AgentWorkspaceViewTest.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/DaemonTranscriptProjection.h"

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
    view.resetMessages(rows, "agent-note");
    auto* root = view.quickView()->rootObject();
    QQuickItem* note = nullptr;
    QTRY_VERIFY((note = findVisibleItem(root, "transcriptAgentNote")) != nullptr);
    auto* text = findVisualItem(note, "transcriptAgentNoteText");
    QVERIFY(text);
    // The other agent's words are text, never markup.
    QCOMPARE(text->property("text").toString(),
             QStringLiteral("<b>Message to @agent/8941</b>&nbsp;&nbsp;DS Icon · Claude · "
                            "Replied<br>Is the &lt;b&gt;spinner&lt;/b&gt; fine?"));
    // The note replaces the notice banner the row would otherwise draw.
    QVERIFY(!findVisibleItem(note->parentItem(), "transcriptNotice"));
}
