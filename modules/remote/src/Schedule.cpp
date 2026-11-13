#include "Schedule.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>

namespace ketplus::schedule {
namespace {

void fail(Failure* failure, const char* code, const QString& message) {
    if (failure)
        *failure = Failure{QString::fromLatin1(code), message};
}

void accept(Failure* failure) {
    if (failure)
        *failure = {};
}

QString firstUnknown(const QJsonObject& object, const QStringList& allowed) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        if (!allowed.contains(it.key()))
            return it.key();
    return {};
}

bool validId(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$"));
    return pattern.match(value).hasMatch();
}

bool timestamp(const QString& value) {
    // A zone-less value is local time for Qt, so two processes would disagree.
    // Require Z or a numeric offset (2026-11-13T10:00:00Z, ...+07:00, ...+0700).
    static const QRegularExpression zone(QStringLiteral(R"((?:[Zz]|[+-]\d{2}:?\d{2})$)"));
    if (!zone.match(value).hasMatch())
        return false;
    return QDateTime::fromString(value, Qt::ISODateWithMs).isValid() ||
           QDateTime::fromString(value, Qt::ISODate).isValid();
}

std::optional<QString> stringField(const QJsonObject& object, const QString& key, bool required,
                                   Failure* failure) {
    if (!object.contains(key)) {
        if (!required)
            return QString{};
        fail(failure, invalidPayloadCode, QStringLiteral("%1 is required.").arg(key));
        return std::nullopt;
    }
    const QJsonValue value = object.value(key);
    if (!value.isString()) {
        fail(failure, invalidPayloadCode, QStringLiteral("%1 must be a string.").arg(key));
        return std::nullopt;
    }
    return value.toString();
}

bool bounded(const QString& value, const qsizetype limit, const QString& key, Failure* failure) {
    if (value.size() <= limit)
        return true;
    fail(failure, invalidPayloadCode, QStringLiteral("%1 is too long.").arg(key));
    return false;
}

std::optional<QString> idField(const QJsonObject& object, const QString& key, bool required,
                               bool allowEmpty, Failure* failure) {
    const auto text = stringField(object, key, required, failure);
    if (!text)
        return std::nullopt;
    if (text->isEmpty()) {
        if (allowEmpty)
            return text;
        fail(failure, invalidPayloadCode, QStringLiteral("%1 is required.").arg(key));
        return std::nullopt;
    }
    if (!validId(*text)) {
        fail(failure, invalidPayloadCode,
             QStringLiteral("%1 must be 1-128 letters, digits, or . _ : -.").arg(key));
        return std::nullopt;
    }
    return text;
}

std::optional<QString> timeField(const QJsonObject& object, const QString& key, bool allowEmpty,
                                 Failure* failure) {
    const auto text = stringField(object, key, true, failure);
    if (!text)
        return std::nullopt;
    if (text->isEmpty()) {
        if (allowEmpty)
            return text;
        fail(failure, invalidPayloadCode, QStringLiteral("%1 is required.").arg(key));
        return std::nullopt;
    }
    if (!timestamp(*text)) {
        fail(failure, invalidPayloadCode,
             QStringLiteral("%1 must be an ISO-8601 timestamp with a UTC offset.").arg(key));
        return std::nullopt;
    }
    return text;
}

QStringList targetKeys(const TargetKind kind) {
    switch (kind) {
    case TargetKind::Conversation:
        return {QStringLiteral("kind"), QStringLiteral("session_id")};
    case TargetKind::NewConversation:
        return {QStringLiteral("kind"),          QStringLiteral("workspace"),
                QStringLiteral("provider"),      QStringLiteral("model"),
                QStringLiteral("effort"),        QStringLiteral("isolated_worktree"),
                QStringLiteral("title_template")};
    case TargetKind::Team:
    case TargetKind::TeamStart:
        return {QStringLiteral("kind"), QStringLiteral("team_id")};
    }
    return {};
}

} // namespace

Failure validatePrompt(const QString& prompt, const TargetKind kind) {
    if (kind == TargetKind::TeamStart) {
        if (!prompt.trimmed().isEmpty())
            return {QString::fromLatin1(promptNotAllowedCode),
                    QStringLiteral("A team start has no prompt.")};
        return {};
    }
    if (prompt.trimmed().isEmpty())
        return {QString::fromLatin1(emptyPromptCode), QStringLiteral("Prompt is required.")};
    if (prompt.size() > maximumSchedulePromptLength)
        return {QString::fromLatin1(invalidPayloadCode), QStringLiteral("prompt is too long.")};
    return {};
}

Failure validateScheduleId(const QString& id, const QString& field, const bool allowEmpty) {
    if (id.isEmpty()) {
        if (allowEmpty)
            return {};
        return {QString::fromLatin1(invalidPayloadCode),
                QStringLiteral("%1 is required.").arg(field)};
    }
    if (!validId(id))
        return {QString::fromLatin1(invalidPayloadCode),
                QStringLiteral("%1 must be 1-128 letters, digits, or . _ : -.").arg(field)};
    return {};
}

Failure validateCreatedBy(const QString& createdBy) {
    if (createdBy == QStringLiteral("user") || validId(createdBy))
        return {};
    return {QString::fromLatin1(invalidPayloadCode),
            QStringLiteral("created_by must be \"user\" or a conversation id.")};
}

QString nameOf(const TargetKind kind) {
    switch (kind) {
    case TargetKind::Conversation:
        return QStringLiteral("conversation");
    case TargetKind::NewConversation:
        return QStringLiteral("new");
    case TargetKind::Team:
        return QStringLiteral("team");
    case TargetKind::TeamStart:
        return QStringLiteral("team_start");
    }
    return {};
}

QString nameOf(const BusyPolicy policy) {
    return policy == BusyPolicy::Queue ? QStringLiteral("queue") : QStringLiteral("skip");
}

QString nameOf(const MissedPolicy policy) {
    return policy == MissedPolicy::RunOnce ? QStringLiteral("run_once") : QStringLiteral("skip");
}

QString nameOf(const RunStatus status) {
    switch (status) {
    case RunStatus::Queued:
        return QStringLiteral("queued");
    case RunStatus::Running:
        return QStringLiteral("running");
    case RunStatus::Succeeded:
        return QStringLiteral("succeeded");
    case RunStatus::Failed:
        return QStringLiteral("failed");
    case RunStatus::Skipped:
        return QStringLiteral("skipped");
    case RunStatus::Missed:
        return QStringLiteral("missed");
    }
    return {};
}

std::optional<TargetKind> targetKindFromName(const QString& name) {
    if (name == QStringLiteral("conversation"))
        return TargetKind::Conversation;
    if (name == QStringLiteral("new"))
        return TargetKind::NewConversation;
    if (name == QStringLiteral("team"))
        return TargetKind::Team;
    if (name == QStringLiteral("team_start"))
        return TargetKind::TeamStart;
    return std::nullopt;
}

std::optional<BusyPolicy> busyPolicyFromName(const QString& name) {
    if (name == QStringLiteral("queue"))
        return BusyPolicy::Queue;
    if (name == QStringLiteral("skip"))
        return BusyPolicy::Skip;
    return std::nullopt;
}

std::optional<MissedPolicy> missedPolicyFromName(const QString& name) {
    if (name == QStringLiteral("run_once"))
        return MissedPolicy::RunOnce;
    if (name == QStringLiteral("skip"))
        return MissedPolicy::Skip;
    return std::nullopt;
}

std::optional<RunStatus> runStatusFromName(const QString& name) {
    if (name == QStringLiteral("queued"))
        return RunStatus::Queued;
    if (name == QStringLiteral("running"))
        return RunStatus::Running;
    if (name == QStringLiteral("succeeded"))
        return RunStatus::Succeeded;
    if (name == QStringLiteral("failed"))
        return RunStatus::Failed;
    if (name == QStringLiteral("skipped"))
        return RunStatus::Skipped;
    if (name == QStringLiteral("missed"))
        return RunStatus::Missed;
    return std::nullopt;
}

Failure validateRule(const QString& rule, const bool teamStart) {
    if (rule.isEmpty())
        return {QString::fromLatin1(invalidRuleCode), QStringLiteral("rule is required.")};
    if (rule.size() > maximumScheduleRuleLength)
        return {QString::fromLatin1(invalidRuleCode), QStringLiteral("rule is too long.")};
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(rule.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return {QString::fromLatin1(invalidRuleCode),
                QStringLiteral("rule must be a JSON object string.")};
    if (teamStart &&
        document.object().value(QStringLiteral("kind")).toString() != QStringLiteral("once"))
        return {QString::fromLatin1(invalidRuleCode),
                QStringLiteral("A team start accepts only a once rule.")};
    return {};
}

QJsonObject toJson(const Target& target) {
    switch (target.kind) {
    case TargetKind::Conversation:
        return {{QStringLiteral("kind"), nameOf(target.kind)},
                {QStringLiteral("session_id"), target.sessionId}};
    case TargetKind::NewConversation:
        return {{QStringLiteral("kind"), nameOf(target.kind)},
                {QStringLiteral("workspace"), target.workspace},
                {QStringLiteral("provider"), target.provider},
                {QStringLiteral("model"), target.model},
                {QStringLiteral("effort"), target.effort},
                {QStringLiteral("isolated_worktree"), target.isolatedWorktree},
                {QStringLiteral("title_template"), target.titleTemplate}};
    case TargetKind::Team:
    case TargetKind::TeamStart:
        return {{QStringLiteral("kind"), nameOf(target.kind)},
                {QStringLiteral("team_id"), target.teamId}};
    }
    return {};
}

std::optional<Target> targetFromJson(const QJsonObject& json, Failure* failure) {
    const auto kindText = stringField(json, QStringLiteral("kind"), true, failure);
    if (!kindText)
        return std::nullopt;
    const auto kind = targetKindFromName(*kindText);
    if (!kind) {
        fail(failure, invalidEnumCode,
             QStringLiteral("target kind \"%1\" is not a known value.").arg(*kindText));
        return std::nullopt;
    }
    if (const QString extra = firstUnknown(json, targetKeys(*kind)); !extra.isEmpty()) {
        fail(failure, unknownFieldCode, QStringLiteral("Unknown field \"%1\".").arg(extra));
        return std::nullopt;
    }
    Target target;
    target.kind = *kind;
    if (kind == TargetKind::Conversation) {
        const auto sessionId = idField(json, QStringLiteral("session_id"), true, false, failure);
        if (!sessionId)
            return std::nullopt;
        target.sessionId = *sessionId;
    } else if (kind == TargetKind::NewConversation) {
        const auto workspace = stringField(json, QStringLiteral("workspace"), true, failure);
        if (!workspace)
            return std::nullopt;
        const auto provider = stringField(json, QStringLiteral("provider"), true, failure);
        if (!provider)
            return std::nullopt;
        const auto model = stringField(json, QStringLiteral("model"), true, failure);
        if (!model)
            return std::nullopt;
        const auto effort = stringField(json, QStringLiteral("effort"), true, failure);
        if (!effort)
            return std::nullopt;
        const auto title = stringField(json, QStringLiteral("title_template"), true, failure);
        if (!title)
            return std::nullopt;
        if (workspace->trimmed().isEmpty()) {
            fail(failure, invalidPayloadCode, QStringLiteral("workspace is required."));
            return std::nullopt;
        }
        if (!bounded(*workspace, maximumSchedulePathLength, QStringLiteral("workspace"), failure) ||
            !bounded(*provider, maximumScheduleProviderLength, QStringLiteral("provider"),
                     failure) ||
            !bounded(*model, maximumScheduleModelLength, QStringLiteral("model"), failure) ||
            !bounded(*effort, maximumScheduleEffortLength, QStringLiteral("effort"), failure) ||
            !bounded(*title, maximumScheduleTitleLength, QStringLiteral("title_template"), failure))
            return std::nullopt;
        if (!json.value(QStringLiteral("isolated_worktree")).isBool()) {
            fail(failure, invalidPayloadCode,
                 QStringLiteral("isolated_worktree must be a boolean."));
            return std::nullopt;
        }
        target.workspace = *workspace;
        target.provider = *provider;
        target.model = *model;
        target.effort = *effort;
        target.isolatedWorktree = json.value(QStringLiteral("isolated_worktree")).toBool();
        target.titleTemplate = *title;
    } else {
        const auto teamId = idField(json, QStringLiteral("team_id"), true, false, failure);
        if (!teamId)
            return std::nullopt;
        target.teamId = *teamId;
    }
    accept(failure);
    return target;
}

QJsonObject toJson(const Schedule& schedule) {
    return {{QStringLiteral("id"), schedule.id},
            {QStringLiteral("name"), schedule.name},
            {QStringLiteral("enabled"), schedule.enabled},
            {QStringLiteral("prompt"), schedule.prompt},
            {QStringLiteral("target"), toJson(schedule.target)},
            {QStringLiteral("rule"), schedule.rule},
            {QStringLiteral("busy_policy"), nameOf(schedule.busyPolicy)},
            {QStringLiteral("missed_policy"), nameOf(schedule.missedPolicy)},
            {QStringLiteral("created_by"), schedule.createdBy},
            {QStringLiteral("created_at"), schedule.createdAt},
            {QStringLiteral("updated_at"), schedule.updatedAt},
            {QStringLiteral("next_run_at"), schedule.nextRunAt},
            {QStringLiteral("last_run_at"), schedule.lastRunAt}};
}

std::optional<Schedule> scheduleFromJson(const QJsonObject& json, Failure* failure) {
    const QStringList allowed{QStringLiteral("id"),          QStringLiteral("name"),
                              QStringLiteral("enabled"),     QStringLiteral("prompt"),
                              QStringLiteral("target"),      QStringLiteral("rule"),
                              QStringLiteral("busy_policy"), QStringLiteral("missed_policy"),
                              QStringLiteral("created_by"),  QStringLiteral("created_at"),
                              QStringLiteral("updated_at"),  QStringLiteral("next_run_at"),
                              QStringLiteral("last_run_at")};
    if (const QString extra = firstUnknown(json, allowed); !extra.isEmpty()) {
        fail(failure, unknownFieldCode, QStringLiteral("Unknown field \"%1\".").arg(extra));
        return std::nullopt;
    }
    if (!json.contains(QStringLiteral("target"))) {
        fail(failure, missingTargetCode, QStringLiteral("target is required."));
        return std::nullopt;
    }
    if (!json.value(QStringLiteral("target")).isObject()) {
        fail(failure, invalidPayloadCode, QStringLiteral("target must be an object."));
        return std::nullopt;
    }
    const auto target = targetFromJson(json.value(QStringLiteral("target")).toObject(), failure);
    if (!target)
        return std::nullopt;
    const auto id = idField(json, QStringLiteral("id"), true, false, failure);
    if (!id)
        return std::nullopt;
    const auto name = stringField(json, QStringLiteral("name"), true, failure);
    if (!name)
        return std::nullopt;
    if (name->trimmed().isEmpty()) {
        fail(failure, invalidPayloadCode, QStringLiteral("name is required."));
        return std::nullopt;
    }
    if (!bounded(*name, maximumScheduleNameLength, QStringLiteral("name"), failure))
        return std::nullopt;
    if (!json.value(QStringLiteral("enabled")).isBool()) {
        fail(failure, invalidPayloadCode, QStringLiteral("enabled must be a boolean."));
        return std::nullopt;
    }
    QString prompt;
    if (!json.contains(QStringLiteral("prompt"))) {
        if (target->kind != TargetKind::TeamStart) {
            fail(failure, emptyPromptCode, QStringLiteral("Prompt is required."));
            return std::nullopt;
        }
    } else if (!json.value(QStringLiteral("prompt")).isString()) {
        fail(failure, invalidPayloadCode, QStringLiteral("prompt must be a string."));
        return std::nullopt;
    } else {
        prompt = json.value(QStringLiteral("prompt")).toString();
    }
    if (const Failure promptError = validatePrompt(prompt, target->kind); !promptError.ok()) {
        if (failure)
            *failure = promptError;
        return std::nullopt;
    }
    const auto rule = stringField(json, QStringLiteral("rule"), true, failure);
    if (!rule)
        return std::nullopt;
    if (const Failure ruleError = validateRule(*rule, target->kind == TargetKind::TeamStart);
        !ruleError.ok()) {
        if (failure)
            *failure = ruleError;
        return std::nullopt;
    }
    const auto busyText = stringField(json, QStringLiteral("busy_policy"), true, failure);
    if (!busyText)
        return std::nullopt;
    const auto busy = busyPolicyFromName(*busyText);
    if (!busy) {
        fail(failure, invalidEnumCode,
             QStringLiteral("busy_policy \"%1\" is not a known value.").arg(*busyText));
        return std::nullopt;
    }
    const auto missedText = stringField(json, QStringLiteral("missed_policy"), true, failure);
    if (!missedText)
        return std::nullopt;
    const auto missed = missedPolicyFromName(*missedText);
    if (!missed) {
        fail(failure, invalidEnumCode,
             QStringLiteral("missed_policy \"%1\" is not a known value.").arg(*missedText));
        return std::nullopt;
    }
    if (target->kind == TargetKind::TeamStart && *missed != MissedPolicy::RunOnce) {
        fail(failure, teamStartConstraintCode,
             QStringLiteral("A team start uses missed policy run_once."));
        return std::nullopt;
    }
    const auto createdBy = stringField(json, QStringLiteral("created_by"), true, failure);
    if (!createdBy)
        return std::nullopt;
    if (const Failure createdByError = validateCreatedBy(*createdBy); !createdByError.ok()) {
        if (failure)
            *failure = createdByError;
        return std::nullopt;
    }
    const auto createdAt = timeField(json, QStringLiteral("created_at"), false, failure);
    if (!createdAt)
        return std::nullopt;
    const auto updatedAt = timeField(json, QStringLiteral("updated_at"), false, failure);
    if (!updatedAt)
        return std::nullopt;
    const auto nextRunAt = timeField(json, QStringLiteral("next_run_at"), true, failure);
    if (!nextRunAt)
        return std::nullopt;
    const auto lastRunAt = timeField(json, QStringLiteral("last_run_at"), true, failure);
    if (!lastRunAt)
        return std::nullopt;
    Schedule schedule;
    schedule.id = *id;
    schedule.name = *name;
    schedule.enabled = json.value(QStringLiteral("enabled")).toBool();
    schedule.prompt = target->kind == TargetKind::TeamStart ? QString{} : prompt;
    schedule.target = *target;
    schedule.rule = *rule;
    schedule.busyPolicy = *busy;
    schedule.missedPolicy = *missed;
    schedule.createdBy = *createdBy;
    schedule.createdAt = *createdAt;
    schedule.updatedAt = *updatedAt;
    schedule.nextRunAt = *nextRunAt;
    schedule.lastRunAt = *lastRunAt;
    accept(failure);
    return schedule;
}

QJsonObject toJson(const Run& run) {
    return {{QStringLiteral("id"), run.id},
            {QStringLiteral("schedule_id"), run.scheduleId},
            {QStringLiteral("scheduled_for"), run.scheduledFor},
            {QStringLiteral("started_at"), run.startedAt},
            {QStringLiteral("finished_at"), run.finishedAt},
            {QStringLiteral("status"), nameOf(run.status)},
            {QStringLiteral("session_id"), run.sessionId},
            {QStringLiteral("turn_id"), run.turnId},
            {QStringLiteral("error_code"), run.errorCode},
            {QStringLiteral("error_message"), run.errorMessage}};
}

std::optional<Run> runFromJson(const QJsonObject& json, Failure* failure) {
    const QStringList allowed{QStringLiteral("id"),
                              QStringLiteral("schedule_id"),
                              QStringLiteral("scheduled_for"),
                              QStringLiteral("started_at"),
                              QStringLiteral("finished_at"),
                              QStringLiteral("status"),
                              QStringLiteral("session_id"),
                              QStringLiteral("turn_id"),
                              QStringLiteral("error_code"),
                              QStringLiteral("error_message")};
    if (const QString extra = firstUnknown(json, allowed); !extra.isEmpty()) {
        fail(failure, unknownFieldCode, QStringLiteral("Unknown field \"%1\".").arg(extra));
        return std::nullopt;
    }
    const auto id = idField(json, QStringLiteral("id"), true, false, failure);
    if (!id)
        return std::nullopt;
    const auto scheduleId = idField(json, QStringLiteral("schedule_id"), true, false, failure);
    if (!scheduleId)
        return std::nullopt;
    const auto scheduledFor = timeField(json, QStringLiteral("scheduled_for"), false, failure);
    if (!scheduledFor)
        return std::nullopt;
    const auto startedAt = timeField(json, QStringLiteral("started_at"), true, failure);
    if (!startedAt)
        return std::nullopt;
    const auto finishedAt = timeField(json, QStringLiteral("finished_at"), true, failure);
    if (!finishedAt)
        return std::nullopt;
    const auto statusText = stringField(json, QStringLiteral("status"), true, failure);
    if (!statusText)
        return std::nullopt;
    const auto sessionId = idField(json, QStringLiteral("session_id"), true, true, failure);
    if (!sessionId)
        return std::nullopt;
    const auto turnId = idField(json, QStringLiteral("turn_id"), true, true, failure);
    if (!turnId)
        return std::nullopt;
    const auto errorCode = stringField(json, QStringLiteral("error_code"), true, failure);
    if (!errorCode)
        return std::nullopt;
    const auto errorMessage = stringField(json, QStringLiteral("error_message"), true, failure);
    if (!errorMessage)
        return std::nullopt;
    const auto status = runStatusFromName(*statusText);
    if (!status) {
        fail(failure, invalidEnumCode,
             QStringLiteral("status \"%1\" is not a known value.").arg(*statusText));
        return std::nullopt;
    }
    if (!bounded(*errorCode, maximumScheduleIdLength, QStringLiteral("error_code"), failure) ||
        !bounded(*errorMessage, maximumSchedulePromptLength, QStringLiteral("error_message"),
                 failure))
        return std::nullopt;
    Run run;
    run.id = *id;
    run.scheduleId = *scheduleId;
    run.scheduledFor = *scheduledFor;
    run.startedAt = *startedAt;
    run.finishedAt = *finishedAt;
    run.status = *status;
    run.sessionId = *sessionId;
    run.turnId = *turnId;
    run.errorCode = *errorCode;
    run.errorMessage = *errorMessage;
    accept(failure);
    return run;
}

} // namespace ketplus::schedule
