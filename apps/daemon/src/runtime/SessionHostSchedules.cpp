#include "runtime/SessionHost.h"

#include "protocol/ScheduleProtocol.h"

#include <QUuid>

namespace ketplus::daemon {
namespace {

QJsonObject failed(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

QJsonObject failed(const schedule::Failure& failure) {
    return failed(failure.code, failure.message);
}

ScheduleRecord toRecord(const schedule::Schedule& schedule) {
    ScheduleRecord record;
    record.id = schedule.id;
    record.name = schedule.name;
    record.enabled = schedule.enabled;
    record.prompt = schedule.prompt;
    record.ruleJson = schedule.rule;
    record.busyPolicy = schedule::nameOf(schedule.busyPolicy);
    record.missedPolicy = schedule::nameOf(schedule.missedPolicy);
    record.createdBy = schedule.createdBy;
    record.createdAt = schedule.createdAt;
    record.updatedAt = schedule.updatedAt;
    record.nextRunAt = schedule.nextRunAt;
    record.lastRunAt = schedule.lastRunAt;
    record.target.kind = schedule::nameOf(schedule.target.kind);
    record.target.sessionId = schedule.target.sessionId;
    record.target.teamId = schedule.target.teamId;
    record.target.workspacePath = schedule.target.workspace;
    record.target.provider = schedule.target.provider;
    record.target.model = schedule.target.model;
    record.target.effort = schedule.target.effort;
    record.target.isolatedWorktree = schedule.target.isolatedWorktree;
    record.target.titleTemplate = schedule.target.titleTemplate;
    return record;
}

std::optional<schedule::Schedule> fromRecord(const ScheduleRecord& record,
                                             schedule::Failure* failure) {
    schedule::Schedule schedule;
    const auto kind = schedule::targetKindFromName(record.target.kind);
    if (!kind) {
        if (failure)
            *failure = {QString::fromLatin1(schedule::invalidEnumCode),
                        QStringLiteral("Stored schedule target kind is invalid.")};
        return std::nullopt;
    }
    schedule.id = record.id;
    schedule.name = record.name;
    schedule.enabled = record.enabled;
    schedule.prompt = record.prompt;
    schedule.target.kind = *kind;
    schedule.target.sessionId = record.target.sessionId;
    schedule.target.teamId = record.target.teamId;
    schedule.target.workspace = record.target.workspacePath;
    schedule.target.provider = record.target.provider;
    schedule.target.model = record.target.model;
    schedule.target.effort = record.target.effort;
    schedule.target.isolatedWorktree = record.target.isolatedWorktree;
    schedule.target.titleTemplate = record.target.titleTemplate;
    schedule.rule = record.ruleJson;
    const auto busy = schedule::busyPolicyFromName(record.busyPolicy);
    const auto missed = schedule::missedPolicyFromName(record.missedPolicy);
    if (!busy || !missed) {
        if (failure)
            *failure = {QString::fromLatin1(schedule::invalidEnumCode),
                        QStringLiteral("Stored schedule policy is invalid.")};
        return std::nullopt;
    }
    schedule.busyPolicy = *busy;
    schedule.missedPolicy = *missed;
    schedule.createdBy = record.createdBy;
    schedule.createdAt = record.createdAt;
    schedule.updatedAt = record.updatedAt;
    schedule.nextRunAt = record.nextRunAt;
    schedule.lastRunAt = record.lastRunAt;
    schedule.status = record.status;
    // Re-check the stored document with the shared parser so a bad row is not sent.
    return schedule::scheduleFromJson(schedule::toJson(schedule), failure);
}

std::optional<qint64> canonicalTeamId(const QString& text) {
    if (text.isEmpty() || !text.front().isDigit())
        return std::nullopt;
    bool ok = false;
    const qint64 value = text.toLongLong(&ok);
    if (!ok || value <= 0 || QString::number(value) != text)
        return std::nullopt;
    return value;
}

// A conversation or team target must name a row the cascade can find. A schedule whose
// conversation is already recorded as deleted may still be renamed or pointed elsewhere.
QJsonObject requireLiveTarget(LocalJournal& journal, const schedule::Schedule& schedule,
                              const ScheduleRecord* previous) {
    if (schedule.target.kind == schedule::TargetKind::Conversation) {
        const bool recordedGone =
            previous && previous->status == QLatin1String(protocol::scheduleTargetDeletedCode) &&
            previous->target.kind == QLatin1String("conversation") &&
            previous->target.sessionId == schedule.target.sessionId;
        if (!recordedGone && !journal.sessionExists(schedule.target.sessionId))
            return failed(QString::fromLatin1(protocol::scheduleTargetNotFoundCode),
                          QStringLiteral("This schedule's conversation does not exist."));
        return {};
    }
    if (schedule.target.kind != schedule::TargetKind::Team &&
        schedule.target.kind != schedule::TargetKind::TeamStart)
        return {};
    const auto teamId = canonicalTeamId(schedule.target.teamId);
    if (!teamId || !journal.teamExists(*teamId))
        return failed(QString::fromLatin1(protocol::scheduleTargetNotFoundCode),
                      QStringLiteral("This schedule's team does not exist."));
    return {};
}

schedule::Run fromRun(const ScheduleRunRecord& record) {
    schedule::Run run;
    run.id = record.id;
    run.scheduleId = record.scheduleId;
    run.scheduledFor = record.scheduledFor;
    run.startedAt = record.startedAt;
    run.finishedAt = record.finishedAt;
    run.status = schedule::runStatusFromName(record.status).value_or(schedule::RunStatus::Queued);
    run.sessionId = record.sessionId;
    run.turnId = record.turnReference;
    run.errorCode = record.errorCode;
    run.errorMessage = record.errorMessage;
    return run;
}

} // namespace

QJsonObject SessionHost::scheduleCommand(const QString& type, const QJsonObject& request) {
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    schedule::Failure failure;
    const auto journalError = [&](const QString& message) {
        return failed(QStringLiteral("journal_error"),
                      message.isEmpty() ? journal_->errorString() : message);
    };
    const auto changed = [&](const QString& change, const schedule::Schedule& schedule) {
        emit scheduleEvent(QString::fromLatin1(protocol::scheduleChangedEvent),
                           protocol::scheduleChangedPayload(change, schedule));
    };
    const auto load = [&](const QString& id) -> std::optional<ScheduleRecord> {
        auto record = journal_->schedule(id);
        if (!record && !journal_->errorString().isEmpty())
            failure = {QStringLiteral("journal_error"), journal_->errorString()};
        return record;
    };

    if (type == QLatin1String(protocol::scheduleListType)) {
        const auto query = protocol::parseScheduleList(payload, &failure);
        if (!query)
            return failed(failure);
        ScheduleFilter filter;
        if (query->targetKind)
            filter.targetKind = schedule::nameOf(*query->targetKind);
        filter.sessionId = query->sessionId;
        filter.teamId = query->teamId;
        const auto records = journal_->schedules(filter);
        if (!records)
            return journalError({});
        QList<schedule::Schedule> schedules;
        for (const ScheduleRecord& record : *records) {
            const auto schedule = fromRecord(record, &failure);
            if (!schedule)
                return failed(failure);
            schedules.append(*schedule);
        }
        return protocol::scheduleListResult(schedules);
    }

    if (type == QLatin1String(protocol::scheduleGetType)) {
        const auto id = protocol::parseScheduleId(payload, &failure);
        if (!id)
            return failed(failure);
        const auto record = load(*id);
        if (!failure.ok())
            return failed(failure);
        if (!record)
            return failed(QString::fromLatin1(protocol::scheduleNotFoundCode),
                          QStringLiteral("Schedule was not found."));
        const auto schedule = fromRecord(*record, &failure);
        if (!schedule)
            return failed(failure);
        return protocol::scheduleDocumentResult(*schedule);
    }

    if (type == QLatin1String(protocol::scheduleCreateType)) {
        auto created = protocol::parseScheduleCreate(payload, &failure);
        if (!created)
            return failed(failure);
        created->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        // ADR 0126: the client cannot choose created_by. schedule.* is only
        // accepted on a person's connection, so this is "user". An agent
        // conversation id belongs to a later agent tool, not this request.
        created->createdBy = QStringLiteral("user");
        if (const QJsonObject missing = requireLiveTarget(*journal_, *created, nullptr);
            !missing.isEmpty())
            return missing;
        QString error;
        if (!journal_->createSchedule(toRecord(*created), &error))
            return journalError(error);
        const auto record = load(created->id);
        if (!failure.ok())
            return failed(failure);
        if (!record)
            return journalError(QStringLiteral("The schedule was not stored."));
        const auto schedule = fromRecord(*record, &failure);
        if (!schedule)
            return failed(failure);
        changed(QString::fromLatin1(protocol::scheduleCreatedChange), *schedule);
        noteSchedulesChanged();
        return protocol::scheduleDocumentResult(*schedule);
    }

    if (type == QLatin1String(protocol::scheduleUpdateType)) {
        const auto update = protocol::parseScheduleUpdate(payload, &failure);
        if (!update)
            return failed(failure);
        const auto record = load(update->id);
        if (!failure.ok())
            return failed(failure);
        if (!record)
            return failed(QString::fromLatin1(protocol::scheduleNotFoundCode),
                          QStringLiteral("Schedule was not found."));
        auto merged = fromRecord(*record, &failure);
        if (!merged)
            return failed(failure);
        if (update->name)
            merged->name = *update->name;
        if (update->enabled)
            merged->enabled = *update->enabled;
        if (update->prompt)
            merged->prompt = *update->prompt;
        if (update->target)
            merged->target = *update->target;
        if (update->rule)
            merged->rule = *update->rule;
        if (update->busyPolicy)
            merged->busyPolicy = *update->busyPolicy;
        if (update->missedPolicy)
            merged->missedPolicy = *update->missedPolicy;
        auto checked = schedule::scheduleFromJson(schedule::toJson(*merged), &failure);
        if (!checked)
            return failed(failure);
        if (const QJsonObject missing = requireLiveTarget(*journal_, *checked, &(*record));
            !missing.isEmpty())
            return missing;
        ScheduleRecord next = toRecord(*checked);
        const bool sameConversation = checked->target.kind == schedule::TargetKind::Conversation &&
                                      record->target.kind == QLatin1String("conversation") &&
                                      checked->target.sessionId == record->target.sessionId;
        next.status = sameConversation ? record->status : QString{};
        if (next.status == QLatin1String(protocol::scheduleTargetDeletedCode) && next.enabled)
            return failed(QString::fromLatin1(protocol::scheduleTargetDeletedCode),
                          QStringLiteral("This schedule's conversation was deleted."));
        QString error;
        if (!journal_->updateSchedule(next, &error))
            return journalError(error);
        const auto stored = load(update->id);
        if (!failure.ok())
            return failed(failure);
        if (!stored)
            return journalError(QStringLiteral("The schedule was not stored."));
        const auto schedule = fromRecord(*stored, &failure);
        if (!schedule)
            return failed(failure);
        changed(QString::fromLatin1(protocol::scheduleUpdatedChange), *schedule);
        noteSchedulesChanged();
        return protocol::scheduleDocumentResult(*schedule);
    }

    if (type == QLatin1String(protocol::scheduleDeleteType)) {
        const auto id = protocol::parseScheduleId(payload, &failure);
        if (!id)
            return failed(failure);
        const auto record = load(*id);
        if (!failure.ok())
            return failed(failure);
        if (!record)
            return failed(QString::fromLatin1(protocol::scheduleNotFoundCode),
                          QStringLiteral("Schedule was not found."));
        const auto schedule = fromRecord(*record, &failure);
        if (!schedule)
            return failed(failure);
        QString error;
        if (!journal_->deleteSchedule(*id, &error))
            return journalError(error);
        changed(QString::fromLatin1(protocol::scheduleDeletedChange), *schedule);
        noteSchedulesChanged();
        return protocol::scheduleDeletedResult(*id);
    }

    if (type == QLatin1String(protocol::scheduleSetEnabledType)) {
        const auto enabled = protocol::parseScheduleSetEnabled(payload, &failure);
        if (!enabled)
            return failed(failure);
        const auto record = load(enabled->id);
        if (!failure.ok())
            return failed(failure);
        if (!record)
            return failed(QString::fromLatin1(protocol::scheduleNotFoundCode),
                          QStringLiteral("Schedule was not found."));
        QString error;
        QString code;
        if (!journal_->setScheduleEnabled(enabled->id, enabled->enabled, &error, &code)) {
            if (code == QLatin1String(protocol::scheduleTargetDeletedCode))
                return failed(QString::fromLatin1(protocol::scheduleTargetDeletedCode), error);
            if (code == QLatin1String(protocol::scheduleNotFoundCode))
                return failed(QString::fromLatin1(protocol::scheduleNotFoundCode), error);
            return journalError(error);
        }
        const auto stored = load(enabled->id);
        if (!failure.ok())
            return failed(failure);
        if (!stored)
            return journalError(QStringLiteral("The schedule was not stored."));
        const auto schedule = fromRecord(*stored, &failure);
        if (!schedule)
            return failed(failure);
        changed(QString::fromLatin1(protocol::scheduleUpdatedChange), *schedule);
        noteSchedulesChanged();
        return protocol::scheduleDocumentResult(*schedule);
    }

    if (type == QLatin1String(protocol::scheduleRunNowType)) {
        const auto id = protocol::parseScheduleId(payload, &failure);
        if (!id)
            return failed(failure);
        return runScheduleNow(*id);
    }

    if (type == QLatin1String(protocol::scheduleRunsType)) {
        const auto query = protocol::parseScheduleRuns(payload, &failure);
        if (!query)
            return failed(failure);
        const auto record = load(query->id);
        if (!failure.ok())
            return failed(failure);
        if (!record)
            return failed(QString::fromLatin1(protocol::scheduleNotFoundCode),
                          QStringLiteral("Schedule was not found."));
        const auto page = journal_->scheduleRuns(query->id, query->limit, query->beforeId);
        if (!page)
            return journalError(journal_->errorString());
        QList<schedule::Run> runs;
        for (const ScheduleRunRecord& run : page->runs)
            runs.append(fromRun(run));
        return protocol::scheduleRunsResult(runs, !page->nextBefore.isEmpty());
    }

    return failed(QString::fromLatin1(schedule::invalidPayloadCode),
                  QStringLiteral("Unknown schedule request."));
}

void SessionHost::publishScheduleChange(const QString& change, const ScheduleRecord& record) {
    schedule::Failure failure;
    const auto schedule = fromRecord(record, &failure);
    if (!schedule)
        return;
    emit scheduleEvent(QString::fromLatin1(protocol::scheduleChangedEvent),
                       protocol::scheduleChangedPayload(change, *schedule));
}

void SessionHost::publishDisabledSessionSchedules(const QString& sessionId) {
    const auto rows = journal_->schedules(ScheduleFilter{.sessionId = sessionId});
    if (!rows)
        return;
    for (const ScheduleRecord& record : *rows)
        if (record.target.kind == QLatin1String("conversation"))
            publishScheduleChange(QString::fromLatin1(protocol::scheduleUpdatedChange), record);
    noteSchedulesChanged();
}

void SessionHost::schedulesDeleted(const QList<ScheduleRecord>& schedules) {
    for (const ScheduleRecord& record : schedules)
        publishScheduleChange(QString::fromLatin1(protocol::scheduleDeletedChange), record);
    noteSchedulesChanged();
}

} // namespace ketplus::daemon

#include "schedule/JournalScheduleStore.h"
#include "schedule/ScheduleRunner.h"

#include <QFileInfo>

namespace ketplus::daemon {

class HostScheduleRuntime final : public schedule::ScheduleRuntime {
  public:
    explicit HostScheduleRuntime(SessionHost* host) : host_(host) {}

    schedule::Inspection inspect(const ::ketplus::schedule::Schedule& schedule) const override {
        schedule::Inspection inspection;
        using Kind = ::ketplus::schedule::TargetKind;
        if (schedule.target.kind == Kind::Conversation) {
            if (!host_->journal_->sessionExists(schedule.target.sessionId)) {
                inspection.state = schedule::ConversationState::Missing;
                return inspection;
            }
            const auto it = host_->sessions_.constFind(schedule.target.sessionId);
            if (it == host_->sessions_.cend()) {
                inspection.state = schedule::ConversationState::ProviderNotReady;
                return inspection;
            }
            const QString state = it->stored.state;
            if (it->startingTurn || state == QLatin1String("running") ||
                state == QLatin1String("starting")) {
                inspection.state = schedule::ConversationState::Busy;
                inspection.sessionId = schedule.target.sessionId;
                return inspection;
            }
            return inspection;
        }
        if (schedule.target.kind == Kind::NewConversation) {
            if (schedule.target.workspace.trimmed().isEmpty() ||
                !QFileInfo(schedule.target.workspace).isDir())
                inspection.state = schedule::ConversationState::WorkspaceMissing;
            else
                inspection.state = schedule::ConversationState::ProviderNotReady;
            return inspection;
        }
        inspection.state = schedule::ConversationState::ProviderNotReady;
        return inspection;
    }

    schedule::Delivery deliver(const ::ketplus::schedule::Schedule& schedule,
                               const ::ketplus::schedule::Run& run) override {
        schedule::Delivery delivery;
        const auto it = host_->sessions_.constFind(schedule.target.sessionId);
        if (schedule.target.kind != ::ketplus::schedule::TargetKind::Conversation ||
            it == host_->sessions_.cend()) {
            delivery.errorCode = QString::fromLatin1(schedule::providerNotReadyCode);
            delivery.errorMessage = QStringLiteral("The provider is not ready.");
            return delivery;
        }
        const QJsonObject result = host_->startTurn(
            {{QStringLiteral("message_type"), QStringLiteral("session.start_turn")},
             {QStringLiteral("session_id"), schedule.target.sessionId},
             {QStringLiteral("session_generation"), it->stored.generation},
             {QStringLiteral("payload"),
              QJsonObject{
                  {QStringLiteral("prompt"), schedule.prompt},
                  {QStringLiteral("display_text"), schedule.prompt},
                  {QStringLiteral("schedule_id"), schedule.id},
                  {QStringLiteral("run_id"), run.id},
                  {QStringLiteral("expected_context_revision"), it->stored.contextRevision}}}},
            true);
        if (!result.value(QStringLiteral("ok")).toBool()) {
            delivery.errorCode = result.value(QStringLiteral("error_code"))
                                     .toString(QString::fromLatin1(schedule::providerNotReadyCode));
            delivery.errorMessage = result.value(QStringLiteral("error_message")).toString();
            if (delivery.errorMessage.isEmpty())
                delivery.errorMessage = QStringLiteral("The provider is not ready.");
            return delivery;
        }
        delivery.sessionId =
            result.value(QStringLiteral("session_id")).toString(schedule.target.sessionId);
        delivery.turnId = result.value(QStringLiteral("turn_id")).toString();
        return delivery;
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
    SessionHost* host_;
};

struct SessionHost::ScheduleEngine {
    schedule::SystemScheduleClock clock;
    schedule::QtScheduleAlarm alarm;
    JournalScheduleStore store;
    HostScheduleRuntime runtime;
    schedule::ScheduleRunner runner;

    ScheduleEngine(SessionHost* host, LocalJournal* journal)
        : store(journal), runtime(host), runner(clock, alarm, store, runtime) {}
};

void SessionHost::startScheduleRunner() {
    if (scheduleEngine_ != nullptr || journal_ == nullptr)
        return;
    scheduleEngine_ = new ScheduleEngine(this, journal_);
    scheduleEngine_->runner.start();
}

void SessionHost::stopScheduleRunner() {
    delete scheduleEngine_;
    scheduleEngine_ = nullptr;
}

void SessionHost::noteSchedulesChanged() {
    if (scheduleEngine_ != nullptr)
        scheduleEngine_->runner.schedulesChanged();
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
