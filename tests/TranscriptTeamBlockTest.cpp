// #468: a team's own messages read as their blocks. The bodies follow the daemon's
// TeamCoordinator texts (assignmentText, digestText, reviewText, the verdict and the answered
// decision); run with QT_QPA_PLATFORM=offscreen.
#include "AgentWorkspaceViewTest.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/TeamTranscriptBlocks.h"

#include <QJsonArray>
#include <QQuickView>
#include <QtTest>

using namespace agent_workspace_test;

namespace {

QJsonObject teamMessage(const QString& kind, const QStringList& lines,
                        const QString& from = QString{}) {
    return {{"message_id", "m-" + kind},
            {"kind", kind},
            {"from_number", from},
            {"from_title", from.isEmpty() ? QStringLiteral("Team Shop") : QStringLiteral("Lead")},
            {"body", lines.join('\n')}};
}

QJsonObject assignmentMessage() {
    return teamMessage(
        "assignment",
        {"Team Shop: task T-3 \"Checkout page\" is assigned to you by Lead.",
         "Branch: team/shop/t-3", "Worktree: /work/shop-t-3 (your conversation now works there)",
         "Lane: Web", "", "One page from cart to receipt.", "", "Acceptance criteria:",
         "1. [x] The cart total is shown", "2. [ ] Payment errors stay on the page", "",
         "Start it with team_task_update (state in_progress), report through the task, "
         "and hand it to review (state in_review) once the criteria are met and your work "
         "is committed."},
        "1201");
}

// Ten updates over three groups, two of them for the leader.
QJsonObject digestMessage() {
    return teamMessage(
        "digest",
        {"Team Shop: what changed since your last digest.", "", "T-1 \"Cart\" (in_review)",
         "- T-1 opened #12", "- T-1 moved to in_review", "- Review round 1 of 3 sent to Review.",
         "", "T-3 \"Checkout page\" (in_progress)", "- T-3 assigned to Dev 1",
         "- T-3 moved to in_progress", "- [needs you] T-3 edited docs/x.md outside its lane",
         "- T-3 criteria changed", "", "Team", "- Dev 2 joined", "- [needs you] T-4 has no owner",
         "- Budget at 30 of 40 turns", "",
         "Act on the [needs you] lines: plan, assign, hand over, change a lane or raise a "
         "decision. Read a task with team_tasks."});
}

QVariantList transcriptOf(const QJsonObject& message, const QString& text = "Delivered") {
    return ketplus::agent_message::daemonWindowMessages(
        {{"events", QJsonArray{QJsonObject{
                        {"cursor", 1},
                        {"turn_sequence", 1},
                        {"event_type", "user.message"},
                        {"payload", QJsonObject{{"text", text},
                                                {"origin", "agent"},
                                                {"agent_messages", QJsonArray{message}}}}}}}});
}

int countVisible(QQuickItem* item, const QString& name) {
    if (!item->isVisible())
        return 0;
    int count = item->objectName() == name ? 1 : 0;
    for (auto* child : item->childItems())
        count += countVisible(child, name);
    return count;
}

} // namespace

void AgentWorkspaceViewTest::teamMessagesReadAsTheirBlocks() {
    using namespace ketplus::team_blocks;
    const QVariantMap assigned = fromMessage(assignmentMessage());
    QCOMPARE(assigned.value("kind").toString(), QStringLiteral("assignment"));
    QCOMPARE(assigned.value("team").toString(), QStringLiteral("Shop"));
    QCOMPARE(assigned.value("taskId").toString(), QStringLiteral("T-3"));
    QCOMPARE(assigned.value("title").toString(), QStringLiteral("Checkout page"));
    QCOMPARE(assigned.value("facts").toStringList(),
             (QStringList{"Lane Web", "team/shop/t-3", "Assigned by Lead"}));
    QCOMPARE(assigned.value("text").toString(), QStringLiteral("One page from cart to receipt."));
    const QVariantList criteria = assigned.value("criteria").toList();
    QCOMPARE(criteria.size(), 2);
    QVERIFY(criteria.first().toMap().value("done").toBool());
    QVERIFY(!criteria.last().toMap().value("done").toBool());
    QCOMPARE(assigned.value("accessibleName").toString(),
             QStringLiteral("Assignment: T-3 Checkout page"));

    const QVariantMap handed = fromMessage(teamMessage(
        "assignment", {"Team Shop: task T-3 \"Checkout page\" was handed over to you by You. "
                       "Reason: Dev 1 is on T-1",
                       "Branch: team/shop/t-3", "Lane: (none)"}));
    QCOMPARE(handed.value("facts").toStringList(),
             (QStringList{"team/shop/t-3", "Handed over by You"}));
    QCOMPARE(handed.value("text").toString(), QStringLiteral("Reason: Dev 1 is on T-1"));

    // Ten changes are one block, grouped by task, what needs the leader marked.
    const QVariantMap digest = fromMessage(digestMessage());
    QCOMPARE(digest.value("kind").toString(), QStringLiteral("digest"));
    QCOMPARE(digest.value("changes").toInt(), 10);
    QCOMPARE(digest.value("title").toString(), QStringLiteral("10 changes since your last digest"));
    const QVariantList groups = digest.value("groups").toList();
    QCOMPARE(groups.size(), 3);
    QCOMPARE(groups.at(0).toMap().value("state").toString(), QStringLiteral("In review"));
    QCOMPARE(groups.at(2).toMap().value("taskId").toString(), QString{});
    const QVariantList checkout = groups.at(1).toMap().value("lines").toList();
    QCOMPARE(checkout.size(), 4);
    QVERIFY(checkout.at(2).toMap().value("needsYou").toBool());
    QCOMPARE(checkout.at(2).toMap().value("text").toString(),
             QStringLiteral("T-3 edited docs/x.md outside its lane"));

    const QVariantMap request = fromMessage(teamMessage(
        "review", {"Team Shop: review task T-1 \"Cart\" (round 2 of 3).",
                   "Branch: team/shop/t-1, against team/shop", "Worktree: /work/shop-t-1", "",
                   "Acceptance criteria:", "1. The cart total is shown", "",
                   "Changed files: a.cpp, b.cpp", "", "```diff", "+x", "```"}));
    QCOMPARE(request.value("kind").toString(), QStringLiteral("review"));
    QCOMPARE(request.value("facts").toStringList(),
             (QStringList{"Round 2 of 3", "team/shop/t-1, against team/shop"}));
    QCOMPARE(request.value("files").toStringList(), (QStringList{"a.cpp", "b.cpp"}));
    QVERIFY(!request.value("criteria").toList().first().toMap().value("checked").toBool());

    const QVariantMap verdict = fromMessage(teamMessage(
        "review",
        {"Team Shop: the review of T-1 \"Cart\" (round 1 of 3) found issues.", "", "",
         "Verdict: changes requested", "1. The total rounds down.", "",
         "Fix them, commit, and move the task to in_review again with team_task_update."}));
    QCOMPARE(verdict.value("kind").toString(), QStringLiteral("verdict"));
    QCOMPARE(verdict.value("text").toString(),
             QStringLiteral("Verdict: changes requested\n1. The total rounds down."));

    const QVariantMap decision = fromMessage(
        teamMessage("decision", {"Team Shop: the user answered D-1.", "Question: Use Stripe?",
                                 "Answer: Stripe", "Task T-2 can go on."}));
    QCOMPARE(decision.value("taskId").toString(), QStringLiteral("T-2"));
    QCOMPARE(decision.value("question").toString(), QStringLiteral("Use Stripe?"));
    QCOMPARE(decision.value("answer").toString(), QStringLiteral("Stripe"));

    // Anything else stays the message's text.
    QVERIFY(fromMessage(teamMessage("team", {"Team Shop: you joined as a developer."})).isEmpty());
    QVERIFY(
        fromMessage(teamMessage("request", {"Team Shop: task T-1 \"x\" is assigned"})).isEmpty());
    QVERIFY(fromMessage(teamMessage("digest", {"No heading"})).isEmpty());
    QVERIFY(fromAgentMessages(QJsonArray{digestMessage(), assignmentMessage()}).isEmpty());
}

void AgentWorkspaceViewTest::teamBlockOpensItsTask() {
    const QVariantList rows = transcriptOf(digestMessage());
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().toMap().value("subtype").toString(), QStringLiteral("teamMessage"));
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    view.setTeamRoles({{"lead-session", QVariantMap{{"team_id", "7"}, {"team_name", "Shop"}}}});
    view.resetMessages(rows, "team-block");
    auto* root = view.quickView()->rootObject();
    QQuickItem* block = nullptr;
    QTRY_VERIFY((block = findVisibleItem(root, "transcriptTeamBlock")) != nullptr);
    QCOMPARE(countVisible(root, "transcriptTeamBlock"), 1);
    QCOMPARE(countVisible(block, "transcriptTeamDigestGroup"), 3);
    QCOMPARE(findVisualItem(block, "transcriptTeamBlockTitle")->property("text").toString(),
             QStringLiteral("10 changes since your last digest"));
    // One item for a screen reader, named by its kind and title; no bubble under it.
    QCOMPARE(block->property("accessibleName").toString(),
             QStringLiteral("Digest: 10 changes since your last digest"));
    QVERIFY(!findVisibleItem(root, "messageBody"));
    QVERIFY(!findVisibleItem(root, "agentMessageOrigin"));
    QVERIFY(block->height() > 200);

    // The assignment's Open T-3 asks for its task on the team the block names.
    view.resetMessages(transcriptOf(assignmentMessage()), "team-block-assignment");
    QQuickItem* open = nullptr;
    QTRY_VERIFY((open = findVisibleItem(root, "transcriptTeamBlockOpenTask")) != nullptr);
    QSignalSpy requested(&view, &ketplus::AgentWorkspaceView::teamTaskRequested);
    QVERIFY(QMetaObject::invokeMethod(open, "clicked"));
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.first().at(0).toString(), QStringLiteral("7"));
    QCOMPARE(requested.first().at(1).toString(), QStringLiteral("T-3"));
}

// A kind this client does not draw keeps the agent message row and its text.
void AgentWorkspaceViewTest::unknownTeamMessageStaysText() {
    const QVariantList rows =
        transcriptOf(teamMessage("team", {"Team Shop: you joined as a developer."}),
                     "Team Shop: you joined as a developer.");
    QCOMPARE(rows.first().toMap().value("subtype").toString(), QStringLiteral("agentMessage"));
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    view.resetMessages(rows, "team-text");
    auto* root = view.quickView()->rootObject();
    QQuickItem* body = nullptr;
    QTRY_VERIFY((body = findVisibleItem(root, "messageBody")) != nullptr);
    QVERIFY(body->property("text").toString().contains("you joined as a developer"));
    QVERIFY(!findVisibleItem(root, "transcriptTeamBlock"));
}
