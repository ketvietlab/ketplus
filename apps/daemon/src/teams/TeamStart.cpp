#include "teams/TeamStart.h"

#include "schedule/ScheduleRule.h"

#include <QRegularExpression>
#include <QTimeZone>

namespace ketplus::daemon::teams {
namespace {

constexpr qint64 minimumLeadMs = 60 * 1000;
constexpr qint64 maximumLeadMs = 30LL * 24 * 60 * 60 * 1000;

TeamStartError error(const QString& code, const QString& message) { return {code, message}; }

} // namespace

std::optional<TeamStartError> parseTeamStart(const QJsonObject& startAt, TeamStartRequest* request) {
    if (!request)
        return error(QStringLiteral("start_at_invalid"), QStringLiteral("start_at is invalid."));
    for (auto it = startAt.constBegin(); it != startAt.constEnd(); ++it) {
        if (it.key() != QStringLiteral("at") && it.key() != QStringLiteral("timezone"))
            return error(QStringLiteral("start_at_invalid"),
                         QStringLiteral("start_at has an unknown field."));
    }
    const QString at = startAt.value(QStringLiteral("at")).toString();
    const QString zone = startAt.value(QStringLiteral("timezone")).toString();
    static const QRegularExpression civil(QStringLiteral(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}$)"));
    if (!civil.match(at).hasMatch() || !QTimeZone(zone.toUtf8()).isValid())
        return error(QStringLiteral("start_at_invalid"),
                     QStringLiteral("start_at needs a local time YYYY-MM-DDTHH:mm and an IANA "
                                    "timezone."));
    request->at = at;
    request->timeZone = zone;
    return std::nullopt;
}

std::optional<TeamStartError> planTeamStart(const TeamStartRequest& request, const QDateTime& now,
                                            TeamStartPlan* plan) {
    if (!plan || !now.isValid())
        return error(QStringLiteral("start_at_invalid"), QStringLiteral("start_at is invalid."));
    const QStringList parts = request.at.split(QLatin1Char('T'));
    schedule::ScheduleRule rule;
    rule.kind = schedule::Kind::Once;
    rule.timeZone = QTimeZone(request.timeZone.toUtf8());
    rule.date = QDate::fromString(parts.value(0), Qt::ISODate);
    rule.time = QTime::fromString(parts.value(1), QStringLiteral("HH:mm"));
    if (!rule.date.isValid() || !rule.time.isValid() || !rule.timeZone.isValid())
        return error(QStringLiteral("start_at_invalid"),
                     QStringLiteral("start_at needs a local time YYYY-MM-DDTHH:mm and an IANA "
                                    "timezone."));
    const schedule::ParseResult parsed = schedule::parseRule(schedule::serialize(rule));
    if (!parsed.ok())
        return error(QStringLiteral("start_at_invalid"), parsed.message);
    const auto next = schedule::nextRunAfter(*parsed.rule, now.toUTC());
    if (!next)
        return error(QStringLiteral("start_at_too_soon"),
                     QStringLiteral("The team can start at least one minute from now."));
    const qint64 lead = now.toUTC().msecsTo(next->toUTC());
    if (lead < minimumLeadMs)
        return error(QStringLiteral("start_at_too_soon"),
                     QStringLiteral("The team can start at least one minute from now."));
    if (lead > maximumLeadMs)
        return error(QStringLiteral("start_at_too_late"),
                     QStringLiteral("The team can start at most 30 days from now."));
    plan->ruleJson = QString::fromUtf8(schedule::serialize(*parsed.rule));
    plan->nextRunAt = next->toUTC().toString(Qt::ISODateWithMs);
    return std::nullopt;
}

} // namespace ketplus::daemon::teams
