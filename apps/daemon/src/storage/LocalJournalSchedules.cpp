#include "storage/LocalJournal.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QtGlobal>

namespace ketplus::daemon {
namespace {

constexpr int maximumRuleBytes = 8192;
constexpr int maximumNameBytes = 512;

constexpr auto scheduleColumns =
    "id, name, enabled, status, prompt, target_kind, session_id, team_id, workspace_path, "
    "repository_path, provider, model, effort, isolated_worktree, title_template, rule_json, "
    "busy_policy, missed_policy, created_by, created_at, updated_at, next_run_at, last_run_at";

constexpr auto runColumns = "id, schedule_id, scheduled_for, started_at, finished_at, status, "
                            "session_id, turn_ref, error_code, error_message";

QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }

// A null QString binds as SQL NULL, which the NOT NULL text columns reject.
QVariant text(const QString& value) {
    return value.isNull() ? QVariant(QStringLiteral("")) : QVariant(value);
}

bool oneOf(const QString& value, std::initializer_list<const char*> allowed) {
    for (const char* item : allowed)
        if (value == QLatin1String(item))
            return true;
    return false;
}

bool jsonRule(const QString& value) {
    if (value.toUtf8().size() > maximumRuleBytes)
        return false;
    const QJsonDocument document = QJsonDocument::fromJson(value.toUtf8());
    return !document.isNull() && (document.isObject() || document.isArray());
}

QString validateTarget(const ScheduleTarget& target) {
    const bool conversation = target.kind == QLatin1String("conversation");
    const bool created = target.kind == QLatin1String("new");
    const bool team = target.kind == QLatin1String("team");
    const bool teamStart = target.kind == QLatin1String("team_start");
    if (!conversation && !created && !team && !teamStart)
        return QStringLiteral("Schedule target kind is invalid.");
    if (conversation) {
        if (target.sessionId.isEmpty())
            return QStringLiteral("A conversation schedule needs a session id.");
        if (!target.teamId.isEmpty())
            return QStringLiteral("A conversation schedule has no team id.");
    } else if (!target.sessionId.isEmpty()) {
        return QStringLiteral("Only a conversation schedule has a session id.");
    }
    if (team || teamStart) {
        if (target.teamId.isEmpty())
            return QStringLiteral("A team schedule needs a team id.");
    } else if (!target.teamId.isEmpty()) {
        return QStringLiteral("Only a team schedule has a team id.");
    }
    if (created) {
        if (target.workspacePath.isEmpty())
            return QStringLiteral("A new-conversation schedule needs a workspace path.");
    } else if (!target.workspacePath.isEmpty() || !target.repositoryPath.isEmpty() ||
               !target.provider.isEmpty() || !target.model.isEmpty() || !target.effort.isEmpty() ||
               target.isolatedWorktree || !target.titleTemplate.isEmpty()) {
        return QStringLiteral("Only a new-conversation schedule stores workspace fields.");
    }
    return {};
}

QString validateSchedule(const ScheduleRecord& schedule, const bool creating) {
    if (schedule.id.isEmpty())
        return QStringLiteral("Schedule id is empty.");
    if (schedule.name.isEmpty() || schedule.name.toUtf8().size() > maximumNameBytes)
        return QStringLiteral("Schedule name is empty or too long.");
    if (const QString target = validateTarget(schedule.target); !target.isEmpty())
        return target;
    if (!jsonRule(schedule.ruleJson))
        return QStringLiteral("Schedule rule must be a JSON object or array.");
    if (!oneOf(schedule.busyPolicy, {"queue", "skip"}))
        return QStringLiteral("Schedule busy policy is invalid.");
    if (!oneOf(schedule.missedPolicy, {"run_once", "skip"}))
        return QStringLiteral("Schedule missed policy is invalid.");
    if (schedule.createdBy.isEmpty())
        return QStringLiteral("Schedule createdBy is empty.");
    if (creating) {
        if (!schedule.status.isEmpty())
            return QStringLiteral("Schedule status is recorded by the journal.");
    } else if (!schedule.status.isEmpty() && schedule.status != QLatin1String("target_deleted")) {
        return QStringLiteral("Schedule status is invalid.");
    }
    return {};
}

QString validateRun(const ScheduleRunRecord& run) {
    if (run.id.isEmpty())
        return QStringLiteral("Schedule run id is empty.");
    if (run.scheduleId.isEmpty())
        return QStringLiteral("Schedule run needs a schedule id.");
    if (run.scheduledFor.isEmpty())
        return QStringLiteral("Schedule run needs the time it was due.");
    if (!oneOf(run.status, {"queued", "running", "succeeded", "failed", "skipped", "missed"}))
        return QStringLiteral("Schedule run status is invalid.");
    return {};
}

ScheduleRecord readSchedule(const QSqlQuery& query) {
    ScheduleRecord schedule;
    schedule.id = query.value(0).toString();
    schedule.name = query.value(1).toString();
    schedule.enabled = query.value(2).toInt() != 0;
    schedule.status = query.value(3).toString();
    schedule.prompt = query.value(4).toString();
    schedule.target.kind = query.value(5).toString();
    schedule.target.sessionId = query.value(6).toString();
    schedule.target.teamId = query.value(7).toString();
    schedule.target.workspacePath = query.value(8).toString();
    schedule.target.repositoryPath = query.value(9).toString();
    schedule.target.provider = query.value(10).toString();
    schedule.target.model = query.value(11).toString();
    schedule.target.effort = query.value(12).toString();
    schedule.target.isolatedWorktree = query.value(13).toInt() != 0;
    schedule.target.titleTemplate = query.value(14).toString();
    schedule.ruleJson = query.value(15).toString();
    schedule.busyPolicy = query.value(16).toString();
    schedule.missedPolicy = query.value(17).toString();
    schedule.createdBy = query.value(18).toString();
    schedule.createdAt = query.value(19).toString();
    schedule.updatedAt = query.value(20).toString();
    schedule.nextRunAt = query.value(21).toString();
    schedule.lastRunAt = query.value(22).toString();
    return schedule;
}

ScheduleRunRecord readRun(const QSqlQuery& query, const int first) {
    ScheduleRunRecord run;
    run.id = query.value(first).toString();
    run.scheduleId = query.value(first + 1).toString();
    run.scheduledFor = query.value(first + 2).toString();
    run.startedAt = query.value(first + 3).toString();
    run.finishedAt = query.value(first + 4).toString();
    run.status = query.value(first + 5).toString();
    run.sessionId = query.value(first + 6).toString();
    run.turnReference = query.value(first + 7).toString();
    run.errorCode = query.value(first + 8).toString();
    run.errorMessage = query.value(first + 9).toString();
    return run;
}

void bindTarget(QSqlQuery& query, const ScheduleRecord& schedule) {
    query.addBindValue(text(schedule.target.kind));
    query.addBindValue(text(schedule.target.sessionId));
    query.addBindValue(text(schedule.target.teamId));
    query.addBindValue(text(schedule.target.workspacePath));
    query.addBindValue(text(schedule.target.repositoryPath));
    query.addBindValue(text(schedule.target.provider));
    query.addBindValue(text(schedule.target.model));
    query.addBindValue(text(schedule.target.effort));
    query.addBindValue(schedule.target.isolatedWorktree ? 1 : 0);
    query.addBindValue(text(schedule.target.titleTemplate));
    query.addBindValue(text(schedule.ruleJson));
    query.addBindValue(text(schedule.busyPolicy));
    query.addBindValue(text(schedule.missedPolicy));
    query.addBindValue(text(schedule.createdBy));
}

} // namespace

bool LocalJournal::initializeSchedules() {
    QSqlQuery query(database_);
    const QStringList statements{
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS schedules ("
            "id TEXT PRIMARY KEY, name TEXT NOT NULL, "
            "enabled INTEGER NOT NULL CHECK(enabled IN (0, 1)), "
            "status TEXT NOT NULL DEFAULT '' CHECK(status IN ('', 'target_deleted')), "
            "prompt TEXT NOT NULL, "
            "target_kind TEXT NOT NULL CHECK(target_kind IN "
            "('conversation', 'new', 'team', 'team_start')), "
            "session_id TEXT NOT NULL DEFAULT '', team_id TEXT NOT NULL DEFAULT '', "
            "workspace_path TEXT NOT NULL DEFAULT '', repository_path TEXT NOT NULL DEFAULT '', "
            "provider TEXT NOT NULL DEFAULT '', model TEXT NOT NULL DEFAULT '', "
            "effort TEXT NOT NULL DEFAULT '', "
            "isolated_worktree INTEGER NOT NULL DEFAULT 0 CHECK(isolated_worktree IN (0, 1)), "
            "title_template TEXT NOT NULL DEFAULT '', "
            "rule_json TEXT NOT NULL CHECK(length(CAST(rule_json AS BLOB))<=8192), "
            "busy_policy TEXT NOT NULL CHECK(busy_policy IN ('queue', 'skip')), "
            "missed_policy TEXT NOT NULL CHECK(missed_policy IN ('run_once', 'skip')), "
            "created_by TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL, "
            "next_run_at TEXT NOT NULL DEFAULT '', last_run_at TEXT NOT NULL DEFAULT '')"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS schedules_target ON "
                       "schedules(target_kind, session_id, team_id)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS schedule_runs ("
            "id TEXT PRIMARY KEY, schedule_id TEXT NOT NULL REFERENCES schedules(id) "
            "ON DELETE CASCADE, scheduled_for TEXT NOT NULL, "
            "started_at TEXT NOT NULL DEFAULT '', finished_at TEXT NOT NULL DEFAULT '', "
            "status TEXT NOT NULL CHECK(status IN "
            "('queued', 'running', 'succeeded', 'failed', 'skipped', 'missed')), "
            "session_id TEXT NOT NULL DEFAULT '', turn_ref TEXT NOT NULL DEFAULT '', "
            "error_code TEXT NOT NULL DEFAULT '', error_message TEXT NOT NULL DEFAULT '')"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS schedule_runs_schedule ON schedule_runs(schedule_id)")};
    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            setError(query.lastError().text());
            return false;
        }
    }
    return true;
}

bool LocalJournal::createSchedule(const ScheduleRecord& schedule, QString* error) {
    const auto reject = [&](const QString& message) {
        setError(message);
        if (error)
            *error = message;
        return false;
    };
    if (const QString reason = validateSchedule(schedule, true); !reason.isEmpty())
        return reject(reason);
    QSqlQuery existing(database_);
    existing.prepare(QStringLiteral("SELECT 1 FROM schedules WHERE id = ?"));
    existing.addBindValue(schedule.id);
    if (!existing.exec())
        return reject(existing.lastError().text());
    if (existing.next())
        return reject(QStringLiteral("Schedule id already exists."));
    existing.finish();

    const QString writtenAt = now();
    const QString createdAt = schedule.createdAt.isEmpty() ? writtenAt : schedule.createdAt;
    QSqlQuery insert(database_);
    insert.prepare(QStringLiteral(
        "INSERT INTO schedules(id, name, enabled, status, prompt, target_kind, session_id, "
        "team_id, workspace_path, repository_path, provider, model, effort, isolated_worktree, "
        "title_template, rule_json, busy_policy, missed_policy, created_by, created_at, "
        "updated_at, next_run_at, last_run_at) "
        "VALUES(?, ?, ?, '', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(text(schedule.id));
    insert.addBindValue(text(schedule.name));
    insert.addBindValue(schedule.enabled ? 1 : 0);
    insert.addBindValue(text(schedule.prompt));
    bindTarget(insert, schedule);
    insert.addBindValue(text(createdAt));
    insert.addBindValue(text(writtenAt));
    insert.addBindValue(text(schedule.nextRunAt));
    insert.addBindValue(text(schedule.lastRunAt));
    if (!insert.exec() || insert.numRowsAffected() != 1)
        return reject(insert.lastError().text().isEmpty()
                          ? QStringLiteral("The schedule was not stored.")
                          : insert.lastError().text());
    setError({});
    return true;
}

std::optional<ScheduleRecord> LocalJournal::schedule(const QString& id) const {
    setError({});
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT %1 FROM schedules WHERE id = ?").arg(scheduleColumns));
    query.addBindValue(id);
    if (!query.exec()) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    if (!query.next())
        return std::nullopt;
    return readSchedule(query);
}

std::optional<QList<ScheduleRecord>> LocalJournal::schedules(const ScheduleFilter& filter) const {
    setError({});
    QStringList clauses;
    QVariantList values;
    if (!filter.targetKind.isEmpty()) {
        clauses.append(QStringLiteral("target_kind = ?"));
        values.append(filter.targetKind);
    }
    if (!filter.sessionId.isEmpty()) {
        clauses.append(QStringLiteral("session_id = ?"));
        values.append(filter.sessionId);
    }
    if (!filter.teamId.isEmpty()) {
        clauses.append(QStringLiteral("team_id = ?"));
        values.append(filter.teamId);
    }
    QString sql = QStringLiteral("SELECT %1 FROM schedules").arg(scheduleColumns);
    if (!clauses.isEmpty())
        sql += QStringLiteral(" WHERE ") + clauses.join(QStringLiteral(" AND "));
    sql += QStringLiteral(" ORDER BY created_at, id");
    QSqlQuery query(database_);
    query.prepare(sql);
    for (const QVariant& value : values)
        query.addBindValue(value);
    if (!query.exec()) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    QList<ScheduleRecord> result;
    while (query.next())
        result.append(readSchedule(query));
    return result;
}

bool LocalJournal::updateSchedule(const ScheduleRecord& schedule, QString* error) {
    const auto reject = [&](const QString& message) {
        setError(message);
        if (error)
            *error = message;
        return false;
    };
    if (const QString reason = validateSchedule(schedule, false); !reason.isEmpty())
        return reject(reason);
    QSqlQuery update(database_);
    update.prepare(QStringLiteral(
        "UPDATE schedules SET name = ?, enabled = ?, status = ?, prompt = ?, target_kind = ?, "
        "session_id = ?, team_id = ?, workspace_path = ?, repository_path = ?, provider = ?, "
        "model = ?, effort = ?, isolated_worktree = ?, title_template = ?, rule_json = ?, "
        "busy_policy = ?, missed_policy = ?, created_by = ?, updated_at = ?, next_run_at = ?, "
        "last_run_at = ? WHERE id = ?"));
    update.addBindValue(text(schedule.name));
    update.addBindValue(schedule.enabled ? 1 : 0);
    update.addBindValue(text(schedule.status));
    update.addBindValue(text(schedule.prompt));
    bindTarget(update, schedule);
    update.addBindValue(text(now()));
    update.addBindValue(text(schedule.nextRunAt));
    update.addBindValue(text(schedule.lastRunAt));
    update.addBindValue(text(schedule.id));
    if (!update.exec())
        return reject(update.lastError().text());
    if (update.numRowsAffected() != 1)
        return reject(QStringLiteral("Schedule was not found."));
    setError({});
    return true;
}

bool LocalJournal::deleteSchedule(const QString& id, QString* error) {
    const auto reject = [&](const QString& message) {
        setError(message);
        if (error)
            *error = message;
        return false;
    };
    if (id.isEmpty())
        return reject(QStringLiteral("Schedule id is empty."));
    if (!database_.transaction())
        return reject(database_.lastError().text());
    bool open = true;
    const auto rollback = qScopeGuard([&] {
        if (open)
            database_.rollback();
    });
    QSqlQuery runs(database_);
    runs.prepare(QStringLiteral("DELETE FROM schedule_runs WHERE schedule_id = ?"));
    runs.addBindValue(id);
    if (!runs.exec())
        return reject(runs.lastError().text());
    QSqlQuery schedule(database_);
    schedule.prepare(QStringLiteral("DELETE FROM schedules WHERE id = ?"));
    schedule.addBindValue(id);
    if (!schedule.exec())
        return reject(schedule.lastError().text());
    if (schedule.numRowsAffected() != 1)
        return reject(QStringLiteral("Schedule was not found."));
    if (!database_.commit())
        return reject(database_.lastError().text());
    open = false;
    setError({});
    return true;
}

bool LocalJournal::setScheduleEnabled(const QString& id, const bool enabled, QString* error) {
    const auto reject = [&](const QString& message) {
        setError(message);
        if (error)
            *error = message;
        return false;
    };
    if (id.isEmpty())
        return reject(QStringLiteral("Schedule id is empty."));
    QSqlQuery current(database_);
    current.prepare(QStringLiteral("SELECT status FROM schedules WHERE id = ?"));
    current.addBindValue(id);
    if (!current.exec())
        return reject(current.lastError().text());
    if (!current.next())
        return reject(QStringLiteral("Schedule was not found."));
    if (enabled && current.value(0).toString() == QLatin1String("target_deleted"))
        return reject(QStringLiteral("This schedule's conversation was deleted."));
    current.finish();
    QSqlQuery update(database_);
    update.prepare(QStringLiteral("UPDATE schedules SET enabled = ?, updated_at = ? WHERE id = ?"));
    update.addBindValue(enabled ? 1 : 0);
    update.addBindValue(text(now()));
    update.addBindValue(id);
    if (!update.exec() || update.numRowsAffected() != 1)
        return reject(update.lastError().text().isEmpty()
                          ? QStringLiteral("Schedule was not found.")
                          : update.lastError().text());
    setError({});
    return true;
}

bool LocalJournal::removeSchedulesForTeam(const QString& teamId, QString* error) {
    const auto reject = [&](const QString& message) {
        setError(message);
        if (error)
            *error = message;
        return false;
    };
    if (teamId.isEmpty())
        return reject(QStringLiteral("Schedule team id is empty."));
    QSqlQuery runs(database_);
    runs.prepare(
        QStringLiteral("DELETE FROM schedule_runs WHERE schedule_id IN (SELECT id FROM schedules "
                       "WHERE team_id = ? AND target_kind IN ('team', 'team_start'))"));
    runs.addBindValue(teamId);
    if (!runs.exec())
        return reject(runs.lastError().text());
    QSqlQuery schedules(database_);
    schedules.prepare(QStringLiteral(
        "DELETE FROM schedules WHERE team_id = ? AND target_kind IN ('team', 'team_start')"));
    schedules.addBindValue(teamId);
    if (!schedules.exec())
        return reject(schedules.lastError().text());
    return true;
}

bool LocalJournal::deleteSchedulesForTeam(const QString& teamId, QString* error) {
    const auto reject = [&](const QString& message) {
        setError(message);
        if (error)
            *error = message;
        return false;
    };
    if (!database_.transaction())
        return reject(database_.lastError().text());
    bool open = true;
    const auto rollback = qScopeGuard([&] {
        if (open)
            database_.rollback();
    });
    if (!removeSchedulesForTeam(teamId, error))
        return false;
    if (!database_.commit())
        return reject(database_.lastError().text());
    open = false;
    setError({});
    return true;
}

bool LocalJournal::disableSchedulesForSession(const QString& sessionId, QString* error) {
    const auto reject = [&](const QString& message) {
        setError(message);
        if (error)
            *error = message;
        return false;
    };
    if (sessionId.isEmpty())
        return reject(QStringLiteral("Schedule session id is empty."));
    QSqlQuery update(database_);
    update.prepare(QStringLiteral(
        "UPDATE schedules SET enabled = 0, status = 'target_deleted', updated_at = ? "
        "WHERE target_kind = 'conversation' AND session_id = ?"));
    update.addBindValue(text(now()));
    update.addBindValue(sessionId);
    if (!update.exec())
        return reject(update.lastError().text());
    setError({});
    return true;
}

bool LocalJournal::appendScheduleRun(const ScheduleRunRecord& run, QString* error) {
    const auto reject = [&](const QString& message) {
        setError(message);
        if (error)
            *error = message;
        return false;
    };
    if (const QString reason = validateRun(run); !reason.isEmpty())
        return reject(reason);
    if (!database_.transaction())
        return reject(database_.lastError().text());
    bool open = true;
    const auto rollback = qScopeGuard([&] {
        if (open)
            database_.rollback();
    });
    QSqlQuery schedule(database_);
    schedule.prepare(QStringLiteral("SELECT 1 FROM schedules WHERE id = ?"));
    schedule.addBindValue(run.scheduleId);
    if (!schedule.exec())
        return reject(schedule.lastError().text());
    if (!schedule.next())
        return reject(QStringLiteral("Schedule was not found."));
    schedule.finish();
    QSqlQuery insert(database_);
    insert.prepare(QStringLiteral(
        "INSERT INTO schedule_runs(id, schedule_id, scheduled_for, started_at, finished_at, "
        "status, session_id, turn_ref, error_code, error_message) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(text(run.id));
    insert.addBindValue(text(run.scheduleId));
    insert.addBindValue(text(run.scheduledFor));
    insert.addBindValue(text(run.startedAt));
    insert.addBindValue(text(run.finishedAt));
    insert.addBindValue(text(run.status));
    insert.addBindValue(text(run.sessionId));
    insert.addBindValue(text(run.turnReference));
    insert.addBindValue(text(run.errorCode));
    insert.addBindValue(text(run.errorMessage));
    if (!insert.exec())
        return reject(insert.lastError().text().isEmpty()
                          ? QStringLiteral("The schedule run was not stored.")
                          : insert.lastError().text());
    QSqlQuery prune(database_);
    prune.prepare(QStringLiteral(
        "DELETE FROM schedule_runs WHERE schedule_id = ? AND rowid NOT IN ("
        "SELECT rowid FROM schedule_runs WHERE schedule_id = ? ORDER BY rowid DESC LIMIT ?)"));
    prune.addBindValue(run.scheduleId);
    prune.addBindValue(run.scheduleId);
    prune.addBindValue(maximumScheduleRuns);
    if (!prune.exec())
        return reject(prune.lastError().text());
    if (!database_.commit())
        return reject(database_.lastError().text());
    open = false;
    setError({});
    return true;
}

bool LocalJournal::updateScheduleRun(const ScheduleRunRecord& run, QString* error) {
    const auto reject = [&](const QString& message) {
        setError(message);
        if (error)
            *error = message;
        return false;
    };
    if (const QString reason = validateRun(run); !reason.isEmpty())
        return reject(reason);
    QSqlQuery update(database_);
    update.prepare(QStringLiteral(
        "UPDATE schedule_runs SET started_at = ?, finished_at = ?, status = ?, session_id = ?, "
        "turn_ref = ?, error_code = ?, error_message = ? WHERE id = ? AND schedule_id = ?"));
    update.addBindValue(text(run.startedAt));
    update.addBindValue(text(run.finishedAt));
    update.addBindValue(text(run.status));
    update.addBindValue(text(run.sessionId));
    update.addBindValue(text(run.turnReference));
    update.addBindValue(text(run.errorCode));
    update.addBindValue(text(run.errorMessage));
    update.addBindValue(text(run.id));
    update.addBindValue(text(run.scheduleId));
    if (!update.exec())
        return reject(update.lastError().text());
    if (update.numRowsAffected() != 1)
        return reject(QStringLiteral("Schedule run was not found."));
    setError({});
    return true;
}

std::optional<ScheduleRunPage> LocalJournal::scheduleRuns(const QString& scheduleId,
                                                          const int limit,
                                                          const QString& beforeId) const {
    setError({});
    if (scheduleId.isEmpty() || limit < 1) {
        setError(QStringLiteral("Schedule run page is invalid."));
        return std::nullopt;
    }
    const int page = qMin(limit, maximumScheduleRuns);
    if (!beforeId.isEmpty()) {
        QSqlQuery cursor(database_);
        cursor.prepare(
            QStringLiteral("SELECT 1 FROM schedule_runs WHERE id = ? AND schedule_id = ?"));
        cursor.addBindValue(beforeId);
        cursor.addBindValue(scheduleId);
        if (!cursor.exec()) {
            setError(cursor.lastError().text());
            return std::nullopt;
        }
        if (!cursor.next()) {
            setError(QStringLiteral("Schedule run page cursor was not found."));
            return std::nullopt;
        }
    }
    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral("SELECT rowid, %1 FROM schedule_runs WHERE schedule_id = ? AND "
                       "(? = '' OR rowid < (SELECT rowid FROM schedule_runs WHERE id = ? AND "
                       "schedule_id = ?)) ORDER BY rowid DESC LIMIT ?")
            .arg(runColumns));
    query.addBindValue(scheduleId);
    query.addBindValue(text(beforeId));
    query.addBindValue(text(beforeId));
    query.addBindValue(scheduleId);
    query.addBindValue(page);
    if (!query.exec()) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    ScheduleRunPage result;
    qint64 lastRow = 0;
    while (query.next()) {
        lastRow = query.value(0).toLongLong();
        result.runs.append(readRun(query, 1));
    }
    if (result.runs.size() == page) {
        QSqlQuery more(database_);
        more.prepare(QStringLiteral(
            "SELECT 1 FROM schedule_runs WHERE schedule_id = ? AND rowid < ? LIMIT 1"));
        more.addBindValue(scheduleId);
        more.addBindValue(lastRow);
        if (!more.exec()) {
            setError(more.lastError().text());
            return std::nullopt;
        }
        if (more.next())
            result.nextBefore = result.runs.last().id;
    }
    return result;
}

} // namespace ketplus::daemon
