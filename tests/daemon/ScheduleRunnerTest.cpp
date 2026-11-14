#include "schedule/ScheduleRunner.h"
#include "protocol/ScheduleProtocol.h"
#include "schedule/ScheduleRule.h"

#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <QTimeZone>

#include <QtTest>

namespace ketplus::daemon {
namespace {

namespace documents = ketplus::schedule;

QDateTime at(int year, int month, int day, int hour, int minute, int second = 0, int ms = 0) {
    return QDateTime(QDate(year, month, day), QTime(hour, minute, second, ms), QTimeZone::utc());
}

QString stamp(const QDateTime& datetime) { return datetime.toUTC().toString(Qt::ISODateWithMs); }

documents::Schedule makeDaily(const QString& id) {
    documents::Schedule schedule;
    schedule.id = id;
    schedule.name = QStringLiteral("CI");
    schedule.enabled = true;
    schedule.prompt = QStringLiteral("Recheck CI");
    schedule.rule = QStringLiteral(R"({"kind":"daily","time":"08:00","time_zone":"UTC"})");
    schedule.busyPolicy = documents::BusyPolicy::Queue;
    schedule.missedPolicy = documents::MissedPolicy::RunOnce;
    schedule.createdBy = QStringLiteral("user");
    schedule.createdAt = QStringLiteral("2026-11-13T00:00:00.000Z");
    schedule.updatedAt = schedule.createdAt;
    schedule.target.kind = documents::TargetKind::Conversation;
    schedule.target.sessionId = QStringLiteral("session-1");
    schedule.nextRunAt = QStringLiteral("2026-11-13T08:00:00.000Z");
    return schedule;
}

class ManualClock final : public schedule::ScheduleClock {
  public:
    [[nodiscard]] QDateTime now() const override { return now_; }
    QDateTime now_{at(2026, 11, 13, 7, 0)};
};

class ManualAlarm final : public schedule::ScheduleAlarm {
  public:
    void arm(std::chrono::milliseconds delay) override {
        armed_ = true;
        delay_ = delay;
    }
    void disarm() override {
        armed_ = false;
        delay_ = std::chrono::milliseconds::zero();
    }
    void fire() {
        if (handler_)
            handler_();
    }

    bool armed_{false};
    std::chrono::milliseconds delay_{};
};

class MemoryStore final : public schedule::ScheduleStore {
  public:
    [[nodiscard]] QList<documents::Schedule> schedules() const override { return schedules_; }

    [[nodiscard]] std::optional<documents::Schedule> schedule(const QString& id) const override {
        for (const auto& schedule : schedules_) {
            if (schedule.id == id)
                return schedule;
        }
        return std::nullopt;
    }

    bool save(const documents::Schedule& schedule) override {
        for (auto& existing : schedules_) {
            if (existing.id == schedule.id) {
                existing = schedule;
                return true;
            }
        }
        schedules_.append(schedule);
        return true;
    }

    documents::Run insertRun(documents::Run run) override {
        if (run.id.isEmpty())
            run.id = QStringLiteral("run-%1").arg(++sequence_);
        runs_.append(run);
        return run;
    }

    bool saveRun(const documents::Run& run) override {
        for (auto& existing : runs_) {
            if (existing.id == run.id) {
                existing = run;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<documents::Run> run(const QString& id) const override {
        for (const auto& run : runs_) {
            if (run.id == id)
                return run;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<documents::Run>
    queuedRun(const QString& scheduleId) const override {
        for (const auto& run : runs_) {
            if (run.scheduleId == scheduleId && run.status == documents::RunStatus::Queued)
                return run;
        }
        return std::nullopt;
    }

    [[nodiscard]] QList<documents::Run> unfinishedRuns() const override {
        QList<documents::Run> open;
        for (const auto& run : runs_) {
            if (run.status == documents::RunStatus::Queued ||
                run.status == documents::RunStatus::Running)
                open.append(run);
        }
        return open;
    }

    [[nodiscard]] QList<documents::Run> runsFor(const QString& scheduleId) const {
        QList<documents::Run> found;
        for (const auto& run : runs_) {
            if (run.scheduleId == scheduleId)
                found.append(run);
        }
        return found;
    }

  private:
    QList<documents::Schedule> schedules_;
    QList<documents::Run> runs_;
    int sequence_{0};
};

class FakeRuntime final : public schedule::ScheduleRuntime {
  public:
    struct Record {
        documents::Schedule schedule;
        documents::Run run;
        QJsonObject message;
    };

    void setState(const QString& id, schedule::ConversationState state,
                  const QString& sessionId = {}) {
        states_.insert(id, state);
        if (!sessionId.isEmpty())
            busySessions_.insert(id, sessionId);
    }

    [[nodiscard]] schedule::Inspection inspect(const documents::Schedule& schedule) const override {
        schedule::Inspection inspection;
        inspection.state = states_.value(schedule.id, schedule::ConversationState::Idle);
        inspection.sessionId = busySessions_.value(schedule.id);
        if (inspection.sessionId.isEmpty() &&
            schedule.target.kind == documents::TargetKind::Conversation) {
            inspection.sessionId = schedule.target.sessionId;
        }
        return inspection;
    }

    schedule::Delivery deliver(const documents::Schedule& schedule,
                               const documents::Run& run) override {
        Record record;
        record.schedule = schedule;
        record.run = run;
        record.message = schedule::scheduledUserMessage(schedule, run);
        deliveries.append(record);
        schedule::Delivery delivery;
        if (schedule.target.kind == documents::TargetKind::NewConversation)
            delivery.sessionId = QStringLiteral("session-new-%1").arg(++sessions_);
        else if (!schedule.target.sessionId.isEmpty())
            delivery.sessionId = schedule.target.sessionId;
        else
            delivery.sessionId = QStringLiteral("session-team-%1").arg(++sessions_);
        delivery.turnId = QStringLiteral("turn-%1").arg(deliveries.size());
        return delivery;
    }

    void emitRun(const documents::Run& run) override { events.append(run); }

    void emitChanged(const QString& change, const documents::Schedule&) override {
        changes.append(change);
    }

    QList<Record> deliveries;
    QList<documents::Run> events;
    QStringList changes;

  private:
    QHash<QString, schedule::ConversationState> states_;
    QHash<QString, QString> busySessions_;
    int sessions_{0};
};

struct World {
    ManualClock clock;
    ManualAlarm alarm;
    MemoryStore store;
    FakeRuntime runtime;
    schedule::ScheduleRunner runner{clock, alarm, store, runtime};
};

const QString nextDay = QStringLiteral("2026-11-14T08:00:00.000Z");

} // namespace

class ScheduleRunnerTest final : public QObject {
    Q_OBJECT

  private slots:
    void firesIntoExistingConversation();
    void createsNewConversation();
    void queuesOneRunWhileBusy();
    void startsQueuedRunWhenTheTurnEnds();
    void skipsWhileBusy();
    void missedRunOnceAfterRestart();
    void missedSkipAfterRestart();
    void clockJumpFiresOnce();
    void treatsOneMinuteLateAsOnTime();
    void onceDisablesItself();
    void recordsFailures();
    void invalidRuleDisablesTheSchedule();
    void runNowDoesNotConsumeTheRule();
    void runNowWhileBusyReturnsTheQueuedRun();
    void runNowOnAPausedScheduleStartsWhenTheTurnEnds();
    void runNowUnknownId();
    void rearmsForTheEarliestSchedule();
    void earlyAlarmDoesNotFire();
    void fillsNextWhenCreated();
    void resumesQueuedRunOnStart();
    void restartFailsARunningRunAndAQueuedRunWhoseTurnIsGone();
    void deliversTeamTargets();
    void capsTheAlarmDelay();
    void qtAlarmCanArmAndDisarm();
};

void ScheduleRunnerTest::firesIntoExistingConversation() {
    World world;
    world.store.save(makeDaily(QStringLiteral("s1")));
    world.clock.now_ = at(2026, 11, 13, 8, 0);
    world.alarm.fire();

    const auto runs = world.store.runsFor(QStringLiteral("s1"));
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.first().status, documents::RunStatus::Running);
    QCOMPARE(runs.first().sessionId, QStringLiteral("session-1"));
    QCOMPARE(runs.first().turnId, QStringLiteral("turn-1"));
    QCOMPARE(runs.first().scheduledFor, QStringLiteral("2026-11-13T08:00:00.000Z"));
    QCOMPARE(world.runtime.deliveries.size(), 1);
    const QJsonObject message = world.runtime.deliveries.first().message;
    QCOMPARE(message.value(QStringLiteral("text")).toString(), QStringLiteral("Recheck CI"));
    QCOMPARE(message.value(QStringLiteral("schedule_id")).toString(), QStringLiteral("s1"));
    QCOMPARE(message.value(QStringLiteral("run_id")).toString(), runs.first().id);
    const auto schedule = world.store.schedule(QStringLiteral("s1"));
    QVERIFY(schedule);
    QCOMPARE(schedule->nextRunAt, nextDay);
    QCOMPARE(schedule->lastRunAt, stamp(world.clock.now_));
    QVERIFY(schedule->enabled);
    QCOMPARE(world.alarm.delay_.count(), 24LL * 60 * 60 * 1000);
    QCOMPARE(world.runtime.changes, QStringList{QStringLiteral("updated")});
}

void ScheduleRunnerTest::createsNewConversation() {
    World world;
    auto schedule = makeDaily(QStringLiteral("s1"));
    schedule.target.kind = documents::TargetKind::NewConversation;
    schedule.target.sessionId.clear();
    schedule.target.workspace = QStringLiteral("/repo");
    schedule.target.provider = QStringLiteral("codex");
    schedule.target.model = QStringLiteral("gpt");
    schedule.target.effort = QStringLiteral("high");
    schedule.target.isolatedWorktree = true;
    schedule.target.titleTemplate = QStringLiteral("CI {date}");
    world.store.save(schedule);
    world.clock.now_ = at(2026, 11, 13, 8, 0);
    world.runner.alarmFired();

    const auto runs = world.store.runsFor(QStringLiteral("s1"));
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.first().sessionId, QStringLiteral("session-new-1"));
    QCOMPARE(world.runtime.deliveries.size(), 1);
    const auto& target = world.runtime.deliveries.first().schedule.target;
    QCOMPARE(target.workspace, QStringLiteral("/repo"));
    QCOMPARE(target.provider, QStringLiteral("codex"));
    QCOMPARE(target.model, QStringLiteral("gpt"));
    QCOMPARE(target.effort, QStringLiteral("high"));
    QCOMPARE(target.isolatedWorktree, true);
    QCOMPARE(target.titleTemplate, QStringLiteral("CI {date}"));
    QCOMPARE(
        world.runtime.deliveries.first().message.value(QStringLiteral("schedule_id")).toString(),
        QStringLiteral("s1"));
}

void ScheduleRunnerTest::queuesOneRunWhileBusy() {
    World world;
    world.store.save(makeDaily(QStringLiteral("s1")));
    world.runtime.setState(QStringLiteral("s1"), schedule::ConversationState::Busy,
                           QStringLiteral("session-1"));
    world.clock.now_ = at(2026, 11, 13, 8, 0);
    world.runner.alarmFired();

    auto runs = world.store.runsFor(QStringLiteral("s1"));
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.first().status, documents::RunStatus::Queued);
    QCOMPARE(runs.first().sessionId, QStringLiteral("session-1"));
    QCOMPARE(world.runtime.deliveries.size(), 0);
    QCOMPARE(world.store.schedule(QStringLiteral("s1"))->nextRunAt, nextDay);

    world.clock.now_ = at(2026, 11, 14, 8, 0);
    const int events = world.runtime.events.size();
    world.runner.alarmFired();
    runs = world.store.runsFor(QStringLiteral("s1"));
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.first().status, documents::RunStatus::Queued);
    QCOMPARE(world.runtime.events.size(), events);
    QCOMPARE(world.store.schedule(QStringLiteral("s1"))->nextRunAt,
             QStringLiteral("2026-11-15T08:00:00.000Z"));
}

void ScheduleRunnerTest::startsQueuedRunWhenTheTurnEnds() {
    World world;
    world.store.save(makeDaily(QStringLiteral("s1")));
    world.clock.now_ = at(2026, 11, 13, 8, 0);
    world.runner.alarmFired();
    const QString firstId = world.store.runsFor(QStringLiteral("s1")).first().id;

    world.runtime.setState(QStringLiteral("s1"), schedule::ConversationState::Busy,
                           QStringLiteral("session-1"));
    world.clock.now_ = at(2026, 11, 14, 8, 0);
    world.runner.alarmFired();
    QCOMPARE(world.store.runsFor(QStringLiteral("s1")).size(), 2);

    world.runtime.setState(QStringLiteral("s1"), schedule::ConversationState::Idle);
    world.runner.runFinished(firstId, true, {}, {});
    const auto runs = world.store.runsFor(QStringLiteral("s1"));
    QCOMPARE(runs.at(0).status, documents::RunStatus::Succeeded);
    QCOMPARE(runs.at(1).status, documents::RunStatus::Running);
    QCOMPARE(runs.at(1).sessionId, QStringLiteral("session-1"));
    QCOMPARE(world.runtime.deliveries.size(), 2);
}

void ScheduleRunnerTest::skipsWhileBusy() {
    World world;
    auto schedule = makeDaily(QStringLiteral("s1"));
    schedule.busyPolicy = documents::BusyPolicy::Skip;
    world.store.save(schedule);
    world.runtime.setState(QStringLiteral("s1"), schedule::ConversationState::Busy,
                           QStringLiteral("session-1"));
    world.clock.now_ = at(2026, 11, 13, 8, 0);
    world.runner.alarmFired();

    const auto runs = world.store.runsFor(QStringLiteral("s1"));
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.first().status, documents::RunStatus::Skipped);
    QCOMPARE(runs.first().errorCode, QString::fromLatin1(schedule::busyCode));
    QVERIFY(!runs.first().errorMessage.isEmpty());
    QCOMPARE(world.runtime.deliveries.size(), 0);
    QCOMPARE(world.store.schedule(QStringLiteral("s1"))->nextRunAt, nextDay);
}

void ScheduleRunnerTest::missedRunOnceAfterRestart() {
    World world;
    auto schedule = makeDaily(QStringLiteral("s1"));
    schedule.nextRunAt = QStringLiteral("2026-11-10T08:00:00.000Z");
    world.store.save(schedule);
    world.clock.now_ = at(2026, 11, 13, 9, 0);
    const auto parsed = schedule::parseRule(schedule.rule.toUtf8());
    QVERIFY(parsed.ok());
    QVERIFY(schedule::missedRuns(*parsed.rule, at(2026, 11, 10, 8, 0).addMSecs(-1),
                                 world.clock.now_) > 1);

    world.runner.start();
    const auto runs = world.store.runsFor(QStringLiteral("s1"));
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.first().status, documents::RunStatus::Running);
    QCOMPARE(runs.first().scheduledFor, QStringLiteral("2026-11-10T08:00:00.000Z"));
    QCOMPARE(world.runtime.deliveries.size(), 1);
    QCOMPARE(world.store.schedule(QStringLiteral("s1"))->nextRunAt, nextDay);
}

void ScheduleRunnerTest::missedSkipAfterRestart() {
    World world;
    auto schedule = makeDaily(QStringLiteral("s1"));
    schedule.missedPolicy = documents::MissedPolicy::Skip;
    schedule.nextRunAt = QStringLiteral("2026-11-10T08:00:00.000Z");
    world.store.save(schedule);
    world.clock.now_ = at(2026, 11, 13, 9, 0);
    world.runner.start();

    const auto runs = world.store.runsFor(QStringLiteral("s1"));
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.first().status, documents::RunStatus::Missed);
    QCOMPARE(runs.first().errorCode, QString::fromLatin1(schedule::missedCode));
    QCOMPARE(world.runtime.deliveries.size(), 0);
    QCOMPARE(world.store.schedule(QStringLiteral("s1"))->nextRunAt, nextDay);
    QVERIFY(world.store.schedule(QStringLiteral("s1"))->enabled);
}

void ScheduleRunnerTest::clockJumpFiresOnce() {
    World world;
    world.store.save(makeDaily(QStringLiteral("s1")));
    world.runner.start();
    QCOMPARE(world.store.runsFor(QStringLiteral("s1")).size(), 0);
    QVERIFY(world.alarm.delay_.count() > 1000);

    world.clock.now_ = at(2026, 11, 16, 10, 0);
    world.runner.noteClockChanged();
    QCOMPARE(world.store.runsFor(QStringLiteral("s1")).size(), 1);
    QCOMPARE(world.store.runsFor(QStringLiteral("s1")).first().status,
             documents::RunStatus::Running);
    QCOMPARE(world.store.schedule(QStringLiteral("s1"))->nextRunAt,
             QStringLiteral("2026-11-17T08:00:00.000Z"));
}

void ScheduleRunnerTest::treatsOneMinuteLateAsOnTime() {
    World world;
    auto onTime = makeDaily(QStringLiteral("on-time"));
    onTime.missedPolicy = documents::MissedPolicy::Skip;
    onTime.nextRunAt = stamp(at(2026, 11, 13, 7, 59));
    auto late = makeDaily(QStringLiteral("late"));
    late.missedPolicy = documents::MissedPolicy::Skip;
    late.target.sessionId = QStringLiteral("session-2");
    late.nextRunAt = stamp(at(2026, 11, 13, 7, 58, 59));
    world.store.save(onTime);
    world.store.save(late);
    world.clock.now_ = at(2026, 11, 13, 8, 0);
    world.runner.alarmFired();

    QCOMPARE(world.store.runsFor(QStringLiteral("on-time")).first().status,
             documents::RunStatus::Running);
    QCOMPARE(world.store.runsFor(QStringLiteral("late")).first().status,
             documents::RunStatus::Missed);
}

void ScheduleRunnerTest::onceDisablesItself() {
    World world;
    auto schedule = makeDaily(QStringLiteral("once"));
    schedule.rule = QStringLiteral(R"({"kind":"once","time_zone":"UTC","at":"2026-11-13T08:00"})");
    world.store.save(schedule);
    auto extra = makeDaily(QStringLiteral("later"));
    extra.rule = QStringLiteral(R"({"kind":"once","time_zone":"UTC","at":"2026-11-20T08:00"})");
    extra.nextRunAt = QStringLiteral("2026-11-20T08:00:00.000Z");
    extra.target.sessionId = QStringLiteral("session-2");
    world.store.save(extra);

    world.clock.now_ = at(2026, 11, 13, 8, 0);
    world.runner.alarmFired();
    const auto once = world.store.schedule(QStringLiteral("once"));
    QVERIFY(once);
    QVERIFY(!once->enabled);
    QVERIFY(once->nextRunAt.isEmpty());
    QCOMPARE(world.store.runsFor(QStringLiteral("once")).first().status,
             documents::RunStatus::Running);
    QVERIFY(!world.alarm.armed_ || world.store.schedule(QStringLiteral("later"))->enabled);

    const auto result = world.runner.runNow(QStringLiteral("later"));
    QVERIFY(result.ok);
    const auto later = world.store.schedule(QStringLiteral("later"));
    QVERIFY(later->enabled);
    QCOMPARE(later->nextRunAt, QStringLiteral("2026-11-20T08:00:00.000Z"));
}

void ScheduleRunnerTest::recordsFailures() {
    World world;
    world.clock.now_ = at(2026, 11, 13, 8, 0);
    const struct {
        const char* id;
        schedule::ConversationState state;
        const char* code;
    } cases[] = {
        {"provider", schedule::ConversationState::ProviderNotReady, schedule::providerNotReadyCode},
        {"signed", schedule::ConversationState::SignedOut, schedule::signedOutCode},
        {"deleted", schedule::ConversationState::Missing, schedule::targetDeletedCode},
        {"workspace", schedule::ConversationState::WorkspaceMissing,
         schedule::workspaceMissingCode},
    };
    for (const auto& item : cases) {
        world.store.save(makeDaily(QString::fromLatin1(item.id)));
        world.runtime.setState(QString::fromLatin1(item.id), item.state);
    }
    world.runner.alarmFired();
    QCOMPARE(world.runtime.deliveries.size(), 0);
    for (const auto& item : cases) {
        const auto runs = world.store.runsFor(QString::fromLatin1(item.id));
        QCOMPARE(runs.size(), 1);
        QCOMPARE(runs.first().status, documents::RunStatus::Failed);
        QCOMPARE(runs.first().errorCode, QString::fromLatin1(item.code));
        QVERIFY(!runs.first().errorMessage.isEmpty());
        const auto schedule = world.store.schedule(QString::fromLatin1(item.id));
        QVERIFY(schedule->enabled);
        QCOMPARE(schedule->nextRunAt, nextDay);
    }
}

void ScheduleRunnerTest::invalidRuleDisablesTheSchedule() {
    World world;
    auto schedule = makeDaily(QStringLiteral("s1"));
    schedule.rule = QStringLiteral("{");
    schedule.nextRunAt = QStringLiteral("2026-11-13T08:00:00.000Z");
    world.store.save(schedule);
    world.clock.now_ = at(2026, 11, 13, 8, 0);
    world.runner.alarmFired();
    const auto runs = world.store.runsFor(QStringLiteral("s1"));
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.first().status, documents::RunStatus::Failed);
    QCOMPARE(runs.first().errorCode, QString::fromLatin1(documents::invalidRuleCode));
    QVERIFY(!world.store.schedule(QStringLiteral("s1"))->enabled);
    world.runner.alarmFired();
    QCOMPARE(world.store.runsFor(QStringLiteral("s1")).size(), 1);
    QVERIFY(!world.alarm.armed_);
}

void ScheduleRunnerTest::runNowDoesNotConsumeTheRule() {
    World world;
    world.store.save(makeDaily(QStringLiteral("s1")));
    world.runner.start();
    const auto armed = world.alarm.delay_;
    const auto result = world.runner.runNow(QStringLiteral("s1"));
    QVERIFY(result.ok);
    QVERIFY(!result.runId.isEmpty());
    const auto runs = world.store.runsFor(QStringLiteral("s1"));
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.first().id, result.runId);
    QCOMPARE(runs.first().status, documents::RunStatus::Running);
    QCOMPARE(world.store.schedule(QStringLiteral("s1"))->nextRunAt,
             QStringLiteral("2026-11-13T08:00:00.000Z"));
    QVERIFY(world.store.schedule(QStringLiteral("s1"))->enabled);
    QCOMPARE(world.store.schedule(QStringLiteral("s1"))->lastRunAt, stamp(world.clock.now_));
    QCOMPARE(world.alarm.delay_, armed);

    auto disabled = makeDaily(QStringLiteral("off"));
    disabled.enabled = false;
    disabled.target.sessionId = QStringLiteral("session-2");
    world.store.save(disabled);
    const auto off = world.runner.runNow(QStringLiteral("off"));
    QVERIFY(off.ok);
    QVERIFY(!world.store.schedule(QStringLiteral("off"))->enabled);
}

void ScheduleRunnerTest::runNowWhileBusyReturnsTheQueuedRun() {
    World world;
    auto schedule = makeDaily(QStringLiteral("s1"));
    schedule.busyPolicy = documents::BusyPolicy::Skip;
    world.store.save(schedule);
    world.runtime.setState(QStringLiteral("s1"), schedule::ConversationState::Busy,
                           QStringLiteral("session-1"));
    const auto skipped = world.runner.runNow(QStringLiteral("s1"));
    QVERIFY(skipped.ok);
    QCOMPARE(world.store.runsFor(QStringLiteral("s1")).first().status,
             documents::RunStatus::Skipped);
    QCOMPARE(world.store.schedule(QStringLiteral("s1"))->nextRunAt,
             QStringLiteral("2026-11-13T08:00:00.000Z"));

    auto queued = makeDaily(QStringLiteral("q"));
    queued.target.sessionId = QStringLiteral("session-2");
    world.store.save(queued);
    world.runtime.setState(QStringLiteral("q"), schedule::ConversationState::Busy,
                           QStringLiteral("session-2"));
    const auto first = world.runner.runNow(QStringLiteral("q"));
    const auto second = world.runner.runNow(QStringLiteral("q"));
    QVERIFY(first.ok);
    QCOMPARE(second.runId, first.runId);
    QCOMPARE(world.store.runsFor(QStringLiteral("q")).size(), 1);
    QCOMPARE(world.store.runsFor(QStringLiteral("q")).first().status, documents::RunStatus::Queued);
}

void ScheduleRunnerTest::runNowOnAPausedScheduleStartsWhenTheTurnEnds() {
    World world;
    auto schedule = makeDaily(QStringLiteral("paused"));
    schedule.enabled = false;
    schedule.target.sessionId = QStringLiteral("session-1");
    world.store.save(schedule);
    world.runtime.setState(QStringLiteral("paused"), schedule::ConversationState::Busy,
                           QStringLiteral("session-1"));
    const auto queued = world.runner.runNow(QStringLiteral("paused"));
    QVERIFY(queued.ok);
    QCOMPARE(world.store.runsFor(QStringLiteral("paused")).first().status,
             documents::RunStatus::Queued);

    world.runtime.setState(QStringLiteral("paused"), schedule::ConversationState::Idle,
                           QStringLiteral("session-1"));
    world.runner.turnFinished(QStringLiteral("session-1"));
    QCOMPARE(world.store.runsFor(QStringLiteral("paused")).first().status,
             documents::RunStatus::Running);

    const auto again = world.runner.runNow(QStringLiteral("paused"));
    QVERIFY(again.ok);
    QVERIFY(again.runId != queued.runId);
}

void ScheduleRunnerTest::runNowUnknownId() {
    World world;
    const auto result = world.runner.runNow(QStringLiteral("missing"));
    QVERIFY(!result.ok);
    QCOMPARE(result.code, QString::fromLatin1(protocol::scheduleNotFoundCode));
    QVERIFY(result.runId.isEmpty());
}

void ScheduleRunnerTest::rearmsForTheEarliestSchedule() {
    World world;
    world.store.save(makeDaily(QStringLiteral("early")));
    auto later = makeDaily(QStringLiteral("later"));
    later.nextRunAt = QStringLiteral("2026-11-13T09:00:00.000Z");
    later.target.sessionId = QStringLiteral("session-2");
    world.store.save(later);
    world.runner.start();
    QCOMPARE(world.alarm.delay_.count(), 60LL * 60 * 1000);

    auto early = *world.store.schedule(QStringLiteral("early"));
    early.enabled = false;
    world.store.save(early);
    world.runner.schedulesChanged();
    QCOMPARE(world.alarm.delay_.count(), 2LL * 60 * 60 * 1000);

    auto both = *world.store.schedule(QStringLiteral("later"));
    both.enabled = false;
    world.store.save(both);
    world.runner.schedulesChanged();
    QVERIFY(!world.alarm.armed_);
}

void ScheduleRunnerTest::earlyAlarmDoesNotFire() {
    World world;
    world.store.save(makeDaily(QStringLiteral("s1")));
    world.runner.start();
    const auto delay = world.alarm.delay_;
    world.runner.alarmFired();
    QCOMPARE(world.store.runsFor(QStringLiteral("s1")).size(), 0);
    QCOMPARE(world.alarm.delay_, delay);
    QVERIFY(world.alarm.armed_);
}

void ScheduleRunnerTest::fillsNextWhenCreated() {
    World world;
    auto schedule = makeDaily(QStringLiteral("s1"));
    schedule.nextRunAt.clear();
    world.store.save(schedule);
    world.runner.schedulesChanged();
    QCOMPARE(world.store.runsFor(QStringLiteral("s1")).size(), 0);
    QCOMPARE(world.store.schedule(QStringLiteral("s1"))->nextRunAt,
             QStringLiteral("2026-11-13T08:00:00.000Z"));
    QCOMPARE(world.alarm.delay_.count(), 60LL * 60 * 1000);

    world.clock.now_ = at(2026, 11, 13, 9, 0);
    auto later = makeDaily(QStringLiteral("later"));
    later.nextRunAt.clear();
    later.target.sessionId = QStringLiteral("session-2");
    world.store.save(later);
    world.runner.schedulesChanged();
    QCOMPARE(world.store.runsFor(QStringLiteral("later")).size(), 0);
    QCOMPARE(world.store.schedule(QStringLiteral("later"))->nextRunAt, nextDay);

    auto once = makeDaily(QStringLiteral("once"));
    once.rule = QStringLiteral(R"({"kind":"once","time_zone":"UTC","at":"2026-11-13T08:00"})");
    once.nextRunAt.clear();
    once.target.sessionId = QStringLiteral("session-3");
    world.store.save(once);
    world.runner.schedulesChanged();
    QVERIFY(!world.store.schedule(QStringLiteral("once"))->enabled);
    QCOMPARE(world.store.runsFor(QStringLiteral("once")).size(), 0);
}

void ScheduleRunnerTest::resumesQueuedRunOnStart() {
    World world;
    auto schedule = makeDaily(QStringLiteral("s1"));
    schedule.nextRunAt = nextDay;
    world.store.save(schedule);
    documents::Run queued;
    queued.scheduleId = QStringLiteral("s1");
    queued.status = documents::RunStatus::Queued;
    queued.sessionId = QStringLiteral("session-1");
    queued.scheduledFor = QStringLiteral("2026-11-13T08:00:00.000Z");
    world.store.insertRun(queued);
    world.runner.start();
    QCOMPARE(world.store.runsFor(QStringLiteral("s1")).first().status,
             documents::RunStatus::Running);
    QCOMPARE(world.runtime.deliveries.size(), 1);
}

void ScheduleRunnerTest::restartFailsARunningRunAndAQueuedRunWhoseTurnIsGone() {
    World world;
    auto schedule = makeDaily(QStringLiteral("s1"));
    schedule.nextRunAt = nextDay;
    world.store.save(schedule);
    documents::Run running;
    running.id = QStringLiteral("running");
    running.scheduleId = QStringLiteral("s1");
    running.status = documents::RunStatus::Running;
    running.sessionId = QStringLiteral("session-1");
    running.scheduledFor = QStringLiteral("2026-11-13T08:00:00.000Z");
    running.startedAt = QStringLiteral("2026-11-13T08:00:01.000Z");
    world.store.insertRun(running);
    documents::Run gone;
    gone.id = QStringLiteral("queued-gone");
    gone.scheduleId = QStringLiteral("s1");
    gone.status = documents::RunStatus::Queued;
    gone.sessionId = QStringLiteral("session-gone");
    gone.scheduledFor = QStringLiteral("2026-11-13T08:00:00.000Z");
    world.store.insertRun(gone);
    documents::Run waiting;
    waiting.id = QStringLiteral("queued-live");
    waiting.scheduleId = QStringLiteral("s1");
    waiting.status = documents::RunStatus::Queued;
    waiting.sessionId = QStringLiteral("session-1");
    waiting.scheduledFor = QStringLiteral("2026-11-13T08:05:00.000Z");
    world.store.insertRun(waiting);

    world.runner.abandonRunsWithoutATurn(
        [](const documents::Run& run) { return run.sessionId == QStringLiteral("session-1"); });

    const auto failedRunning = world.store.run(QStringLiteral("running"));
    const auto failedGone = world.store.run(QStringLiteral("queued-gone"));
    const auto stillQueued = world.store.run(QStringLiteral("queued-live"));
    QVERIFY(failedRunning && failedGone && stillQueued);
    QCOMPARE(failedRunning->status, documents::RunStatus::Failed);
    QCOMPARE(failedGone->status, documents::RunStatus::Failed);
    QCOMPARE(failedRunning->errorCode, QString::fromLatin1(schedule::runtimeStoppedCode));
    QCOMPARE(failedGone->errorCode, QString::fromLatin1(schedule::runtimeStoppedCode));
    QVERIFY(!failedRunning->finishedAt.isEmpty());
    QVERIFY(!failedGone->finishedAt.isEmpty());
    QCOMPARE(stillQueued->status, documents::RunStatus::Queued);
    QCOMPARE(world.store.schedule(QStringLiteral("s1"))->nextRunAt, nextDay);
    QCOMPARE(world.runtime.events.size(), 2);
    QCOMPARE(world.runtime.events.first().id, QStringLiteral("running"));
    QCOMPARE(world.runtime.events.last().id, QStringLiteral("queued-gone"));
}

void ScheduleRunnerTest::deliversTeamTargets() {
    World world;
    auto team = makeDaily(QStringLiteral("team"));
    team.target.kind = documents::TargetKind::Team;
    team.target.sessionId.clear();
    team.target.teamId = QStringLiteral("team-9");
    world.store.save(team);
    auto start = makeDaily(QStringLiteral("start"));
    start.target.kind = documents::TargetKind::TeamStart;
    start.target.sessionId.clear();
    start.target.teamId = QStringLiteral("team-9");
    start.prompt.clear();
    start.rule = QStringLiteral(R"({"kind":"once","time_zone":"UTC","at":"2026-11-13T08:00"})");
    world.store.save(start);
    world.clock.now_ = at(2026, 11, 13, 8, 0);
    world.runner.alarmFired();

    QCOMPARE(world.runtime.deliveries.size(), 2);
    QCOMPARE(world.store.runsFor(QStringLiteral("team")).first().status,
             documents::RunStatus::Running);
    QVERIFY(world.store.runsFor(QStringLiteral("team"))
                .first()
                .sessionId.startsWith(QStringLiteral("session-team-")));
    QCOMPARE(world.runtime.deliveries.first().message.value(QStringLiteral("text")).toString(),
             QStringLiteral("Recheck CI"));
    QVERIFY(!world.store.schedule(QStringLiteral("start"))->enabled);
    QCOMPARE(world.runtime.deliveries.at(1).message.value(QStringLiteral("text")).toString(),
             QString());
}

void ScheduleRunnerTest::capsTheAlarmDelay() {
    QCOMPARE(schedule::cappedAlarmDelay(std::chrono::hours{5}).count(), 60LL * 60 * 1000);
    QCOMPARE(schedule::cappedAlarmDelay(std::chrono::minutes{30}).count(), 30LL * 60 * 1000);
    QCOMPARE(schedule::cappedAlarmDelay(std::chrono::milliseconds{-5}).count(), 0);
}

void ScheduleRunnerTest::qtAlarmCanArmAndDisarm() {
    schedule::QtScheduleAlarm alarm;
    bool fired = false;
    alarm.setHandler([&fired] { fired = true; });
    alarm.arm(std::chrono::hours{5});
    alarm.disarm();
    QVERIFY(!fired);
}

} // namespace ketplus::daemon

QTEST_GUILESS_MAIN(ketplus::daemon::ScheduleRunnerTest)
#include "ScheduleRunnerTest.moc"
