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

class OpenCoordinator final : public TeamCoordinator {
  public:
    using TeamCoordinator::TeamCoordinator;
    using TeamCoordinator::wake;
};

QJsonObject startAt(const QDateTime& at) {
    return {{QStringLiteral("at"), at.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm"))},
            {QStringLiteral("timezone"), QStringLiteral("UTC")}};
}

} // namespace

class TeamScheduleStartTest final : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir directory_;
    FakeHost host_;
    ManualClock clock_;
    ManualAlarm alarm_;
    std::unique_ptr<LocalJournal> journal_;
    std::unique_ptr<OpenCoordinator> teams_;
    std::unique_ptr<JournalScheduleStore> store_;
    std::unique_ptr<schedule::TeamStartRuntime> runtime_;
    std::unique_ptr<schedule::ScheduleRunner> runner_;

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
        teams_ = std::make_unique<OpenCoordinator>(journal_.get(), &host_);
        teams_->load();
        store_ = std::make_unique<JournalScheduleStore>(journal_.get());
        runtime_ = std::make_unique<schedule::TeamStartRuntime>(teams_.get());
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

    bool leaderStarted(const qint64 teamId) const {
        const Team* team = teams_->team(teamId);
        return team && team->leaderStarted;
    }

  private slots:
    void immediateCreateStillStartsTheLeader() {
        const TeamResult created = teams_->create(draft(), QStringLiteral("user"));
        QVERIFY2(created.ok(), created.error ? qPrintable(created.error->message) : "");
        QVERIFY(leaderStarted(created.teamId));
        QCOMPARE(host_.messages.size(), 1);
        QCOMPARE(host_.messages.first().toSession, QStringLiteral("lead"));
    }

    void scheduledStartFiresAndAMissedOneFiresOnce() {
        const TeamResult created = teams_->create(draft(), QStringLiteral("user"),
                                                  startAt(clock_.now_.addSecs(120)), clock_.now_);
        QVERIFY2(created.ok(), created.error ? qPrintable(created.error->message) : "");
        QVERIFY(!leaderStarted(created.teamId));
        QVERIFY(host_.messages.isEmpty());
        const Team* waiting = teams_->team(created.teamId);
        QVERIFY(waiting->startWaiting);
        QCOMPARE(toJson(*waiting).value(QStringLiteral("status")).toString(),
                 QStringLiteral("scheduled"));
        QVERIFY(toJson(*waiting).contains(QStringLiteral("start_at")));
        QCOMPARE(
            journal_->schedules(ScheduleFilter{.targetKind = QStringLiteral("team_start")})->size(),
            1);

        clock_.now_ = clock_.now_.addSecs(180);
        runner_->noteClockChanged();
        QVERIFY(leaderStarted(created.teamId));
        QCOMPARE(host_.messages.size(), 1);

        QVERIFY(!teams_->remove(created.teamId));
        host_.messages.clear();
        const TeamResult later = teams_->create(draft(), QStringLiteral("user"),
                                                startAt(clock_.now_.addSecs(120)), clock_.now_);
        QVERIFY(later.ok());
        runner_.reset();
        clock_.now_ = clock_.now_.addDays(1);
        runner_ = std::make_unique<schedule::ScheduleRunner>(clock_, alarm_, *store_, *runtime_);
        runner_->start();
        QVERIFY(leaderStarted(later.teamId));
        QCOMPARE(host_.messages.size(), 1);
        runner_->noteClockChanged();
        QCOMPARE(host_.messages.size(), 1);
    }

    void aPausedTeamStartsWhenResumed() {
        const TeamResult created = teams_->create(draft(), QStringLiteral("user"),
                                                  startAt(clock_.now_.addSecs(120)), clock_.now_);
        QVERIFY(created.ok());
        QVERIFY(
            teams_
                ->update(created.teamId, {{QStringLiteral("paused"), true}}, QStringLiteral("user"))
                .ok());
        clock_.now_ = clock_.now_.addSecs(180);
        runner_->noteClockChanged();
        QVERIFY(!leaderStarted(created.teamId));
        QVERIFY(teams_
                    ->update(created.teamId, {{QStringLiteral("paused"), false}},
                             QStringLiteral("user"))
                    .ok());
        QVERIFY(leaderStarted(created.teamId));
        QCOMPARE(host_.messages.size(), 1);
    }

    void startNowChangeAndCancel() {
        const TeamResult created = teams_->create(draft(), QStringLiteral("user"),
                                                  startAt(clock_.now_.addSecs(3600)), clock_.now_);
        QVERIFY(created.ok());
        const QString first = teams_->team(created.teamId)->startScheduleId;
        const TeamResult moved =
            teams_->updateStartAt(created.teamId, startAt(clock_.now_.addSecs(7200)),
                                  QStringLiteral("user"), clock_.now_);
        QVERIFY2(moved.ok(), moved.error ? qPrintable(moved.error->message) : "");
        QVERIFY(teams_->team(created.teamId)->startScheduleId != first);
        QVERIFY(!journal_->schedule(first));
        QVERIFY(!leaderStarted(created.teamId));

        QVERIFY(teams_->cancelStartAt(created.teamId, QStringLiteral("user")).ok());
        const Team* cancelled = teams_->team(created.teamId);
        QVERIFY(cancelled->startWaiting);
        QVERIFY(cancelled->startScheduleId.isEmpty());
        QVERIFY(!leaderStarted(created.teamId));
        QCOMPARE(
            journal_->schedules(ScheduleFilter{.targetKind = QStringLiteral("team_start")})->size(),
            0);

        QVERIFY(teams_->startNow(created.teamId, QStringLiteral("user")).ok());
        QVERIFY(leaderStarted(created.teamId));
        const TeamResult again = teams_->startNow(created.teamId, QStringLiteral("user"));
        QVERIFY(!again.ok());
        QCOMPARE(again.error->code, QStringLiteral("team_already_started"));
    }

    void deletingTheTeamDeletesTheSchedule() {
        const TeamResult created = teams_->create(draft(), QStringLiteral("user"),
                                                  startAt(clock_.now_.addSecs(120)), clock_.now_);
        QVERIFY(created.ok());
        QVERIFY(!teams_->remove(created.teamId));
        QCOMPARE(journal_->schedules(ScheduleFilter{}).value_or(QList<ScheduleRecord>{}).size(), 0);
    }

    void noTurnIsSentBeforeTheLeaderStarts() {
        const TeamResult created = teams_->create(draft(), QStringLiteral("user"),
                                                  startAt(clock_.now_.addSecs(120)), clock_.now_);
        QVERIFY(created.ok());
        teams_->wake(*teams_->team(created.teamId),
                     {.teamId = created.teamId,
                      .toSession = QStringLiteral("dev1"),
                      .kind = QStringLiteral("assignment"),
                      .body = QStringLiteral("Start the web lane.")});
        QVERIFY(host_.messages.isEmpty());
        QCOMPARE(teams_->team(created.teamId)->held.size(), 1);
        clock_.now_ = clock_.now_.addSecs(180);
        runner_->noteClockChanged();
        QCOMPARE(host_.messages.size(), 2);
    }
};

QTEST_GUILESS_MAIN(TeamScheduleStartTest)
#include "TeamScheduleStartTest.moc"
