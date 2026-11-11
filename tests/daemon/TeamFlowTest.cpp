// ADR 0112 §7 and open point 2: what starts a turn, the leader's digest, pause and the budget
// of automatic turns. The coordinator runs on a real journal with a fake runtime and a short
// quiet period. No provider is started.
#include "AgentTestFixture.h"
#include "TeamFakeHost.h"
#include "storage/LocalJournal.h"

#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace ketplus::daemon;
using namespace team_test;

class TeamFlowTest final : public QObject {
    Q_OBJECT

  private:
    static constexpr int quiet = 60;
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    void reopen() {
        teams_.reset();
        journal_.reset();
        journal_ = std::make_unique<LocalJournal>(directory_.filePath("journal.sqlite3"));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->setQuietPeriod(quiet);
        teams_->load();
    }

    void start(const int budget = defaultTurnBudget) {
        Team draft = standardTeam();
        draft.turnBudget = budget;
        team_ = teams_->create(draft, "user").teamId;
        QVERIFY(team_ > 0);
    }

    QString task(const QString& title) {
        QString id;
        (void)teams_->createTask(team_, {.title = title, .lane = "Web", .criteria = {"Works"}},
                                 "lead", &id);
        return id;
    }

    const Team& team() { return *teams_->team(team_); }

    // Waits longer than the quiet period, letting its timer fire.
    static void settle() { QTest::qWait(quiet * 4); }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        QFile::remove(directory_.filePath("journal.sqlite3"));
        host_ = {};
        for (const QString& id : {"lead", "dev1", "dev2"})
            host_.add(id);
        host_.add("rev", "claude");
        reopen();
    }

    void anAssignmentWakesOnlyItsNewOwner() {
        start();
        const QString id = task("Cart");
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        QCOMPARE(host_.messages.size(), 1);
        QCOMPARE(host_.messages.first().kind, QStringLiteral("assignment"));
        QCOMPARE(host_.messages.first().toSession, QStringLiteral("dev1"));
        QVERIFY(teams_->handoverTask(team_, id, "dev2", "Api knows the cart", "lead").ok());
        QCOMPARE(host_.messages.size(), 2);
        QCOMPARE(host_.messages.last().toSession, QStringLiteral("dev2"));
        QCOMPARE(team().automaticTurns, 2);
    }

    void aNoteOrAMetMarkAloneStartsNoTurn() {
        start();
        const QString id = task("Cart");
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        host_.messages.clear();
        QVERIFY(teams_->updateTask(team_, id, {{"note", "Halfway"}}, "dev1").ok());
        QVERIFY(teams_
                    ->updateTask(team_, id,
                                 {{"met", QJsonArray{QJsonObject{{"index", 0}, {"met", true}}}}},
                                 "dev1")
                    .ok());
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        settle();
        teams_->memberTurnEnded("lead");
        teams_->memberTurnEnded("dev1");
        QVERIFY(host_.messages.isEmpty());
    }

    void tenUpdatesDuringALeaderTurnBecomeOneDigestAfterIt() {
        start();
        const QString id = task("Cart");
        QVERIFY(teams_->assignTask(team_, id, "dev1", "lead").ok());
        host_.messages.clear();
        host_.setIdle("lead", false);
        QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        for (int index = 0; index < 5; ++index) {
            QVERIFY(teams_
                        ->moveTask(team_, id, state::blocked, "dev1",
                                   QStringLiteral("Question %1?").arg(index))
                        .ok());
            QVERIFY(teams_->moveTask(team_, id, state::inProgress, "dev1").ok());
        }
        QVERIFY(teams_->updateTask(team_, id, {{"note", "A note rides along"}}, "dev1").ok());
        settle();
        QVERIFY2(host_.messages.isEmpty(), "No turn starts while the leader's turn runs.");
        host_.setIdle("lead", true);
        teams_->memberTurnEnded("lead");
        QCOMPARE(host_.messages.size(), 1);
        const TeamMessage digest = host_.messages.first();
        QCOMPARE(digest.kind, QStringLiteral("digest"));
        QCOMPARE(digest.toSession, QStringLiteral("lead"));
        QVERIFY(digest.fromSession.isEmpty());
        QVERIFY(digest.body.contains("T-1 \"Cart\""));
        QCOMPARE(digest.body.count("[needs you]"), 5 + 1); // five questions, plus the footer
        QVERIFY(digest.body.contains("A note rides along"));
        // Nothing waits any more: neither the next turn end nor the timer sends another.
        teams_->memberTurnEnded("lead");
        settle();
        QCOMPARE(host_.messages.size(), 1);
    }

    void anIdleLeaderReadsABurstAsOneDigestAfterTheQuietPeriod() {
        start();
        const QString first = task("Cart");
        const QString second = task("Api");
        QVERIFY(teams_->assignTask(team_, first, "dev1", "lead").ok());
        QVERIFY(teams_->assignTask(team_, second, "dev2", "lead").ok());
        host_.messages.clear();
        QVERIFY(teams_->moveTask(team_, first, state::blocked, "dev1", "Which currency?").ok());
        QVERIFY(teams_->moveTask(team_, second, state::blocked, "dev2", "Which route?").ok());
        QVERIFY(host_.messages.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(host_.of("digest").size(), 1, 2000);
        const QString body = host_.of("digest").first().body;
        QVERIFY(body.indexOf("T-1") < body.indexOf("T-2"));
        QVERIFY(body.contains("Which currency?") && body.contains("Which route?"));
        settle();
        QCOMPARE(host_.of("digest").size(), 1);
    }

    void pauseHoldsEveryTurnAndResumeReleasesThemInOrder() {
        start();
        const QString first = task("Cart");
        const QString second = task("Api");
        QVERIFY(teams_->update(team_, {{"paused", true}}, "user").ok());
        QVERIFY(teams_->assignTask(team_, first, "dev1", "lead").ok());
        QVERIFY(teams_->assignTask(team_, second, "dev2", "lead").ok());
        QVERIFY(teams_->moveTask(team_, first, state::blocked, "dev1", "Which currency?").ok());
        settle();
        QVERIFY(host_.messages.isEmpty());
        QCOMPARE(team().held.size(), 2);
        // Held messages survive a restart.
        reopen();
        QCOMPARE(team().held.size(), 2);
        QVERIFY(teams_->update(team_, {{"paused", false}}, "user").ok());
        QCOMPARE(host_.messages.size(), 2);
        QCOMPARE(host_.messages.at(0).toSession, QStringLiteral("dev1"));
        QCOMPARE(host_.messages.at(1).toSession, QStringLiteral("dev2"));
        QVERIFY(team().held.isEmpty());
        // The question asked while paused reaches the leader after the quiet period.
        QTRY_COMPARE_WITH_TIMEOUT(host_.of("digest").size(), 1, 2000);
    }

    void aHeldMessageToAMemberThatLeftIsDropped() {
        start();
        Member third{.sessionId = "dev3",
                     .role = role::developer,
                     .name = "Dev 3",
                     .lane = "Ui",
                     .paths = {"ui"}};
        host_.add("dev3");
        QVERIFY(teams_->addMember(team_, third, "user").ok());
        const QString id = task("Cart");
        QVERIFY(teams_->update(team_, {{"paused", true}}, "user").ok());
        QVERIFY(teams_->assignTask(team_, id, "dev3", "lead").ok());
        QVERIFY(teams_->removeMember(team_, "dev3", "user").ok());
        QVERIFY(teams_->update(team_, {{"paused", false}}, "user").ok());
        QVERIFY(host_.of("assignment").isEmpty());
    }

    void aUsedUpBudgetPausesOnceAndContinuesAfterTheUser() {
        start(2);
        QStringList ids;
        for (int index = 0; index < 4; ++index)
            ids << task(QStringLiteral("Task %1").arg(index));
        QVERIFY(teams_->assignTask(team_, ids.at(0), "dev1", "lead").ok());
        QVERIFY(teams_->assignTask(team_, ids.at(1), "dev2", "lead").ok());
        QCOMPARE(host_.messages.size(), 2);
        QVERIFY(teams_->assignTask(team_, ids.at(2), "dev1", "lead").ok());
        QVERIFY(teams_->assignTask(team_, ids.at(3), "dev2", "lead").ok());
        QCOMPARE(host_.messages.size(), 2);
        QVERIFY(team().paused);
        QCOMPARE(team().pausedReason, QStringLiteral("budget"));
        QCOMPARE(team().held.size(), 2);
        int budget = 0;
        for (const Decision& decision : team().decisions)
            budget += decision.kind == "budget" && decision.state == "open" ? 1 : 0;
        QCOMPARE(budget, 1);
        QCOMPARE(team().decisions.first().choices.first(),
                 QStringLiteral("Continue (2 more turns)"));
        // The user continues: the count starts again and the held turns run in order.
        QVERIFY(teams_->update(team_, {{"paused", false}}, "user").ok());
        QCOMPARE(host_.messages.size(), 4);
        QCOMPARE(host_.messages.at(2).taskId, ids.at(2));
        QCOMPARE(host_.messages.at(3).taskId, ids.at(3));
        QCOMPARE(team().automaticTurns, 2);
        QCOMPARE(team().decisions.first().state, QStringLiteral("withdrawn"));
        QVERIFY(!team().paused);
    }
    void theLeadersTurnEndSendsTheWaitingDigestEndToEnd() {
        agent_test::Fixture fixture;
        QVERIFY(fixture.open(true));
        const QString repository = fixture.directory.filePath("shop");
        QVERIFY(QDir().mkpath(repository));
        for (const QStringList& arguments :
             {QStringList{"init", "-q", "-b", "integration"},
              QStringList{"-c", "user.email=t@example.com", "-c", "user.name=T", "commit", "-q",
                          "--allow-empty", "-m", "start"}}) {
            QProcess process;
            process.setWorkingDirectory(repository);
            process.start("git", arguments);
            QVERIFY(process.waitForFinished(15000) && process.exitCode() == 0);
        }
        const auto command = [&](const QString& type, const QJsonObject& payload) {
            return fixture.host->handleCommand(agent_test::request(type, payload), "desktop-test");
        };
        const QJsonObject created = command(
            "team.create",
            {{"repository", repository},
             {"name", "Shop"},
             {"integration_branch", "integration"},
             {"plan", QJsonObject{{"leader", QJsonObject{{"provider", "claude"}}},
                                  {"developers", QJsonObject{{"count", 1}, {"provider", "codex"}}},
                                  {"reviewer", QJsonObject{{"provider", "claude"}}}}}});
        QVERIFY2(created.value("ok").toBool(), qPrintable(agent_test::text(created)));
        const QJsonArray members = created.value("team").toObject().value("members").toArray();
        const QString teamId = created.value("team").toObject().value("id").toString();
        const QString leader = members.at(0).toObject().value("session_id").toString();
        const QString developer = members.at(1).toObject().value("session_id").toString();
        QVERIFY(command("team.task.create", {{"team_id", teamId}, {"title", "Cart"}})
                    .value("ok")
                    .toBool());
        QVERIFY(command("team.task.assign",
                        {{"team_id", teamId}, {"task_id", "T-1"}, {"owner", developer}})
                    .value("ok")
                    .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.count(developer, "turn.completed") >= 1, 8000);
        QVERIFY(fixture
                    .agents(developer, "agents.team_task_update",
                            {{"task_id", "T-1"}, {"state", "in_progress"}})
                    .value("ok")
                    .toBool());
        // An actionable question waits; the quiet period (20 s) has not passed.
        QVERIFY(fixture
                    .agents(developer, "agents.team_task_update",
                            {{"task_id", "T-1"}, {"state", "blocked"}, {"question", "EUR?"}})
                    .value("ok")
                    .toBool());
        // Any turn of the leader ends with the digest.
        QVERIFY(fixture.agents(developer, "agents.send", {{"to", "leader"}, {"text", "Hello"}})
                    .value("ok")
                    .toBool());
        const auto digests = [&] {
            int count = 0;
            for (const auto& message : fixture.journal->agentMessagesOf(leader, 1, 20))
                count += message.kind == QStringLiteral("digest") ? 1 : 0;
            return count;
        };
        QTRY_COMPARE_WITH_TIMEOUT(digests(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.count(leader, "turn.completed") >= 2, 10000);
        QCOMPARE(digests(), 1);
    }
};

QTEST_GUILESS_MAIN(TeamFlowTest)
#include "TeamFlowTest.moc"
