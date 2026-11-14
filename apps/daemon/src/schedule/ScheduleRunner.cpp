#include "schedule/ScheduleRunner.h"

#include "protocol/ScheduleProtocol.h"
#include "schedule/ScheduleRule.h"

#include <QTimer>

#include <algorithm>
#include <utility>

namespace ketplus::daemon::schedule {
namespace {

namespace documents = ::ketplus::schedule;

QString stamp(const QDateTime& datetime) { return datetime.toUTC().toString(Qt::ISODateWithMs); }

QDateTime parseStamp(const QString& text) {
    if (text.isEmpty())
        return {};
    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid())
        parsed = QDateTime::fromString(text, Qt::ISODate);
    return parsed.isValid() ? parsed.toUTC() : QDateTime{};
}

bool dueIsLate(const QDateTime& due, const QDateTime& now) {
    return due.msecsTo(now) > onTimeGrace.count();
}

QString codeFor(ConversationState state) {
    switch (state) {
    case ConversationState::ProviderNotReady:
        return QString::fromLatin1(providerNotReadyCode);
    case ConversationState::SignedOut:
        return QString::fromLatin1(signedOutCode);
    case ConversationState::Missing:
        return QString::fromLatin1(targetDeletedCode);
    case ConversationState::WorkspaceMissing:
        return QString::fromLatin1(workspaceMissingCode);
    case ConversationState::Busy:
        return QString::fromLatin1(busyCode);
    case ConversationState::Idle:
        break;
    }
    return {};
}

QString messageFor(ConversationState state) {
    switch (state) {
    case ConversationState::ProviderNotReady:
        return QStringLiteral("The provider is not ready.");
    case ConversationState::SignedOut:
        return QStringLiteral("The provider is signed out.");
    case ConversationState::Missing:
        return QStringLiteral("The schedule target no longer exists.");
    case ConversationState::WorkspaceMissing:
        return QStringLiteral("The workspace is missing.");
    case ConversationState::Busy:
        return QStringLiteral("The target conversation is busy.");
    case ConversationState::Idle:
        break;
    }
    return {};
}

QStringList scheduleIds(const ScheduleStore& store) {
    QStringList ids;
    for (const auto& schedule : store.schedules())
        ids.append(schedule.id);
    return ids;
}

} // namespace

std::chrono::milliseconds cappedAlarmDelay(std::chrono::milliseconds delay) {
    if (delay <= std::chrono::milliseconds::zero())
        return std::chrono::milliseconds::zero();
    return std::min(delay, maximumAlarmDelay);
}

struct QtScheduleAlarm::Timer {
    explicit Timer(QtScheduleAlarm* owner) : owner(owner) {
        timer.setSingleShot(true);
        // CoarseTimer can be about 5% late. A 60-minute cap would then miss the 60-second
        // on-time window and record a skip-policy fire as missed.
        timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&timer, &QTimer::timeout, &timer, [this] { this->owner->notify(); });
    }

    QtScheduleAlarm* owner{nullptr};
    QTimer timer;
};

QtScheduleAlarm::QtScheduleAlarm() : timer_(std::make_unique<Timer>(this)) {}

QtScheduleAlarm::~QtScheduleAlarm() = default;

void QtScheduleAlarm::arm(std::chrono::milliseconds delay) {
    timer_->timer.start(cappedAlarmDelay(delay));
}

void QtScheduleAlarm::disarm() { timer_->timer.stop(); }

bool QtScheduleAlarm::armed() const { return timer_ && timer_->timer.isActive(); }

void QtScheduleAlarm::notify() {
    if (handler_)
        handler_();
}

ScheduleRunner::ScheduleRunner(ScheduleClock& clock, ScheduleAlarm& alarm, ScheduleStore& store,
                               ScheduleRuntime& runtime)
    : clock_(clock), alarm_(alarm), store_(store), runtime_(runtime) {
    alarm_.setHandler([this] { alarmFired(); });
}

ScheduleRunner::~ScheduleRunner() { alarm_.setHandler({}); }

void ScheduleRunner::start() {
    fillMissingNext();
    resumeQueued();
    reconcile();
    arm();
}

void ScheduleRunner::noteClockChanged() {
    reconcile();
    arm();
}

void ScheduleRunner::schedulesChanged() {
    fillMissingNext();
    resumeQueued();
    reconcile();
    arm();
}

void ScheduleRunner::alarmFired() {
    reconcile();
    arm();
}

void ScheduleRunner::turnFinished(const QString& sessionId) {
    promoteWaitingOn(sessionId, QString());
    arm();
}

void ScheduleRunner::runFinished(const QString& runId, bool succeeded, const QString& errorCode,
                                 const QString& errorMessage) {
    auto stored = store_.run(runId);
    if (!stored)
        return;
    const QDateTime now = clock_.now();
    stored->status = succeeded ? documents::RunStatus::Succeeded : documents::RunStatus::Failed;
    stored->finishedAt = stamp(now);
    if (succeeded) {
        stored->errorCode.clear();
        stored->errorMessage.clear();
    } else {
        stored->errorCode = errorCode;
        stored->errorMessage = errorMessage;
    }
    store_.saveRun(*stored);
    runtime_.emitRun(*stored);
    promoteWaitingOn(stored->sessionId, stored->scheduleId);
    arm();
}

RunNowResult ScheduleRunner::runNow(const QString& scheduleId) {
    RunNowResult result;
    const auto loaded = store_.schedule(scheduleId);
    if (!loaded) {
        result.code = QString::fromLatin1(protocol::scheduleNotFoundCode);
        result.message = QStringLiteral("No schedule has that id.");
        return result;
    }
    const QDateTime now = clock_.now();
    const Run run = fire(*loaded, now, now, false);
    result.ok = true;
    result.runId = run.id;
    arm();
    return result;
}

void ScheduleRunner::fillMissingNext() {
    const QDateTime now = clock_.now();
    for (const QString& id : scheduleIds(store_)) {
        auto loaded = store_.schedule(id);
        if (!loaded || !loaded->enabled || !loaded->nextRunAt.isEmpty())
            continue;
        const ParseResult parsed = parseRule(loaded->rule.toUtf8());
        const auto next = parsed.ok() ? nextRunAfter(*parsed.rule, now) : std::nullopt;
        if (!parsed.ok() || !next) {
            loaded->enabled = false;
            loaded->nextRunAt.clear();
        } else {
            loaded->nextRunAt = stamp(*next);
        }
        publish(*loaded);
    }
}

void ScheduleRunner::reconcile() {
    const QDateTime now = clock_.now();
    for (const QString& id : scheduleIds(store_)) {
        const auto loaded = store_.schedule(id);
        if (!loaded || !loaded->enabled || loaded->nextRunAt.isEmpty())
            continue;
        const QDateTime due = parseStamp(loaded->nextRunAt);
        if (!due.isValid()) {
            (void)finish(*loaded, now, now, documents::RunStatus::Failed,
                         QString::fromLatin1(documents::invalidRuleCode),
                         QStringLiteral("next_run_at is not a timestamp."), true);
            continue;
        }
        if (due > now)
            continue;
        if (dueIsLate(due, now) && loaded->missedPolicy == documents::MissedPolicy::Skip) {
            (void)finish(*loaded, due, now, documents::RunStatus::Missed,
                         QString::fromLatin1(missedCode),
                         QStringLiteral("The scheduled run was missed."), true);
            continue;
        }
        (void)fire(*loaded, due, now, true);
    }
}

void ScheduleRunner::resumeQueued() {
    for (const auto& schedule : store_.schedules()) {
        if (const auto queued = store_.queuedRun(schedule.id))
            promote(schedule, *queued);
    }
}

void ScheduleRunner::promoteWaitingOn(const QString& sessionId, const QString& scheduleId) {
    for (const auto& schedule : store_.schedules()) {
        const auto queued = store_.queuedRun(schedule.id);
        if (!queued)
            continue;
        const bool sameSchedule = !scheduleId.isEmpty() && schedule.id == scheduleId;
        const bool waitingHere = !sessionId.isEmpty() && (queued->sessionId == sessionId ||
                                                          schedule.target.sessionId == sessionId);
        if (sameSchedule || waitingHere)
            promote(schedule, *queued);
    }
}

void ScheduleRunner::arm() {
    const QDateTime now = clock_.now();
    std::optional<QDateTime> earliest;
    for (const auto& schedule : store_.schedules()) {
        if (!schedule.enabled)
            continue;
        const QDateTime due = parseStamp(schedule.nextRunAt);
        if (!due.isValid() || due <= now)
            continue;
        if (!earliest || due < *earliest)
            earliest = due;
    }
    if (!earliest) {
        alarm_.disarm();
        return;
    }
    alarm_.arm(std::chrono::milliseconds{now.msecsTo(*earliest)});
}

ScheduleRunner::Run ScheduleRunner::fire(Schedule schedule, const QDateTime& due,
                                         const QDateTime& now, bool consume) {
    const ParseResult parsed = parseRule(schedule.rule.toUtf8());
    if (!parsed.ok()) {
        return finish(std::move(schedule), due, now, documents::RunStatus::Failed,
                      QString::fromLatin1(documents::invalidRuleCode), parsed.message, true);
    }
    const Inspection inspection = runtime_.inspect(schedule);
    if (inspection.state == ConversationState::Busy) {
        if (schedule.busyPolicy == documents::BusyPolicy::Skip) {
            return finish(std::move(schedule), due, now, documents::RunStatus::Skipped,
                          QString::fromLatin1(busyCode),
                          QStringLiteral("The target conversation is busy."), consume);
        }
        if (const auto queued = store_.queuedRun(schedule.id)) {
            if (consume)
                consumeOccurrence(std::move(schedule), now);
            return *queued;
        }
        return enqueue(std::move(schedule), due, now, inspection.sessionId, consume);
    }
    if (inspection.state != ConversationState::Idle) {
        return finish(std::move(schedule), due, now, documents::RunStatus::Failed,
                      codeFor(inspection.state), messageFor(inspection.state), consume);
    }
    return deliverNew(std::move(schedule), due, now, consume);
}

ScheduleRunner::Run ScheduleRunner::finish(Schedule schedule, const QDateTime& due,
                                           const QDateTime& now, documents::RunStatus status,
                                           const QString& code, const QString& message,
                                           bool consume) {
    Run run;
    run.scheduleId = schedule.id;
    run.scheduledFor = stamp(due);
    run.startedAt = stamp(now);
    run.finishedAt = stamp(now);
    run.status = status;
    run.errorCode = code;
    run.errorMessage = message;
    run = store_.insertRun(std::move(run));
    runtime_.emitRun(run);
    persistOccurrence(std::move(schedule), now, consume);
    return run;
}

ScheduleRunner::Run ScheduleRunner::enqueue(Schedule schedule, const QDateTime& due,
                                            const QDateTime& now, const QString& waitSessionId,
                                            bool consume) {
    Run run;
    run.scheduleId = schedule.id;
    run.scheduledFor = stamp(due);
    run.status = documents::RunStatus::Queued;
    // While queued, sessionId is the conversation whose turn must end.
    run.sessionId = waitSessionId;
    run = store_.insertRun(std::move(run));
    runtime_.emitRun(run);
    persistOccurrence(std::move(schedule), now, consume);
    return run;
}

ScheduleRunner::Run ScheduleRunner::deliverNew(Schedule schedule, const QDateTime& due,
                                               const QDateTime& now, bool consume) {
    Run run;
    run.scheduleId = schedule.id;
    run.scheduledFor = stamp(due);
    run.startedAt = stamp(now);
    run.status = documents::RunStatus::Running;
    run = store_.insertRun(std::move(run));
    const Delivery delivery = runtime_.deliver(schedule, run);
    run = applyDelivery(std::move(run), delivery, now);
    persistOccurrence(std::move(schedule), now, consume);
    return run;
}

void ScheduleRunner::promote(Schedule schedule, Run run) {
    // A paused schedule still owes a run that runNow already accepted. Leaving it queued
    // blocks every later runNow, which returns that same row.
    const auto current = store_.queuedRun(schedule.id);
    if (!current || current->id != run.id)
        return;
    const QDateTime now = clock_.now();
    const Inspection inspection = runtime_.inspect(schedule);
    if (inspection.state == ConversationState::Busy)
        return;
    if (inspection.state != ConversationState::Idle) {
        Run failed = *current;
        failed.status = documents::RunStatus::Failed;
        failed.startedAt = failed.startedAt.isEmpty() ? stamp(now) : failed.startedAt;
        failed.finishedAt = stamp(now);
        failed.errorCode = codeFor(inspection.state);
        failed.errorMessage = messageFor(inspection.state);
        store_.saveRun(failed);
        runtime_.emitRun(failed);
        return;
    }
    Run running = *current;
    running.status = documents::RunStatus::Running;
    running.startedAt = stamp(now);
    const Delivery delivery = runtime_.deliver(schedule, running);
    (void)applyDelivery(std::move(running), delivery, now);
}

ScheduleRunner::Run ScheduleRunner::applyDelivery(Run run, const Delivery& delivery,
                                                  const QDateTime& now) {
    if (!delivery.errorCode.isEmpty()) {
        run.status = documents::RunStatus::Failed;
        run.finishedAt = stamp(now);
        run.errorCode = delivery.errorCode;
        run.errorMessage = delivery.errorMessage;
    } else {
        run.status = documents::RunStatus::Running;
        run.sessionId = delivery.sessionId;
        run.turnId = delivery.turnId;
        run.errorCode.clear();
        run.errorMessage.clear();
    }
    store_.saveRun(run);
    runtime_.emitRun(run);
    return run;
}

void ScheduleRunner::persistOccurrence(Schedule schedule, const QDateTime& now, bool consume) {
    if (consume)
        consumeOccurrence(std::move(schedule), now);
    else
        touchLast(std::move(schedule), now);
}

void ScheduleRunner::touchLast(Schedule schedule, const QDateTime& now) {
    schedule.lastRunAt = stamp(now);
    publish(std::move(schedule));
}

void ScheduleRunner::consumeOccurrence(Schedule schedule, const QDateTime& now) {
    const ParseResult parsed = parseRule(schedule.rule.toUtf8());
    const bool once = parsed.ok() && parsed.rule->kind == Kind::Once;
    const auto next = parsed.ok() ? nextRunAfter(*parsed.rule, now) : std::nullopt;
    if (!parsed.ok() || once || !next) {
        schedule.enabled = false;
        schedule.nextRunAt.clear();
    } else {
        schedule.nextRunAt = stamp(*next);
    }
    schedule.lastRunAt = stamp(now);
    publish(std::move(schedule));
}

void ScheduleRunner::publish(Schedule schedule) {
    schedule.updatedAt = stamp(clock_.now());
    if (store_.save(schedule))
        runtime_.emitChanged(QString::fromLatin1(protocol::scheduleUpdatedChange), schedule);
}

} // namespace ketplus::daemon::schedule
