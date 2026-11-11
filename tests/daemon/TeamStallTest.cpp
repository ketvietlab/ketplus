// ADR 0114 §8: the stall watchdog. A team with work left and nobody working is moved on: the
// member that left its work open is nudged once, then the leader is woken, then the user is
// asked. The coordinator runs on a real journal with a fake runtime. No provider is started.
#include "TeamFakeHost.h"
#include "storage/LocalJournal.h"

#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace ketplus::daemon;
using namespace team_test;

class TeamStallTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    QString task(const QString& title) {
        QString id;
        (void)teams_->createTask(team_, {.title = title, .lane = "Web", .criteria = {"Works"}},
                                 "lead", &id);
        return id;
    }

    // A task dev1 took and left in progress.
    QString inProgress() {
        const QString id = task("Cart");
        (void)teams_->assignTask(team_, id, "dev1", "lead");
        (void)teams_->moveTask(team_, id, state::inProgress, "dev1");
        host_.messages.clear();
        return id;
    }

    const Team& team() { return *teams_->team(team_); }
    [[nodiscard]] QStringList kinds() const {
        QStringList result;
        for (const TeamMessage& message : host_.messages)
            result.append(message.kind + QLatin1Char('>') + message.toSession);
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
        teams_->load();
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
    }

    void aDeveloperThatLeftItsTaskInProgressIsNudgedOnce() {
        const QString id = inProgress();
        // Someone else still works: the leader is left alone, the idle owner is not.
        host_.setIdle("dev2", false);
        teams_->checkStall(team_);
        QCOMPARE(kinds(), QStringList{"nudge>dev1"});
        QVERIFY(host_.messages.first().body.contains(id));
        teams_->checkStall(team_);
        QCOMPARE(host_.messages.size(), 1);
    }

    void aStandingTeamWakesTheLeaderTwiceThenAsksTheUser() {
        const QString id = task("Cart");
        teams_->checkStall(team_);
        teams_->checkStall(team_);
        QCOMPARE(kinds(), (QStringList{"stall>lead", "stall>lead"}));
        QVERIFY(host_.messages.first().body.contains(id));
        QVERIFY(host_.messages.first().body.contains("nobody owns it"));
        QCOMPARE(team().openDecisions(), 0);
        teams_->checkStall(team_);
        QCOMPARE(host_.messages.size(), 2);
        QCOMPARE(team().openDecisions(), 1);
        const Decision asked = team().decisions.last();
        QCOMPARE(asked.kind, QStringLiteral("stalled"));
        QCOMPARE(asked.recommended, wakeLeaderChoice);
        QVERIFY(!team().paused);
        // A team that waits for the user stands still on purpose.
        teams_->checkStall(team_);
        QCOMPARE(host_.messages.size(), 2);
        QCOMPARE(team().decisions.size(), 1);
        // The answer reaches the leader and the watchdog starts over.
        QVERIFY(teams_->answerDecision(team_, asked.id, wakeLeaderChoice, "user").ok());
        QCOMPARE(kinds().last(), QStringLiteral("decision>lead"));
        teams_->checkStall(team_);
        QCOMPARE(kinds().last(), QStringLiteral("stall>lead"));
    }

    void pausingFromTheQuestionPausesTheTeam() {
        (void)task("Cart");
        for (int round = 0; round < 3; ++round)
            teams_->checkStall(team_);
        QCOMPARE(team().openDecisions(), 1);
        QVERIFY(teams_->answerDecision(team_, team().decisions.last().id, pauseTeamChoice, "user")
                    .ok());
        QVERIFY(team().paused);
        QCOMPARE(team().pausedReason, QStringLiteral("user"));
        const int sent = static_cast<int>(host_.messages.size());
        teams_->checkStall(team_);
        QCOMPARE(host_.messages.size(), sent);
    }

    void workThatMovedStartsTheCountAgain() {
        (void)task("Cart");
        teams_->checkStall(team_);
        teams_->checkStall(team_);
        (void)task("Checkout");
        teams_->checkStall(team_);
        QCOMPARE(kinds().last(), QStringLiteral("stall>lead"));
        QCOMPARE(team().openDecisions(), 0);
    }

    void theUsersStopIsRespected() {
        (void)inProgress();
        teams_->memberTurnEnded("dev1", true);
        teams_->checkStall(team_);
        // No nudge for the member the user stopped; the leader hears why the team stands.
        QCOMPARE(kinds(), QStringList{"stall>lead"});
        QVERIFY(host_.messages.first().body.contains("stopped by the user"));
        // A leader the user stopped is not woken either: the user is asked at once.
        teams_->memberTurnEnded("lead", true);
        (void)task("Checkout");
        teams_->checkStall(team_);
        QCOMPARE(host_.messages.size(), 1);
        QCOMPARE(team().decisions.last().kind, QStringLiteral("stalled"));
        // Waking the leader lifts the leader's Stop, not the developer's; neither does a
        // worker that exits without a new turn.
        QVERIFY(teams_->answerDecision(team_, team().decisions.last().id, wakeLeaderChoice, "user")
                    .ok());
        teams_->memberTurnEnded("dev1", std::nullopt);
        host_.messages.clear();
        teams_->checkStall(team_);
        QCOMPARE(kinds(), QStringList{"stall>lead"});
        // The developer's next turn that ends by itself does.
        teams_->memberTurnEnded("dev1");
        teams_->checkStall(team_);
        QCOMPARE(kinds(), (QStringList{"stall>lead", "nudge>dev1"}));
    }

    void aTeamThatWorksOrIsDoneIsLeftAlone() {
        const QString id = task("Cart");
        host_.setIdle("lead", false);
        teams_->checkStall(team_);
        QVERIFY(host_.messages.isEmpty());
        host_.setIdle("lead", true);
        QVERIFY(teams_->update(team_, {{"paused", true}}, "user").ok());
        teams_->checkStall(team_);
        QVERIFY(host_.messages.isEmpty());
        Q_UNUSED(id);
    }

    void theWatchStartsWhenATurnEnds() {
        (void)task("Cart");
        teams_->setStallPeriod(40);
        teams_->memberTurnEnded("lead");
        QVERIFY(host_.messages.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(kinds(), QStringList{"stall>lead"}, 2000);
        // Stall wakes are automatic turns: they count against the budget.
        QCOMPARE(team().automaticTurns, 1);
    }
};

QTEST_GUILESS_MAIN(TeamStallTest)
#include "TeamStallTest.moc"
