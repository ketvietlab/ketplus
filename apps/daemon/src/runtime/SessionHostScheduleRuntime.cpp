#include "runtime/SessionHost.h"

#include "protocol/ScheduleProtocol.h"
#include "schedule/JournalScheduleStore.h"
#include "schedule/ScheduleRule.h"
#include "schedule/ScheduleRunner.h"
#include "schedule/TeamStartRuntime.h"
#include "teams/Team.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QTimer>

#include <cstdint>

#ifdef Q_OS_MACOS
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#endif

namespace ketplus::daemon {
namespace {

#ifdef Q_OS_MACOS
struct WakeWatch {
    void close() {
        host = nullptr;
        if (notifier != 0) {
            IODeregisterForSystemPower(&notifier);
            notifier = 0;
        }
        if (port != nullptr) {
            IONotificationPortDestroy(port);
            port = nullptr;
        }
        connection = 0;
    }

    io_connect_t connection{0};
    IONotificationPortRef port{nullptr};
    io_object_t notifier{0};
    SessionHost* host{nullptr};
};

void powerCallback(void* refcon, io_service_t, uint32_t messageType, void* argument) {
    auto* watch = static_cast<WakeWatch*>(refcon);
    if (watch == nullptr)
        return;
    if (messageType == kIOMessageSystemHasPoweredOn) {
        if (watch->host != nullptr)
            QMetaObject::invokeMethod(watch->host, "noteScheduleClockChanged",
                                      Qt::QueuedConnection);
        return;
    }
    if (messageType == kIOMessageCanSystemSleep || messageType == kIOMessageSystemWillSleep)
        IOAllowPowerChange(watch->connection,
                           static_cast<long>(reinterpret_cast<std::intptr_t>(argument)));
}
#endif

} // namespace

class HostScheduleRuntime final : public schedule::ScheduleRuntime {
  public:
    explicit HostScheduleRuntime(SessionHost* host) : host_(host) {}

    schedule::Inspection inspect(const ::ketplus::schedule::Schedule& schedule) const override {
        using Kind = ::ketplus::schedule::TargetKind;
        if (schedule.target.kind == Kind::Conversation)
            return conversation(schedule.target.sessionId);
        if (schedule.target.kind == Kind::NewConversation)
            return fresh(schedule);
        if (schedule.target.kind == Kind::TeamStart)
            return schedule::inspectTeamStart(host_->teams_, schedule);
        if (schedule.target.kind == Kind::Team)
            return teamRun(schedule);
        return leader(schedule);
    }

    schedule::Delivery deliver(const ::ketplus::schedule::Schedule& schedule,
                               const ::ketplus::schedule::Run& run) override {
        using Kind = ::ketplus::schedule::TargetKind;
        if (schedule.target.kind == Kind::TeamStart)
            return schedule::deliverTeamStart(host_->teams_, schedule);
        if (schedule.target.kind == Kind::Team)
            return deliverTeamRun(schedule, run);
        const QJsonObject tagged = schedule::scheduledUserMessage(schedule, run);
        QString prompt = tagged.value(QStringLiteral("text")).toString();
        if (schedule.target.kind == Kind::NewConversation)
            return deliverNew(schedule, run, prompt);
        const QString sessionId = schedule.target.kind == Kind::Conversation
                                      ? schedule.target.sessionId
                                      : leaderSessionId(schedule);
        return deliverTo(sessionId, schedule, run, prompt);
    }

    void emitRun(const ::ketplus::schedule::Run& run) override {
        emit host_->scheduleEvent(QString::fromLatin1(protocol::scheduleRunEvent),
                                  protocol::scheduleRunPayload(run));
    }

    void emitChanged(const QString& change,
                     const ::ketplus::schedule::Schedule& schedule) override {
        emit host_->scheduleEvent(QString::fromLatin1(protocol::scheduleChangedEvent),
                                  protocol::scheduleChangedPayload(change, schedule));
    }

  private:
    schedule::Inspection conversation(const QString& sessionId) const {
        schedule::Inspection inspection;
        if (!host_->journal_->sessionExists(sessionId)) {
            inspection.state = schedule::ConversationState::Missing;
            return inspection;
        }
        const auto it = host_->sessions_.constFind(sessionId);
        if (it == host_->sessions_.cend()) {
            inspection.state = schedule::ConversationState::ProviderNotReady;
            return inspection;
        }
        const QString state = it->stored.state;
        if (it->startingTurn || state == QLatin1String("running") ||
            state == QLatin1String("starting")) {
            inspection.state = schedule::ConversationState::Busy;
            inspection.sessionId = sessionId;
            return inspection;
        }
        inspection.sessionId = sessionId;
        return inspection;
    }

    schedule::Inspection fresh(const ::ketplus::schedule::Schedule& schedule) const {
        schedule::Inspection inspection;
        if (schedule.target.workspace.trimmed().isEmpty() ||
            !QFileInfo(schedule.target.workspace).isDir())
            inspection.state = schedule::ConversationState::WorkspaceMissing;
        return inspection;
    }

    schedule::Inspection teamRun(const ::ketplus::schedule::Schedule& schedule) const {
        schedule::Inspection inspection;
        if (host_->teams_ == nullptr) {
            inspection.state = schedule::ConversationState::Missing;
            return inspection;
        }
        const teams::TeamCoordinator::TeamRunLook look =
            host_->teams_->lookAtTeamRun(schedule.target.teamId.toLongLong());
        if (look.gate == teams::TeamCoordinator::TeamRunGate::Busy) {
            inspection.state = schedule::ConversationState::Busy;
            inspection.sessionId = look.sessionId;
            return inspection;
        }
        if (look.gate == teams::TeamCoordinator::TeamRunGate::Missing) {
            inspection.state = schedule::ConversationState::Missing;
            return inspection;
        }
        return inspection;
    }

    schedule::Delivery deliverTeamRun(const ::ketplus::schedule::Schedule& schedule,
                                      const ::ketplus::schedule::Run& run) {
        schedule::Delivery delivery;
        if (host_->teams_ == nullptr) {
            delivery.errorCode = QString::fromLatin1(schedule::targetDeletedCode);
            delivery.errorMessage = QStringLiteral("The schedule target no longer exists.");
            return delivery;
        }
        QString when;
        QDateTime due = QDateTime::fromString(run.scheduledFor, Qt::ISODateWithMs);
        if (!due.isValid())
            due = QDateTime::fromString(run.scheduledFor, Qt::ISODate);
        if (due.isValid()) {
            due = due.toUTC();
            const auto parsed = schedule::parseRule(schedule.rule.toUtf8());
            if (parsed.ok())
                due = due.toTimeZone(parsed.rule->timeZone);
            when = due.toString(QStringLiteral("yyyy-MM-dd"));
        }
        const teams::TeamCoordinator::TeamRunDelivery sent =
            host_->teams_->deliverTeamRun(schedule.target.teamId.toLongLong(), schedule.name,
                                          schedule.prompt, when, schedule.id, run.id);
        delivery.sessionId = sent.sessionId;
        delivery.errorCode = sent.errorCode;
        delivery.errorMessage = sent.errorMessage;
        delivery.finished = sent.errorCode.isEmpty();
        return delivery;
    }

    schedule::Inspection leader(const ::ketplus::schedule::Schedule& schedule) const {
        schedule::Inspection inspection;
        const QString sessionId = leaderSessionId(schedule);
        if (sessionId.isEmpty()) {
            inspection.state = schedule::ConversationState::Missing;
            return inspection;
        }
        return conversation(sessionId);
    }

    [[nodiscard]] QString leaderSessionId(const ::ketplus::schedule::Schedule& schedule) const {
        if (host_->teams_ == nullptr)
            return {};
        bool ok = false;
        const qint64 teamId = schedule.target.teamId.toLongLong(&ok);
        const teams::Team* team = ok ? host_->teams_->team(teamId) : nullptr;
        if (team == nullptr)
            return {};
        const teams::Member* leader = team->memberWithRole(teams::role::leader);
        return leader == nullptr ? QString{} : leader->sessionId;
    }

    schedule::Delivery deliverNew(const ::ketplus::schedule::Schedule& schedule,
                                  const ::ketplus::schedule::Run& run, const QString& prompt) {
        QJsonObject payload{{QStringLiteral("prompt"), prompt},
                            {QStringLiteral("display_text"), prompt},
                            {QStringLiteral("schedule_id"), schedule.id},
                            {QStringLiteral("run_id"), run.id},
                            {QStringLiteral("worktree_path"), schedule.target.workspace}};
        QJsonObject options;
        if (!schedule.target.provider.isEmpty())
            options.insert(QStringLiteral("provider"), schedule.target.provider);
        if (!schedule.target.model.isEmpty())
            options.insert(QStringLiteral("model"), schedule.target.model);
        if (!schedule.target.effort.isEmpty())
            options.insert(QStringLiteral("effort"), schedule.target.effort);
        if (!options.isEmpty())
            payload.insert(QStringLiteral("options"), options);
        auto delivery = accepted(host_->startTurn({{QStringLiteral("message_type"),
                                                    QStringLiteral("session.start_turn")},
                                                   {QStringLiteral("payload"), payload}},
                                                  true),
                                 run);
        if (delivery.errorCode.isEmpty() && !schedule.target.titleTemplate.isEmpty()) {
            const auto created = host_->sessions_.find(delivery.sessionId);
            if (created != host_->sessions_.end())
                (void)host_->updateSessionTitle(*created, schedule.target.titleTemplate, true);
        }
        return delivery;
    }

    schedule::Delivery deliverTo(const QString& sessionId,
                                 const ::ketplus::schedule::Schedule& schedule,
                                 const ::ketplus::schedule::Run& run, const QString& prompt) {
        const auto it = host_->sessions_.constFind(sessionId);
        if (sessionId.isEmpty() || it == host_->sessions_.cend()) {
            schedule::Delivery delivery;
            delivery.errorCode = QString::fromLatin1(schedule::providerNotReadyCode);
            delivery.errorMessage = QStringLiteral("The provider is not ready.");
            return delivery;
        }
        return accepted(host_->startTurn(
                            {{QStringLiteral("message_type"), QStringLiteral("session.start_turn")},
                             {QStringLiteral("session_id"), sessionId},
                             {QStringLiteral("session_generation"), it->stored.generation},
                             {QStringLiteral("payload"),
                              QJsonObject{{QStringLiteral("prompt"), prompt},
                                          {QStringLiteral("display_text"), prompt},
                                          {QStringLiteral("schedule_id"), schedule.id},
                                          {QStringLiteral("run_id"), run.id},
                                          {QStringLiteral("expected_context_revision"),
                                           it->stored.contextRevision}}}},
                            true),
                        run);
    }

    schedule::Delivery accepted(const QJsonObject& result, const ::ketplus::schedule::Run& run) {
        schedule::Delivery delivery;
        if (!result.value(QStringLiteral("ok")).toBool()) {
            delivery.errorCode = result.value(QStringLiteral("error_code"))
                                     .toString(QString::fromLatin1(schedule::providerNotReadyCode));
            delivery.errorMessage = result.value(QStringLiteral("error_message")).toString();
            if (delivery.errorMessage.isEmpty())
                delivery.errorMessage = QStringLiteral("The provider is not ready.");
            return delivery;
        }
        delivery.sessionId = result.value(QStringLiteral("session_id")).toString();
        delivery.turnId = result.value(QStringLiteral("turn_id")).toString();
        if (!delivery.sessionId.isEmpty())
            remember(delivery.sessionId, run.id);
        return delivery;
    }

    void remember(const QString& sessionId, const QString& runId);

    SessionHost* host_;
};

class OverridableScheduleClock final : public schedule::ScheduleClock {
  public:
    [[nodiscard]] QDateTime now() const override {
        return pinned_.isValid() ? pinned_ : QDateTime::currentDateTimeUtc();
    }
    void pin(const QDateTime& instant) { pinned_ = instant.toUTC(); }

  private:
    QDateTime pinned_;
};

struct SessionHost::ScheduleEngine {
    OverridableScheduleClock clock;
    schedule::QtScheduleAlarm alarm;
    JournalScheduleStore store;
    HostScheduleRuntime runtime;
    schedule::ScheduleRunner runner;
    QTimer clockWatch;
    QDateTime clockSample;
    QHash<QString, QString> runningBySession;
#ifdef Q_OS_MACOS
    WakeWatch wake;
#endif

    ScheduleEngine(SessionHost* host, LocalJournal* journal)
        : store(journal), runtime(host), runner(clock, alarm, store, runtime) {}

    ~ScheduleEngine() {
        clockWatch.stop();
#ifdef Q_OS_MACOS
        wake.close();
#endif
    }

    void watchClock(SessionHost* host) {
        clockSample = QDateTime::currentDateTimeUtc();
        clockWatch.setTimerType(Qt::PreciseTimer);
        clockWatch.setInterval(60'000);
        QObject::connect(&clockWatch, &QTimer::timeout, host, [this, host] {
            const QDateTime now = QDateTime::currentDateTimeUtc();
            // The timer itself uses monotonic time, so a wall-clock jump shows up as a
            // gap larger than this interval. Sleep is caught separately, on wake.
            if (clockSample.isValid() && clockSample.msecsTo(now) > 70'000)
                host->noteScheduleClockChanged();
            clockSample = now;
        });
        clockWatch.start();
#ifdef Q_OS_MACOS
        wake.host = host;
        wake.connection =
            IORegisterForSystemPower(&wake, &wake.port, &powerCallback, &wake.notifier);
        if (wake.connection != 0 && wake.port != nullptr) {
            CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(wake.port),
                               kCFRunLoopCommonModes);
        }
#endif
    }
};

void HostScheduleRuntime::remember(const QString& sessionId, const QString& runId) {
    if (host_->scheduleEngine_ != nullptr)
        host_->scheduleEngine_->runningBySession.insert(sessionId, runId);
}

void SessionHost::startScheduleRunner() {
    if (scheduleEngine_ != nullptr || journal_ == nullptr)
        return;
    scheduleEngine_ = new ScheduleEngine(this, journal_);
    // Sessions that were running are already interrupted, so their turn id is gone.
    // A queued run for one of those turns must not start; an idle session still owes
    // its queued run and runner.start resumes that one.
    scheduleEngine_->runner.abandonRunsWithoutATurn([this](const ::ketplus::schedule::Run& run) {
        const auto session = sessions_.constFind(run.sessionId);
        return !run.sessionId.isEmpty() && session != sessions_.cend() &&
               session->stored.state != QStringLiteral("interrupted");
    });
    scheduleEngine_->runner.start();
    scheduleEngine_->watchClock(this);
}

void SessionHost::stopScheduleRunner() {
    delete scheduleEngine_;
    scheduleEngine_ = nullptr;
}

void SessionHost::scheduledTeamMayStart(const qint64 teamId) {
    if (scheduleEngine_ != nullptr)
        scheduleEngine_->runner.turnFinished(QStringLiteral("team:") + QString::number(teamId));
}

void SessionHost::noteSchedulesChanged() {
    if (scheduleEngine_ != nullptr)
        scheduleEngine_->runner.schedulesChanged();
}

void SessionHost::noteScheduleClockChanged() {
    if (scheduleEngine_ != nullptr)
        scheduleEngine_->runner.noteClockChanged();
}

void SessionHost::setScheduleTimeForTest(const QDateTime& now) {
    if (scheduleEngine_ == nullptr)
        startScheduleRunner();
    if (scheduleEngine_ == nullptr)
        return;
    scheduleEngine_->clock.pin(now);
    scheduleEngine_->runner.noteClockChanged();
}

void SessionHost::noteScheduleTurnEnded(const QString& sessionId, bool succeeded,
                                        const QString& errorCode, const QString& errorMessage) {
    if (scheduleEngine_ == nullptr)
        return;
    const QString runId = scheduleEngine_->runningBySession.take(sessionId);
    if (!runId.isEmpty())
        scheduleEngine_->runner.runFinished(runId, succeeded, errorCode, errorMessage);
    scheduleEngine_->runner.turnFinished(sessionId);
}

QJsonObject SessionHost::runScheduleNow(const QString& id) {
    if (scheduleEngine_ == nullptr)
        startScheduleRunner();
    const schedule::RunNowResult result = scheduleEngine_->runner.runNow(id);
    if (!result.ok)
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("error_code"), result.code},
                {QStringLiteral("error_message"), result.message}};
    const auto run = scheduleEngine_->store.run(result.runId);
    if (!run)
        return {
            {QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), QStringLiteral("journal_error")},
            {QStringLiteral("error_message"), QStringLiteral("The schedule run was not stored.")}};
    return protocol::scheduleRunResult(*run);
}

} // namespace ketplus::daemon
