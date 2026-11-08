// ADR 0067: automatic review of finished turns. Scripted fake providers only.
#include "AgentTestFixture.h"

#include <QDir>
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
        const QString reviewer = fixture.start("codex", "reviewer");
        const QString cursor = fixture.start("cursor", "cursor");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 1, 5000);

        const auto code = [](const QJsonObject& result) {
            return result.value("error_code").toString();
        };
        QCOMPARE(code(setReviewer(fixture, working, {{"reviewer_session_id", working}})),
                 "invalid_reviewer");
        // Cursor cannot enforce the read-only review guard.
        QCOMPARE(code(setReviewer(fixture, working, {{"reviewer_session_id", cursor}})),
                 "invalid_reviewer");
        QCOMPARE(code(setReviewer(fixture, working, {{"new_provider", "cursor"}})),
                 "invalid_reviewer");
        QCOMPARE(code(setReviewer(fixture, working, {{"reviewer_session_id", "missing"}})),
                 "reviewer_not_found");
        QCOMPARE(code(setReviewer(fixture, working,
                                  {{"reviewer_session_id", reviewer}, {"new_provider", "codex"}})),
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
        QCOMPARE(chosen.value("provider").toString(), "codex");
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
        const QJsonObject set = setReviewer(fixture, working, {{"new_provider", "codex"}});
        QVERIFY2(set.value("ok").toBool(), qPrintable(text(set)));
        const QJsonObject reviewer = set.value("reviewer").toObject();
        const QString reviewerId = reviewer.value("session_id").toString();
        QVERIFY(!reviewerId.isEmpty());
        QCOMPARE(reviewer.value("provider").toString(), "codex");
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
        const QJsonObject review = delivered.value("agent_messages").toArray().first().toObject();
        QCOMPARE(review.value("kind").toString(), "review");
        QVERIFY(review.value("expect_reply").toBool());
        const QString body = review.value("body").toString();
        QVERIFY2(body.contains("Changed files: README.md"), qPrintable(body));
        QVERIFY(body.contains("+Parser fix"));
        QVERIFY(body.contains("New untracked files (read them in the worktree): notes.txt"));
        QVERIFY(body.contains("Fix the parser"));
        QVERIFY(body.contains("round 1 of 2"));
        QVERIFY(body.contains("\"Verdict: findings\" or \"Verdict: no findings\""));
        // The review turn ran read-only and could not ask for more.
        const QByteArray log = logText(fixture.claudeLog);
        QVERIFY(log.contains("\"reviewGuard\": true"));
        QVERIFY(log.contains("\"permission\": \"read-only\""));

        // "finished" has no verdict, so it counts as findings: the working agent fixes them.
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 3, 10000);
        const QJsonObject fix = fixture.payloads(working, "user.message").last();
        QCOMPARE(fix.value("origin").toString(), "agent");
        QCOMPARE(fix.value("agent_messages").toArray().first().toObject().value("kind").toString(),
                 "reply");
        QVERIFY(!fix.value("review_guard").toBool());
        // The fix turn changed nothing, so no second review is asked for.
        QTRY_VERIFY_WITH_TIMEOUT(
            lastNotice(fixture, working, "review_skipped").value("reason").toString() ==
                "no_changes",
            5000);
        QCOMPARE(reviewsFrom(fixture, working).size(), 1);
        QCOMPARE(reviewsFrom(fixture, reviewer).size(), 0);
        QCOMPARE(lastNotice(fixture, working, "sent").value("kind").toString(), "review");
    }

    void codexReviewTurnIsSandboxedWithoutApprovals() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString worktree = repository(fixture);
        QVERIFY(!worktree.isEmpty());
        const QString working = fixture.start("claude", "working", {}, worktree);
        const QString reviewer = fixture.start("codex", "reviewer", {}, worktree);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(working, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 1, 5000);
        // Even a full-access Codex conversation reviews read-only.
        qint64 revision = -1;
        for (const auto& session : fixture.journal->readSessions().value_or(QList<StoredSession>{}))
            if (session.id == reviewer)
                revision = session.configurationRevision;
        const QJsonObject full =
            fixture.host->handleCommand(request("session.execution_policy.apply",
                                                {{"execution_policy", "full"},
                                                 {"risk_acknowledged", true},
                                                 {"expected_revision", revision}},
                                                reviewer, 1),
                                        "desktop-test");
        QVERIFY2(full.value("ok").toBool(), qPrintable(text(full)));
        QVERIFY(
            setReviewer(fixture, working, {{"reviewer_session_id", reviewer}, {"auto_fix", false}})
                .value("ok")
                .toBool());
        QTest::qWait(300);
        QVERIFY(write(worktree + "/README.md", "Codex review\n"));
        QVERIFY(!fixture.start("claude", "Change it", working, worktree).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(reviewer, "turn.completed"), 2, 10000);
        const QList<QByteArray> lines = logText(fixture.codexLog).split('\n');
        QByteArray last;
        for (const QByteArray& line : lines)
            if (line.startsWith("turn-start:"))
                last = line;
        QVERIFY2(last.contains("\"approvalPolicy\": \"never\""), last.constData());
        QVERIFY2(last.contains("\"type\": \"readOnly\""), last.constData());
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
            // The fake reviewer answers "Verdict: no findings" when the diff says so.
            QVERIFY(write(worktree + "/README.md", clean ? "no-findings\n" : "Needs work\n"));
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
                                             {"review_guard", true}}}}),
            "desktop-test");
        QVERIFY2(result.value("ok").toBool(), qPrintable(text(result)));
        const QString sessionId = result.value("session_id").toString();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sessionId, "turn.completed"), 1, 5000);
        const QByteArray log = logText(fixture.claudeLog);
        QVERIFY(!log.contains("\"reviewGuard\""));
        QVERIFY(!fixture.payloads(sessionId, "user.message").last().contains("review_guard"));
    }
};

QTEST_GUILESS_MAIN(TurnReviewTest)
#include "TurnReviewTest.moc"
