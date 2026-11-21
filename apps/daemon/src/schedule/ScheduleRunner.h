#pragma once

#include "Schedule.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace ketplus::daemon::schedule {

// One timer for the earliest enabled schedule. Storage and the turn host plug in
// behind ScheduleStore and ScheduleRuntime; this unit does not open the journal or
// call SessionHost. Call it on the daemon thread and do not re-enter it from the
// runtime callbacks.

inline constexpr auto busyCode = "busy";
inline constexpr auto missedCode = "missed";
inline constexpr auto providerNotReadyCode = "provider_not_ready";
inline constexpr auto signedOutCode = "signed_out";
inline constexpr auto targetDeletedCode = "target_deleted";
inline constexpr auto workspaceMissingCode = "workspace_missing";
// A run left running, or queued for a turn this process no longer has.
inline constexpr auto runtimeStoppedCode = "runtime_stopped";

// A fire this close to nextRunAt is on time, including a restart inside the minute.
inline constexpr std::chrono::milliseconds onTimeGrace{60'000};
// QTimer is one-shot and capped; an early fire re-arms. This is not a poll.
inline constexpr std::chrono::milliseconds maximumAlarmDelay{60LL * 60 * 1000};

[[nodiscard]] std::chrono::milliseconds cappedAlarmDelay(std::chrono::milliseconds delay);

// The user.message a future host journals for a prompt delivery. Wire names are snake_case.
[[nodiscard]] inline QJsonObject scheduledUserMessage(const ::ketplus::schedule::Schedule& schedule,
                                                      const ::ketplus::schedule::Run& run) {
    return QJsonObject{{QStringLiteral("text"), schedule.prompt},
                       {QStringLiteral("schedule_id"), schedule.id},
                       {QStringLiteral("run_id"), run.id}};
}

class ScheduleClock {
  public:
    virtual ~ScheduleClock() = default;
    [[nodiscard]] virtual QDateTime now() const = 0;
};

class SystemScheduleClock final : public ScheduleClock {
  public:
    [[nodiscard]] QDateTime now() const override { return QDateTime::currentDateTimeUtc(); }
};

class ScheduleAlarm {
  public:
    virtual ~ScheduleAlarm() = default;
    virtual void arm(std::chrono::milliseconds delay) = 0;
    virtual void disarm() = 0;
    void setHandler(std::function<void()> handler) { handler_ = std::move(handler); }

  protected:
    std::function<void()> handler_;
};

// Production alarm. Tests inject a fake and never construct this.
class QtScheduleAlarm final : public ScheduleAlarm {
  public:
    QtScheduleAlarm();
    ~QtScheduleAlarm() override;

    void arm(std::chrono::milliseconds delay) override;
    void disarm() override;

  private:
    struct Timer;
    std::unique_ptr<Timer> timer_;
    void notify();
};

enum class ConversationState { Idle, Busy, Missing, ProviderNotReady, SignedOut, WorkspaceMissing };

struct Inspection final {
    ConversationState state{ConversationState::Idle};
    // While Busy, the conversation whose turn must end before a queued run can start.
    QString sessionId;
};

struct Delivery final {
    QString sessionId;
    QString turnId;
    QString errorCode;
    QString errorMessage;
};

class ScheduleStore {
  public:
    virtual ~ScheduleStore() = default;
    [[nodiscard]] virtual QList<::ketplus::schedule::Schedule> schedules() const = 0;
    [[nodiscard]] virtual std::optional<::ketplus::schedule::Schedule>
    schedule(const QString& id) const = 0;
    virtual bool save(const ::ketplus::schedule::Schedule& schedule) = 0;
    // Assigns id when it is empty, then stores the run.
    virtual ::ketplus::schedule::Run insertRun(::ketplus::schedule::Run run) = 0;
    virtual bool saveRun(const ::ketplus::schedule::Run& run) = 0;
    [[nodiscard]] virtual std::optional<::ketplus::schedule::Run> run(const QString& id) const = 0;
    [[nodiscard]] virtual std::optional<::ketplus::schedule::Run>
    queuedRun(const QString& scheduleId) const = 0;
    // Runs still queued or running. A restart closes the ones whose turn is gone.
    [[nodiscard]] virtual QList<::ketplus::schedule::Run> unfinishedRuns() const = 0;
};

class ScheduleRuntime {
  public:
    virtual ~ScheduleRuntime() = default;
    [[nodiscard]] virtual Inspection
    inspect(const ::ketplus::schedule::Schedule& schedule) const = 0;
    // Journal scheduledUserMessage and start the turn, or start the team. Do not re-enter
    // the runner. After a successful delivery, a later inspect of that conversation is Busy
    // until the turn ends.
    virtual Delivery deliver(const ::ketplus::schedule::Schedule& schedule,
                             const ::ketplus::schedule::Run& run) = 0;
    virtual void emitRun(const ::ketplus::schedule::Run& run) = 0;
    virtual void emitChanged(const QString& change,
                             const ::ketplus::schedule::Schedule& schedule) = 0;
};

struct RunNowResult final {
    bool ok{false};
    QString runId;
    QString code;
    QString message;
};

class ScheduleRunner final {
  public:
    ScheduleRunner(ScheduleClock& clock, ScheduleAlarm& alarm, ScheduleStore& store,
                   ScheduleRuntime& runtime);
    ~ScheduleRunner();

    // Arm the timer, catch up missed fires, and resume a stored queued run.
    void start();
    // Fail every running run, and every queued run whose turn is gone, with
    // runtime_stopped. Does not move nextRunAt. A queued run whose turn is still
    // in progress stays queued.
    void abandonRunsWithoutATurn(
        const std::function<bool(const ::ketplus::schedule::Run&)>& turnStillExists);
    // Wake, or a wall-clock jump. One catch-up or one missed record per schedule.
    void noteClockChanged();
    // schedule.create, update, delete or setEnabled.
    void schedulesChanged();
    void alarmFired();
    // The busy conversation's turn ended.
    void turnFinished(const QString& sessionId);
    // The provider finished the turn this run started.
    void runFinished(const QString& runId, bool succeeded, const QString& errorCode,
                     const QString& errorMessage);
    // An extra run. Does not move nextRunAt and does not disable a once schedule.
    [[nodiscard]] RunNowResult runNow(const QString& scheduleId);

  private:
    using Schedule = ::ketplus::schedule::Schedule;
    using Run = ::ketplus::schedule::Run;

    void fillMissingNext();
    void reconcile();
    void resumeQueued();
    void promoteWaitingOn(const QString& sessionId, const QString& scheduleId);
    void arm();
    [[nodiscard]] Run fire(Schedule schedule, const QDateTime& due, const QDateTime& now,
                           bool consume);
    [[nodiscard]] Run finish(Schedule schedule, const QDateTime& due, const QDateTime& now,
                             ::ketplus::schedule::RunStatus status, const QString& code,
                             const QString& message, bool consume);
    [[nodiscard]] Run enqueue(Schedule schedule, const QDateTime& due, const QDateTime& now,
                              const QString& waitSessionId, bool consume);
    [[nodiscard]] Run deliverNew(Schedule schedule, const QDateTime& due, const QDateTime& now,
                                 bool consume);
    void promote(Schedule schedule, Run run);
    [[nodiscard]] Run applyDelivery(Run run, const Delivery& delivery, const QDateTime& now);
    void persistOccurrence(Schedule schedule, const QDateTime& now, bool consume);
    void touchLast(Schedule schedule, const QDateTime& now);
    void consumeOccurrence(Schedule schedule, const QDateTime& now);
    void publish(Schedule schedule);

    ScheduleClock& clock_;
    ScheduleAlarm& alarm_;
    ScheduleStore& store_;
    ScheduleRuntime& runtime_;
};

} // namespace ketplus::daemon::schedule
