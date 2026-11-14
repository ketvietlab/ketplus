// ADR 0114 §3 and open points 2 and 5: decisions, the first plan and the budget's answer. The
// coordinator cases use a fake runtime; the protocol case the scripted fake providers. No
// real provider is started.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"

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

    void theAnswerReachesTheLeaderAndTheBlockedOwnerOnce() {
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
        QVERIFY(teams_->answerDecision(team_, raised, "EUR", "user").ok());
        QCOMPARE(decision(raised).state, QStringLiteral("answered"));
        QCOMPARE(decision(raised).answer, QStringLiteral("EUR"));
        QCOMPARE(team().task(id)->state, state::inProgress);
        QVERIFY(team().task(id)->question.isEmpty());
        QCOMPARE(recipients("decision"), (QStringList{"lead", "dev1"}));
        QVERIFY(host_.of("decision").first().body.contains("Answer: EUR"));
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

    void theFirstPlanWaitsForTheUserAndStartReleasesIt() {
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
        QVERIFY(teams_->answerDecision(team_, plan.id, "Start", "user").ok());
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

    void theBudgetDecisionContinuesOrStops() {
        start(true, 2);
        QStringList ids;
        for (int index = 0; index < 4; ++index)
            ids << task(QStringLiteral("Task %1").arg(index));
        QVERIFY(teams_->assignTask(team_, ids.at(0), "dev1", "lead").ok());
        QVERIFY(teams_->assignTask(team_, ids.at(1), "dev2", "lead").ok());
        QVERIFY(teams_->assignTask(team_, ids.at(2), "dev1", "lead").ok());
        QVERIFY(team().paused);
        const QString budget = team().decisions.last().id;
        QVERIFY(teams_->answerDecision(team_, budget, "Continue (2 more turns)", "user").ok());
        QVERIFY(!team().paused);
        QCOMPARE(recipients("assignment"), (QStringList{"dev1", "dev2", "dev1"}));
        QCOMPARE(recipients("decision"), QStringList{"lead"});
        // Both turns of the new budget are used; Stop keeps the team paused by the user.
        QVERIFY(teams_->assignTask(team_, ids.at(3), "dev2", "lead").ok());
        QVERIFY(team().paused);
        QVERIFY(teams_->answerDecision(team_, team().decisions.last().id, "Stop", "user").ok());
        QVERIFY(team().paused);
        QCOMPARE(team().pausedReason, QStringLiteral("user"));
        QVERIFY(!team().held.isEmpty());
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
