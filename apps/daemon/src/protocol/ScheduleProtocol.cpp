#include "protocol/ScheduleProtocol.h"

#include <QJsonArray>

#include <utility>

namespace ketplus::daemon::protocol {
namespace {

using schedule::Failure;

void fail(Failure* failure, const char* code, const QString& message) {
    if (failure)
        *failure = Failure{QString::fromLatin1(code), message};
}

void accept(Failure* failure) {
    if (failure)
        *failure = {};
}

MessageValidation rejected(const Failure& failure) {
    MessageValidation result;
    result.code = failure.code;
    result.message = failure.message;
    return result;
}

MessageValidation accepted(QJsonObject canonical) {
    MessageValidation result;
    result.ok = true;
    result.canonical = std::move(canonical);
    return result;
}

QString firstUnknown(const QJsonObject& object, const QStringList& allowed) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        if (!allowed.contains(it.key()))
            return it.key();
    return {};
}

bool onlyKeys(const QJsonObject& object, const QStringList& allowed, Failure* failure) {
    if (const QString extra = firstUnknown(object, allowed); !extra.isEmpty()) {
        fail(failure, schedule::unknownFieldCode,
             QStringLiteral("Unknown field \"%1\".").arg(extra));
        return false;
    }
    return true;
}

std::optional<QString> requiredString(const QJsonObject& object, const QString& key,
                                      Failure* failure) {
    if (!object.contains(key)) {
        fail(failure, schedule::invalidPayloadCode, QStringLiteral("%1 is required.").arg(key));
        return std::nullopt;
    }
    if (!object.value(key).isString()) {
        fail(failure, schedule::invalidPayloadCode,
             QStringLiteral("%1 must be a string.").arg(key));
        return std::nullopt;
    }
    return object.value(key).toString();
}

QJsonObject createPayload(const schedule::Schedule& schedule) {
    return {{QStringLiteral("name"), schedule.name},
            {QStringLiteral("enabled"), schedule.enabled},
            {QStringLiteral("prompt"), schedule.prompt},
            {QStringLiteral("target"), schedule::toJson(schedule.target)},
            {QStringLiteral("rule"), schedule.rule},
            {QStringLiteral("busy_policy"), schedule::nameOf(schedule.busyPolicy)},
            {QStringLiteral("missed_policy"), schedule::nameOf(schedule.missedPolicy)}};
}

QJsonObject updatePayload(const ScheduleUpdate& update) {
    QJsonObject payload{{QStringLiteral("id"), update.id}};
    if (update.name)
        payload.insert(QStringLiteral("name"), *update.name);
    if (update.enabled)
        payload.insert(QStringLiteral("enabled"), *update.enabled);
    if (update.prompt)
        payload.insert(QStringLiteral("prompt"), *update.prompt);
    if (update.target)
        payload.insert(QStringLiteral("target"), schedule::toJson(*update.target));
    if (update.rule)
        payload.insert(QStringLiteral("rule"), *update.rule);
    if (update.busyPolicy)
        payload.insert(QStringLiteral("busy_policy"), schedule::nameOf(*update.busyPolicy));
    if (update.missedPolicy)
        payload.insert(QStringLiteral("missed_policy"), schedule::nameOf(*update.missedPolicy));
    return payload;
}

QJsonObject listPayload(const ScheduleListQuery& query) {
    QJsonObject payload;
    if (query.targetKind)
        payload.insert(QStringLiteral("target_kind"), schedule::nameOf(*query.targetKind));
    if (!query.sessionId.isEmpty())
        payload.insert(QStringLiteral("session_id"), query.sessionId);
    if (!query.teamId.isEmpty())
        payload.insert(QStringLiteral("team_id"), query.teamId);
    return payload;
}

bool wholeNumber(const QJsonValue& value, int* number) {
    if (!value.isDouble())
        return false;
    const double raw = value.toDouble();
    const int whole = static_cast<int>(raw);
    if (raw != static_cast<double>(whole))
        return false;
    *number = whole;
    return true;
}

std::optional<QJsonArray> documentArray(const QJsonObject& payload, const QString& key,
                                        Failure* failure) {
    if (!payload.value(key).isArray()) {
        fail(failure, schedule::invalidPayloadCode,
             QStringLiteral("%1 must be an array.").arg(key));
        return std::nullopt;
    }
    return payload.value(key).toArray();
}

bool successFlag(const QJsonObject& payload, Failure* failure) {
    if (!payload.value(QStringLiteral("ok")).isBool() ||
        !payload.value(QStringLiteral("ok")).toBool()) {
        fail(failure, schedule::invalidPayloadCode, QStringLiteral("ok must be true."));
        return false;
    }
    return true;
}

} // namespace

bool isScheduleRequest(const QString& type) {
    return type == QLatin1String(scheduleListType) || type == QLatin1String(scheduleGetType) ||
           type == QLatin1String(scheduleCreateType) || type == QLatin1String(scheduleUpdateType) ||
           type == QLatin1String(scheduleDeleteType) ||
           type == QLatin1String(scheduleSetEnabledType) ||
           type == QLatin1String(scheduleRunNowType) || type == QLatin1String(scheduleRunsType);
}

bool isScheduleEvent(const QString& type) {
    return type == QLatin1String(scheduleChangedEvent) || type == QLatin1String(scheduleRunEvent);
}

std::optional<ScheduleListQuery> parseScheduleList(const QJsonObject& payload, Failure* failure) {
    if (!onlyKeys(payload,
                  {QStringLiteral("target_kind"), QStringLiteral("session_id"),
                   QStringLiteral("team_id")},
                  failure))
        return std::nullopt;
    ScheduleListQuery query;
    if (payload.contains(QStringLiteral("target_kind"))) {
        const auto kindText = requiredString(payload, QStringLiteral("target_kind"), failure);
        if (!kindText)
            return std::nullopt;
        query.targetKind = schedule::targetKindFromName(*kindText);
        if (!query.targetKind) {
            fail(failure, schedule::invalidEnumCode,
                 QStringLiteral("target_kind \"%1\" is not a known value.").arg(*kindText));
            return std::nullopt;
        }
    }
    if (payload.contains(QStringLiteral("session_id"))) {
        const auto sessionId = requiredString(payload, QStringLiteral("session_id"), failure);
        if (!sessionId)
            return std::nullopt;
        if (const Failure idError =
                schedule::validateScheduleId(*sessionId, QStringLiteral("session_id"), false);
            !idError.ok()) {
            if (failure)
                *failure = idError;
            return std::nullopt;
        }
        query.sessionId = *sessionId;
    }
    if (payload.contains(QStringLiteral("team_id"))) {
        const auto teamId = requiredString(payload, QStringLiteral("team_id"), failure);
        if (!teamId)
            return std::nullopt;
        if (const Failure idError =
                schedule::validateScheduleId(*teamId, QStringLiteral("team_id"), false);
            !idError.ok()) {
            if (failure)
                *failure = idError;
            return std::nullopt;
        }
        query.teamId = *teamId;
    }
    accept(failure);
    return query;
}

std::optional<QString> parseScheduleId(const QJsonObject& payload, Failure* failure) {
    if (!onlyKeys(payload, {QStringLiteral("id")}, failure))
        return std::nullopt;
    const auto id = requiredString(payload, QStringLiteral("id"), failure);
    if (!id)
        return std::nullopt;
    if (const Failure idError = schedule::validateScheduleId(*id, QStringLiteral("id"), false);
        !idError.ok()) {
        if (failure)
            *failure = idError;
        return std::nullopt;
    }
    accept(failure);
    return id;
}

std::optional<schedule::Schedule> parseScheduleCreate(const QJsonObject& payload,
                                                      Failure* failure) {
    // created_by is server-owned, like id and the timestamps. The handler sets it
    // from the connection, so a client that sends it is rejected.
    const QStringList allowed{QStringLiteral("name"),         QStringLiteral("enabled"),
                              QStringLiteral("prompt"),       QStringLiteral("target"),
                              QStringLiteral("rule"),         QStringLiteral("busy_policy"),
                              QStringLiteral("missed_policy")};
    if (!onlyKeys(payload, allowed, failure))
        return std::nullopt;
    if (!payload.contains(QStringLiteral("target"))) {
        fail(failure, schedule::missingTargetCode, QStringLiteral("target is required."));
        return std::nullopt;
    }
    if (!payload.value(QStringLiteral("target")).isObject()) {
        fail(failure, schedule::invalidPayloadCode, QStringLiteral("target must be an object."));
        return std::nullopt;
    }
    const auto target =
        schedule::targetFromJson(payload.value(QStringLiteral("target")).toObject(), failure);
    if (!target)
        return std::nullopt;
    const auto name = requiredString(payload, QStringLiteral("name"), failure);
    if (!name)
        return std::nullopt;
    if (name->trimmed().isEmpty()) {
        fail(failure, schedule::invalidPayloadCode, QStringLiteral("name is required."));
        return std::nullopt;
    }
    if (name->size() > schedule::maximumScheduleNameLength) {
        fail(failure, schedule::invalidPayloadCode, QStringLiteral("name is too long."));
        return std::nullopt;
    }
    if (!payload.value(QStringLiteral("enabled")).isBool()) {
        fail(failure, schedule::invalidPayloadCode, QStringLiteral("enabled must be a boolean."));
        return std::nullopt;
    }
    QString prompt;
    if (payload.contains(QStringLiteral("prompt"))) {
        const auto text = requiredString(payload, QStringLiteral("prompt"), failure);
        if (!text)
            return std::nullopt;
        prompt = *text;
    }
    if (const Failure promptError = schedule::validatePrompt(prompt, target->kind);
        !promptError.ok()) {
        if (failure)
            *failure = promptError;
        return std::nullopt;
    }
    const auto rule = requiredString(payload, QStringLiteral("rule"), failure);
    if (!rule)
        return std::nullopt;
    if (const Failure ruleError =
            schedule::validateRule(*rule, target->kind == schedule::TargetKind::TeamStart);
        !ruleError.ok()) {
        if (failure)
            *failure = ruleError;
        return std::nullopt;
    }
    const auto busyText = requiredString(payload, QStringLiteral("busy_policy"), failure);
    if (!busyText)
        return std::nullopt;
    const auto busy = schedule::busyPolicyFromName(*busyText);
    if (!busy) {
        fail(failure, schedule::invalidEnumCode,
             QStringLiteral("busy_policy \"%1\" is not a known value.").arg(*busyText));
        return std::nullopt;
    }
    const auto missedText = requiredString(payload, QStringLiteral("missed_policy"), failure);
    if (!missedText)
        return std::nullopt;
    const auto missed = schedule::missedPolicyFromName(*missedText);
    if (!missed) {
        fail(failure, schedule::invalidEnumCode,
             QStringLiteral("missed_policy \"%1\" is not a known value.").arg(*missedText));
        return std::nullopt;
    }
    if (target->kind == schedule::TargetKind::TeamStart &&
        *missed != schedule::MissedPolicy::RunOnce) {
        fail(failure, schedule::teamStartConstraintCode,
             QStringLiteral("A team start uses missed policy run_once."));
        return std::nullopt;
    }
    schedule::Schedule schedule;
    schedule.name = *name;
    schedule.enabled = payload.value(QStringLiteral("enabled")).toBool();
    schedule.prompt = target->kind == schedule::TargetKind::TeamStart ? QString{} : prompt;
    schedule.target = *target;
    schedule.rule = *rule;
    schedule.busyPolicy = *busy;
    schedule.missedPolicy = *missed;
    accept(failure);
    return schedule;
}

std::optional<ScheduleUpdate> parseScheduleUpdate(const QJsonObject& payload, Failure* failure) {
    const QStringList allowed{QStringLiteral("id"),          QStringLiteral("name"),
                              QStringLiteral("enabled"),     QStringLiteral("prompt"),
                              QStringLiteral("target"),      QStringLiteral("rule"),
                              QStringLiteral("busy_policy"), QStringLiteral("missed_policy")};
    if (!onlyKeys(payload, allowed, failure))
        return std::nullopt;
    const auto id = requiredString(payload, QStringLiteral("id"), failure);
    if (!id)
        return std::nullopt;
    if (const Failure idError = schedule::validateScheduleId(*id, QStringLiteral("id"), false);
        !idError.ok()) {
        if (failure)
            *failure = idError;
        return std::nullopt;
    }
    ScheduleUpdate update;
    update.id = *id;
    if (payload.contains(QStringLiteral("target"))) {
        if (!payload.value(QStringLiteral("target")).isObject()) {
            fail(failure, schedule::invalidPayloadCode,
                 QStringLiteral("target must be an object."));
            return std::nullopt;
        }
        const auto target =
            schedule::targetFromJson(payload.value(QStringLiteral("target")).toObject(), failure);
        if (!target)
            return std::nullopt;
        update.target = *target;
    }
    if (payload.contains(QStringLiteral("name"))) {
        const auto name = requiredString(payload, QStringLiteral("name"), failure);
        if (!name)
            return std::nullopt;
        if (name->trimmed().isEmpty()) {
            fail(failure, schedule::invalidPayloadCode, QStringLiteral("name is required."));
            return std::nullopt;
        }
        if (name->size() > schedule::maximumScheduleNameLength) {
            fail(failure, schedule::invalidPayloadCode, QStringLiteral("name is too long."));
            return std::nullopt;
        }
        update.name = *name;
    }
    if (payload.contains(QStringLiteral("enabled"))) {
        if (!payload.value(QStringLiteral("enabled")).isBool()) {
            fail(failure, schedule::invalidPayloadCode,
                 QStringLiteral("enabled must be a boolean."));
            return std::nullopt;
        }
        update.enabled = payload.value(QStringLiteral("enabled")).toBool();
    }
    if (payload.contains(QStringLiteral("prompt"))) {
        const auto prompt = requiredString(payload, QStringLiteral("prompt"), failure);
        if (!prompt)
            return std::nullopt;
        const schedule::TargetKind kind =
            update.target ? update.target->kind : schedule::TargetKind::Conversation;
        if (const Failure promptError = schedule::validatePrompt(*prompt, kind);
            !promptError.ok()) {
            if (failure)
                *failure = promptError;
            return std::nullopt;
        }
        update.prompt = kind == schedule::TargetKind::TeamStart ? QString{} : *prompt;
    }
    if (payload.contains(QStringLiteral("rule"))) {
        const auto rule = requiredString(payload, QStringLiteral("rule"), failure);
        if (!rule)
            return std::nullopt;
        const bool teamStart =
            update.target && update.target->kind == schedule::TargetKind::TeamStart;
        if (const Failure ruleError = schedule::validateRule(*rule, teamStart); !ruleError.ok()) {
            if (failure)
                *failure = ruleError;
            return std::nullopt;
        }
        update.rule = *rule;
    }
    if (payload.contains(QStringLiteral("busy_policy"))) {
        const auto text = requiredString(payload, QStringLiteral("busy_policy"), failure);
        if (!text)
            return std::nullopt;
        const auto busy = schedule::busyPolicyFromName(*text);
        if (!busy) {
            fail(failure, schedule::invalidEnumCode,
                 QStringLiteral("busy_policy \"%1\" is not a known value.").arg(*text));
            return std::nullopt;
        }
        update.busyPolicy = *busy;
    }
    if (payload.contains(QStringLiteral("missed_policy"))) {
        const auto text = requiredString(payload, QStringLiteral("missed_policy"), failure);
        if (!text)
            return std::nullopt;
        const auto missed = schedule::missedPolicyFromName(*text);
        if (!missed) {
            fail(failure, schedule::invalidEnumCode,
                 QStringLiteral("missed_policy \"%1\" is not a known value.").arg(*text));
            return std::nullopt;
        }
        if (update.target && update.target->kind == schedule::TargetKind::TeamStart &&
            *missed != schedule::MissedPolicy::RunOnce) {
            fail(failure, schedule::teamStartConstraintCode,
                 QStringLiteral("A team start uses missed policy run_once."));
            return std::nullopt;
        }
        update.missedPolicy = *missed;
    }
    if (!update.name && !update.enabled && !update.prompt && !update.target && !update.rule &&
        !update.busyPolicy && !update.missedPolicy) {
        fail(failure, schedule::invalidPayloadCode, QStringLiteral("Change at least one field."));
        return std::nullopt;
    }
    accept(failure);
    return update;
}

std::optional<ScheduleEnabled> parseScheduleSetEnabled(const QJsonObject& payload,
                                                       Failure* failure) {
    if (!onlyKeys(payload, {QStringLiteral("id"), QStringLiteral("enabled")}, failure))
        return std::nullopt;
    const auto id = requiredString(payload, QStringLiteral("id"), failure);
    if (!id)
        return std::nullopt;
    if (const Failure idError = schedule::validateScheduleId(*id, QStringLiteral("id"), false);
        !idError.ok()) {
        if (failure)
            *failure = idError;
        return std::nullopt;
    }
    if (!payload.value(QStringLiteral("enabled")).isBool()) {
        fail(failure, schedule::invalidPayloadCode, QStringLiteral("enabled must be a boolean."));
        return std::nullopt;
    }
    accept(failure);
    return ScheduleEnabled{*id, payload.value(QStringLiteral("enabled")).toBool()};
}

std::optional<ScheduleRunsQuery> parseScheduleRuns(const QJsonObject& payload, Failure* failure) {
    if (!onlyKeys(payload,
                  {QStringLiteral("id"), QStringLiteral("limit"), QStringLiteral("before_id")},
                  failure))
        return std::nullopt;
    const auto id = requiredString(payload, QStringLiteral("id"), failure);
    if (!id)
        return std::nullopt;
    if (const Failure idError = schedule::validateScheduleId(*id, QStringLiteral("id"), false);
        !idError.ok()) {
        if (failure)
            *failure = idError;
        return std::nullopt;
    }
    ScheduleRunsQuery query;
    query.id = *id;
    if (payload.contains(QStringLiteral("limit"))) {
        int limit = 0;
        if (!wholeNumber(payload.value(QStringLiteral("limit")), &limit) || limit < 1 ||
            limit > schedule::maximumRunsPage) {
            fail(failure, schedule::invalidPayloadCode,
                 QStringLiteral("limit must be an integer from 1 to %1.")
                     .arg(schedule::maximumRunsPage));
            return std::nullopt;
        }
        query.limit = limit;
    }
    if (payload.contains(QStringLiteral("before_id"))) {
        const auto beforeId = requiredString(payload, QStringLiteral("before_id"), failure);
        if (!beforeId)
            return std::nullopt;
        if (const Failure beforeError =
                schedule::validateScheduleId(*beforeId, QStringLiteral("before_id"), false);
            !beforeError.ok()) {
            if (failure)
                *failure = beforeError;
            return std::nullopt;
        }
        query.beforeId = *beforeId;
    }
    accept(failure);
    return query;
}

MessageValidation validateScheduleRequest(const QString& type, const QJsonObject& payload) {
    Failure failure;
    if (type == QLatin1String(scheduleListType)) {
        const auto query = parseScheduleList(payload, &failure);
        return query ? accepted(listPayload(*query)) : rejected(failure);
    }
    if (type == QLatin1String(scheduleGetType) || type == QLatin1String(scheduleDeleteType) ||
        type == QLatin1String(scheduleRunNowType)) {
        const auto id = parseScheduleId(payload, &failure);
        return id ? accepted(QJsonObject{{QStringLiteral("id"), *id}}) : rejected(failure);
    }
    if (type == QLatin1String(scheduleCreateType)) {
        const auto schedule = parseScheduleCreate(payload, &failure);
        return schedule ? accepted(createPayload(*schedule)) : rejected(failure);
    }
    if (type == QLatin1String(scheduleUpdateType)) {
        const auto update = parseScheduleUpdate(payload, &failure);
        return update ? accepted(updatePayload(*update)) : rejected(failure);
    }
    if (type == QLatin1String(scheduleSetEnabledType)) {
        const auto enabled = parseScheduleSetEnabled(payload, &failure);
        return enabled ? accepted(QJsonObject{{QStringLiteral("id"), enabled->id},
                                              {QStringLiteral("enabled"), enabled->enabled}})
                       : rejected(failure);
    }
    if (type == QLatin1String(scheduleRunsType)) {
        const auto query = parseScheduleRuns(payload, &failure);
        if (!query)
            return rejected(failure);
        QJsonObject canonical{{QStringLiteral("id"), query->id},
                              {QStringLiteral("limit"), query->limit}};
        if (!query->beforeId.isEmpty())
            canonical.insert(QStringLiteral("before_id"), query->beforeId);
        return accepted(canonical);
    }
    fail(&failure, schedule::invalidPayloadCode, QStringLiteral("Unknown schedule request."));
    return rejected(failure);
}

QJsonObject scheduleChangedPayload(const QString& change, const schedule::Schedule& schedule) {
    return {{QStringLiteral("change"), change},
            {QStringLiteral("schedule"), schedule::toJson(schedule)}};
}

QJsonObject scheduleRunPayload(const schedule::Run& run) {
    return {{QStringLiteral("run"), schedule::toJson(run)}};
}

std::optional<ScheduleChanged> parseScheduleChanged(const QJsonObject& payload, Failure* failure) {
    if (!onlyKeys(payload, {QStringLiteral("change"), QStringLiteral("schedule")}, failure))
        return std::nullopt;
    const auto change = requiredString(payload, QStringLiteral("change"), failure);
    if (!change)
        return std::nullopt;
    if (*change != QLatin1String(scheduleCreatedChange) &&
        *change != QLatin1String(scheduleUpdatedChange) &&
        *change != QLatin1String(scheduleDeletedChange)) {
        fail(failure, schedule::invalidEnumCode,
             QStringLiteral("change \"%1\" is not a known value.").arg(*change));
        return std::nullopt;
    }
    if (!payload.value(QStringLiteral("schedule")).isObject()) {
        fail(failure, schedule::invalidPayloadCode, QStringLiteral("schedule must be an object."));
        return std::nullopt;
    }
    const auto schedule =
        schedule::scheduleFromJson(payload.value(QStringLiteral("schedule")).toObject(), failure);
    if (!schedule)
        return std::nullopt;
    accept(failure);
    return ScheduleChanged{*change, *schedule};
}

std::optional<schedule::Run> parseScheduleRunEvent(const QJsonObject& payload, Failure* failure) {
    if (!onlyKeys(payload, {QStringLiteral("run")}, failure))
        return std::nullopt;
    if (!payload.value(QStringLiteral("run")).isObject()) {
        fail(failure, schedule::invalidPayloadCode, QStringLiteral("run must be an object."));
        return std::nullopt;
    }
    return schedule::runFromJson(payload.value(QStringLiteral("run")).toObject(), failure);
}

MessageValidation validateScheduleEvent(const QString& type, const QJsonObject& payload) {
    Failure failure;
    if (type == QLatin1String(scheduleChangedEvent)) {
        const auto changed = parseScheduleChanged(payload, &failure);
        return changed ? accepted(scheduleChangedPayload(changed->change, changed->schedule))
                       : rejected(failure);
    }
    if (type == QLatin1String(scheduleRunEvent)) {
        const auto run = parseScheduleRunEvent(payload, &failure);
        return run ? accepted(scheduleRunPayload(*run)) : rejected(failure);
    }
    fail(&failure, schedule::invalidPayloadCode, QStringLiteral("Unknown schedule event."));
    return rejected(failure);
}

QJsonObject scheduleListResult(const QList<schedule::Schedule>& schedules) {
    QJsonArray array;
    for (const schedule::Schedule& schedule : schedules)
        array.append(schedule::toJson(schedule));
    return {{QStringLiteral("ok"), true}, {QStringLiteral("schedules"), array}};
}

QJsonObject scheduleDocumentResult(const schedule::Schedule& schedule) {
    return {{QStringLiteral("ok"), true}, {QStringLiteral("schedule"), schedule::toJson(schedule)}};
}

QJsonObject scheduleDeletedResult(const QString& id) {
    return {{QStringLiteral("ok"), true}, {QStringLiteral("id"), id}};
}

QJsonObject scheduleRunsResult(const QList<schedule::Run>& runs, const bool hasMore) {
    QJsonArray array;
    for (const schedule::Run& run : runs)
        array.append(schedule::toJson(run));
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("runs"), array},
            {QStringLiteral("has_more"), hasMore}};
}

QJsonObject scheduleRunResult(const schedule::Run& run) {
    return {{QStringLiteral("ok"), true}, {QStringLiteral("run"), schedule::toJson(run)}};
}

MessageValidation validateScheduleResult(const QString& type, const QJsonObject& payload) {
    Failure failure;
    auto done = [&](QJsonObject canonical) {
        accept(&failure);
        return accepted(std::move(canonical));
    };
    if (type == QLatin1String(scheduleListType)) {
        if (!onlyKeys(payload, {QStringLiteral("ok"), QStringLiteral("schedules")}, &failure) ||
            !successFlag(payload, &failure))
            return rejected(failure);
        const auto schedules = documentArray(payload, QStringLiteral("schedules"), &failure);
        if (!schedules)
            return rejected(failure);
        QList<schedule::Schedule> parsed;
        for (const QJsonValue& value : *schedules) {
            if (!value.isObject()) {
                fail(&failure, schedule::invalidPayloadCode,
                     QStringLiteral("schedules must contain objects."));
                return rejected(failure);
            }
            const auto schedule = schedule::scheduleFromJson(value.toObject(), &failure);
            if (!schedule)
                return rejected(failure);
            parsed.append(*schedule);
        }
        return done(scheduleListResult(parsed));
    }
    if (type == QLatin1String(scheduleGetType) || type == QLatin1String(scheduleCreateType) ||
        type == QLatin1String(scheduleUpdateType) ||
        type == QLatin1String(scheduleSetEnabledType)) {
        if (!onlyKeys(payload, {QStringLiteral("ok"), QStringLiteral("schedule")}, &failure) ||
            !successFlag(payload, &failure))
            return rejected(failure);
        if (!payload.value(QStringLiteral("schedule")).isObject()) {
            fail(&failure, schedule::invalidPayloadCode,
                 QStringLiteral("schedule must be an object."));
            return rejected(failure);
        }
        const auto schedule = schedule::scheduleFromJson(
            payload.value(QStringLiteral("schedule")).toObject(), &failure);
        if (!schedule)
            return rejected(failure);
        return done(scheduleDocumentResult(*schedule));
    }
    if (type == QLatin1String(scheduleDeleteType)) {
        if (!onlyKeys(payload, {QStringLiteral("ok"), QStringLiteral("id")}, &failure) ||
            !successFlag(payload, &failure))
            return rejected(failure);
        const auto id = requiredString(payload, QStringLiteral("id"), &failure);
        if (!id)
            return rejected(failure);
        if (const Failure idError = schedule::validateScheduleId(*id, QStringLiteral("id"), false);
            !idError.ok())
            return rejected(idError);
        return done(scheduleDeletedResult(*id));
    }
    if (type == QLatin1String(scheduleRunsType)) {
        if (!onlyKeys(payload,
                      {QStringLiteral("ok"), QStringLiteral("runs"), QStringLiteral("has_more")},
                      &failure) ||
            !successFlag(payload, &failure))
            return rejected(failure);
        if (!payload.value(QStringLiteral("has_more")).isBool()) {
            fail(&failure, schedule::invalidPayloadCode,
                 QStringLiteral("has_more must be a boolean."));
            return rejected(failure);
        }
        const auto runs = documentArray(payload, QStringLiteral("runs"), &failure);
        if (!runs)
            return rejected(failure);
        QList<schedule::Run> parsed;
        for (const QJsonValue& value : *runs) {
            if (!value.isObject()) {
                fail(&failure, schedule::invalidPayloadCode,
                     QStringLiteral("runs must contain objects."));
                return rejected(failure);
            }
            const auto run = schedule::runFromJson(value.toObject(), &failure);
            if (!run)
                return rejected(failure);
            parsed.append(*run);
        }
        return done(scheduleRunsResult(parsed, payload.value(QStringLiteral("has_more")).toBool()));
    }
    if (type == QLatin1String(scheduleRunNowType)) {
        if (!onlyKeys(payload, {QStringLiteral("ok"), QStringLiteral("run")}, &failure) ||
            !successFlag(payload, &failure))
            return rejected(failure);
        if (!payload.value(QStringLiteral("run")).isObject()) {
            fail(&failure, schedule::invalidPayloadCode, QStringLiteral("run must be an object."));
            return rejected(failure);
        }
        const auto run =
            schedule::runFromJson(payload.value(QStringLiteral("run")).toObject(), &failure);
        if (!run)
            return rejected(failure);
        return done(scheduleRunResult(*run));
    }
    fail(&failure, schedule::invalidPayloadCode, QStringLiteral("Unknown schedule result."));
    return rejected(failure);
}

} // namespace ketplus::daemon::protocol
