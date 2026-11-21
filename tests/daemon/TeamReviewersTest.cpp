// T-23: a team has one or two reviewers. Each task in review goes to exactly one of them, two
// tasks are reviewed at the same time, a task's next round goes to the reviewer of its last
// one, and a reviewer that leaves hands its reviews over. The coordinator runs on a real
// journal with a fake runtime; no provider is started.
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

class TeamReviewersTest final : public QObject {
    Q_OBJECT

    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    QString created(const QString& title, const QString& owner) {
        QString id;
        if (!teams_
                 ->createTask(team_, {.title = title, .lane = "Web", .criteria = {"Adds an item"}},
                              "lead", &id)
                 .ok() ||
            !teams_->assignTask(team_, id, owner, "lead").ok() ||
            !teams_->moveTask(team_, id, state::inProgress, owner).ok())
            return {};
        return id;
    }
    QString inReview(const QString& title, const QString& owner) {
        const QString id = created(title, owner);
        if (id.isEmpty() || !teams_->moveTask(team_, id, state::inReview, owner).ok())
            return {};
        return id;
    }
    bool addReviewer(const QString& session) {
        return teams_
            ->addMember(team_, {.sessionId = session, .role = role::reviewer, .name = session},
                        "user")
            .ok();
    }
    const Team& team() { return *teams_->team(team_); }
    const Task& task(const QString& id) { return *team().task(id); }
    ReviewSlot slot(const QString& reviewer) {
        const ReviewSlot* found = team().reviewSlot(reviewer);
        return found ? *found : ReviewSlot{};
    }
    QList<TeamMessage> reviewsTo(const QString& session) {
        QList<TeamMessage> result;
        for (const TeamMessage& message : host_.of("review"))
            if (message.toSession == session)
                result.append(message);
        return result;
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
        for (const QString& id : {"rev", "rev2", "rev3"})
            host_.add(id, "claude");
        reload();
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
    }

    // Criterion 1: one or two, refused in words past that and below it.
    void aTeamHasOneOrTwoReviewers() {
        Team three = standardTeam();
        three.name = "Three";
        for (const QString& id : {"rev2", "rev3"})
            three.members.append({.sessionId = id, .role = role::reviewer, .name = id});
        for (Member& member : three.members)
            member.sessionId = member.sessionId + "-x";
        const auto refused = teams_->create(three, "user");
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error->code, QStringLiteral("role_taken"));
        QCOMPARE(refused.error->message, QStringLiteral("A team has at most two reviewers."));

        const auto last = teams_->removeMember(team_, "rev", "user");
        QCOMPARE(last.error.value_or(TeamError{}).code, QStringLiteral("last_reviewer"));
        QVERIFY(last.error->message.contains("at least one reviewer"));
        QVERIFY(addReviewer("rev2"));
        QCOMPARE(team().count(role::reviewer), 2);
        QCOMPARE(team().reviews.size(), 2);
        const auto third = teams_->addMember(
            team_, {.sessionId = "rev3", .role = role::reviewer, .name = "rev3"}, "user");
        QCOMPARE(third.error.value_or(TeamError{}).code, QStringLiteral("role_taken"));
        QVERIFY(third.error->message.contains("two reviewers"));
        QVERIFY(teams_->removeMember(team_, "rev", "user").ok());
        QCOMPARE(team().count(role::reviewer), 1);
        QCOMPARE(team().reviews.size(), 1);
        QCOMPARE(team().reviews.first().reviewer, QStringLiteral("rev2"));
        QVERIFY(!team().paused);
    }

    // Criterion 2: the idle one first, else the shorter queue; two reviews at the same time.
    void twoReviewersReviewTwoTasksAtOnce() {
        QVERIFY(addReviewer("rev2"));
        const QString one = inReview("One", "dev1");
        const QString two = inReview("Two", "dev2");
        QCOMPARE(slot("rev").reviewing, one);
        QCOMPARE(slot("rev2").reviewing, two);
        QCOMPARE(reviewsTo("rev").size(), 1);
        QCOMPARE(reviewsTo("rev2").size(), 1);
        QCOMPARE(reviewsTo("rev2").first().taskId, two);
        QCOMPARE(task(one).reviewer, QStringLiteral("rev"));
        QCOMPARE(task(two).reviewer, QStringLiteral("rev2"));
        // Both busy with one each: the first to join takes the tie, then the other.
        const QString three = inReview("Three", "dev1");
        const QString four = inReview("Four", "dev2");
        QCOMPARE(slot("rev").queue, QStringList{three});
        QCOMPARE(slot("rev2").queue, QStringList{four});
        // Each answer frees only its reviewer.
        finishReview(*teams_, "rev2", "Fine.\nVerdict: no findings", true);
        QCOMPARE(task(two).state, state::ready);
        QCOMPARE(slot("rev2").reviewing, four);
        QCOMPARE(slot("rev").reviewing, one);
        // The state survives a restart.
        reload();
        QCOMPARE(slot("rev").reviewing, one);
        QCOMPARE(slot("rev").queue, QStringList{three});
        QCOMPARE(slot("rev2").reviewing, four);
        finishReview(*teams_, "rev", "Fine.\nVerdict: no findings", true);
        QCOMPARE(task(one).state, state::ready);
        QCOMPARE(slot("rev").reviewing, three);
    }

    // Criterion 3: the next round goes to the last round's reviewer, even when the other idles.
    void theNextRoundGoesToTheLastRoundsReviewer() {
        QVERIFY(addReviewer("rev2"));
        const QString one = inReview("One", "dev1");
        const QString two = inReview("Two", "dev2");
        finishReview(*teams_, "rev2", "api/x.txt:1 high: wrong.\nVerdict: findings", false);
        QCOMPARE(task(two).state, state::inProgress);
        const QString three = inReview("Three", "dev1");
        QCOMPARE(slot("rev2").reviewing, three);
        finishReview(*teams_, "rev", "Fine.\nVerdict: no findings", true);
        QVERIFY(slot("rev").reviewing.isEmpty());
        QVERIFY(teams_->moveTask(team_, two, state::inReview, "dev2").ok());
        QCOMPARE(slot("rev2").queue, QStringList{two});
        QVERIFY(slot("rev").reviewing.isEmpty());
        QVERIFY(slot("rev").queue.isEmpty());
        // Once that reviewer is gone, the other one takes it.
        QVERIFY(teams_->removeMember(team_, "rev2", "user").ok());
        QCOMPARE(slot("rev").reviewing, three);
        QCOMPARE(slot("rev").queue, QStringList{two});
    }

    // Criterion 4: the review in flight is sent again first and not counted, the queue
    // follows, and the late answer of the reviewer that left changes nothing.
    void aRemovedReviewerHandsItsReviewsOver() {
        QVERIFY(addReviewer("rev2"));
        const QString one = inReview("One", "dev1");
        const QString two = inReview("Two", "dev2");
        const QString three = inReview("Three", "dev1");
        QCOMPARE(slot("rev").reviewing, one);
        QCOMPARE(slot("rev").queue, QStringList{three});
        const QString late = slot("rev").message;
        QCOMPARE(task(one).round, 1);
        QVERIFY(teams_->removeMember(team_, "rev", "user").ok());
        QCOMPARE(task(one).round, 0);
        QCOMPARE(task(one).state, state::inReview);
        QCOMPARE(slot("rev2").reviewing, two);
        QCOMPARE(slot("rev2").queue, (QStringList{one, three}));
        teams_->reviewFinished("rev", late, "Verdict: no findings", true);
        QCOMPARE(task(one).state, state::inReview);
        // No task in review is left without a reviewer.
        for (const Task& entry : team().tasks)
            if (entry.state == state::inReview)
                QVERIFY(team().reviewSlotOf(entry.id));
        finishReview(*teams_, "rev2", "Fine.\nVerdict: no findings", true);
        QCOMPARE(slot("rev2").reviewing, one);
        QVERIFY(reviewsTo("rev2").last().body.contains("(round 1 of 2)"));
        QCOMPARE(task(one).round, 1);
    }

    // A deleted reviewer of two leaves without pausing the team.
    void aDeletedReviewerOfTwoDoesNotPauseTheTeam() {
        QVERIFY(addReviewer("rev2"));
        const QString one = inReview("One", "dev1");
        teams_->conversationDeleted("rev");
        QVERIFY(!team().paused);
        QCOMPARE(slot("rev2").reviewing, one);
        QCOMPARE(task(one).round, 1);
        QCOMPARE(reviewsTo("rev2").size(), 1);
    }

    // Criterion 5: "reviewer" names the reviewer of the sender's work, or no one.
    void theReviewerOfAMemberIsTheOneOfItsTasks() {
        QCOMPARE(TeamCoordinator::reviewerFor(team(), "dev1"), QStringLiteral("rev"));
        QVERIFY(addReviewer("rev2"));
        QVERIFY(TeamCoordinator::reviewerFor(team(), "dev1").isEmpty());
        inReview("One", "dev1");
        inReview("Two", "dev2");
        QCOMPARE(TeamCoordinator::reviewerFor(team(), "dev1"), QStringLiteral("rev"));
        QCOMPARE(TeamCoordinator::reviewerFor(team(), "dev2"), QStringLiteral("rev2"));
        // The leader owns no task: not clear.
        QVERIFY(TeamCoordinator::reviewerFor(team(), "lead").isEmpty());
        // A developer with work at both: not clear.
        inReview("Three", "dev1");
        finishReview(*teams_, "rev2", "Fine.\nVerdict: no findings", true);
        QCOMPARE(slot("rev2").reviewing, QString{});
        inReview("Four", "dev1");
        QCOMPARE(slot("rev2").reviewing, QStringLiteral("T-4"));
        QVERIFY(TeamCoordinator::reviewerFor(team(), "dev1").isEmpty());
    }
};

QTEST_GUILESS_MAIN(TeamReviewersTest)
#include "TeamReviewersTest.moc"
