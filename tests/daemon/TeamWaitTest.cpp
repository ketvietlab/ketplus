// ADR 0114 §8: what a member waits for outside the team. team_wait records the wait, keeps
// the watchdog off the member and wakes it to check again; a message to a conversation
// outside the team that failed is reported to its sender. The coordinator runs on a real
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
};

QTEST_GUILESS_MAIN(TeamWaitTest)
#include "TeamWaitTest.moc"
