// ADR 0114 §8: what a member waits for outside the team. team_wait records the wait, keeps
// the watchdog off the member and wakes it to check again; a message to a conversation
// outside the team that failed is reported to its sender, and a managed background job keeps
// the watchdog off its owner without ending its wait. The coordinator runs on a real
// journal with a fake runtime. No provider is started.
#include "TeamFakeHost.h"
#include "storage/LocalJournal.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace ketplus::daemon;
using namespace team_test;

class TeamWaitTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    qint64 team_{0};

    // A task dev1 took and left in progress.
    QString inProgress() {
        QString id;
        (void)teams_->createTask(team_, {.title = "Cart", .lane = "Web", .criteria = {"Works"}},
                                 "lead", &id);
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
    [[nodiscard]] QString refused(const TeamResult& result) const {
        return result.ok() ? QString{} : result.error->code;
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
        teams_->setWaitMinuteForTesting(10);
        team_ = teams_->create(standardTeam(), "user").teamId;
        QVERIFY(team_ > 0);
    }

    void waitTargetIsValidated() {
        QCOMPARE(refused(teams_->wait(team_, "dev1", 5, "CI", {}, "other")),
                 QStringLiteral("invalid_payload"));
        QCOMPARE(refused(teams_->wait(team_, "lead", 5, "merge", {}, "leader")),
                 QStringLiteral("not_allowed"));
        QVERIFY(!teams_->isWaiting("lead"));
        QVERIFY(teams_->wait(team_, "dev1", 5, "leader", {}, "external").ok());
        QVERIFY(teams_->wait(team_, "dev2", 5, "leader", {}, "user").ok());
        QVERIFY(host_.messages.isEmpty());
    }

    void developerNotifiesLeaderOnceAcrossReminders() {
        const QString id = inProgress();
        const int turns = team().automaticTurns;
        QVERIFY(teams_->wait(team_, "lead", 60, "the deployment").ok());
        QVERIFY(teams_->wait(team_, "dev1", 1, "merge the PR", {}, "leader").ok());
        QCOMPARE(kinds(), QStringList{"waiting_for_leader>lead"});
        const auto notice = host_.messages.first();
        QCOMPARE(notice.taskId, id);
        QVERIFY(notice.body.contains("Dev 1"));
        QVERIFY(notice.body.contains(id));
        QVERIFY(notice.body.contains("merge the PR"));
        QCOMPARE(team().automaticTurns, turns + 1);
        QVERIFY(teams_->wait(team_, "dev1", 1, "merge  the PR", id, "leader").ok());
        QCOMPARE(host_.of("waiting_for_leader").size(), 1);
        QTRY_VERIFY(!teams_->isWaiting("dev1"));
        QCOMPARE(host_.of("wait").size(), 1);
        QVERIFY(teams_->wait(team_, "dev1", 60, "merge the PR", id, "leader").ok());
        QCOMPARE(host_.of("waiting_for_leader").size(), 1);
        QVERIFY(teams_->wait(team_, "dev1", 60, "decide on the PR", id, "leader").ok());
        QCOMPARE(host_.of("waiting_for_leader").size(), 2);
    }

    void developerNotifiesLeaderAgainAfterTheFirstWaitWasAnswered() {
        const QString id = inProgress();
        QVERIFY(teams_->wait(team_, "dev1", 1, "merge the PR", id, "leader").ok());
        QCOMPARE(host_.of("waiting_for_leader").size(), 1);
        // The reminder fires; the developer goes back to work and its turn ends.
        QTRY_VERIFY(!teams_->isWaiting("dev1"));
        teams_->memberTurnEnded("dev1");
        QVERIFY(teams_->wait(team_, "dev1", 60, "merge the PR", id, "leader").ok());
        QCOMPARE(host_.of("waiting_for_leader").size(), 2);
        // A repeated wait inside the reminder turn still does not notify twice.
        QVERIFY(teams_->wait(team_, "dev1", 1, "merge the PR", id, "leader").ok());
        QTRY_VERIFY(!teams_->isWaiting("dev1"));
        QVERIFY(teams_->wait(team_, "dev1", 60, "merge the PR", id, "leader").ok());
        QCOMPARE(host_.of("waiting_for_leader").size(), 2);
    }

    void pausedTeamHoldsOneLeaderNotificationUntilResume() {
        const QString id = inProgress();
        const int turns = team().automaticTurns;
        QVERIFY(teams_->update(team_, {{"paused", true}}, "user").ok());
        QVERIFY(teams_->wait(team_, "dev1", 60, "merge the PR", id, "leader").ok());
        QVERIFY(teams_->wait(team_, "dev1", 60, "merge the PR", id, "leader").ok());
        QVERIFY(host_.messages.isEmpty());
        QCOMPARE(team().held.size(), 1);
        QCOMPARE(team().automaticTurns, turns);
        QVERIFY(teams_->update(team_, {{"paused", false}}, "user").ok());
        QCOMPARE(kinds(), QStringList{"waiting_for_leader>lead"});
        // Resume resets the budget, then delivery uses the first turn.
        QCOMPARE(team().automaticTurns, 1);
    }

    void aWaitingMemberIsLeftAloneThenWokenToCheck() {
        const QString id = inProgress();
        const int turns = team().automaticTurns;
        QVERIFY(teams_->wait(team_, "dev1", 5, "  CI on the\npull request ", id).ok());
        QVERIFY(teams_->isWaiting("dev1"));
        // Nobody works, yet the watchdog nudges nobody and leaves the leader asleep.
        teams_->checkStall(team_);
        QVERIFY(host_.messages.isEmpty());
        bool recorded = false;
        for (const auto& value : teams_->updatesJson(team_))
            recorded |= value.toObject().value("kind").toString() == "wait" &&
                        value.toObject().value("text").toString().contains(
                            "waits 5 min: CI on the pull request");
        QVERIFY(recorded);

        QTRY_COMPARE_WITH_TIMEOUT(kinds(), QStringList{"wait>dev1"}, 2000);
        QVERIFY(host_.messages.first().body.contains("CI on the pull request"));
        QCOMPARE(host_.messages.first().taskId, id);
        QVERIFY(!teams_->isWaiting("dev1"));
        // The wake is an automatic turn, and the watchdog is back on the member.
        QCOMPARE(team().automaticTurns, turns + 1);
        teams_->checkStall(team_);
        QCOMPARE(kinds(), (QStringList{"wait>dev1", "nudge>dev1"}));
    }

    void aNewWaitReplacesTheOldOne() {
        (void)inProgress();
        QVERIFY(teams_->wait(team_, "lead", 1, "the first build").ok());
        QVERIFY(teams_->wait(team_, "lead", 2, "the second build").ok());
        QTRY_COMPARE_WITH_TIMEOUT(host_.messages.size(), 1, 2000);
        QVERIFY(host_.messages.first().body.contains("the second build"));
        QTest::qWait(60);
        QCOMPARE(host_.messages.size(), 1);
    }

    void waitsAreRefusedWithAReason() {
        const QString id = inProgress();
        QCOMPARE(refused(teams_->wait(team_, "rev", 5, "CI")), QStringLiteral("not_allowed"));
        QCOMPARE(refused(teams_->wait(team_, "nobody", 5, "CI")), QStringLiteral("not_on_team"));
        QCOMPARE(refused(teams_->wait(team_, "dev1", 5, "  ")), QStringLiteral("invalid_payload"));
        QCOMPARE(refused(teams_->wait(team_, "dev1", 0, "CI")), QStringLiteral("invalid_payload"));
        QCOMPARE(refused(teams_->wait(team_, "dev1", TeamCoordinator::maxWaitMinutes + 1, "CI")),
                 QStringLiteral("invalid_payload"));
        QCOMPARE(refused(teams_->wait(team_, "dev1", 5, "CI", "T-99")),
                 QStringLiteral("task_not_found"));
        QVERIFY(!teams_->isWaiting("dev1"));
        Q_UNUSED(id);
    }

    void theUsersStopAndLeavingTheTeamEndAWait() {
        (void)inProgress();
        QVERIFY(teams_->wait(team_, "dev1", 1, "CI").ok());
        teams_->memberTurnEnded("dev1", true);
        QVERIFY(!teams_->isWaiting("dev1"));
        QVERIFY(teams_->wait(team_, "dev2", 1, "a deployment").ok());
        QVERIFY(teams_->removeMember(team_, "dev2", "user").ok());
        QVERIFY(!teams_->isWaiting("dev2"));
        teams_->setStallPeriod(60 * 1000);
        QTest::qWait(60);
        QVERIFY(host_.of("wait").isEmpty());
    }

    // ADR 0114 §11: the leader's interrupt is checked, recorded and limited by the coordinator.
    void theLeadersInterruptIsCheckedRecordedAndLimited() {
        const QString id = inProgress();
        QVERIFY(teams_->wait(team_, "dev1", 1, "CI").ok());
        QCOMPARE(refused(teams_->noteLeaderInterrupt(team_, "dev2", "why", "dev1")),
                 QStringLiteral("role_forbidden"));
        QCOMPARE(refused(teams_->noteLeaderInterrupt(team_, "lead", "why", "lead")),
                 QStringLiteral("not_allowed"));
        QCOMPARE(refused(teams_->noteLeaderInterrupt(team_, "stranger", "why", "lead")),
                 QStringLiteral("not_member"));
        QCOMPARE(refused(teams_->noteLeaderInterrupt(team_, "dev1", "  ", "lead")),
                 QStringLiteral("invalid_payload"));
        QVERIFY(teams_->update(team_, {{"paused", true}}, "user").ok());
        QCOMPARE(refused(teams_->noteLeaderInterrupt(team_, "dev1", "why", "lead")),
                 QStringLiteral("team_paused"));
        QVERIFY(teams_->update(team_, {{"paused", false}}, "user").ok());
        const qsizetype before = team().task(id)->history.size();
        const QString stateBefore = team().task(id)->state;
        QVERIFY(teams_->noteLeaderInterrupt(team_, "dev1", "wrong_way", "lead").ok());
        const Task& task = *team().task(id);
        QCOMPARE(task.history.size(), before + 1);
        QVERIFY(task.history.last().text.contains("leader interrupted"));
        QVERIFY(task.history.last().text.contains("wrong_way"));
        QCOMPARE(task.state, stateBefore);
        QCOMPARE(task.owner, QStringLiteral("dev1"));
        // The leader's stop is a normal end of the turn: the member's wait stays.
        teams_->memberTurnEnded("dev1", false);
        QVERIFY(teams_->isWaiting("dev1"));
        QVERIFY(teams_->noteLeaderInterrupt(team_, "dev1", "again", "lead").ok());
        QVERIFY(teams_->noteLeaderInterrupt(team_, "dev1", "third", "lead").ok());
        QCOMPARE(refused(teams_->noteLeaderInterrupt(team_, "dev1", "fourth", "lead")),
                 QStringLiteral("interrupt_limit"));
        QVERIFY(teams_->noteLeaderInterrupt(team_, "dev2", "other", "lead").ok());
    }

    void aMemberThatWorksAgainIsNotInterrupted() {
        (void)inProgress();
        QVERIFY(teams_->wait(team_, "dev1", 1, "CI").ok());
        host_.setIdle("dev1", false);
        QTRY_VERIFY_WITH_TIMEOUT(!teams_->isWaiting("dev1"), 2000);
        QVERIFY(host_.messages.isEmpty());
    }

    void aPausedTeamHoldsTheWake() {
        (void)inProgress();
        QVERIFY(teams_->wait(team_, "dev1", 1, "CI").ok());
        QVERIFY(teams_->update(team_, {{"paused", true}}, "user").ok());
        QTRY_VERIFY_WITH_TIMEOUT(!teams_->isWaiting("dev1"), 2000);
        QVERIFY(host_.messages.isEmpty());
        QCOMPARE(team().held.size(), 1);
    }

    // The wait ended while the team was paused, so its wake is held for Resume. The user's
    // Stop ends that too; an ordinary Pause and Resume still delivers it (above).
    void theUsersStopAlsoDropsAHeldWake() {
        (void)inProgress();
        QVERIFY(teams_->wait(team_, "dev1", 1, "CI").ok());
        QVERIFY(teams_->wait(team_, "dev2", 1, "a deployment").ok());
        QVERIFY(teams_->update(team_, {{"paused", true}}, "user").ok());
        QTRY_VERIFY_WITH_TIMEOUT(!teams_->isWaiting("dev1") && !teams_->isWaiting("dev2"), 2000);
        QCOMPARE(team().held.size(), 2);
        teams_->memberTurnEnded("dev1", true);
        QCOMPARE(team().held.size(), 1);
        host_.messages.clear();
        QVERIFY(teams_->update(team_, {{"paused", false}}, "user").ok());
        QCOMPARE(kinds(), QStringList{"wait>dev2"});
    }

    // Resume asks again whether the member is stopped: a held report of a failed message is
    // dropped too, and so is a held wait whose removal at the Stop could not be stored.
    void resumeDoesNotWakeAStoppedMember() {
        (void)inProgress();
        QVERIFY(teams_->update(team_, {{"paused", true}}, "user").ok());
        teams_->outsideMessageFailed("dev1", "#4629", "turn_failed");
        QCOMPARE(team().held.size(), 1);
        teams_->memberTurnEnded("dev1", true);
        host_.messages.clear();
        QVERIFY(teams_->update(team_, {{"paused", false}}, "user").ok());
        QVERIFY2(host_.messages.isEmpty(), qPrintable(kinds().join(',')));
    }

    void aStopWhoseSaveFailedStillHoldsAtResume() {
        (void)inProgress();
        QVERIFY(teams_->wait(team_, "dev1", 1, "CI").ok());
        QVERIFY(teams_->wait(team_, "dev2", 1, "a deployment").ok());
        QVERIFY(teams_->update(team_, {{"paused", true}}, "user").ok());
        QTRY_VERIFY_WITH_TIMEOUT(!teams_->isWaiting("dev1") && !teams_->isWaiting("dev2"), 2000);
        QCOMPARE(team().held.size(), 2);
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", "stop-save");
            database.setDatabaseName(directory_.filePath("journal.sqlite3"));
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec("CREATE TRIGGER no_save BEFORE UPDATE ON teams BEGIN SELECT "
                               "RAISE(ABORT, 'not now'); END"));
            QTest::ignoreMessage(QtWarningMsg, "Unable to drop the held wait of a stopped member.");
            teams_->memberTurnEnded("dev1", true);
            QCOMPARE(team().held.size(), 2);
            QVERIFY(query.exec("DROP TRIGGER no_save"));
            database.close();
        }
        QSqlDatabase::removeDatabase("stop-save");
        host_.messages.clear();
        QVERIFY(teams_->update(team_, {{"paused", false}}, "user").ok());
        QCOMPARE(kinds(), QStringList{"wait>dev2"});
    }

    // T-39: a wait is shown, but a digest that would only repeat waits is not sent.
    void waitsAloneSendNoDigestAndRideAlongWithARealChange() {
        teams_->setQuietPeriod(20);
        teams_->setWaitMinuteForTesting(60000); // no wake-up during this test
        const QString id = inProgress();
        const int turns = team().automaticTurns;
        // The leader's own wait and a developer's wait, with the leader idle and its turn over.
        QVERIFY(teams_->wait(team_, "lead", 30, "the user's answer on the release").ok());
        QVERIFY(teams_->wait(team_, "dev1", 30, "CI on the pull request", id).ok());
        teams_->memberTurnEnded("lead");
        QTest::qWait(100);
        QVERIFY2(host_.messages.isEmpty(), "A digest of waits only starts no turn.");
        QCOMPARE(team().automaticTurns, turns);
        // The team page still shows both.
        int waits = 0;
        for (const auto& value : teams_->updatesJson(team_))
            waits += value.toObject().value("kind").toString() == "wait" ? 1 : 0;
        QCOMPARE(waits, 2);

        // The leader renews its wait: only the latest one is listed, not both.
        QVERIFY(teams_->wait(team_, "lead", 20, "the user's answer, asked again").ok());
        // A real change: the digest carries it and the latest wait of each member.
        QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", "Which endpoint?").ok());
        QTRY_COMPARE_WITH_TIMEOUT(kinds(), QStringList{"digest>lead"}, 2000);
        const QString body = host_.messages.first().body;
        QVERIFY(body.contains("Which endpoint?"));
        QVERIFY(!body.contains("the user's answer on the release"));
        QCOMPARE(body.count("Lead waits"), 1);
        QVERIFY(body.contains("waits 20 min: the user's answer, asked again"));
        QVERIFY(body.contains("waits 30 min: CI on the pull request"));
        // The waits were digested with it: the next turn end repeats nothing.
        teams_->memberTurnEnded("lead");
        QTest::qWait(100);
        QCOMPARE(host_.messages.size(), 1);
        // A new wait alone after that digest sends nothing either.
        QVERIFY(teams_->wait(team_, "dev2", 30, "a device to test on").ok());
        teams_->memberTurnEnded("lead");
        QTest::qWait(100);
        QCOMPARE(host_.messages.size(), 1);
    }

    // T-12: the leader's wait is dropped when a turn reached the leader after it was set.
    void aLeadersWaitEndsQuietlyWhenAnotherTurnReachedTheLeaderMeanwhile() {
        teams_->setQuietPeriod(5);
        const QString id = inProgress();
        QVERIFY(teams_->wait(team_, "lead", 5, "the build").ok());
        QVERIFY(teams_->moveTask(team_, id, state::blocked, "dev1", "Which endpoint?").ok());
        QTRY_COMPARE_WITH_TIMEOUT(kinds(), QStringList{"digest>lead"}, 2000);
        QTest::qWait(150);
        QCOMPARE(kinds(), QStringList{"digest>lead"});
        QVERIFY(!teams_->isWaiting("lead"));
    }

    // T-12: a turn that is only held while the team is paused did not reach the leader, so the
    // leader's wait still ends with its own turn (held as well).
    void aHeldWakeDoesNotDropALeadersWait() {
        const QString id = inProgress();
        QVERIFY(teams_->wait(team_, "lead", 5, "the build").ok());
        QVERIFY(teams_->update(team_, {{"paused", true}}, "user").ok());
        QVERIFY(teams_->wait(team_, "dev1", 30, "an answer", id, "leader").ok());
        QTRY_VERIFY_WITH_TIMEOUT(!teams_->isWaiting("lead"), 2000);
        QStringList held;
        for (const QJsonValue& value : team().held)
            held.append(value.toObject().value("kind").toString() + ">" +
                        value.toObject().value("to").toString());
        QVERIFY2(held.contains("waiting_for_leader>lead"), qPrintable(held.join(",")));
        QVERIFY2(held.contains("wait>lead"), qPrintable(held.join(",")));
        QVERIFY(host_.messages.isEmpty());
    }

    // T-12: nothing changed in the team, so the wait moves once and ends with the turn.
    void aLeadersWaitOnAnUnchangedTeamIsMovedOnce() {
        (void)inProgress();
        QVERIFY(teams_->wait(team_, "lead", 5, "the build").ok());
        QTest::qWait(80);
        QVERIFY2(host_.messages.isEmpty(), "The first end of an unchanged wait starts no turn.");
        QVERIFY(teams_->isWaiting("lead"));
        QTRY_COMPARE_WITH_TIMEOUT(kinds(), QStringList{"wait>lead"}, 2000);
    }

    // T-12: a developer's wait keeps waking its member, whatever reached the leader.
    void aDevelopersWaitIsNotDroppedByALeaderWake() {
        const QString id = inProgress();
        QVERIFY(teams_->wait(team_, "dev1", 5, "CI", id).ok());
        QVERIFY(teams_->wait(team_, "dev2", 5, "a device").ok());
        QTRY_VERIFY_WITH_TIMEOUT(kinds().contains("wait>dev1") && kinds().contains("wait>dev2"),
                                 2000);
    }

    void aFailedMessageOutsideTheTeamWakesItsSender() {
        (void)inProgress();
        const int turns = team().automaticTurns;
        teams_->outsideMessageFailed("lead", "#4629", "no_reply");
        QCOMPARE(kinds(), QStringList{"message_failed>lead"});
        const QString body = host_.messages.first().body;
        QVERIFY(body.contains("#4629"));
        QVERIFY(body.contains("ended without an answer"));
        QVERIFY(body.contains("team_decision_raise"));
        teams_->outsideMessageFailed("dev1", "#4629", "expired_before_delivery");
        QVERIFY(host_.messages.last().body.contains("block your task"));
        QCOMPARE(team().automaticTurns, turns + 2);
        // The user's Stop is respected, and a conversation on no team is not the team's.
        teams_->memberTurnEnded("dev2", true);
        teams_->outsideMessageFailed("dev2", "#4629", "no_reply");
        teams_->outsideMessageFailed("stranger", "#4629", "no_reply");
        QCOMPARE(host_.messages.size(), 2);
    }

    // ADR 0127 §7a: a managed background job wakes its owner when it ends or needs something,
    // so a member that ends its turn on one is at work. A team_wait it set names something
    // else, such as a deployment, and still wakes it to check.
    void aBackgroundJobKeepsTheWatchdogOffAndLeavesAnUnrelatedWait() {
        (void)inProgress();
        host_.conversations["dev1"].backgroundJobs = true;
        teams_->checkStall(team_);
        QVERIFY(host_.messages.isEmpty());

        QVERIFY(teams_->wait(team_, "dev1", 1, "the deployment").ok());
        QTRY_COMPARE(kinds(), QStringList{"wait>dev1"});
        QVERIFY(host_.of("wait").first().body.contains("the deployment"));
    }

    // A job the user stops ends as cancelled and wakes no one, and no turn ends to start the
    // watch again: the watchdog looks again on its own while only jobs hold the team.
    void theWatchdogLooksAgainWhenTheLastJobEndsWithoutAWake() {
        (void)inProgress();
        teams_->setStallPeriod(20);
        host_.conversations["dev1"].backgroundJobs = true;
        teams_->checkStall(team_);
        QTest::qWait(60);
        QVERIFY(host_.messages.isEmpty());

        host_.conversations["dev1"].backgroundJobs = false; // the user stopped the job
        QTRY_COMPARE(kinds(), QStringList{"nudge>dev1"});
    }
};

QTEST_GUILESS_MAIN(TeamWaitTest)
#include "TeamWaitTest.moc"
