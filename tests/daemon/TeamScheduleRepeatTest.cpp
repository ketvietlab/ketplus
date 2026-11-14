#include "TeamFakeHost.h"
#include "schedule/JournalScheduleStore.h"
#include "schedule/ScheduleRunner.h"
#include "schedule/TeamStartRuntime.h"
#include "storage/LocalJournal.h"

#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

using namespace ketplus::daemon;
using namespace ketplus::daemon::teams;
using namespace team_test;

namespace {

class ManualClock final : public schedule::ScheduleClock {
  public:
    [[nodiscard]] QDateTime now() const override { return now_; }
    QDateTime now_{QDateTime(QDate(2026, 11, 14), QTime(9, 0), QTimeZone::UTC)};
};

class ManualAlarm final : public schedule::ScheduleAlarm {
  public:
    void arm(std::chrono::milliseconds delay) override { delay_ = delay; }
    void disarm() override { delay_ = std::chrono::milliseconds{-1}; }
    std::chrono::milliseconds delay_{};
};

QJsonObject daily(const QString& time, const QString& work,
                  const QString& busy = QStringLiteral("queue"),
                  const QString& missed = QStringLiteral("run_once")) {
    return {
        {QStringLiteral("rule"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("daily")},
                                             {QStringLiteral("time_zone"), QStringLiteral("UTC")},
                                             {QStringLiteral("time"), time}}},
        {QStringLiteral("work_order"), work},
        {QStringLiteral("busy_policy"), busy},
        {QStringLiteral("missed_policy"), missed}};
}

} // namespace

class TeamScheduleRepeatTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    ManualClock clock_;
    ManualAlarm alarm_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<TeamCoordinator> teams_;
    std::unique_ptr<JournalScheduleStore> store_;
    std::unique_ptr<schedule::TeamStartRuntime> runtime_;
    std::unique_ptr<schedule::ScheduleRunner> runner_;

    void arm() {
        runner_ = std::make_unique<schedule::ScheduleRunner>(clock_, alarm_, *store_, *runtime_);
        host_.onSchedulesChanged = [this] { runner_->schedulesChanged(); };
        host_.onScheduledTeamMayStart = [this](const qint64 teamId) {
            runner_->turnFinished(QStringLiteral("team:") + QString::number(teamId));
        };
        runner_->start();
    }

    Team draft() const {
        Team team = standardTeam();
        team.goal = QStringLiteral("Ship the checkout.");
        return team;
    }

    QList<TeamMessage> scheduled() const { return host_.of(QStringLiteral("schedule")); }

    QString soon(const int seconds = 120) const {
        return clock_.now_.addSecs(seconds).toUTC().toString(QStringLiteral("HH:mm"));
    }

    QString teamScheduleId() const {
        const auto rows = journal_->schedules(ScheduleFilter{.targetKind = QStringLiteral("team")});
        return rows && !rows->isEmpty() ? rows->first().id : QString{};
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init() {
        runner_.reset();
        runtime_.reset();
        store_.reset();
        teams_.reset();
        journal_.reset();
        QFile::remove(directory_.filePath(QStringLiteral("journal.sqlite3")));
        host_ = {};
        clock_.now_ = QDateTime(QDate(2026, 11, 14), QTime(9, 0), QTimeZone::UTC);
        for (const QString& id :
             {QStringLiteral("lead"), QStringLiteral("dev1"), QStringLiteral("dev2")})
            host_.add(id);
        host_.add(QStringLiteral("rev"), QStringLiteral("claude"));
        journal_ =
            std::make_unique<LocalJournal>(directory_.filePath(QStringLiteral("journal.sqlite3")));
        teams_ = std::make_unique<TeamCoordinator>(journal_.get(), &host_);
        teams_->load();
        store_ = std::make_unique<JournalScheduleStore>(journal_.get());
        runtime_ = std::make_unique<schedule::TeamStartRuntime>(teams_.get());
        arm();
    }

    void createStoresATeamScheduleAndFiresOnce() {
        const TeamResult created = teams_->create(draft(), QStringLiteral("user"), {}, clock_.now_,
                                                  daily(soon(), QStringLiteral("Write the post.")));
        QVERIFY2(created.ok(), created.error ? qPrintable(created.error->message) : "");
        QCOMPARE(journal_->schedules(ScheduleFilter{.targetKind = QStringLiteral("team")})->size(),
                 1);
        QCOMPARE(scheduled().size(), 0);
        clock_.now_ = clock_.now_.addSecs(180);
        runner_->noteClockChanged();
        QCOMPARE(scheduled().size(), 1);
        const TeamMessage message = scheduled().first();
        QCOMPARE(message.toSession, QStringLiteral("lead"));
        QCOMPARE(message.body,
                 QStringLiteral("Scheduled run: Checkout (2026-11-14): Write the post."));
        QCOMPARE(message.scheduleId, teamScheduleId());
        QVERIFY(!message.runId.isEmpty());
        const auto runs = journal_->scheduleRuns(message.scheduleId, 10);
        QVERIFY(runs);
        QCOMPARE(runs->runs.size(), 1);
        QCOMPARE(runs->runs.first().status, QStringLiteral("succeeded"));
        QCOMPARE(runs->runs.first().sessionId, QStringLiteral("lead"));
        bool saw = false;
        for (const QJsonValue& update : teams_->updatesJson(created.teamId))
            if (update.toObject().value(QStringLiteral("text")).toString() ==
                QStringLiteral("Scheduled run: Checkout"))
                saw = true;
        QVERIFY(saw);
        runner_->noteClockChanged();
        QCOMPARE(scheduled().size(), 1);
    }

    void queueWaitsForTheLeaderAndSkipDoesNot() {
        TeamResult created = teams_->create(draft(), QStringLiteral("user"), {}, clock_.now_,
                                            daily(soon(), QStringLiteral("Write.")));
        QVERIFY(created.ok());
        host_.setIdle(QStringLiteral("lead"), false);
        clock_.now_ = clock_.now_.addSecs(180);
        runner_->noteClockChanged();
        QCOMPARE(scheduled().size(), 0);
        const auto queued = journal_->scheduleRuns(teamScheduleId(), 10);
        QVERIFY(queued);
        QCOMPARE(queued->runs.size(), 1);
        QCOMPARE(queued->runs.first().status, QStringLiteral("queued"));
        clock_.now_ = clock_.now_.addDays(1);
        runner_->noteClockChanged();
        const auto still = journal_->scheduleRuns(teamScheduleId(), 10);
        QVERIFY(still);
        QCOMPARE(still->runs.size(), 1);
        host_.setIdle(QStringLiteral("lead"), true);
        runner_->turnFinished(QStringLiteral("lead"));
        QCOMPARE(scheduled().size(), 1);

        QVERIFY(!teams_->remove(created.teamId));
        host_.messages.clear();
        created = teams_->create(draft(), QStringLiteral("user"), {}, clock_.now_,
                                 daily(soon(), QStringLiteral("Write."), QStringLiteral("skip")));
        QVERIFY2(created.ok(), created.error ? qPrintable(created.error->message) : "");
        host_.setIdle(QStringLiteral("lead"), false);
        clock_.now_ = clock_.now_.addSecs(180);
        runner_->noteClockChanged();
        QCOMPARE(scheduled().size(), 0);
        const auto skipped = journal_->scheduleRuns(teamScheduleId(), 10);
        QVERIFY(skipped);
        QCOMPARE(skipped->runs.first().status, QStringLiteral("skipped"));
    }

    void pausedNotStartedAndBudgetDoNotStartATurn() {
        const QJsonObject later = {{QStringLiteral("at"), QStringLiteral("2026-11-14T12:00")},
                                   {QStringLiteral("timezone"), QStringLiteral("UTC")}};
        TeamResult created = teams_->create(draft(), QStringLiteral("user"), later, clock_.now_,
                                            daily(soon(), QStringLiteral("Write.")));
        QVERIFY2(created.ok(), created.error ? qPrintable(created.error->message) : "");
        clock_.now_ = clock_.now_.addSecs(180);
        runner_->noteClockChanged();
        QCOMPARE(scheduled().size(), 0);
        auto runs = journal_->scheduleRuns(teamScheduleId(), 10);
        QVERIFY(runs);
        QCOMPARE(runs->runs.first().status, QStringLiteral("failed"));
        QCOMPARE(runs->runs.first().errorCode, QStringLiteral("team_not_started"));

        QVERIFY(!teams_->remove(created.teamId));
        host_.messages.clear();
        created = teams_->create(draft(), QStringLiteral("user"), {}, clock_.now_,
                                 daily(soon(), QStringLiteral("Write.")));
        QVERIFY(created.ok());
        QVERIFY(
            teams_
                ->update(created.teamId, {{QStringLiteral("paused"), true}}, QStringLiteral("user"))
                .ok());
        clock_.now_ = clock_.now_.addSecs(400);
        runner_->noteClockChanged();
        QCOMPARE(scheduled().size(), 0);
        runs = journal_->scheduleRuns(teamScheduleId(), 10);
        QCOMPARE(runs->runs.first().errorCode, QStringLiteral("team_paused"));

        QVERIFY(!teams_->remove(created.teamId));
        host_.messages.clear();
        Team budget = draft();
        budget.turnBudget = 5;
        created = teams_->create(budget, QStringLiteral("user"), {}, clock_.now_,
                                 daily(soon(), QStringLiteral("Write.")));
        QVERIFY(created.ok());
        for (int turn = 0; turn < 4; ++turn)
            QCOMPARE(teams_->takeMemberTurn(QStringLiteral("lead")), QString{});
        clock_.now_ = clock_.now_.addSecs(700);
        runner_->noteClockChanged();
        QCOMPARE(scheduled().size(), 0);
        runs = journal_->scheduleRuns(teamScheduleId(), 10);
        QCOMPARE(runs->runs.first().errorCode, QStringLiteral("team_budget"));
        QCOMPARE(host_.messages.size(), 1);
    }

    void aMissedDayFiresOnceAndSkipDropsIt() {
        TeamResult created = teams_->create(draft(), QStringLiteral("user"), {}, clock_.now_,
                                            daily(soon(), QStringLiteral("Write.")));
        QVERIFY(created.ok());
        runner_.reset();
        clock_.now_ = clock_.now_.addDays(2);
        arm();
        QCOMPARE(scheduled().size(), 1);
        runner_->noteClockChanged();
        QCOMPARE(scheduled().size(), 1);

        QVERIFY(!teams_->remove(created.teamId));
        host_.messages.clear();
        created = teams_->create(draft(), QStringLiteral("user"), {}, clock_.now_,
                                 daily(soon(), QStringLiteral("Write."), QStringLiteral("queue"),
                                       QStringLiteral("skip")));
        QVERIFY(created.ok());
        runner_.reset();
        clock_.now_ = clock_.now_.addDays(1);
        arm();
        QCOMPARE(scheduled().size(), 0);
        const auto runs = journal_->scheduleRuns(teamScheduleId(), 10);
        QVERIFY(runs);
        QCOMPARE(runs->runs.first().status, QStringLiteral("missed"));
    }

    void deletingTheTeamDeletesTheSchedule() {
        const TeamResult created = teams_->create(draft(), QStringLiteral("user"), {}, clock_.now_,
                                                  daily(soon(), QStringLiteral("Write.")));
        QVERIFY(created.ok());
        QVERIFY(!teams_->remove(created.teamId));
        QCOMPARE(journal_->schedules(ScheduleFilter{.targetKind = QStringLiteral("team")})
                     .value_or(QList<ScheduleRecord>{})
                     .size(),
                 0);
    }
};

QTEST_GUILESS_MAIN(TeamScheduleRepeatTest)
#include "TeamScheduleRepeatTest.moc"
