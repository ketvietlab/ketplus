// T-19: a review reply without a verdict line is not findings, and the leader can void a round
// or accept a review made outside the board. The coordinator runs on a real journal with a
// fake runtime; no provider is started.
#include "TeamFakeHost.h"
#include "storage/LocalJournal.h"
#include "teams/TeamCoordinator.h"

#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace ketplus::daemon;
using namespace ketplus::daemon::teams;
using namespace team_test;

class TeamVerdictTest final : public QObject {
    Q_OBJECT

    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    QString inReview(const QString& title, const QString& owner) {
        QString id;
        if (!teams_
                 ->createTask(team_, {.title = title, .lane = "Web", .criteria = {"Adds an item"}},
                              "lead", &id)
                 .ok() ||
            !teams_->assignTask(team_, id, owner, "lead").ok() ||
            !teams_->moveTask(team_, id, state::inProgress, owner).ok() ||
            !teams_->moveTask(team_, id, state::inReview, owner).ok())
            return {};
        return id;
    }
    const Team& team() { return *teams_->team(team_); }
    const Task& task(const QString& id) { return *team().task(id); }
    QList<TeamMessage> reviewsTo(const QString& session) {
        QList<TeamMessage> result;
        for (const TeamMessage& message : host_.of("review"))
            if (message.toSession == session)
                result.append(message);
        return result;
    }
    int historyOf(const QString& id, const QString& kind) {
        int count = 0;
        for (const HistoryEntry& entry : task(id).history)
            count += entry.kind == kind ? 1 : 0;
        return count;
    }
    // A task blocked by the round limit: two rounds of findings.
    QString blockedByTheLimit() {
        const QString id = inReview("Cart", "dev1");
        finishReview(*teams_, "rev", "web/cart.txt:3 high: wrong.\nVerdict: findings", false);
        if (!teams_->moveTask(team_, id, state::inReview, "dev1").ok())
            return {};
        finishReview(*teams_, "rev", "Still wrong.\nVerdict: findings", false);
        return task(id).state == state::blocked ? id : QString{};
    }
    void reload() {
        teams_.reset();
        journal_.reset();
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(60 * 60 * 1000);
        teams_->load();
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        teams_.reset();
        journal_.reset();
        QFile::remove(directory_.filePath("journal.sqlite3"));
        host_ = {};
        for (const QString& id : {"lead", "dev1", "dev2"})
            host_.add(id);
        host_.add("rev", "claude");
        reload();
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
    }

    // Criteria 1 and 2: twice no verdict (a reply from plan mode, then a pause notice). The
    // round is not counted, nothing goes to the owner, the reviewer is asked once more, then
    // the leader is told once.
    void aReplyWithoutAVerdictIsAskedAgainThenGoesToTheLeader() {
        const QString id = inReview("Cart", "dev1");
        QCOMPARE(task(id).round, 1);
        finishReview(*teams_, "rev", "I am in plan mode and can only read. Here is my plan.",
                     false);
        QCOMPARE(task(id).state, state::inReview);
        QCOMPARE(task(id).round, 1); // Asked again as the same round.
        QCOMPARE(task(id).missingVerdicts, 1);
        QCOMPARE(reviewsTo("dev1").size(), 0);
        QCOMPARE(reviewsTo("rev").size(), 2);
        QVERIFY(reviewsTo("rev").last().body.contains("(round 1 of 2)"));
        QVERIFY(reviewsTo("rev").last().body.contains("had no verdict line"));
        QCOMPARE(firstReviews(team()).reviewing, id);
        bool askedAgain = false;
        for (const HistoryEntry& entry : task(id).history)
            askedAgain = askedAgain || entry.text.contains("sent to Reviewer again");
        QVERIFY(askedAgain);

        // The state survives a restart.
        reload();
        QCOMPARE(task(id).missingVerdicts, 1);
        finishReview(*teams_, "rev", "Paused: the disk is full. I will continue later.", false);
        QCOMPARE(task(id).state, state::blocked);
        QCOMPARE(task(id).round, 0);
        QVERIFY(task(id).question.contains("has no verdict after 2 replies"));
        QCOMPARE(reviewsTo("dev1").size(), 0);
        QCOMPARE(reviewsTo("rev").size(), 2);
        QVERIFY(firstReviews(team()).reviewing.isEmpty());
        QCOMPARE(historyOf(id, "verdict"), 2);
        for (const HistoryEntry& entry : task(id).history)
            if (entry.kind == "verdict")
                QCOMPARE(entry.data.value("verdict").toString(), QStringLiteral("no_verdict"));
        // One line for the leader, and no round limit.
        int needsLeader = 0;
        for (const auto& update : journal_->teamUpdates(team_, true, 100)) {
            QVERIFY(update.kind != "round_limit");
            needsLeader += update.actionable && update.text.contains("no verdict") ? 1 : 0;
        }
        QCOMPARE(needsLeader, 1);

        // A verdict later clears the count.
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inReview, "dev1").ok());
        QCOMPARE(task(id).round, 1);
        finishReview(*teams_, "rev", "Fine.\nVerdict: no findings", true);
        QCOMPARE(task(id).state, state::ready);
        QCOMPARE(task(id).missingVerdicts, 0);
    }

    // Criteria 3 and 5: voiding the second round of findings leaves the round limit without a
    // commit; the task is reviewed again as round 2.
    void theLeaderVoidsARoundBlockedByTheLimit() {
        const QString id = blockedByTheLimit();
        QVERIFY(!id.isEmpty());
        QCOMPARE(task(id).round, 2);
        const QString head = task(id).headCommit;
        for (const QString& actor : {"dev1", "rev", "user"})
            QCOMPARE(teams_->voidReview(team_, id, "Wrong round", actor)
                         .error.value_or(TeamError{})
                         .code,
                     QStringLiteral("role_forbidden"));
        QCOMPARE(teams_->voidReview(team_, id, " ", "lead").error.value_or(TeamError{}).code,
                 QStringLiteral("invalid_payload"));
        QVERIFY(teams_->voidReview(team_, id, "The reviewer read an old diff", "lead").ok());
        QCOMPARE(task(id).state, state::inReview);
        QCOMPARE(task(id).round, 2); // 2 - 1, then counted again by the new request.
        QVERIFY(task(id).question.isEmpty());
        QCOMPARE(task(id).headCommit, head);
        QCOMPARE(reviewsTo("rev").size(), 3);
        QVERIFY(reviewsTo("rev").last().body.contains("(round 2 of 2)"));
        const HistoryEntry* voided = nullptr;
        for (const HistoryEntry& entry : task(id).history)
            if (entry.kind == "verdict_voided")
                voided = &entry;
        QVERIFY(voided);
        QVERIFY(voided->text.contains("Round 2's verdict (findings) voided by Lead"));
        QVERIFY(voided->text.contains("The reviewer read an old diff"));
        QCOMPARE(voided->data.value("round").toInt(), 2);
        QCOMPARE(historyOf(id, "verdict"), 2); // The voided verdict stays in the history.
        // A round in flight has no verdict to void.
        QCOMPARE(teams_->voidReview(team_, id, "Again", "lead").error.value_or(TeamError{}).code,
                 QStringLiteral("invalid_transition"));
    }

    void aNoFindingsVerdictIsVoidedFromReady() {
        const QString id = inReview("Cart", "dev1");
        finishReview(*teams_, "rev", "Fine.\nVerdict: no findings", true);
        QCOMPARE(task(id).state, state::ready);
        QVERIFY(teams_->voidReview(team_, id, "It reviewed another task", "lead").ok());
        QCOMPARE(task(id).state, state::inReview);
        QCOMPARE(task(id).round, 1);
        QCOMPARE(reviewsTo("rev").size(), 2);
        for (const Decision& decision : team().decisions)
            if (decision.kind == "merge" && decision.taskId == id)
                QVERIFY(decision.state != "open");
    }

    // Criteria 4 and 5: a review on the pull request is accepted.
    void theLeaderAcceptsAReviewMadeOutsideTheBoard() {
        const QString id = blockedByTheLimit();
        QVERIFY(!id.isEmpty());
        for (const QString& actor : {"dev1", "rev", "user"})
            QCOMPARE(teams_
                         ->acceptReview(team_, id, "https://example.invalid/pr/1#review",
                                        "Reviewed there", actor)
                         .error.value_or(TeamError{})
                         .code,
                     QStringLiteral("role_forbidden"));
        QCOMPARE(teams_->acceptReview(team_, id, "", "Reviewed there", "lead")
                     .error.value_or(TeamError{})
                     .code,
                 QStringLiteral("invalid_payload"));
        QVERIFY(teams_
                    ->acceptReview(team_, id, "https://example.invalid/pr/1#review",
                                   "The pull request review approved the fix", "lead")
                    .ok());
        QCOMPARE(task(id).state, state::ready);
        QVERIFY(task(id).question.isEmpty());
        QCOMPARE(historyOf(id, "review_accepted"), 1);
        QString accepted;
        for (const HistoryEntry& entry : task(id).history)
            if (entry.kind == "review_accepted")
                accepted = entry.text;
        QVERIFY2(accepted.contains("Review accepted by Lead, made outside the board "
                                   "(https://example.invalid/pr/1#review)"),
                 qPrintable(accepted));
        // Only once: a ready task has nothing to accept.
        QCOMPARE(teams_->acceptReview(team_, id, "x", "y", "lead").error.value_or(TeamError{}).code,
                 QStringLiteral("invalid_transition"));
    }

    void acceptingAReviewInFlightDropsItsAnswer() {
        const QString first = inReview("One", "dev1");
        const QString second = inReview("Two", "dev2");
        QCOMPARE(firstReviews(team()).reviewing, first);
        const QString request = firstReviews(team()).message;
        QVERIFY(teams_->acceptReview(team_, first, "Reviewed on the pull request", "Done", "lead")
                    .ok());
        QCOMPARE(task(first).state, state::ready);
        // The next task's review starts, and the late answer does not touch the first.
        QCOMPARE(firstReviews(team()).reviewing, second);
        teams_->reviewFinished("rev", request, "Verdict: findings", false);
        QCOMPARE(task(first).state, state::ready);
        QCOMPARE(task(second).state, state::inReview);
    }

    // A task not blocked by its review is refused.
    void otherBlockedTasksAreNotAccepted() {
        QString id;
        QVERIFY(teams_->createTask(team_, {.title = "Api", .lane = "Web"}, "lead", &id).ok());
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", "Which API?").ok());
        QCOMPARE(teams_->acceptReview(team_, id, "x", "y", "lead").error.value_or(TeamError{}).code,
                 QStringLiteral("invalid_transition"));
        QCOMPARE(teams_->voidReview(team_, id, "x", "lead").error.value_or(TeamError{}).code,
                 QStringLiteral("invalid_transition"));
    }
};

QTEST_GUILESS_MAIN(TeamVerdictTest)
#include "TeamVerdictTest.moc"
