// ADR 0114 §3 and open points 2 and 5: decisions, the first plan and the budget's answer. The
// coordinator cases use a fake runtime; the protocol case the scripted fake providers. No
// real provider is started.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTest>

using namespace ketplus::daemon;
using namespace team_test;

class TeamDecisionTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    void start(const bool planApproved = true, const int budget = defaultTurnBudget) {
        Team draft = standardTeam();
        draft.planApproved = planApproved;
        draft.turnBudget = budget;
        team_ = teams_->create(draft, "user").teamId;
        QVERIFY(team_ > 0);
    }
    QString task(const QString& title) {
        QString id;
        (void)teams_->createTask(team_, {.title = title, .lane = "Web"}, "lead", &id);
        return id;
    }
    const Team& team() { return *teams_->team(team_); }
    const Decision& decision(const QString& id) { return *team().decision(id); }
    QString raise(const QString& taskId = {}) {
        QString id;
        const auto result = teams_->raiseQuestion(team_,
                                                  {{"question", "Which currency?"},
                                                   {"context", "The cart shows prices."},
                                                   {"choices", QJsonArray{"EUR", "USD"}},
                                                   {"recommended", "EUR"},
                                                   {"task_id", taskId}},
                                                  "lead", &id);
        return result.ok() ? id : QString{};
    }
    TeamResult submit(const QString& id, const QString& answer, const bool batch) {
        if (!batch)
            return teams_->answerDecision(team_, id, answer, "user");
        return teams_->answerDecisions(team_,
                                       {QJsonObject{{"decision_id", id}, {"answer", answer}}},
                                       "user", nullptr, nullptr);
    }

    QStringList recipients(const QString& kind) {
        QStringList result;
        for (const TeamMessage& message : host_.of(kind))
            result.append(message.toSession);
        return result;
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
    }

    void onlyTheLeaderRaisesAValidDecision() {
        start();
        const auto refused = [&](const QJsonObject& draft, const QString& actor) {
            return teams_->raiseQuestion(team_, draft, actor).error.value_or(TeamError{}).code;
        };
        const QJsonObject good{{"question", "Which currency?"},
                               {"choices", QJsonArray{"EUR", "USD"}},
                               {"recommended", "EUR"}};
        QCOMPARE(refused(good, "dev1"), QStringLiteral("role_forbidden"));
        QCOMPARE(refused(good, "rev"), QStringLiteral("role_forbidden"));
        QJsonObject one = good;
        one.insert("choices", QJsonArray{"EUR"});
        QCOMPARE(refused(one, "lead"), QStringLiteral("invalid_payload"));
        QJsonObject five = good;
        five.insert("choices", QJsonArray{"A", "B", "C", "D", "E"});
        QCOMPARE(refused(five, "lead"), QStringLiteral("invalid_payload"));
        QJsonObject stray = good;
        stray.insert("recommended", "GBP");
        QCOMPARE(refused(stray, "lead"), QStringLiteral("invalid_payload"));
        QJsonObject missing = good;
        missing.insert("task_id", "T-9");
        QCOMPARE(refused(missing, "lead"), QStringLiteral("task_not_found"));
        const QString id = raise();
        QCOMPARE(id, QStringLiteral("D-1"));
        QCOMPARE(decision(id).raisedBy, QStringLiteral("lead"));
        QCOMPARE(decision(id).choices, (QStringList{"EUR", "USD"}));
        QCOMPARE(team().openDecisions(), 1);
        QVERIFY(host_.messages.isEmpty());
    }

    void theAnswerReachesTheLeaderAndTheBlockedOwnerOnce_data() {
        QTest::addColumn<bool>("batch");
        QTest::newRow("legacy") << false;
        QTest::newRow("batch") << true;
    }

    void theAnswerReachesTheLeaderAndTheBlockedOwnerOnce() {
        QFETCH(bool, batch);
        start();
        const QString id = task("Cart");
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", "Which currency?").ok());
        const QString raised = raise(id);
        host_.messages.clear();
        // Nothing a member sends answers a decision.
        QCOMPARE(
            teams_->answerDecision(team_, raised, "EUR", "lead").error.value_or(TeamError{}).code,
            QStringLiteral("role_forbidden"));
        QVERIFY(submit(raised, "EUR", batch).ok());
        QCOMPARE(decision(raised).state, QStringLiteral("answered"));
        QCOMPARE(decision(raised).answer, QStringLiteral("EUR"));
        QCOMPARE(team().task(id)->state, state::inProgress);
        QVERIFY(team().task(id)->question.isEmpty());
        QCOMPARE(recipients("decision"), (QStringList{"lead", "dev1"}));
        QVERIFY(host_.of("decision").first().body.contains("Answer: EUR"));
        if (batch) {
            QVERIFY(host_.of("decision")
                        .first()
                        .body.contains(QStringLiteral("Task %1 can go on.").arg(id)));
            bool foundRelease = false;
            for (const auto& update : journal_->teamUpdates(team_, false, 50)) {
                if (update.kind == "state" && update.taskId == id &&
                    update.data.value("from").toString() == state::blocked) {
                    QVERIFY(!update.actionable);
                    foundRelease = true;
                }
            }
            QVERIFY(foundRelease);
            QVERIFY(host_.of("decision").last().body.contains("Answer: EUR"));
        }
        QCOMPARE(
            teams_->answerDecision(team_, raised, "USD", "user").error.value_or(TeamError{}).code,
            QStringLiteral("decision_closed"));
        QCOMPARE(host_.of("decision").size(), 2);
    }

    void aFreeTextAnswerWithoutATaskReachesOnlyTheLeader() {
        start();
        const QString raised = raise();
        QVERIFY(teams_->answerDecision(team_, raised, "Both, EUR first", "user").ok());
        QCOMPARE(recipients("decision"), QStringList{"lead"});
        QCOMPARE(teams_->answerDecision(team_, "D-9", "x", "user").error.value_or(TeamError{}).code,
                 QStringLiteral("invalid_payload"));
    }

    void batchAppliesValidAnswersAndWakesTheLeaderOnce() {
        start();
        const auto first = raise();
        const auto second = raise();
        const auto skipped = raise();
        QVERIFY(!first.isEmpty() && !second.isEmpty() && !skipped.isEmpty());
        const QJsonArray batch{QJsonObject{{"decision_id", first}, {"answer", "EUR"}},
                               QJsonObject{{"decision_id", "missing"}, {"answer", "USD"}},
                               QJsonObject{{"decision_id", first}, {"answer", "duplicate"}},
                               QJsonObject{{"decision_id", second}, {"answer", "USD"}},
                               QJsonObject{{"decision_id", skipped}, {"answer", " "}}};
        QJsonArray rejected;
        QJsonArray accepted;
        QVERIFY(!teams_->answerDecisions(team_, batch, "lead", &rejected, &accepted).ok());
        QVERIFY(host_.of("decision").isEmpty());
        const auto revision = team().revision;
        QVERIFY(teams_->answerDecisions(team_, batch, "user", &rejected, &accepted).ok());
        QCOMPARE(accepted, QJsonArray({first, second}));
        QCOMPARE(rejected.size(), 3);
        QCOMPARE(rejected.at(1).toObject().value("error_code").toString(), "decision_closed");
        QCOMPARE(decision(skipped).state, "open");
        QCOMPARE(decision(first).answer, "EUR");
        QCOMPARE(decision(second).answer, "USD");
        QCOMPARE(team().revision, revision + 2); // One answer commit, one wake.
        QCOMPARE(team().automaticTurns, 1);
        QCOMPARE(recipients("decision"), QStringList{"lead"});
        const auto body = host_.of("decision").first().body;
        QVERIFY(body.contains("Answer: EUR"));
        QVERIFY(body.contains("Answer: USD"));
        QVERIFY(body.contains(skipped));
        int count = 0;
        for (const auto& update : journal_->teamUpdates(team_, false, 50))
            if (update.kind == "decision_answered")
                ++count;
        QCOMPARE(count, 2);
    }

    void allRejectedAnswersLeaveTheTeamAndWakeUnchanged() {
        start();
        const QString id = raise();
        const auto revision = team().revision;
        QJsonArray rejected;
        QJsonArray accepted;
        QVERIFY(teams_
                    ->answerDecisions(team_,
                                      {QJsonObject{{"decision_id", id},
                                                   {"answer", QString(4001, QLatin1Char('a'))}}},
                                      "user", &rejected, &accepted)
                    .ok());
        QCOMPARE(rejected.size(), 1);
        QVERIFY(accepted.isEmpty());
        QCOMPARE(decision(id).state, "open");
        QCOMPARE(team().revision, revision);
        QVERIFY(host_.of("decision").isEmpty());
    }

    void aFailedSecondUpdateRollsBackTheWholeBatch() {
        start();
        const QString first = raise();
        const QString second = raise();
        QVERIFY(!first.isEmpty() && !second.isEmpty());
        const auto revision = team().revision;
        bool installed = false;
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", "decision-rollback");
            database.setDatabaseName(directory_.filePath("journal.sqlite3"));
            installed =
                database.open() &&
                QSqlQuery(database).exec(
                    "CREATE TRIGGER reject_second_answer BEFORE INSERT ON team_updates "
                    "WHEN NEW.kind = 'decision_answered' AND NEW.text LIKE 'D-2 answered:%' "
                    "BEGIN SELECT RAISE(ABORT, 'test failure'); END");
        }
        QSqlDatabase::removeDatabase("decision-rollback");
        QVERIFY(installed);
        QJsonArray accepted;
        QVERIFY(!teams_
                     ->answerDecisions(team_,
                                       {QJsonObject{{"decision_id", first}, {"answer", "EUR"}},
                                        QJsonObject{{"decision_id", second}, {"answer", "USD"}}},
                                       "user", nullptr, &accepted)
                     .ok());
        QVERIFY(accepted.isEmpty());
        QCOMPARE(team().revision, revision);
        QCOMPARE(decision(first).state, "open");
        QCOMPARE(decision(second).state, "open");
        QVERIFY(host_.of("decision").isEmpty());
        for (const auto& update : journal_->teamUpdates(team_, false, 50))
            QVERIFY(update.kind != "decision_answered");
        teams_->load();
        QCOMPARE(team().revision, revision);
        QCOMPARE(decision(first).state, "open");
        QCOMPARE(decision(second).state, "open");
    }

    void theLeaderWithdrawsOnlyItsOwnQuestionsWithAReason() {
        start(true, 0);
        const QString raised = raise();
        QCOMPARE(
            teams_->withdrawDecision(team_, raised, " ", "lead").error.value_or(TeamError{}).code,
            QStringLiteral("invalid_payload"));
        QCOMPARE(
            teams_->withdrawDecision(team_, raised, "no", "dev1").error.value_or(TeamError{}).code,
            QStringLiteral("role_forbidden"));
        QVERIFY(teams_->withdrawDecision(team_, raised, "The spec says EUR.", "lead").ok());
        QCOMPARE(decision(raised).state, QStringLiteral("withdrawn"));
        QCOMPARE(decision(raised).reason, QStringLiteral("The spec says EUR."));
        QCOMPARE(
            teams_->answerDecision(team_, raised, "EUR", "user").error.value_or(TeamError{}).code,
            QStringLiteral("decision_closed"));
        // A budget of 0: the first automatic turn raises the daemon's own decision.
        const QString id = task("Cart");
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        const QString budget = team().decisions.last().id;
        QCOMPARE(team().decisions.last().kind, QStringLiteral("budget"));
        QCOMPARE(teams_->withdrawDecision(team_, budget, "mine", "lead")
                     .error.value_or(TeamError{})
                     .code,
                 QStringLiteral("role_forbidden"));
    }

    void theFirstPlanWaitsForTheUserAndStartReleasesIt_data() {
        QTest::addColumn<bool>("batch");
        QTest::newRow("legacy") << false;
        QTest::newRow("batch") << true;
    }

    void theFirstPlanWaitsForTheUserAndStartReleasesIt() {
        QFETCH(bool, batch);
        start(false);
        const QString first = task("Cart");
        const QString second = task("Api");
        const QString third = task("Docs");
        QVERIFY(teams_->assignTask(team_, first, "dev1", "lead").ok());
        QVERIFY(teams_->assignTask(team_, second, "dev2", "lead").ok());
        QVERIFY(host_.of("assignment").isEmpty());
        QVERIFY(team().task(first)->assignmentHeld && team().task(second)->assignmentHeld);
        QCOMPARE(team().openDecisions(), 1);
        const Decision plan = team().decisions.first();
        QCOMPARE(plan.kind, QStringLiteral("first_plan"));
        QCOMPARE(plan.question, QStringLiteral("Start these 2 tasks?"));
        QVERIFY(plan.context.contains("T-1 \"Cart\" → Dev 1"));
        // The user's own assignment is not held.
        QVERIFY(teams_->assignTask(team_, third, "dev1", "user").ok());
        QCOMPARE(recipients("assignment"), QStringList{"dev1"});
        QVERIFY(submit(plan.id, "Start", batch).ok());
        QVERIFY(team().planApproved);
        QCOMPARE(recipients("assignment"), (QStringList{"dev1", "dev1", "dev2"}));
        QVERIFY(!team().task(first)->assignmentHeld);
        // Later re-plans are not held.
        const QString fourth = task("More");
        QVERIFY(teams_->assignTask(team_, fourth, "dev2", "lead").ok());
        QCOMPARE(host_.of("assignment").size(), 4);
        QCOMPARE(team().openDecisions(), 0);
    }

    void changingTheFirstPlanKeepsItHeldAndTellsTheLeader() {
        start(false);
        const QString first = task("Cart");
        QVERIFY(teams_->assignTask(team_, first, "dev1", "lead").ok());
        QVERIFY(teams_
                    ->answerDecision(team_, team().decisions.first().id, "Change the plan first",
                                     "user")
                    .ok());
        QVERIFY(!team().planApproved);
        QVERIFY(team().task(first)->assignmentHeld);
        QCOMPARE(recipients("decision"), QStringList{"lead"});
        QVERIFY(host_.of("assignment").isEmpty());
        // The leader changed the plan without assigning: its turn's end asks again.
        teams_->memberTurnEnded("lead");
        QCOMPARE(team().openDecisions(), 1);
        QCOMPARE(team().decisions.last().question, QStringLiteral("Start these 1 tasks?"));
        teams_->memberTurnEnded("lead");
        QCOMPARE(team().decisions.size(), 2);
        // The next assignment adds to the open question, with every held task.
        const QString second = task("Api");
        QVERIFY(teams_->assignTask(team_, second, "dev2", "lead").ok());
        QCOMPARE(team().decisions.last().question, QStringLiteral("Start these 2 tasks?"));
        QCOMPARE(team().openDecisions(), 1);
    }

    void theBudgetDecisionContinuesOrStops_data() {
        QTest::addColumn<bool>("batch");
        QTest::newRow("legacy") << false;
        QTest::newRow("batch") << true;
    }

    void theBudgetDecisionContinuesOrStops() {
        QFETCH(bool, batch);
        start(true, 2);
        QStringList ids;
        for (int index = 0; index < 4; ++index)
            ids << task(QStringLiteral("Task %1").arg(index));
        QVERIFY(teams_->assignTask(team_, ids.at(0), "dev1", "lead").ok());
        QVERIFY(teams_->assignTask(team_, ids.at(1), "dev2", "lead").ok());
        QVERIFY(teams_->assignTask(team_, ids.at(2), "dev1", "lead").ok());
        QVERIFY(team().paused);
        const QString budget = team().decisions.last().id;
        QVERIFY(submit(budget, "Continue (2 more turns)", batch).ok());
        QVERIFY(!team().paused);
        QCOMPARE(recipients("assignment"), (QStringList{"dev1", "dev2", "dev1"}));
        QCOMPARE(recipients("decision"), QStringList{"lead"});
        // Both turns of the new budget are used; Stop keeps the team paused by the user.
        QVERIFY(teams_->assignTask(team_, ids.at(3), "dev2", "lead").ok());
        QVERIFY(team().paused);
        QVERIFY(submit(team().decisions.last().id, "Stop", batch).ok());
        QVERIFY(team().paused);
        QCOMPARE(team().pausedReason, QStringLiteral("user"));
        QVERIFY(!team().held.isEmpty());
    }

    void aPauseWinsOverResumeRegardlessOfOrder_data() {
        QTest::addColumn<bool>("pauseFirst");
        QTest::newRow("pause first") << true;
        QTest::newRow("continue first") << false;
    }

    void aPauseWinsOverResumeRegardlessOfOrder() {
        QFETCH(bool, pauseFirst);
        start();
        Team paused = team();
        paused.paused = true;
        paused.pausedReason = "budget";
        paused.decisions = {
            {.id = "D-1",
             .kind = "budget",
             .question = "Continue?",
             .choices = {"Continue", "Stop"}},
            {.id = "D-2", .kind = "stalled", .question = "Wake?", .choices = {"Wake", "Pause"}},
        };
        QString error;
        const auto revision = paused.revision++;
        QVERIFY2(
            journal_->saveTeam(team_, revision, toJson(paused), paused.memberSessions(), &error),
            qPrintable(error));
        teams_->load();

        const QJsonObject keepGoing{{"decision_id", "D-1"}, {"answer", "Continue"}};
        const QJsonObject pause{{"decision_id", "D-2"}, {"answer", "Pause"}};
        const QJsonArray batch =
            pauseFirst ? QJsonArray{pause, keepGoing} : QJsonArray{keepGoing, pause};
        QJsonArray accepted;
        QVERIFY(teams_->answerDecisions(team_, batch, "user", nullptr, &accepted).ok());
        QCOMPARE(accepted.size(), 2);
        QVERIFY(team().paused);
        QCOMPARE(team().pausedReason, "user");
    }

    void decisionsOverTheProtocolAndTheLeadersTool() {
        agent_test::Fixture fixture;
        QVERIFY(fixture.open());
        const QString repository = fixture.directory.filePath("shop");
        QVERIFY(QDir().mkpath(repository));
        const auto command = [&](const QString& type, const QJsonObject& payload) {
            return fixture.host->handleCommand(agent_test::request(type, payload), "desktop-test");
        };
        const QJsonObject created = command(
            "team.create",
            {{"repository", repository},
             {"name", "Shop"},
             {"plan", QJsonObject{{"leader", QJsonObject{{"provider", "codex"}}},
                                  {"developers", QJsonObject{{"count", 1}, {"provider", "codex"}}},
                                  {"reviewer", QJsonObject{{"provider", "claude"}}}}}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(agent_test::text(created)));
        const QJsonArray members = created.value("team").toObject().value("members").toArray();
        const QString teamId = created.value("team").toObject().value("id").toString();
        const QString leader = members.at(0).toObject().value("session_id").toString();
        const QString developer = members.at(1).toObject().value("session_id").toString();
        QCOMPARE(fixture
                     .agents(developer, "agents.team_decision_raise",
                             {{"question", "?"}, {"choices", QJsonArray{"A", "B"}}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("role_forbidden"));
        const QJsonObject raised = fixture.agents(leader, "agents.team_decision_raise",
                                                  {{"question", "Which currency?"},
                                                   {"choices", QJsonArray{"EUR", "USD"}},
                                                   {"recommended", "EUR"}});
        QVERIFY2(raised.value("ok").toBool(), qPrintable(agent_test::text(raised)));
        QCOMPARE(raised.value("decision").toObject().value("id").toString(), QStringLiteral("D-1"));
        const QJsonArray listed =
            command("team.decision.list", {{"team_id", teamId}, {"state", "open"}})
                .value("decisions")
                .toArray();
        QCOMPARE(listed.size(), 1);
        QCOMPARE(listed.first().toObject().value("recommended").toString(), QStringLiteral("EUR"));
        const QJsonObject answered =
            command("team.decision.answer",
                    {{"team_id", teamId}, {"decision_id", "D-1"}, {"choice", "EUR"}});
        QVERIFY2(answered.value("ok").toBool(), qPrintable(agent_test::text(answered)));
        QCOMPARE(answered.value("team").toObject().value("open_decisions").toInt(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.journal->agentMessagesOf(leader, 1, 10).isEmpty(), 5000);
        QCOMPARE(fixture.journal->agentMessagesOf(leader, 1, 10).first().kind,
                 QStringLiteral("decision"));
        QCOMPARE(command("team.decision.answer",
                         {{"team_id", teamId}, {"decision_id", "D-1"}, {"text", "USD"}})
                     .value("error_code")
                     .toString(),
                 QStringLiteral("decision_closed"));
        const QJsonObject withdrawn =
            fixture.agents(leader, "agents.team_decision_withdraw",
                           {{"decision_id", "D-1"}, {"reason", "answered already"}});
        QCOMPARE(withdrawn.value("error_code").toString(), QStringLiteral("decision_closed"));
    }
};

QTEST_GUILESS_MAIN(TeamDecisionTest)
#include "TeamDecisionTest.moc"
