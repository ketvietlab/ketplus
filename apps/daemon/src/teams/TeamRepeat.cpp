#include "teams/TeamRepeat.h"

#include "schedule/ScheduleRule.h"

namespace ketplus::daemon::teams {
namespace {

TeamError invalid(const QString& message) { return {QStringLiteral("repeat_invalid"), message}; }

} // namespace

std::optional<TeamError> parseTeamRepeat(const QJsonObject& repeat, const QDateTime& now,
                                         TeamRepeatPlan* plan) {
    if (repeat.isEmpty())
        return invalid(QStringLiteral("repeat is invalid."));
    for (auto it = repeat.constBegin(); it != repeat.constEnd(); ++it) {
        const QString key = it.key();
        if (key != QStringLiteral("rule") && key != QStringLiteral("work_order") &&
            key != QStringLiteral("busy_policy") && key != QStringLiteral("missed_policy"))
            return invalid(QStringLiteral("repeat has an unknown field."));
    }
    const QJsonValue ruleValue = repeat.value(QStringLiteral("rule"));
    if (!ruleValue.isObject())
        return invalid(QStringLiteral("repeat.rule must be an object."));
    const QString workOrder = repeat.value(QStringLiteral("work_order")).toString().trimmed();
    if (!repeat.value(QStringLiteral("work_order")).isString() || workOrder.isEmpty())
        return invalid(QStringLiteral("repeat.work_order must be a non-empty string."));
    if (workOrder.toUtf8().size() > 8 * 1024)
        return invalid(QStringLiteral("repeat.work_order is too long."));
    const auto parsed = schedule::parseRule(ruleValue.toObject());
    if (!parsed.ok())
        return TeamError{QStringLiteral("invalid_rule"), parsed.message};
    const QDateTime clock = now.isValid() ? now : QDateTime::currentDateTimeUtc();
    const auto next = schedule::nextRunAfter(*parsed.rule, clock);
    if (!next)
        return invalid(QStringLiteral("This rule has no upcoming run."));
    QString busy = repeat.value(QStringLiteral("busy_policy")).toString(QStringLiteral("queue"));
    if (!repeat.contains(QStringLiteral("busy_policy")))
        busy = QStringLiteral("queue");
    if (busy != QStringLiteral("queue") && busy != QStringLiteral("skip"))
        return TeamError{QStringLiteral("invalid_enum"),
                         QStringLiteral("busy_policy must be queue or skip.")};
    QString missed =
        repeat.value(QStringLiteral("missed_policy")).toString(QStringLiteral("run_once"));
    if (!repeat.contains(QStringLiteral("missed_policy")))
        missed = QStringLiteral("run_once");
    if (missed != QStringLiteral("run_once") && missed != QStringLiteral("skip"))
        return TeamError{QStringLiteral("invalid_enum"),
                         QStringLiteral("missed_policy must be run_once or skip.")};
    if (!plan)
        return std::nullopt;
    plan->ruleJson = QString::fromUtf8(schedule::serialize(*parsed.rule));
    plan->workOrder = workOrder;
    plan->busyPolicy = busy;
    plan->missedPolicy = missed;
    plan->nextRunAt = next->toUTC().toString(Qt::ISODateWithMs);
    return std::nullopt;
}

} // namespace ketplus::daemon::teams
