#include "schedule/JournalScheduleStore.h"

#include "storage/LocalJournal.h"

#include <QUuid>

namespace documents = ::ketplus::schedule;

namespace ketplus::daemon {
namespace {

ScheduleRecord toRecord(const documents::Schedule& schedule) {
    ScheduleRecord record;
    record.id = schedule.id;
    record.name = schedule.name;
    record.enabled = schedule.enabled;
    record.status = schedule.status;
    record.prompt = schedule.prompt;
    record.ruleJson = schedule.rule;
    record.busyPolicy = documents::nameOf(schedule.busyPolicy);
    record.missedPolicy = documents::nameOf(schedule.missedPolicy);
    record.createdBy = schedule.createdBy;
    record.createdAt = schedule.createdAt;
    record.updatedAt = schedule.updatedAt;
    record.nextRunAt = schedule.nextRunAt;
    record.lastRunAt = schedule.lastRunAt;
    record.target.kind = documents::nameOf(schedule.target.kind);
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

std::optional<documents::Schedule> fromRecord(const ScheduleRecord& record) {
    const auto kind = documents::targetKindFromName(record.target.kind);
    const auto busy = documents::busyPolicyFromName(record.busyPolicy);
    const auto missed = documents::missedPolicyFromName(record.missedPolicy);
    if (!kind || !busy || !missed)
        return std::nullopt;
    documents::Schedule schedule;
    schedule.id = record.id;
    schedule.name = record.name;
    schedule.enabled = record.enabled;
    schedule.status = record.status;
    schedule.prompt = record.prompt;
    schedule.rule = record.ruleJson;
    schedule.busyPolicy = *busy;
    schedule.missedPolicy = *missed;
    schedule.createdBy = record.createdBy;
    schedule.createdAt = record.createdAt;
    schedule.updatedAt = record.updatedAt;
    schedule.nextRunAt = record.nextRunAt;
    schedule.lastRunAt = record.lastRunAt;
    schedule.target.kind = *kind;
    schedule.target.sessionId = record.target.sessionId;
    schedule.target.teamId = record.target.teamId;
    schedule.target.workspace = record.target.workspacePath;
    schedule.target.provider = record.target.provider;
    schedule.target.model = record.target.model;
    schedule.target.effort = record.target.effort;
    schedule.target.isolatedWorktree = record.target.isolatedWorktree;
    schedule.target.titleTemplate = record.target.titleTemplate;
    return schedule;
}

documents::Run fromRun(const ScheduleRunRecord& record) {
    documents::Run run;
    run.id = record.id;
    run.scheduleId = record.scheduleId;
    run.scheduledFor = record.scheduledFor;
    run.startedAt = record.startedAt;
    run.finishedAt = record.finishedAt;
    run.status = documents::runStatusFromName(record.status).value_or(documents::RunStatus::Queued);
    run.sessionId = record.sessionId;
    run.turnId = record.turnReference;
    run.errorCode = record.errorCode;
    run.errorMessage = record.errorMessage;
    return run;
}

ScheduleRunRecord toRun(const documents::Run& run) {
    ScheduleRunRecord record;
    record.id = run.id;
    record.scheduleId = run.scheduleId;
    record.scheduledFor = run.scheduledFor;
    record.startedAt = run.startedAt;
    record.finishedAt = run.finishedAt;
    record.status = documents::nameOf(run.status);
    record.sessionId = run.sessionId;
    record.turnReference = run.turnId;
    record.errorCode = run.errorCode;
    record.errorMessage = run.errorMessage;
    return record;
}

} // namespace

JournalScheduleStore::JournalScheduleStore(LocalJournal* journal) : journal_(journal) {}

QList<documents::Schedule> JournalScheduleStore::schedules() const {
    QList<documents::Schedule> list;
    const auto records = journal_->schedules(ScheduleFilter{});
    if (!records)
        return list;
    for (const ScheduleRecord& record : *records) {
        if (const auto schedule = fromRecord(record))
            list.append(*schedule);
    }
    return list;
}

std::optional<documents::Schedule> JournalScheduleStore::schedule(const QString& id) const {
    const auto record = journal_->schedule(id);
    if (!record)
        return std::nullopt;
    return fromRecord(*record);
}

bool JournalScheduleStore::save(const documents::Schedule& schedule) {
    const auto existing = journal_->schedule(schedule.id);
    if (!existing)
        return false;
    ScheduleRecord record = toRecord(schedule);
    record.target.repositoryPath = existing->target.repositoryPath;
    QString error;
    return journal_->updateSchedule(record, &error);
}

documents::Run JournalScheduleStore::insertRun(documents::Run run) {
    if (run.id.isEmpty())
        run.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString error;
    if (!journal_->appendScheduleRun(toRun(run), &error))
        return {};
    return run;
}

bool JournalScheduleStore::saveRun(const documents::Run& run) {
    QString error;
    return journal_->updateScheduleRun(toRun(run), &error);
}

std::optional<documents::Run> JournalScheduleStore::run(const QString& id) const {
    const auto record = journal_->scheduleRun(id);
    if (!record)
        return std::nullopt;
    return fromRun(*record);
}

std::optional<documents::Run> JournalScheduleStore::queuedRun(const QString& scheduleId) const {
    const auto record = journal_->queuedScheduleRun(scheduleId);
    if (!record)
        return std::nullopt;
    return fromRun(*record);
}

} // namespace ketplus::daemon
