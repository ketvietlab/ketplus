// ADR 0067: automatic review of finished turns. Scripted fake providers only.
#include "AgentTestFixture.h"
#include "agents/ReviewIntegrity.h"

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QtTest>

using namespace ketplus::daemon;
using namespace agent_test;

namespace {

bool git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start(QStringLiteral("git"), arguments);
    return process.waitForFinished(10000) && process.exitCode() == 0;
}

bool write(const QString& path, const QByteArray& text) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(text) == text.size();
}

// A committed repository apart from the fixture's logs and journal.
QString repository(Fixture& fixture) {
    const QString path = fixture.directory.filePath("repository");
    if (!QDir().mkpath(path) || !git(path, {"init", "-q"}) ||
        !git(path, {"config", "user.email", "review@example.invalid"}) ||
        !git(path, {"config", "user.name", "Review Test"}) ||
        !write(path + "/README.md", "Base\n") || !git(path, {"add", "README.md"}) ||
        !git(path, {"commit", "-q", "-m", "Base"}))
        return {};
    return path;
}

QJsonObject setReviewer(Fixture& fixture, const QString& working, const QJsonObject& payload,
                        const QString& client = QStringLiteral("desktop-test")) {
    return fixture.host->handleCommand(request("session.reviewer.set", payload, working, 1),
                                       client);
}

QList<AgentMessageRecord> reviewsFrom(const Fixture& fixture, const QString& sessionId) {
    QList<AgentMessageRecord> reviews;
    for (const auto& message : fixture.journal->agentMessagesOf(sessionId, 1, 100))
        if (message.kind == "review" && message.fromSession == sessionId)
            reviews.append(message);
    return reviews;
}

QJsonObject lastNotice(const Fixture& fixture, const QString& sessionId, const QString& state) {
    QJsonObject found;
    for (const auto& notice : fixture.payloads(sessionId, "agent_message.state"))
        if (notice.value("state").toString() == state)
            found = notice;
    return found;
}

} // namespace

class TurnReviewTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void assignmentIsLocalValidatedAndEndsWithEitherConversation() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString working = fixture.start("claude", "working");
        const QString reviewer = fixture.start("claude", "reviewer");
        const QString codex = fixture.start("codex", "codex");
        const QString cursor = fixture.start("cursor", "cursor");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 1, 5000);

        const auto code = [](const QJsonObject& result) {
            return result.value("error_code").toString();
        };
        QCOMPARE(code(setReviewer(fixture, working, {{"reviewer_session_id", working}})),
                 "invalid_reviewer");
        // Only Claude enforces the read-only review guard on every tool call.
        for (const QString& unguarded : {codex, cursor}) {
            QTRY_COMPARE_WITH_TIMEOUT(fixture.count(unguarded, "turn.completed"), 1, 5000);
            QCOMPARE(code(setReviewer(fixture, working, {{"reviewer_session_id", unguarded}})),
                     "invalid_reviewer");
        }
        for (const QString provider : {"codex", "cursor"})
            QCOMPARE(code(setReviewer(fixture, working, {{"new_provider", provider}})),
                     "invalid_reviewer");
        QCOMPARE(code(setReviewer(fixture, working, {{"reviewer_session_id", "missing"}})),
                 "reviewer_not_found");
        QCOMPARE(code(setReviewer(fixture, working,
                                  {{"reviewer_session_id", reviewer}, {"new_provider", "claude"}})),
                 "invalid_reviewer");
        for (const int rounds : {0, 4})
            QCOMPARE(code(setReviewer(fixture, working,
                                      {{"reviewer_session_id", reviewer}, {"max_rounds", rounds}})),
                     "invalid_reviewer");
        // Only the local Desktop chooses a reviewer.
        QCOMPARE(code(setReviewer(fixture, working, {{"reviewer_session_id", reviewer}},
                                  "cloud-control-v2")),
                 "forbidden");
        QVERIFY(!fixture.journal->conversationReviewer(working));

        const QJsonObject set = setReviewer(
            fixture, working,
            {{"reviewer_session_id", reviewer}, {"auto_fix", false}, {"max_rounds", 3}});
        QVERIFY2(set.value("ok").toBool(), qPrintable(text(set)));
        const QJsonObject chosen =
            fixture.host
                ->handleCommand(request("session.reviewer.get", {}, working, 1), "desktop-test")
                .value("reviewer")
                .toObject();
        QCOMPARE(chosen.value("session_id").toString(), reviewer);
        QCOMPARE(chosen.value("number").toString(), fixture.number(reviewer));
        QCOMPARE(chosen.value("title").toString(), "reviewer");
        QCOMPARE(chosen.value("provider").toString(), "claude");
        QCOMPARE(chosen.value("auto_fix").toBool(), false);
        QCOMPARE(chosen.value("max_rounds").toInt(), 3);

        const QJsonObject cleared = setReviewer(fixture, working, {});
        QVERIFY(cleared.value("ok").toBool());
        QVERIFY(cleared.value("reviewer").isNull());
        QVERIFY(setReviewer(fixture, working, {{"reviewer_session_id", reviewer}})
                    .value("ok")
                    .toBool());
        const QJsonObject deleted =
            fixture.host->handleCommand(request("session.delete", {}, reviewer, 1), "desktop-test");
        QVERIFY2(deleted.value("ok").toBool(), qPrintable(text(deleted)));
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.journal->conversationReviewer(working), 5000);
    }

    void newReviewAgentStartsReadOnlyInTheSameWorktree() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString worktree = repository(fixture);
        QVERIFY(!worktree.isEmpty());
        const QString working = fixture.start("claude", "working", {}, worktree);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 1, 5000);
        const QJsonObject set = setReviewer(fixture, working, {{"new_provider", "claude"}});
        QVERIFY2(set.value("ok").toBool(), qPrintable(text(set)));
        const QJsonObject reviewer = set.value("reviewer").toObject();
        const QString reviewerId = reviewer.value("session_id").toString();
        QVERIFY(!reviewerId.isEmpty());
        QCOMPARE(reviewer.value("provider").toString(), "claude");
        QCOMPARE(reviewer.value("title").toString(), "Reviewer for #" + fixture.number(working));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewerId, "turn.completed"), 1, 5000);
        const auto stored = fixture.journal->readSessions();
        QVERIFY(stored);
        bool found = false;
        for (const auto& session : *stored) {
            if (session.id != reviewerId)
                continue;
            found = true;
            QCOMPARE(QFileInfo(session.worktreePath).canonicalFilePath(),
                     QFileInfo(worktree).canonicalFilePath());
            QCOMPARE(session.executionPolicy, "read-only");
        }
        QVERIFY(found);
    }

    void finishedTurnGetsOneReadOnlyReviewAndItsFindingsAreFixed() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString worktree = repository(fixture);
        QVERIFY(!worktree.isEmpty());
        const QString working = fixture.start("claude", "working", {}, worktree);
        const QString reviewer = fixture.start("claude", "reviewer", {}, worktree);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 1, 5000);
        QVERIFY(setReviewer(fixture, working, {{"reviewer_session_id", reviewer}})
                    .value("ok")
                    .toBool());
        // The reviewer is also reviewed by the working conversation: reviews are never reviewed.
        QVERIFY(setReviewer(fixture, reviewer, {{"reviewer_session_id", working}})
                    .value("ok")
                    .toBool());
        QTest::qWait(300); // The baseline snapshot after assignment.
        QVERIFY(write(worktree + "/README.md", "Base\nParser fix\n"));
        QVERIFY(write(worktree + "/notes.txt", "Untracked\n"));

        QVERIFY(!fixture.start("claude", "Fix the parser", working, worktree).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 2, 10000);
        const QJsonObject delivered = fixture.payloads(reviewer, "user.message").last();
        QCOMPARE(delivered.value("origin").toString(), "agent");
        QVERIFY(delivered.value("review_guard").toBool());
        // ADR 0067's turn review keeps the read profile (ADR 0114 §2 has the verify one).
        QCOMPARE(delivered.value("review_profile").toString(), "read");
        const QJsonObject review = delivered.value("agent_messages").toArray().first().toObject();
        QCOMPARE(review.value("kind").toString(), "review");
        QVERIFY(review.value("expect_reply").toBool());
        const QString body = review.value("body").toString();
        QVERIFY2(body.contains("Changed files: README.md, notes.txt (added)"), qPrintable(body));
        QVERIFY(body.contains("+Parser fix"));
        QVERIFY(body.contains("+Untracked")); // untracked contents are part of the diff
        QVERIFY(body.contains("Fix the parser"));
        QVERIFY(body.contains("round 1 of 2"));
        QVERIFY(body.contains("\"Verdict: findings\" or \"Verdict: no findings\""));
        // The review turn ran read-only and could not ask for more.
        const QByteArray log = logText(fixture.claudeLog);
        QVERIFY(log.contains("\"reviewGuard\": true"));
        QVERIFY(log.contains("\"reviewProfile\": \"read\""));
        QVERIFY(!log.contains("scratchDirectory"));
        QVERIFY(log.contains("\"permission\": \"read-only\""));

        // T-19: "finished" has no verdict line. It is recorded as "no verdict" and is not
        // findings: no fix turn starts, and nothing asks for another round.
        QTRY_VERIFY_WITH_TIMEOUT(
            lastNotice(fixture, working, "review_recorded").value("verdict").toString() ==
                "no_verdict",
            10000);
        QTest::qWait(500);
        QCOMPARE(fixture.count(working, "turn.completed"), 2);
        QCOMPARE(reviewsFrom(fixture, working).size(), 1);
        QCOMPARE(reviewsFrom(fixture, reviewer).size(), 0);
        QCOMPARE(lastNotice(fixture, working, "sent").value("kind").toString(), "review");
    }

    void aReviewerThatStopsEnforcingTheGuardGetsNoReview() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString worktree = repository(fixture);
        QVERIFY(!worktree.isEmpty());
        const QString working = fixture.start("claude", "working", {}, worktree);
        const QString reviewer = fixture.start("claude", "reviewer", {}, worktree);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 1, 5000);
        QVERIFY(setReviewer(fixture, working, {{"reviewer_session_id", reviewer}})
                    .value("ok")
                    .toBool());
        // Handed off to Cursor after it was chosen.
        QVERIFY(!fixture.start("cursor", "switch", reviewer, worktree).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 2, 5000);
        QVERIFY(write(worktree + "/README.md", "Unguarded\n"));
        QVERIFY(!fixture.start("claude", "Change it", working, worktree).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(
            lastNotice(fixture, working, "review_skipped").value("reason").toString() ==
                "reviewer_unsupported",
            10000);
        QCOMPARE(reviewsFrom(fixture, working).size(), 0);
        QCOMPARE(fixture.count(reviewer, "turn.completed"), 2);
    }

    // ketplus#295: Codex may review only with Full access, checked when it is chosen and again
    // when a review is requested or delivered.
    void aCodexReviewerNeedsFullAccess() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString worktree = repository(fixture);
        QVERIFY(!worktree.isEmpty());
        const QString working = fixture.start("claude", "working", {}, worktree);
        const QString asking = fixture.start("codex", "asking", {}, worktree);
        const QString full = fixture.start("codex", "full", {}, worktree, "danger-full-access");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(asking, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(full, "turn.completed"), 1, 5000);
        const QJsonObject lowered =
            setReviewer(fixture, working, {{"reviewer_session_id", asking}});
        QCOMPARE(lowered.value("error_code").toString(), "invalid_reviewer");
        QCOMPARE(lowered.value("error_message").toString(),
                 "Codex can review only with Full access.");
        // A new reviewer starts read-only, so it cannot be Codex.
        QCOMPARE(setReviewer(fixture, working, {{"new_provider", "codex"}})
                     .value("error_message")
                     .toString(),
                 "Codex can review only with Full access.");
        const QJsonObject chosen = setReviewer(fixture, working, {{"reviewer_session_id", full}});
        QVERIFY2(chosen.value("ok").toBool(), qPrintable(text(chosen)));
        // Lowered after it was chosen: the next turn gets no review.
        QVERIFY(!fixture.start("codex", "lower", full, worktree).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(full, "turn.completed"), 2, 5000);
        QVERIFY(write(worktree + "/README.md", "Lowered\n"));
        QVERIFY(!fixture.start("claude", "Change it", working, worktree).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(
            lastNotice(fixture, working, "review_skipped").value("reason").toString() ==
                "reviewer_unsupported",
            10000);
        QCOMPARE(fixture.count(full, "turn.completed"), 2);
    }

    // ketplus#295: a Codex review turn runs with the conversation's own Full access, and the
    // daemon compares the worktree before and after it. A change voids the verdict.
    void aCodexReviewIsCheckedAroundTheTurn_data() {
        QTest::addColumn<QString>("action");
        QTest::addColumn<QString>("field");
        QTest::newRow("unchanged") << QString{} << QString{};
        QTest::newRow("new commit") << "git commit -qam tampered" << "head";
        QTest::newRow("dirty worktree") << "echo dirty > dirty.txt" << "status";
        QTest::newRow("switched branch") << "git checkout -q -b elsewhere" << "symbolic_ref";
    }

    void aCodexReviewIsCheckedAroundTheTurn() {
        QFETCH(QString, action);
        QFETCH(QString, field);
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString worktree = repository(fixture);
        QVERIFY(!worktree.isEmpty());
        const QString working = fixture.start("claude", "working", {}, worktree);
        const QString reviewer =
            fixture.start("codex", "reviewer", {}, worktree, "danger-full-access");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 1, 5000);
        QVERIFY(setReviewer(fixture, working, {{"reviewer_session_id", reviewer}})
                    .value("ok")
                    .toBool());
        QTest::qWait(300); // The baseline snapshot after assignment.
        QVERIFY(write(worktree + "/README.md", "Base\nParser fix\n"));
        if (!action.isEmpty())
            QVERIFY(write(fixture.codexLog + ".review", action.toUtf8()));
        QVERIFY(!fixture.start("claude", "Fix the parser", working, worktree).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 2, 10000);
        // Full access, with the review role in the prompt only.
        const QByteArray log = logText(fixture.codexLog);
        QVERIFY(log.contains("review-turn"));
        const qsizetype last = log.lastIndexOf("turn-start:");
        const QByteArray start = log.mid(last, log.indexOf('\n', last) - last);
        QVERIFY2(start.contains("dangerFullAccess") && start.contains("\"never\""),
                 start.constData());
        const QJsonObject delivered = fixture.payloads(reviewer, "user.message").last();
        QVERIFY(delivered.value("review_guard").toBool());
        if (field.isEmpty()) {
            QTRY_VERIFY_WITH_TIMEOUT(
                lastNotice(fixture, working, "review_recorded").value("verdict").toString() ==
                    "no_findings",
                10000);
            QVERIFY(lastNotice(fixture, working, "review_tampered").isEmpty());
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(!lastNotice(fixture, working, "review_tampered").isEmpty(), 10000);
        const QJsonObject notice = lastNotice(fixture, working, "review_tampered");
        bool named = false;
        for (const QJsonValue& change : notice.value("changes").toArray())
            named = named || change.toObject().value("field").toString() == field;
        QVERIFY2(named, qPrintable(text(notice)));
        QVERIFY(!notice.value("summary").toString().isEmpty());
        // Not counted: no verdict was recorded, and nothing was reverted.
        QTest::qWait(300);
        QVERIFY(lastNotice(fixture, working, "review_recorded").isEmpty());
        const auto request = fixture.journal->agentMessage(notice.value("message_id").toString());
        QVERIFY(request);
        QCOMPARE(request->state, "failed");
        QCOMPARE(request->error, "review_tampered");
        // No round used: the next review of this exchange is round 1 again.
        QCOMPARE(fixture.journal->reviewRequestsInChain(request->chainId, working), 0);
        if (field == "status")
            QVERIFY(QFileInfo::exists(worktree + "/dirty.txt"));
    }

    // A worktree the daemon could not read before the review turn cannot be vouched for.
    void anUnreadableWorktreeVoidsTheReview() {
        review_integrity::Snapshot before;
        review_integrity::Snapshot after;
        after.local = true;
        after.head = QStringLiteral("abc");
        const QJsonArray changed = review_integrity::changes(before, after);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.first().toObject().value("field").toString(), "worktree");
        QCOMPARE(changed.first().toObject().value("before").toString(), "unreadable");
        before.local = true;
        before.head = after.head;
        QVERIFY(review_integrity::changes(before, after).isEmpty());
    }

    // The Claude guard is unchanged: a Claude review still runs read-only and is not checked.
    void aClaudeReviewIsNotBranchChecked() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString worktree = repository(fixture);
        QVERIFY(!worktree.isEmpty());
        const QString working = fixture.start("claude", "working", {}, worktree);
        const QString reviewer =
            fixture.start("claude", "reviewer", {}, worktree, "danger-full-access");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 1, 5000);
        QVERIFY(setReviewer(fixture, working, {{"reviewer_session_id", reviewer}})
                    .value("ok")
                    .toBool());
        QTest::qWait(300);
        QVERIFY(write(worktree + "/README.md", "Base\nClaude\n"));
        QVERIFY(!fixture.start("claude", "Fix it", working, worktree).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 2, 10000);
        const QByteArray log = logText(fixture.claudeLog);
        bool readOnly = false;
        for (const QByteArray& line : log.split('\n'))
            if (line.startsWith("setup:") && line.contains("\"reviewGuard\": true"))
                readOnly = line.contains("\"permission\": \"read-only\"");
        QVERIFY2(readOnly, log.constData());
        QVERIFY(!log.contains("KetPlus records the reviewed branch"));
    }

    void aPendingReviewNeverJoinsTheReviewersOwnTurn() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString working = fixture.start("claude", "working");
        const QString reviewer = fixture.start("claude", "reviewer");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 1, 5000);
        AgentMessageRecord held;
        held.id = QStringLiteral("held-review");
        held.chainId = QStringLiteral("chain");
        held.hop = 7;
        held.fromSession = working;
        held.fromGeneration = 1;
        held.fromNumber = fixture.number(working);
        held.toSession = reviewer;
        held.toGeneration = 1;
        held.toNumber = fixture.number(reviewer);
        held.kind = QStringLiteral("review");
        held.expectReply = true;
        held.body = QStringLiteral("Review this held diff");
        held.state = QStringLiteral("held");
        held.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QString error;
        QVERIFY2(fixture.journal->insertAgentMessage(held, &error), qPrintable(error));
        // The user's own turn keeps its mode and does not carry the review.
        QVERIFY(!fixture.start("claude", "plain", reviewer).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 2, 5000);
        const QJsonObject own = fixture.payloads(reviewer, "user.message").last();
        QVERIFY(!own.contains("agent_messages"));
        QVERIFY(!QJsonDocument(own).toJson().contains("held diff"));
        QVERIFY(!logText(fixture.claudeLog).contains("held diff"));
        QCOMPARE(fixture.journal->agentMessage("held-review")->state, "held");
        // Nor can an unguarded turn read it from the inbox and review outside the guard.
        const QJsonObject inbox = fixture.agents(reviewer, "agents.inbox");
        QVERIFY2(inbox.value("ok").toBool(), qPrintable(text(inbox)));
        QVERIFY(!QJsonDocument(inbox).toJson().contains("held diff"));
        const QJsonObject one =
            fixture.agents(reviewer, "agents.inbox", {{"message_id", "held-review"}});
        QVERIFY(one.value("ok").toBool());
        QVERIFY(one.value("message").toObject().value("body").toString().contains(
            "review turn of its own"));
        // The sender still sees what it sent.
        QVERIFY(
            QJsonDocument(fixture.agents(working, "agents.inbox")).toJson().contains("held diff"));
    }

    void aReplySentWithAgentsSendFollowsTheVerdictPolicy() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString worktree = repository(fixture);
        QVERIFY(!worktree.isEmpty());
        const QString working = fixture.start("claude", "working", {}, worktree);
        const QString reviewer = fixture.start("claude", "reviewer", {}, worktree);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 1, 5000);
        QVERIFY(
            setReviewer(fixture, working,
                        {{"reviewer_session_id", reviewer}, {"auto_fix", false}, {"max_rounds", 1}})
                .value("ok")
                .toBool());
        QTest::qWait(300);
        // The diff makes the fake reviewer's turn wait, so it answers with agents_send.
        QVERIFY(write(worktree + "/README.md", "please hang\n"));
        QVERIFY(!fixture.start("claude", "Change it", working, worktree).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.started"), 2, 10000);
        const auto reviews = reviewsFrom(fixture, working);
        QCOMPARE(reviews.size(), 1);
        const QJsonObject sent =
            fixture.agents(reviewer, "agents.send",
                           {{"to", fixture.number(working)},
                            {"reply_to", reviews.first().id},
                            {"text", "README.md:1 is wrong.\nVerdict: findings"}});
        QVERIFY2(sent.value("ok").toBool(), qPrintable(text(sent)));
        QCOMPARE(sent.value("delivery").toString(), "recorded");
        const QJsonObject recorded = lastNotice(fixture, working, "review_recorded");
        QCOMPARE(recorded.value("verdict").toString(), "findings");
        QCOMPARE(fixture.journal->agentMessage(reviews.first().id)->state, "answered");
        QTest::qWait(500);
        QCOMPARE(fixture.count(working, "turn.completed"), 2); // auto_fix is off
        QVERIFY(fixture.host
                    ->handleCommand(request("session.interrupt", {}, reviewer, 1), "desktop-test")
                    .value("ok")
                    .toBool());
    }

    void untrackedEditsFailedSnapshotsAndLargeDiffsAreReviewed() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString worktree = repository(fixture);
        QVERIFY(!worktree.isEmpty());
        QVERIFY(write(worktree + "/draft.txt", "Draft\n")); // untracked before the baseline
        const QString working = fixture.start("claude", "working", {}, worktree);
        const QString reviewer = fixture.start("claude", "reviewer", {}, worktree);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 1, 5000);
        QVERIFY(
            setReviewer(fixture, working,
                        {{"reviewer_session_id", reviewer}, {"auto_fix", false}, {"max_rounds", 3}})
                .value("ok")
                .toBool());
        QTest::qWait(300);
        const auto lastReview = [&] {
            AgentMessageRecord newest;
            for (const auto& review : reviewsFrom(fixture, working))
                if (review.createdAt > newest.createdAt)
                    newest = review;
            return newest.body;
        };

        // An edit to an untracked file that already existed is a change.
        QVERIFY(write(worktree + "/draft.txt", "Draft\nEdited\n"));
        QVERIFY(!fixture.start("claude", "Edit the draft", working, worktree).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(reviewsFrom(fixture, working).size(), 1, 10000);
        QVERIFY2(lastReview().contains("Changed files: draft.txt"), qPrintable(lastReview()));
        QVERIFY(lastReview().contains("+Edited"));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 2, 10000);

        // A snapshot git cannot take is reported, and its changes wait for the next turn.
        QVERIFY(QDir(worktree).rename(".git", "git-away"));
        QVERIFY(write(worktree + "/README.md", "Base\nWhile away\n"));
        QVERIFY(!fixture.start("claude", "Change it", working, worktree).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(
            lastNotice(fixture, working, "review_skipped").value("reason").toString() ==
                "snapshot_failed",
            10000);
        QCOMPARE(reviewsFrom(fixture, working).size(), 1);
        QVERIFY(QDir(worktree).rename("git-away", ".git"));

        // The whole request stays within one message and names what it left out.
        QByteArray large;
        for (int line = 0; line < 2000; ++line)
            large += "Line " + QByteArray::number(line) + " of a generated file\n";
        QVERIFY(write(worktree + "/large.txt", large));
        QVERIFY(!fixture.start("claude", "Generate it", working, worktree).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(reviewsFrom(fixture, working).size(), 2, 10000);
        const QString body = lastReview();
        QVERIFY(body.toUtf8().size() <= AgentBroker::maximumBodyBytes);
        QVERIFY2(body.contains("+While away"), qPrintable(body.left(2000)));
        QVERIFY(body.contains("The diff was shortened to fit one message. Read these files in "
                              "the worktree: large.txt"));
        QVERIFY(body.contains("\"Verdict: findings\" or \"Verdict: no findings\""));
    }

    void noFindingsAndTheRoundLimitStopTheLoop() {
        for (const bool clean : {true, false}) {
            Fixture fixture;
            QVERIFY(fixture.open());
            const QString worktree = repository(fixture);
            QVERIFY(!worktree.isEmpty());
            const QString working = fixture.start("claude", "working", {}, worktree);
            const QString reviewer = fixture.start("claude", "reviewer", {}, worktree);
            QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 1, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 1, 5000);
            QVERIFY(setReviewer(fixture, working,
                                {{"reviewer_session_id", reviewer}, {"max_rounds", clean ? 2 : 1}})
                        .value("ok")
                        .toBool());
            QTest::qWait(300);
            // The fake reviewer answers "Verdict: no findings" or "Verdict: findings" when the
            // diff says so.
            QVERIFY(write(worktree + "/README.md", clean ? "no-findings\n" : "with-findings\n"));
            QVERIFY(!fixture.start("claude", "Change it", working, worktree).isEmpty());
            QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 2, 10000);
            QTRY_VERIFY_WITH_TIMEOUT(!lastNotice(fixture, working, "review_recorded").isEmpty(),
                                     5000);
            const QJsonObject recorded = lastNotice(fixture, working, "review_recorded");
            QCOMPARE(recorded.value("verdict").toString(), clean ? "no_findings" : "findings");
            QCOMPARE(recorded.value("peer_number").toString(), fixture.number(reviewer));
            QVERIFY(!recorded.value("body").toString().isEmpty());
            // Shown without starting another working turn.
            QTest::qWait(500);
            QCOMPARE(fixture.count(working, "turn.completed"), 2);
            QCOMPARE(reviewsFrom(fixture, working).size(), 1);
        }
    }

    void interruptedTurnsAndConversationsWithoutReviewerTriggerNothing() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString worktree = repository(fixture);
        QVERIFY(!worktree.isEmpty());
        const QString working = fixture.start("claude", "working", {}, worktree);
        const QString reviewer = fixture.start("claude", "reviewer", {}, worktree);
        const QString alone = fixture.start("claude", "alone", {}, worktree);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(alone, "turn.completed"), 1, 5000);
        QVERIFY(setReviewer(fixture, working, {{"reviewer_session_id", reviewer}})
                    .value("ok")
                    .toBool());
        QTest::qWait(300);
        QVERIFY(write(worktree + "/README.md", "Interrupted\n"));
        QVERIFY(!fixture.start("claude", "hang", working, worktree).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.started"), 2, 5000);
        const QJsonObject interrupted = fixture.host->handleCommand(
            request("session.interrupt", {}, working, 1), "desktop-test");
        QVERIFY2(interrupted.value("ok").toBool(), qPrintable(text(interrupted)));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 2, 5000);
        QVERIFY(!fixture.start("claude", "Edit alone", alone, worktree).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(alone, "turn.completed"), 2, 5000);
        QTest::qWait(500);
        QCOMPARE(reviewsFrom(fixture, working).size(), 0);
        QCOMPARE(reviewsFrom(fixture, alone).size(), 0);
        QCOMPARE(fixture.count(reviewer, "turn.completed"), 1);
    }

    void clientsCannotAskForTheReviewGuard() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QJsonObject result = fixture.host->handleCommand(
            request("session.start_turn",
                    {{"worktree_path", fixture.directory.path()},
                     {"prompt", "plain"},
                     {"options", QJsonObject{{"provider", "claude"},
                                             {"permission", "danger-full-access"},
                                             {"review_guard", true},
                                             {"review_profile", "verify"}}}}),
            "desktop-test");
        QVERIFY2(result.value("ok").toBool(), qPrintable(text(result)));
        const QString sessionId = result.value("session_id").toString();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sessionId, "turn.completed"), 1, 5000);
        const QByteArray log = logText(fixture.claudeLog);
        QVERIFY(!log.contains("\"reviewGuard\""));
        QVERIFY(!log.contains("reviewProfile"));
        QVERIFY(!fixture.payloads(sessionId, "user.message").last().contains("review_guard"));
        QVERIFY(!fixture.payloads(sessionId, "user.message").last().contains("review_profile"));
    }
};

QTEST_GUILESS_MAIN(TurnReviewTest)
#include "TurnReviewTest.moc"
