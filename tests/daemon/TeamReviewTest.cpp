// ADR 0114 §2: the team's reviewer reviews each task on the engine of ADR 0067. The coordinator
// cases run on a real journal with a fake runtime; the end-to-end case uses a temporary Git
// repository and the scripted fake providers. No real provider is started.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"

#include <QDir>
#include <QProcess>
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
        QVERIFY(!team().reviewingMessage.isEmpty());
        teams_->reviewFinished("rev", "Fine.\nVerdict: no findings", true, {}, "another");
        QCOMPARE(task(first).state, state::inReview);
        QCOMPARE(team().reviewing, first);
        teams_->reviewFinished("rev", "Fine.\nVerdict: no findings", true, {},
                               team().reviewingMessage);
        QCOMPARE(task(first).state, state::ready);
        QCOMPARE(reviewsTo("rev").size(), 2);
        QCOMPARE(reviewsTo("rev").last().taskId, second);
        teams_->reviewFinished("rev", "Fine.\nVerdict: no findings", true);
        QCOMPARE(reviewsTo("rev").last().taskId, third_);
        teams_->reviewFinished("rev", "Fine.\nVerdict: no findings", true);
        QCOMPARE(reviewsTo("rev").size(), 3);
        for (const QString& id : {first, second, third_})
            QCOMPARE(task(id).state, state::ready);
        QVERIFY(team().reviewing.isEmpty());
    }

    void findingsGoBackToTheOwnerUntilTheRoundLimit() {
        const QString id = inReview("Cart", "dev1");
        teams_->reviewFinished("rev", "web/cart.txt:3 high: the total is wrong.\nVerdict: findings",
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
        teams_->reviewFinished("rev", "Still wrong.", false);
        QCOMPARE(task(id).state, state::blocked);
        QVERIFY(task(id).question.contains("round limit"));
        QCOMPARE(reviewsTo("dev1").size(), 1);
        bool limit = false;
        for (const auto& update : journal_->teamUpdates(team_, true, 100))
            limit = limit || (update.kind == "round_limit" && update.actionable);
        QVERIFY(limit);
    }

    void aReplyWithoutAVerdictCountsAsFindingsWithANote() {
        const QString id = inReview("Cart", "dev1");
        teams_->reviewFinished("rev", "Looks mostly fine.", false);
        QCOMPARE(task(id).state, state::inProgress);
        QVERIFY(reviewsTo("dev1").first().body.contains("no verdict line"));
    }

    void aReviewThatDoesNotFinishBlocksTheTask() {
        const QString id = inReview("Cart", "dev1");
        teams_->reviewFinished("rev", {}, false, "turn_interrupted");
        QCOMPARE(task(id).state, state::blocked);
        QVERIFY(task(id).question.contains("turn_interrupted"));
        QVERIFY(team().reviewing.isEmpty());
    }

    void onlyTheReviewerFinishesAReview() {
        const QString id = inReview("Cart", "dev1");
        teams_->reviewFinished("dev1", "Verdict: no findings", true);
        QCOMPARE(task(id).state, state::inReview);
        QCOMPARE(team().reviewing, id);
    }

    void aTaskThatLeftReviewWhileQueuedIsSkipped() {
        const QString first = inReview("One", "dev1");
        const QString second = inReview("Two", "dev2");
        QVERIFY(teams_->moveTask(team_, second, state::todo, "lead").ok());
        teams_->reviewFinished("rev", "Verdict: no findings", true);
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
        // The fake reviewer answers "Verdict: no findings" to a prompt naming no-findings,
        // and without a verdict line, which counts as findings, to any other.
        QTest::newRow("no findings") << "Cart no-findings" << "ready";
        QTest::newRow("findings") << "Cart" << "in_progress";
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
