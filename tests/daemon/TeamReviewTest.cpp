// ADR 0114 §2: the team's reviewer reviews each task on the engine of ADR 0067. The coordinator
// cases run on a real journal with a fake runtime; the end-to-end case uses a temporary Git
// repository and the scripted fake providers. No real provider is started.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTest>

using namespace ketplus::daemon;
using namespace team_test;

namespace {

void git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", arguments);
    QVERIFY(process.waitForFinished(15000));
    QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
}

} // namespace

class TeamReviewTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    // A task of `owner`, handed to review.
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

    // A second connection to the journal, as another process would have.
    bool sql(const QString& statement) {
        bool ok = false;
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", "review-sql");
            database.setDatabaseName(directory_.filePath("journal.sqlite3"));
            ok = database.open() && QSqlQuery(database).exec(statement);
            database.close();
        }
        QSqlDatabase::removeDatabase("review-sql");
        return ok;
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
        for (const QString& id : {"lead", "dev1", "dev2", "dev3"})
            host_.add(id);
        host_.add("rev", "claude");
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(60 * 60 * 1000);
        teams_->load();
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
    }

    void aReviewHeldWhileTheTeamIsPausedIsBoundBeforeItIsSent() {
        QVERIFY(teams_->update(team_, {{"paused", true}}, "user").ok());
        const QString id = inReview("Cart", "dev1");
        QVERIFY(!id.isEmpty());
        // Nothing was sent, yet the review already knows which request answers it.
        QVERIFY(reviewsTo("rev").isEmpty());
        QCOMPARE(team().reviewing, id);
        const QString bound = team().reviewingMessage;
        QVERIFY(!bound.isEmpty());
        teams_->reviewFinished("rev", "old-request", "Verdict: no findings", true);
        QCOMPARE(task(id).state, state::inReview);
        QCOMPARE(team().reviewing, id);
        // Resumed, the held request goes out under that id and answers the review.
        QVERIFY(teams_->update(team_, {{"paused", false}}, "user").ok());
        QCOMPARE(reviewsTo("rev").size(), 1);
        QCOMPARE(reviewsTo("rev").first().id, bound);
        teams_->reviewFinished("rev", "old-request", "Verdict: no findings", true);
        QCOMPARE(task(id).state, state::inReview);
        teams_->reviewFinished("rev", bound, "Verdict: no findings", true);
        QCOMPARE(task(id).state, state::ready);
    }

    void theBoundRequestSurvivesAReload() {
        const QString id = inReview("Cart", "dev1");
        const QString bound = team().reviewingMessage;
        QCOMPARE(reviewsTo("rev").first().id, bound);
        reload();
        QCOMPARE(team().reviewing, id);
        QCOMPARE(team().reviewingMessage, bound);
        teams_->reviewFinished("rev", "old-request", "Verdict: no findings", true);
        QCOMPARE(task(id).state, state::inReview);
        teams_->reviewFinished("rev", bound, "Verdict: no findings", true);
        QCOMPARE(task(id).state, state::ready);
    }

    void aReviewThatCannotBeStoredWithItsRequestDoesNotStart() {
        // The journal refuses a team document that binds a review to a request.
        QVERIFY(sql("CREATE TRIGGER refuse_bound BEFORE UPDATE ON teams WHEN NEW.document LIKE "
                    "'%\"reviewing_message\":\"%' AND NEW.document NOT LIKE "
                    "'%\"reviewing_message\":\"\"%' BEGIN SELECT RAISE(ABORT, 'not now'); END"));
        const QString id = inReview("Cart", "dev1");
        QVERIFY(!id.isEmpty());
        // The task waits in the queue: no review in flight, nothing sent, nothing to answer.
        QVERIFY(reviewsTo("rev").isEmpty());
        QVERIFY(team().reviewing.isEmpty());
        QVERIFY(team().reviewingMessage.isEmpty());
        QCOMPARE(team().reviewQueue, QStringList{id});
        teams_->reviewFinished("rev", "old-request", "Verdict: no findings", true);
        QCOMPARE(task(id).state, state::inReview);
        // The same after a restart: the stored team has no unbound review either.
        reload();
        QVERIFY(team().reviewing.isEmpty());
        teams_->reviewFinished("rev", "old-request", "Verdict: no findings", true);
        QCOMPARE(task(id).state, state::inReview);
        // Once the journal takes it, the next change of the team starts the review, bound.
        QVERIFY(sql("DROP TRIGGER refuse_bound"));
        QVERIFY(teams_->update(team_, {{"goal", "Ship the cart"}}, "user").ok());
        QCOMPARE(team().reviewing, id);
        QCOMPARE(reviewsTo("rev").size(), 1);
        QCOMPARE(reviewsTo("rev").first().id, team().reviewingMessage);
        teams_->reviewFinished("rev", "old-request", "Verdict: no findings", true);
        QCOMPARE(task(id).state, state::inReview);
        finishReview(*teams_, "rev", "Verdict: no findings", true);
        QCOMPARE(task(id).state, state::ready);
    }

    void aReviewInFlightInAnOlderJournalIsStillAnswered() {
        // Before reviews were bound, the team stored only the task in review.
        const QString id = inReview("Cart", "dev1");
        teams_.reset();
        journal_.reset();
        QVERIFY(sql("UPDATE teams SET document = json_remove(document, '$.reviewing_message')"));
        reload();
        QCOMPARE(team().reviewing, id);
        QVERIFY(team().reviewingMessage.isEmpty());
        teams_->reviewFinished("rev", "its-request", "Verdict: no findings", true);
        QCOMPARE(task(id).state, state::ready);
        // The next review of that team is bound again.
        const QString next = inReview("Api", "dev2");
        QCOMPARE(team().reviewing, next);
        QVERIFY(!team().reviewingMessage.isEmpty());
    }

    void inReviewSendsOneRequestWithTheCriteriaAndThatTaskOnly() {
        const QString id = inReview("Cart", "dev1");
        QVERIFY(!id.isEmpty());
        QCOMPARE(reviewsTo("rev").size(), 1);
        const TeamMessage request = reviewsTo("rev").first();
        QVERIFY(request.expectReply);
        QCOMPARE(request.taskId, id);
        QVERIFY(request.body.contains("review task T-1 \"Cart\" (round 1 of 2)"));
        QVERIFY(request.body.contains("1. Adds an item"));
        QVERIFY(request.body.contains("web/T-1.txt"));
        QVERIFY(request.body.contains("Verdict: no findings"));
        QCOMPARE(team().reviewing, id);
        QCOMPARE(task(id).round, 1);
    }

    void tasksHandedInAtOnceAreReviewedOneAfterAnother() {
        Member third{.sessionId = "dev3",
                     .role = role::developer,
                     .name = "Dev 3",
                     .lane = "Ui",
                     .paths = {"ui"}};
        QVERIFY(teams_->addMember(team_, third, "user").ok());
        const QString first = inReview("One", "dev1");
        const QString second = inReview("Two", "dev2");
        const QString third_ = inReview("Three", "dev3");
        QCOMPARE(reviewsTo("rev").size(), 1);
        QCOMPARE(team().reviewQueue, (QStringList{second, third_}));
        // A request other than the one in flight that ends is not its answer.
        QCOMPARE(reviewsTo("rev").first().id, team().reviewingMessage);
        QVERIFY(!team().reviewingMessage.isEmpty());
        teams_->reviewFinished("rev", "another", "Fine.\nVerdict: no findings", true);
        QCOMPARE(task(first).state, state::inReview);
        QCOMPARE(team().reviewing, first);
        finishReview(*teams_, "rev", "Fine.\nVerdict: no findings", true);
        QCOMPARE(task(first).state, state::ready);
        QCOMPARE(reviewsTo("rev").size(), 2);
        QCOMPARE(reviewsTo("rev").last().taskId, second);
        finishReview(*teams_, "rev", "Fine.\nVerdict: no findings", true);
        QCOMPARE(reviewsTo("rev").last().taskId, third_);
        finishReview(*teams_, "rev", "Fine.\nVerdict: no findings", true);
        QCOMPARE(reviewsTo("rev").size(), 3);
        for (const QString& id : {first, second, third_})
            QCOMPARE(task(id).state, state::ready);
        QVERIFY(team().reviewing.isEmpty());
    }

    void findingsGoBackToTheOwnerUntilTheRoundLimit() {
        const QString id = inReview("Cart", "dev1");
        finishReview(*teams_, "rev", "web/cart.txt:3 high: the total is wrong.\nVerdict: findings",
                     false);
        QCOMPARE(task(id).state, state::inProgress);
        QCOMPARE(reviewsTo("dev1").size(), 1);
        QVERIFY(reviewsTo("dev1").first().body.contains("the total is wrong"));
        QVERIFY(!reviewsTo("dev1").first().body.contains("no verdict line"));
        QCOMPARE(task(id).history.last().kind, QStringLiteral("state"));
        bool verdict = false;
        for (const HistoryEntry& entry : task(id).history)
            verdict = verdict || (entry.kind == "verdict" &&
                                  entry.data.value("text").toString().contains("total is wrong"));
        QVERIFY(verdict);
        // Round two, findings again: the limit of 2 blocks the task for the leader.
        QVERIFY(teams_->moveTask(team_, id, state::inReview, "dev1").ok());
        QCOMPARE(reviewsTo("rev").size(), 2);
        QVERIFY(reviewsTo("rev").last().body.contains("(round 2 of 2)"));
        finishReview(*teams_, "rev", "Still wrong.\nVerdict: findings", false);
        QCOMPARE(task(id).state, state::blocked);
        QVERIFY(task(id).question.contains("round limit"));
        QCOMPARE(reviewsTo("dev1").size(), 1);
        bool limit = false;
        for (const auto& update : journal_->teamUpdates(team_, true, 100))
            limit = limit || (update.kind == "round_limit" && update.actionable);
        QVERIFY(limit);
    }

    void aReviewThatDoesNotFinishBlocksTheTask() {
        const QString id = inReview("Cart", "dev1");
        finishReview(*teams_, "rev", {}, false, "turn_interrupted");
        QCOMPARE(task(id).state, state::blocked);
        QVERIFY(task(id).question.contains("turn_interrupted"));
        QVERIFY(team().reviewing.isEmpty());
    }

    void onlyTheReviewerFinishesAReview() {
        const QString id = inReview("Cart", "dev1");
        finishReview(*teams_, "dev1", "Verdict: no findings", true);
        QCOMPARE(task(id).state, state::inReview);
        QCOMPARE(team().reviewing, id);
    }

    // The session runtime reads the diff off its event loop. Nothing of the team changes until
    // the diff comes, so a restart meanwhile leaves the task queued and asks again.
    void aReviewStartsWhenItsDiffComesAndARestartMeanwhileLosesNothing() {
        host_.holdAnswers = true;
        const QString id = inReview("Cart", "dev1");
        QVERIFY(!id.isEmpty());
        QVERIFY(teams_->checksOpen());
        QVERIFY(team().reviewing.isEmpty());
        QCOMPARE(team().reviewQueue, QStringList{id});
        QVERIFY(reviewsTo("rev").isEmpty());
        reload();
        QVERIFY(team().reviewing.isEmpty());
        QCOMPARE(team().reviewQueue, QStringList{id});
        // The answer to the coordinator that is gone is dropped; the new one's starts it.
        host_.answer();
        QCOMPARE(team().reviewing, id);
        QCOMPARE(reviewsTo("rev").size(), 1);
        QCOMPARE(reviewsTo("rev").first().id, team().reviewingMessage);
        QVERIFY(reviewsTo("rev").first().body.contains("web/" + id + ".txt"));
        QVERIFY(!teams_->checksOpen());
    }

    // A diff is of one task against one integration branch. When another task is next by the
    // time it comes, or the branch changed, it is dropped and the right one is read.
    void aDiffForATaskThatIsNoLongerNextIsDroppedAndReadAgain() {
        host_.holdAnswers = true;
        const QString first = inReview("One", "dev1");
        const QString second = inReview("Two", "dev2");
        const int asked = host_.asked;
        QVERIFY(teams_->moveTask(team_, first, state::todo, "lead").ok());
        host_.answer();
        QVERIFY(team().reviewing.isEmpty());
        QCOMPARE(host_.asked, asked + 1);
        QVERIFY(teams_->update(team_, {{"integration_branch", "release-other"}}, "user").ok());
        host_.answer();
        QVERIFY(team().reviewing.isEmpty());
        QCOMPARE(host_.asked, asked + 2);
        host_.answer();
        QCOMPARE(team().reviewing, second);
        QCOMPARE(reviewsTo("rev").size(), 1);
        QCOMPARE(reviewsTo("rev").first().taskId, second);
        QVERIFY(team().reviewQueue.isEmpty());
    }

    void aTaskThatLeftReviewWhileQueuedIsSkipped() {
        const QString first = inReview("One", "dev1");
        const QString second = inReview("Two", "dev2");
        QVERIFY(teams_->moveTask(team_, second, state::todo, "lead").ok());
        finishReview(*teams_, "rev", "Verdict: no findings", true);
        QCOMPARE(task(first).state, state::ready);
        QCOMPARE(reviewsTo("rev").size(), 1);
        QVERIFY(team().reviewQueue.isEmpty());
    }

    void aDeletedReviewerLeavesItsReviewForTheNextOne() {
        const QString id = inReview("Cart", "dev1");
        teams_->conversationDeleted("rev");
        QVERIFY(team().reviewing.isEmpty());
        QCOMPARE(team().reviewQueue, QStringList{id});
        host_.add("rev2", "claude");
        QVERIFY(teams_
                    ->addMember(team_,
                                {.sessionId = "rev2", .role = role::reviewer, .name = "Reviewer 2"},
                                "user")
                    .ok());
        QCOMPARE(reviewsTo("rev2").size(), 1);
        QCOMPARE(reviewsTo("rev2").first().taskId, id);
    }

    void theReviewerReviewsATaskEndToEnd_data() {
        QTest::addColumn<QString>("title");
        QTest::addColumn<QString>("verdict");
        // The fake reviewer answers "Verdict: no findings" to a prompt naming no-findings and
        // "Verdict: findings" to one naming with-findings. Without a verdict line (T-19) the
        // reviewer is asked once more, then the task waits for the leader.
        QTest::newRow("no findings") << "Cart no-findings" << "ready";
        QTest::newRow("findings") << "Cart with-findings" << "in_progress";
        QTest::newRow("no verdict") << "Cart" << "blocked";
    }

    void theReviewerReviewsATaskEndToEnd() {
        QFETCH(QString, title);
        QFETCH(QString, verdict);
        agent_test::Fixture fixture;
        QVERIFY(fixture.open(true));
        const QString repository = fixture.directory.filePath("shop");
        QVERIFY(QDir().mkpath(repository));
        git(repository, {"init", "-q", "-b", "integration"});
        git(repository, {"-c", "user.email=t@example.com", "-c", "user.name=T", "commit", "-q",
                         "--allow-empty", "-m", "start"});
        const auto command = [&](const QString& type, const QJsonObject& payload) {
            return fixture.host->handleCommand(agent_test::request(type, payload), "desktop-test");
        };
        const QJsonObject created = command(
            "team.create",
            {{"repository", repository},
             {"name", "Shop"},
             {"integration_branch", "integration"},
             {"plan", QJsonObject{{"leader", QJsonObject{{"provider", "codex"}}},
                                  {"developers", QJsonObject{{"count", 1}, {"provider", "codex"}}},
                                  {"reviewer", QJsonObject{{"provider", "claude"}}}}}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(agent_test::text(created)));
        const QJsonArray members = created.value("team").toObject().value("members").toArray();
        const QString teamId = created.value("team").toObject().value("id").toString();
        const QString developer = members.at(1).toObject().value("session_id").toString();
        const QString reviewer = members.at(2).toObject().value("session_id").toString();
        QVERIFY(command("team.task.create", {{"team_id", teamId},
                                             {"title", title},
                                             {"criteria", QJsonArray{"Adds an item"}}})
                    .value("ok")
                    .toBool());
        const QJsonObject assigned = command(
            "team.task.assign", {{"team_id", teamId}, {"task_id", "T-1"}, {"owner", developer}});
        QVERIFY2(assigned.value("ok").toBool(), qPrintable(agent_test::text(assigned)));
        const QString worktree = assigned.value("task").toObject().value("worktree").toString();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.count(developer, "turn.completed") >= 1, 8000);
        QVERIFY(fixture
                    .agents(developer, "agents.team_task_update",
                            {{"task_id", "T-1"}, {"state", "in_progress"}})
                    .value("ok")
                    .toBool());
        QVERIFY(QDir(worktree).mkpath("web"));
        QFile file(QDir(worktree).filePath("web/cart.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("cart\n");
        file.close();
        git(worktree, {"add", "web/cart.txt"});
        git(worktree,
            {"-c", "user.email=t@example.com", "-c", "user.name=T", "commit", "-q", "-m", "Cart"});
        const QJsonObject handed = fixture.agents(developer, "agents.team_task_update",
                                                  {{"task_id", "T-1"}, {"state", "in_review"}});
        QVERIFY2(handed.value("ok").toBool(), qPrintable(agent_test::text(handed)));
        const auto state = [&] {
            return command("team.task.get", {{"team_id", teamId}, {"task_id", "T-1"}})
                .value("task")
                .toObject()
                .value("state")
                .toString();
        };
        const int turnsBefore = fixture.count(developer, "turn.completed");
        QTRY_COMPARE_WITH_TIMEOUT(state(), verdict, 10000);
        const auto requests = fixture.journal->agentMessagesOf(reviewer, 1, 10);
        QVERIFY(!requests.isEmpty());
        QCOMPARE(requests.first().kind, QStringLiteral("review"));
        QVERIFY(requests.first().body.contains("web/cart.txt"));
        QVERIFY(requests.first().body.contains("+cart"));
        QVERIFY(agent_test::logText(fixture.claudeLog).contains("Adds an item"));
        // ADR 0114 §2: a team's review round verifies, with a scratch directory of its own.
        QJsonObject round;
        for (const QJsonObject& message : fixture.payloads(reviewer, "user.message"))
            if (message.value("review_guard").toBool())
                round = message;
        QVERIFY(round.value("review_guard").toBool());
        QCOMPARE(round.value("review_profile").toString(), QStringLiteral("verify"));
        const QByteArray log = agent_test::logText(fixture.claudeLog);
        QVERIFY(log.contains("\"reviewProfile\": \"verify\""));
        const QString scratch =
            QFileInfo(QFileInfo(fixture.journal->databasePath())
                          .dir()
                          .filePath(QStringLiteral("review-scratch/") + reviewer))
                .canonicalFilePath();
        QVERIFY2(!scratch.isEmpty() && QFileInfo(scratch).isDir(), qPrintable(scratch));
        QVERIFY2(log.contains(("\"scratchDirectory\": \"" + scratch + "\"").toUtf8()),
                 log.constData());
        if (verdict == QStringLiteral("blocked")) {
            // Asked twice, never as findings: the owner received nothing of the review.
            QCOMPARE(fixture.journal->agentMessagesOf(reviewer, 1, 10).size(), 2);
            for (const auto& message : fixture.journal->agentMessagesOf(developer, 1, 10))
                QVERIFY(message.kind != QStringLiteral("review"));
        }
        if (verdict == QStringLiteral("in_progress")) {
            // The findings start an ordinary turn of the owner, whatever its provider: they
            // are not a review request, so they are neither refused nor read-only.
            QTRY_VERIFY_WITH_TIMEOUT(fixture.count(developer, "turn.completed") > turnsBefore,
                                     8000);
            const auto received = fixture.journal->agentMessagesOf(developer, 1, 10);
            QVERIFY(!received.isEmpty());
            QCOMPARE(received.first().kind, QStringLiteral("review"));
            QCOMPARE(received.first().state, QStringLiteral("delivered"));
            QVERIFY(received.first().body.contains("found issues"));
        }
    }
};

QTEST_GUILESS_MAIN(TeamReviewTest)
#include "TeamReviewTest.moc"
