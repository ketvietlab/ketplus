// T-40: a team's review rounds (one or two) and where its reviews happen (the board or the
// task's pull request). Coordinator cases on a real journal with a fake runtime; the comment
// probe runs a scripted stand-in for gh. No real provider or remote is reached.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "runtime/TeamPullRequests.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTest>

using namespace ketplus::daemon;
using namespace team_test;

namespace {

const QString pullRequest = QStringLiteral("https://example.invalid/shop/pull/7");

} // namespace

class TeamReviewPlaceTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    // A task of dev1, with a pull request when `url` is set, handed to review.
    QString inReview(const QString& url = {}, const QString& owner = QStringLiteral("dev1")) {
        QString id;
        if (!teams_
                 ->createTask(team_, {.title = "Cart", .lane = "Web", .criteria = {"Adds an item"}},
                              "lead", &id)
                 .ok() ||
            !teams_->assignTask(team_, id, owner, "lead").ok() ||
            !teams_->moveTask(team_, id, state::inProgress, owner).ok())
            return {};
        if (!url.isEmpty() && !teams_->updateTask(team_, id, {{"pull_request", url}}, "lead").ok())
            return {};
        if (!teams_->moveTask(team_, id, state::inReview, owner).ok())
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
    const HistoryEntry* last(const QString& id, const QString& kind) {
        const QList<HistoryEntry>& history = task(id).history;
        for (auto it = history.crbegin(); it != history.crend(); ++it)
            if (it->kind == kind)
                return &*it;
        return nullptr;
    }
    void again(const QString& id) {
        QVERIFY(teams_->moveTask(team_, id, state::inReview, "dev1").ok());
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
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(60 * 60 * 1000);
        teams_->load();
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
    }

    void aTeamReviewsInTwoRoundsOnTheBoardByDefault() {
        QCOMPARE(team().maxRounds, 2);
        QCOMPARE(team().reviewPlace, place::board);
        const QJsonObject json = toJson(team());
        QCOMPARE(json.value("max_rounds").toInt(), 2);
        QCOMPARE(json.value("review_place").toString(), QStringLiteral("board"));
    }

    void otherRoundsAndPlacesAreRefusedInWords() {
        for (const QJsonValue& rounds :
             {QJsonValue(0), QJsonValue(6), QJsonValue(1.5), QJsonValue(QStringLiteral("2"))}) {
            const TeamResult refused = teams_->update(team_, {{"max_rounds", rounds}}, "user");
            QVERIFY(!refused.ok());
            QCOMPARE(refused.error->code, QStringLiteral("invalid_payload"));
            QVERIFY(refused.error->message.contains("one to five passes"));
        }
        const TeamResult place = teams_->update(team_, {{"review_place", "github"}}, "user");
        QVERIFY(!place.ok());
        QVERIFY(place.error->message.contains("on the board"));
        QCOMPARE(team().maxRounds, 2);
        QCOMPARE(team().reviewPlace, place::board);
        // A new team is checked the same way.
        Team draft = standardTeam();
        draft.name = "Other";
        draft.maxRounds = 6;
        QVERIFY(!teams_->create(draft, "user").ok());
        draft.maxRounds = 1;
        draft.reviewPlace = "github";
        QVERIFY(!teams_->create(draft, "user").ok());
    }

    void aChangeIsAnUpdateOfTheTeam() {
        QVERIFY(teams_->update(team_, {{"max_rounds", 1}, {"review_place", "pull_request"}}, "user")
                    .ok());
        QCOMPARE(team().maxRounds, 1);
        QCOMPARE(team().reviewPlace, place::pullRequest);
        const QJsonObject json = toJson(team());
        QCOMPARE(teamFromJson(json).maxRounds, 1);
        QCOMPARE(teamFromJson(json).reviewPlace, place::pullRequest);
    }

    // T-53: Max review passes per PR is one to five, on create and in the settings.
    void aTeamTakesUpToFivePasses() {
        Team draft = standardTeam();
        draft.name = "Five";
        draft.maxRounds = 5;
        // Its own conversations: one conversation is on one team at a time.
        for (auto& member : draft.members) {
            member.sessionId += QStringLiteral("-five");
            host_.add(member.sessionId, member.role == role::reviewer ? QStringLiteral("claude")
                                                                      : QStringLiteral("codex"));
        }
        const TeamResult created = teams_->create(draft, "user");
        QVERIFY2(created.ok(), qPrintable(created.ok() ? QString() : created.error->message));
        QCOMPARE(teams_->team(created.teamId)->maxRounds, 5);
        QVERIFY(teams_->update(team_, {{"max_rounds", 4}}, "user").ok());
        QCOMPARE(team().maxRounds, 4);
        QCOMPARE(teamFromJson(toJson(team())).maxRounds, 4);
        bool logged = false;
        for (const auto& update : journal_->teamUpdates(team_, false, 100))
            logged = logged || (update.kind == "review_rounds" &&
                                update.text == QStringLiteral("Max review passes: 4"));
        QVERIFY(logged);
        // With four, findings of the third pass still go back to the owner.
        const QString id = inReview();
        for (int pass = 1; pass <= 3; ++pass) {
            QVERIFY(
                reviewsTo("rev").last().body.contains(QStringLiteral("(round %1 of 4)").arg(pass)));
            finishReview(*teams_, "rev", "Verdict: findings", false);
            QCOMPARE(task(id).state, state::inProgress);
            again(id);
        }
        finishReview(*teams_, "rev", "Verdict: findings", false);
        QCOMPARE(task(id).state, state::blocked);
    }

    void aTeamSavedWithSixRoundsKeepsTwo() {
        QJsonObject json = toJson(team());
        json.insert("max_rounds", 6);
        json.insert("review_place", "somewhere");
        const Team read = teamFromJson(json);
        QCOMPARE(read.maxRounds, 2);
        QCOMPARE(read.reviewPlace, place::board);
    }

    void withOneRoundTheFirstFindingsGoToTheLeader() {
        QVERIFY(teams_->update(team_, {{"max_rounds", 1}}, "user").ok());
        const QString id = inReview();
        QVERIFY(reviewsTo("rev").first().body.contains("(round 1 of 1)"));
        finishReview(*teams_, "rev", "web/cart.ts:4 high: no total.\nVerdict: findings", false);
        QCOMPARE(task(id).state, state::blocked);
        QCOMPARE(task(id).question,
                 QStringLiteral("Review round limit reached (1 of 1) with findings."));
        // Exactly as the second round does with two: no message back to the owner.
        QVERIFY(reviewsTo("dev1").isEmpty());
        bool limit = false;
        for (const auto& update : journal_->teamUpdates(team_, true, 100))
            limit = limit || (update.kind == "round_limit" && update.actionable);
        QVERIFY(limit);
    }

    void aChangeAppliesToTasksThatEnterReviewAfterwards() {
        const QString id = inReview();
        QCOMPARE(task(id).roundLimit, 2);
        QVERIFY(teams_->update(team_, {{"max_rounds", 1}}, "user").ok());
        // The round in flight keeps its two rounds: findings go back to the owner.
        finishReview(*teams_, "rev", "Verdict: findings", false);
        QCOMPARE(task(id).state, state::inProgress);
        QCOMPARE(reviewsTo("dev1").size(), 1);
        QVERIFY(reviewsTo("dev1").first().body.contains("(round 1 of 2)"));
        // T-53: its next review request is past the team's one pass, so it goes to the
        // leader instead of to review. Nothing is blocked retroactively before that.
        const int sent = reviewsTo("rev").size();
        again(id);
        QCOMPARE(task(id).state, state::blocked);
        QCOMPARE(task(id).roundLimit, 1);
        QCOMPARE(reviewsTo("rev").size(), sent);
        QVERIFY(task(id).question.startsWith(
            QStringLiteral("Review round limit reached (1 of 1) with findings.")));
        QVERIFY(task(id).question.contains("Max review passes is now 1"));
        bool limit = false;
        for (const auto& update : journal_->teamUpdates(team_, true, 100))
            limit =
                limit || (update.kind == "round_limit" && update.actionable && update.taskId == id);
        QVERIFY(limit);
        // The leader can still hand it to review once more: it gets that pass.
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        again(id);
        QCOMPARE(task(id).state, state::inReview);
        QVERIFY(reviewsTo("rev").last().body.contains("(round 2 of 2)"));
        finishReview(*teams_, "rev", "Verdict: findings", false);
        QCOMPARE(task(id).state, state::blocked);
        QCOMPARE(task(id).question,
                 QStringLiteral("Review round limit reached (2 of 2) with findings."));
    }

    // T-53 round 1 (L1): a limit lowered while the task waits behind another review applies
    // when the reviewer takes it, not one pass later.
    void aLimitLoweredWhileQueuedBlocksWhenTheReviewerTakesIt() {
        QVERIFY(teams_->update(team_, {{"max_rounds", 3}}, "user").ok());
        const QString waiting = inReview();
        for (int pass = 1; pass <= 2; ++pass) {
            finishReview(*teams_, "rev", "Verdict: findings", false);
            QCOMPARE(task(waiting).state, state::inProgress);
            if (pass == 1)
                again(waiting);
        }
        // The reviewer is busy with another task when it asks for its third pass.
        const QString other = inReview({}, QStringLiteral("dev2"));
        QVERIFY(!other.isEmpty());
        QCOMPARE(task(other).round, 1);
        again(waiting);
        QCOMPARE(task(waiting).state, state::inReview);
        QCOMPARE(task(waiting).round, 2);
        QVERIFY(teams_->update(team_, {{"max_rounds", 2}}, "user").ok());
        const int sent = reviewsTo("rev").size();
        finishReview(*teams_, "rev", "Verdict: findings", false);
        QCOMPARE(task(other).state, state::inProgress);
        // Taken from the queue: past the new limit, so it goes to the leader.
        QTRY_COMPARE(task(waiting).state, state::blocked);
        QCOMPARE(task(waiting).round, 2);
        QCOMPARE(task(waiting).roundLimit, 2);
        QCOMPARE(reviewsTo("rev").size(), sent);
        QVERIFY(task(waiting).question.contains("Max review passes is now 2"));
        QCOMPARE(team().reviewSlot("rev")->reviewing, QString());
    }

    // A raised limit gives the task in progress its new passes at the next request.
    void aRaisedLimitAppliesFromTheNextRequest() {
        QVERIFY(teams_->update(team_, {{"max_rounds", 1}}, "user").ok());
        const QString id = inReview();
        QVERIFY(teams_->update(team_, {{"max_rounds", 3}}, "user").ok());
        finishReview(*teams_, "rev", "Verdict: findings", false);
        QCOMPARE(task(id).state, state::blocked);
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        again(id);
        QCOMPARE(task(id).state, state::inReview);
        QVERIFY(reviewsTo("rev").last().body.contains("(round 2 of 3)"));
        finishReview(*teams_, "rev", "Verdict: findings", false);
        QCOMPARE(task(id).state, state::inProgress);
    }

    void onThePullRequestTheBoardKeepsTheVerdictAndTheLink() {
        QVERIFY(teams_->update(team_, {{"review_place", "pull_request"}}, "user").ok());
        const QString id = inReview(pullRequest);
        QCOMPARE(task(id).reviewPlace, place::pullRequest);
        const QString asked = reviewsTo("rev").first().body;
        QVERIFY(asked.contains("posted as a review comment on " + pullRequest));
        QVERIFY(asked.contains("Do not post it yourself"));
        QVERIFY(asked.contains("write it in English"));
        QVERIFY(asked.contains("name no folder of this computer and no person"));
        const QString findings = "web/cart.ts:4 high: no total.\nVerdict: findings";
        finishReview(*teams_, "rev", findings, false);
        // The daemon posted the reviewer's words; the reviewer stayed read-only.
        QCOMPARE(host_.comments.size(), 1);
        QCOMPARE(host_.comments.first().first, pullRequest);
        QVERIFY(host_.comments.first().second.contains("web/cart.ts:4 high: no total."));
        QVERIFY(host_.comments.first().second.contains("round 1 of 2"));
        const QString link = pullRequest + "#issuecomment-1";
        const HistoryEntry* verdict = last(id, "verdict");
        QVERIFY(verdict);
        QCOMPARE(verdict->data.value("verdict").toString(), QStringLiteral("findings"));
        QCOMPARE(verdict->data.value("comment_url").toString(), link);
        QCOMPARE(verdict->data.value("place").toString(), QStringLiteral("pull_request"));
        QVERIFY(!verdict->data.contains("text"));
        QVERIFY(verdict->text.contains(link));
        // Findings return the task to its owner, with the link.
        QCOMPARE(task(id).state, state::inProgress);
        QCOMPARE(reviewsTo("dev1").size(), 1);
        QVERIFY(reviewsTo("dev1").first().body.contains(link));
        // The second round's findings reach the limit: the leader decides, with the link.
        again(id);
        finishReview(*teams_, "rev", findings, false);
        const QString second = pullRequest + "#issuecomment-2";
        QCOMPARE(task(id).state, state::blocked);
        QVERIFY(task(id).question.startsWith(
            QStringLiteral("Review round limit reached (2 of 2) with findings.")));
        QVERIFY(task(id).question.contains(second));
        // The leader's acceptance names the comment.
        QVERIFY(teams_
                    ->acceptReview(team_, id, pullRequest + "#pullrequestreview-9",
                                   "Checked by hand", "lead")
                    .ok());
        const HistoryEntry* accepted = last(id, "review_accepted");
        QVERIFY(accepted);
        QCOMPARE(accepted->data.value("comment_url").toString(), second);
        QVERIFY(accepted->text.contains(second));
        QCOMPARE(task(id).state, state::ready);
    }

    void theCommentNamesNoFolderOfThisComputerAndNoMember() {
        QVERIFY(teams_->update(team_, {{"review_place", "pull_request"}}, "user").ok());
        const QString id = inReview(pullRequest);
        const QString worktree = task(id).worktree;
        QVERIFY(!worktree.isEmpty());
        finishReview(*teams_, "rev",
                     worktree + "/web/cart.ts:4 high: no total.\n" + team().repository +
                         "/api/cart.ts:9 low.\nSee " + QDir::homePath() +
                         "/notes.txt\nVerdict: findings",
                     false);
        QCOMPARE(host_.comments.size(), 1);
        const QString body = host_.comments.first().second;
        QVERIFY(body.contains("<task worktree>/web/cart.ts:4 high: no total."));
        QVERIFY(body.contains("<repository>/api/cart.ts:9 low."));
        QVERIFY(body.contains("See ~/notes.txt"));
        QVERIFY(!body.contains(worktree));
        QVERIFY(!body.contains(QDir::homePath()));
        // Neither the reviewer's name nor the task's title, which the user may have written
        // in another language.
        QVERIFY(!body.contains("Reviewer"));
        QVERIFY(!body.contains("Cart"));
        QVERIFY(body.startsWith("**Team review of " + id + ", round 1 of 2**"));
    }

    void noFindingsOnThePullRequestMakeTheTaskReady() {
        QVERIFY(teams_->update(team_, {{"review_place", "pull_request"}}, "user").ok());
        const QString id = inReview(pullRequest);
        finishReview(*teams_, "rev", "All criteria met.\nVerdict: no findings", true);
        QCOMPARE(host_.comments.size(), 1);
        QCOMPARE(task(id).state, state::ready);
        QCOMPARE(last(id, "verdict")->data.value("comment_url").toString(),
                 pullRequest + "#issuecomment-1");
    }

    void theReviewStaysBoundWhileItsCommentIsPosted() {
        QVERIFY(teams_->update(team_, {{"review_place", "pull_request"}}, "user").ok());
        const QString id = inReview(pullRequest);
        const QString bound = firstReviews(team()).message;
        host_.holdAnswers = true;
        teams_->reviewFinished("rev", bound, "Verdict: findings", false);
        QCOMPARE(task(id).state, state::inReview);
        QCOMPARE(firstReviews(team()).reviewing, id);
        // The same answer again posts nothing more.
        teams_->reviewFinished("rev", bound, "Verdict: findings", false);
        QCOMPARE(host_.comments.size(), 1);
        host_.holdAnswers = false;
        QCOMPARE(host_.held.size(), 1);
        host_.held.takeFirst()();
        QCOMPARE(task(id).state, state::inProgress);
        QVERIFY(firstReviews(team()).reviewing.isEmpty());
    }

    void aTaskWithoutAPullRequestIsReviewedOnTheBoardAndTheReviewerIsTold() {
        QVERIFY(teams_->update(team_, {{"review_place", "pull_request"}}, "user").ok());
        const QString id = inReview();
        QCOMPARE(task(id).reviewPlace, place::board);
        QVERIFY(reviewsTo("rev").first().body.contains(
            "this task has no pull request recorded, so this round is reviewed on the board"));
        QVERIFY(last(id, "review")->text.contains("the task has no pull request"));
        finishReview(*teams_, "rev", "web/cart.ts:4 high.\nVerdict: findings", false);
        QVERIFY(host_.comments.isEmpty());
        QCOMPARE(last(id, "verdict")->data.value("place").toString(), QStringLiteral("board"));
        QVERIFY(last(id, "verdict")->data.value("text").toString().contains("web/cart.ts:4"));
    }

    void aCommentThatCannotBePostedLeavesTheFindingsOnTheBoard() {
        QVERIFY(teams_->update(team_, {{"review_place", "pull_request"}}, "user").ok());
        host_.commentError = "gh: not signed in";
        const QString id = inReview(pullRequest);
        finishReview(*teams_, "rev", "web/cart.ts:4 high.\nVerdict: findings", false);
        const HistoryEntry* verdict = last(id, "verdict");
        QVERIFY(verdict->text.contains("could not be posted on the pull request (gh: not "
                                       "signed in)"));
        QVERIFY(verdict->data.value("text").toString().contains("web/cart.ts:4"));
        QCOMPARE(verdict->data.value("comment_error").toString(),
                 QStringLiteral("gh: not signed in"));
        QCOMPARE(task(id).state, state::inProgress);
        QVERIFY(reviewsTo("dev1").first().body.contains("web/cart.ts:4"));
    }

    void aReplyWithoutAVerdictIsNotPosted() {
        QVERIFY(teams_->update(team_, {{"review_place", "pull_request"}}, "user").ok());
        const QString id = inReview(pullRequest);
        finishReview(*teams_, "rev", "I looked at it.", false);
        QVERIFY(host_.comments.isEmpty());
        QCOMPARE(task(id).state, state::inReview);
    }

    void theCommentIsPostedThroughGhAndItsLinkRead() {
#ifdef Q_OS_WIN
        QSKIP("The stand-in for gh is a shell script.");
#endif
        QTemporaryDir bin;
        const QString script = bin.filePath("gh");
        const QString received = bin.filePath("body.txt");
        QFile file(script);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QStringLiteral("#!/bin/sh\necho \"$@\" > '%1.args'\ncat > '%1'\n"
                                  "echo https://example.invalid/shop/pull/7#issuecomment-42\n")
                       .arg(received)
                       .toUtf8());
        file.close();
        QVERIFY(file.setPermissions(file.permissions() | QFileDevice::ExeOwner));
        const teams::ReviewComment posted = team_pull_requests::comment(
            bin.path(), pullRequest, "Findings\nVerdict: findings", script, 10000);
        QCOMPARE(posted.error, QString{});
        QCOMPARE(posted.url, QStringLiteral("https://example.invalid/shop/pull/7#issuecomment-42"));
        QFile body(received);
        QVERIFY(body.open(QIODevice::ReadOnly));
        QCOMPARE(body.readAll(), QByteArray("Findings\nVerdict: findings"));
        QFile args(received + ".args");
        QVERIFY(args.open(QIODevice::ReadOnly));
        QCOMPARE(args.readAll().trimmed(),
                 QByteArray("pr comment https://example.invalid/shop/pull/7 --body-file -"));
        QCOMPARE(team_pull_requests::commentUrl("Posted.\n"), QString{});
    }
};

QTEST_GUILESS_MAIN(TeamReviewPlaceTest)
#include "TeamReviewPlaceTest.moc"
