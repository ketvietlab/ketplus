#include "runner/jobs/LedgerRecordStore.h"

#include "runner/jobs/RemoteJobRecordJson.h"
#include "runner/jobs/RemoteJobTransitions.h"
#include "storage/BackgroundTaskLedger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSqlQuery>

namespace ketplus::daemon::runner::jobs {

namespace {

Q_LOGGING_CATEGORY(ketplusRemoteJobStore, "ketplus.runner.jobs")

QStringList labelsOf(const QVariant& stored) {
    QStringList labels;
    for (const QJsonValue& label : QJsonDocument::fromJson(stored.toByteArray()).array())
        labels.append(label.toString());
    return labels;
}

QJsonValue runnerJson(const std::optional<RemoteRunner>& runner) {
    if (!runner)
        return QJsonValue::Null;
    return QJsonObject{
        {"runnerId", runner->runnerId}, {"name", runner->name}, {"attempt", runner->attempt}};
}

struct Row {
    qint64 revision = 0;
    QByteArray record;
    QStringList runsOn;
    qint64 timeoutMs = 0;
};

std::optional<Row> rowOf(QSqlDatabase database, const QString& taskId) {
    QSqlQuery query(database);
    query.prepare("SELECT revision,record,runs_on,timeout_ms FROM remote_jobs WHERE task_id=?");
    query.addBindValue(taskId);
    if (!query.exec() || !query.next())
        return std::nullopt;
    return Row{query.value(0).toLongLong(), query.value(1).toByteArray(), labelsOf(query.value(2)),
               query.value(3).toLongLong()};
}

} // namespace

LedgerRecordStore::LedgerRecordStore(QSqlDatabase database, BackgroundTaskLedger& ledger)
    : database_(std::move(database)), ledger_(ledger) {}

bool LedgerRecordStore::put(const RemoteJobRecord& record, const qint64 expected) {
    if (record.taskId.isEmpty() || expected < 0 || !database_.transaction())
        return false;
    if (!write(record, expected)) {
        database_.rollback();
        return false;
    }
    if (!database_.commit()) {
        database_.rollback();
        return false;
    }
    if (record.state == state::interrupted)
        acknowledgements_.remove(record.taskId);
    return true;
}

bool LedgerRecordStore::write(const RemoteJobRecord& record, const qint64 expected) {
    const auto row = rowOf(database_, record.taskId);
    if (!row || row->revision != expected || row->runsOn != record.runsOn ||
        row->timeoutMs != record.timeoutMs)
        return false;
    QJsonObject task = ledger_.runtimeTask(record.taskId);
    if (task.contains("error_code"))
        return false;
    RemoteJobRecord from;
    if (expected == 0) {
        if (task.value("state").toString() != state::queued ||
            !task.value("permission_approved").toBool())
            return false;
        from.taskId = record.taskId;
        from.runsOn = row->runsOn;
        from.timeoutMs = row->timeoutMs;
    } else {
        const auto stored = decodeRecord(row->record);
        if (!stored || stored->taskId != record.taskId)
            return false;
        from = *stored;
    }
    const auto held = acknowledgements_.constFind(record.taskId);
    const RemoteStep step = remoteStep(from, record, held != acknowledgements_.cend());
    if (step.kind == RemoteStep::Kind::Refused)
        return false;
    if (step.kind == RemoteStep::Kind::Task) {
        task.insert("placement", record.placement);
        task.insert("runner", runnerJson(record.runner));
        BackgroundTaskEffects effects;
        effects.userAcknowledgement = step.acknowledgement;
        if (step.acknowledgement) {
            effects.auditSource = held->source;
            effects.auditClientId = held->userClientId;
        }
        const auto applied = ledger_.applyTransition(
            record.taskId, task, task.value("revision").toInteger(), step.change, effects);
        if (!applied.value("ok").toBool())
            return false;
    }
    QSqlQuery query(database_);
    query.prepare("UPDATE remote_jobs SET revision=?,record=?,job_id=?,dispatched_to=? "
                  "WHERE task_id=? AND revision=?");
    query.addBindValue(expected + 1);
    query.addBindValue(encodeRecord(record));
    query.addBindValue(record.jobId.isEmpty() ? QVariant() : QVariant(record.jobId));
    query.addBindValue(record.dispatchedTo.isEmpty() ? QVariant() : QVariant(record.dispatchedTo));
    query.addBindValue(record.taskId);
    query.addBindValue(expected);
    return query.exec() && query.numRowsAffected() == 1;
}

QList<RemoteJobRecord> LedgerRecordStore::load() const {
    unreadable_.clear();
    QList<RemoteJobRecord> records;
    QSqlQuery query(database_);
    if (!query.exec("SELECT r.task_id,r.revision,r.record,r.runs_on,r.timeout_ms FROM "
                    "remote_jobs r JOIN managed_jobs j ON j.task_id=r.task_id "
                    "WHERE r.revision>0 ORDER BY j.sequence"))
        return records;
    while (query.next()) {
        const QString taskId = query.value(0).toString();
        auto record = decodeRecord(query.value(2).toByteArray());
        if (!record || record->taskId != taskId || record->runsOn != labelsOf(query.value(3)) ||
            record->timeoutMs != query.value(4).toLongLong()) {
            qCWarning(ketplusRemoteJobStore).noquote()
                << "remote job record unreadable task=" + taskId;
            unreadable_.append(taskId);
            continue;
        }
        record->revision = query.value(1).toLongLong();
        records.append(*record);
    }
    return records;
}

std::optional<QStringList> LedgerRecordStore::reserved() const {
    QSqlQuery query(database_);
    if (!query.exec("SELECT r.task_id FROM remote_jobs r JOIN managed_jobs j "
                    "ON j.task_id=r.task_id WHERE r.revision=0 ORDER BY j.sequence"))
        return std::nullopt;
    QStringList tasks;
    while (query.next())
        tasks.append(query.value(0).toString());
    return tasks;
}

std::optional<int> LedgerRecordStore::unfinishedReserved() const {
    QSqlQuery query(database_);
    if (!query.exec("SELECT count(*) FROM remote_jobs r JOIN managed_jobs j "
                    "ON j.task_id=r.task_id WHERE r.revision=0 AND j.state NOT IN "
                    "('completed','failed','cancelled','timed_out','interrupted')") ||
        !query.next())
        return std::nullopt;
    return query.value(0).toInt();
}

std::optional<LedgerRecordStore::Reservation>
LedgerRecordStore::reservation(const QString& taskId) const {
    const auto row = rowOf(database_, taskId);
    if (!row || row->revision != 0 || row->runsOn.isEmpty())
        return std::nullopt;
    return Reservation{row->runsOn, row->timeoutMs};
}

std::optional<QString> LedgerRecordStore::abandonUnreadable(const QString& taskId) {
    if (taskId.isEmpty() || !database_.transaction())
        return std::nullopt;
    const auto fail = [this]() -> std::optional<QString> {
        database_.rollback();
        return std::nullopt;
    };
    QSqlQuery query(database_);
    query.prepare("SELECT revision,runs_on,timeout_ms,job_id,dispatched_to,record FROM remote_jobs "
                  "WHERE task_id=? AND revision>0");
    query.addBindValue(taskId);
    if (!query.exec() || !query.next())
        return fail();
    // A record that reads is never ended this way.
    if (const auto readable = decodeRecord(query.value(5).toByteArray());
        readable && readable->taskId == taskId)
        return fail();
    const qint64 revision = query.value(0).toLongLong();
    RemoteJobRecord record;
    record.taskId = taskId;
    record.runsOn = labelsOf(query.value(1));
    record.timeoutMs = query.value(2).toLongLong();
    record.jobId = query.value(3).toString();
    record.dispatchedTo = query.value(4).toString();
    record.state = state::interrupted;
    record.reason = QStringLiteral("record_unreadable");
    record.cleanupStatus = cleanup::unverified;
    query.finish();
    QJsonObject task = ledger_.runtimeTask(taskId);
    if (task.isEmpty())
        return fail();
    const QString where = task.value("placement").toString();
    if (where == placement::relayQueued || where == placement::assigned ||
        where == placement::syncing || where == placement::running)
        record.placement = where;
    if (!isTerminalState(task.value("state").toString())) {
        // The owner hears of it once, through the task's completion.
        BackgroundTaskChange change;
        change.state = record.state;
        change.reason = record.reason;
        change.cleanupStatus = record.cleanupStatus;
        const auto applied =
            ledger_.applyTransition(taskId, task, task.value("revision").toInteger(), change, {});
        if (!applied.value("ok").toBool())
            return fail();
    }
    QSqlQuery update(database_);
    update.prepare("UPDATE remote_jobs SET revision=?,record=? WHERE task_id=? AND revision=?");
    update.addBindValue(revision + 1);
    update.addBindValue(encodeRecord(record));
    update.addBindValue(taskId);
    update.addBindValue(revision);
    if (!update.exec() || update.numRowsAffected() != 1 || !database_.commit())
        return fail();
    acknowledgements_.remove(taskId);
    unreadable_.removeAll(taskId);
    qCWarning(ketplusRemoteJobStore).noquote() << "remote job record abandoned task=" + taskId;
    return record.jobId;
}

bool LedgerRecordStore::acknowledge(const QString& taskId, const QString& source,
                                    const QString& userClientId) {
    if (taskId.isEmpty() || userClientId.isEmpty() ||
        (source != QLatin1String("user") && source != QLatin1String("runner_removed")))
        return false;
    acknowledgements_.insert(taskId, {source, userClientId});
    return true;
}

} // namespace ketplus::daemon::runner::jobs
