#include "schedule/ScheduleRule.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ketplus::daemon::schedule {
namespace {

ParseResult failure(const QString& code, const QString& message) {
    ParseResult result;
    result.code = code;
    result.message = message;
    return result;
}

ParseResult success(ScheduleRule rule) {
    ParseResult result;
    result.rule = std::move(rule);
    return result;
}

bool knownKey(const QString& key) {
    return key == QStringLiteral("kind") || key == QStringLiteral("time_zone") ||
           key == QStringLiteral("time") || key == QStringLiteral("at") ||
           key == QStringLiteral("days") || key == QStringLiteral("every_hours") ||
           key == QStringLiteral("anchor");
}

bool allowedFor(Kind kind, const QString& key) {
    if (key == QStringLiteral("kind") || key == QStringLiteral("time_zone"))
        return true;
    switch (kind) {
    case Kind::Once:
        return key == QStringLiteral("at");
    case Kind::Daily:
    case Kind::Weekdays:
        return key == QStringLiteral("time");
    case Kind::Weekly:
        return key == QStringLiteral("time") || key == QStringLiteral("days");
    case Kind::Interval:
        return key == QStringLiteral("every_hours") || key == QStringLiteral("anchor");
    }
    return false;
}

const char* kindToken(Kind kind) {
    switch (kind) {
    case Kind::Once:
        return "once";
    case Kind::Daily:
        return "daily";
    case Kind::Weekdays:
        return "weekdays";
    case Kind::Weekly:
        return "weekly";
    case Kind::Interval:
        return "interval";
    }
    return nullptr;
}

std::optional<Kind> kindFrom(const QString& token) {
    if (token == QStringLiteral("once"))
        return Kind::Once;
    if (token == QStringLiteral("daily"))
        return Kind::Daily;
    if (token == QStringLiteral("weekdays"))
        return Kind::Weekdays;
    if (token == QStringLiteral("weekly"))
        return Kind::Weekly;
    if (token == QStringLiteral("interval"))
        return Kind::Interval;
    return std::nullopt;
}

bool isDigit(QChar ch) { return ch >= QLatin1Char('0') && ch <= QLatin1Char('9'); }

int digits(const QString& text, int offset, int count) {
    int value = 0;
    for (int i = 0; i < count; ++i) {
        const QChar ch = text.at(offset + i);
        if (!isDigit(ch))
            return -1;
        value = value * 10 + (ch.unicode() - u'0');
    }
    return value;
}

std::optional<QTime> parseClock(const QString& text) {
    if (text.size() != 5 || text.at(2) != QLatin1Char(':'))
        return std::nullopt;
    const int hour = digits(text, 0, 2);
    const int minute = digits(text, 3, 2);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
        return std::nullopt;
    return QTime(hour, minute);
}

std::optional<QDate> parseDate(const QString& text) {
    if (text.size() != 10 || text.at(4) != QLatin1Char('-') || text.at(7) != QLatin1Char('-'))
        return std::nullopt;
    const int year = digits(text, 0, 4);
    const int month = digits(text, 5, 2);
    const int day = digits(text, 8, 2);
    if (year < 0 || month < 0 || day < 0)
        return std::nullopt;
    const QDate date(year, month, day);
    if (!date.isValid())
        return std::nullopt;
    return date;
}

struct Civil final {
    QDate date;
    QTime time;
};

std::optional<Civil> parseCivil(const QString& text) {
    if (text.size() != 16 || text.at(10) != QLatin1Char('T'))
        return std::nullopt;
    const auto date = parseDate(text.left(10));
    const auto time = parseClock(text.mid(11));
    if (!date || !time)
        return std::nullopt;
    return Civil{*date, *time};
}

std::optional<QTimeZone> parseZone(const QJsonValue& value) {
    if (!value.isString())
        return std::nullopt;
    const QString text = value.toString();
    if (text.isEmpty() || text.size() > 128)
        return std::nullopt;
    const QByteArray id = text.toUtf8();
    if (!QTimeZone::isTimeZoneIdAvailable(id))
        return std::nullopt;
    // Qt also accepts offset spellings such as "GMT+7" and "UTC+05:30". Those are
    // not IANA ids (and Etc/GMT+7 means the opposite of GMT+7), so a rule names a
    // region id, UTC, or an Etc/GMT entry instead.
    const bool offsetSpelling = (id.startsWith("GMT") || id.startsWith("UTC")) && id.size() > 3 &&
                                (id.at(3) == '+' || id.at(3) == '-');
    if (offsetSpelling)
        return std::nullopt;
    QTimeZone zone(id);
    if (!zone.isValid())
        return std::nullopt;
    return zone;
}

std::optional<int> dayFromToken(const QString& token) {
    if (token == QStringLiteral("mon"))
        return Qt::Monday;
    if (token == QStringLiteral("tue"))
        return Qt::Tuesday;
    if (token == QStringLiteral("wed"))
        return Qt::Wednesday;
    if (token == QStringLiteral("thu"))
        return Qt::Thursday;
    if (token == QStringLiteral("fri"))
        return Qt::Friday;
    if (token == QStringLiteral("sat"))
        return Qt::Saturday;
    if (token == QStringLiteral("sun"))
        return Qt::Sunday;
    return std::nullopt;
}

const char* dayToken(int day) {
    switch (day) {
    case Qt::Monday:
        return "mon";
    case Qt::Tuesday:
        return "tue";
    case Qt::Wednesday:
        return "wed";
    case Qt::Thursday:
        return "thu";
    case Qt::Friday:
        return "fri";
    case Qt::Saturday:
        return "sat";
    case Qt::Sunday:
        return "sun";
    default:
        return nullptr;
    }
}

const char* dayLabel(int day) {
    switch (day) {
    case Qt::Monday:
        return "Mon";
    case Qt::Tuesday:
        return "Tue";
    case Qt::Wednesday:
        return "Wed";
    case Qt::Thursday:
        return "Thu";
    case Qt::Friday:
        return "Fri";
    case Qt::Saturday:
        return "Sat";
    case Qt::Sunday:
        return "Sun";
    default:
        return nullptr;
    }
}

QString formatClock(QTime time) {
    return QStringLiteral("%1:%2")
        .arg(time.hour(), 2, 10, QChar(u'0'))
        .arg(time.minute(), 2, 10, QChar(u'0'));
}

QString formatDate(QDate date) {
    return QStringLiteral("%1-%2-%3")
        .arg(date.year(), 4, 10, QChar(u'0'))
        .arg(date.month(), 2, 10, QChar(u'0'))
        .arg(date.day(), 2, 10, QChar(u'0'));
}

QString formatCivil(QDate date, QTime time) {
    return formatDate(date) + QLatin1Char('T') + formatClock(time);
}

QString gmtOffset(int offsetSeconds) {
    if (offsetSeconds == 0)
        return QStringLiteral("GMT0");
    const int sign = offsetSeconds < 0 ? -1 : 1;
    const int absolute = offsetSeconds * sign;
    const int hours = absolute / 3600;
    const int minutes = (absolute % 3600) / 60;
    const int seconds = absolute % 60;
    const QChar signChar = sign < 0 ? QLatin1Char('-') : QLatin1Char('+');
    if (minutes == 0 && seconds == 0)
        return QStringLiteral("GMT%1%2").arg(signChar).arg(hours);
    if (seconds == 0)
        return QStringLiteral("GMT%1%2:%3")
            .arg(signChar)
            .arg(hours)
            .arg(minutes, 2, 10, QChar(u'0'));
    return QStringLiteral("GMT%1%2:%3:%4")
        .arg(signChar)
        .arg(hours)
        .arg(minutes, 2, 10, QChar(u'0'))
        .arg(seconds, 2, 10, QChar(u'0'));
}

// Maps a civil date and clock time in `zone` to one UTC instant.
// A fold (the same civil time twice) keeps the earlier instant. A gap (the civil
// time does not exist) keeps the transition instant that opens the other side of
// the gap, which is the first valid instant after the skipped times. Qt's own
// PreferBefore / PreferAfter resolutions shift a gap by the distance into it
// (02:30 becomes 01:30 or 03:30), so a gap is detected by those two results
// disagreeing about the wall clock, then replaced by the transition they bracket.
QDateTime resolveLocal(QDate date, QTime time, const QTimeZone& zone) {
    if (!date.isValid() || !time.isValid() || !zone.isValid())
        return {};
    using Resolution = QDateTime::TransitionResolution;
    const QDateTime before(date, time, zone, Resolution::PreferBefore);
    const QDateTime after(date, time, zone, Resolution::PreferAfter);
    const auto wall = [&](const QDateTime& datetime) {
        return datetime.isValid() && datetime.date() == date && datetime.time() == time;
    };
    if (wall(before) && wall(after)) {
        const QDateTime& earlier =
            before.toMSecsSinceEpoch() <= after.toMSecsSinceEpoch() ? before : after;
        return earlier.toUTC();
    }
    if (!before.isValid() || !after.isValid())
        return {};
    const qint64 lo = std::min(before.toMSecsSinceEpoch(), after.toMSecsSinceEpoch());
    const qint64 hi = std::max(before.toMSecsSinceEpoch(), after.toMSecsSinceEpoch());
    const QDateTime windowStart =
        QDateTime::fromMSecsSinceEpoch(lo, QTimeZone::utc()).addSecs(-86400);
    const QDateTime windowEnd = QDateTime::fromMSecsSinceEpoch(hi, QTimeZone::utc()).addSecs(86400);
    const auto transitions = zone.transitions(windowStart, windowEnd);
    for (const auto& transition : transitions) {
        const qint64 at = transition.atUtc.toMSecsSinceEpoch();
        if (at > lo && at <= hi)
            return transition.atUtc.toUTC();
    }
    return {};
}

bool firesOn(const ScheduleRule& rule, QDate date) {
    const int day = date.dayOfWeek();
    switch (rule.kind) {
    case Kind::Daily:
        return true;
    case Kind::Weekdays:
        return day >= Qt::Monday && day <= Qt::Friday;
    case Kind::Weekly:
        return rule.days.contains(day);
    case Kind::Once:
    case Kind::Interval:
        return false;
    }
    return false;
}

std::optional<QDateTime> nextCalendarFire(const ScheduleRule& rule, const QDateTime& instant) {
    const QDateTime local = instant.toTimeZone(rule.timeZone);
    if (!local.isValid())
        return std::nullopt;
    QDate date = local.date();
    for (int i = 0; i < 8; ++i) {
        if (!date.isValid())
            return std::nullopt;
        if (firesOn(rule, date)) {
            const QDateTime at = resolveLocal(date, rule.time, rule.timeZone);
            if (at.isValid() && at.toMSecsSinceEpoch() > instant.toMSecsSinceEpoch())
                return at;
        }
        date = date.addDays(1);
    }
    return std::nullopt;
}

bool checkedMultiply(qint64 left, qint64 right, qint64* product) {
    if (left < 0 || right < 0)
        return false;
    if (right != 0 && left > std::numeric_limits<qint64>::max() / right)
        return false;
    *product = left * right;
    return true;
}

bool checkedAdd(qint64 left, qint64 right, qint64* sum) {
    if (right > 0 && left > std::numeric_limits<qint64>::max() - right)
        return false;
    if (right < 0 && left < std::numeric_limits<qint64>::min() - right)
        return false;
    *sum = left + right;
    return true;
}

std::optional<QDateTime> nextInterval(const ScheduleRule& rule, const QDateTime& instant) {
    if (rule.everyHours < minimumEveryHours || rule.everyHours > maximumEveryHours)
        return std::nullopt;
    const QDateTime anchor = resolveLocal(rule.date, rule.time, rule.timeZone);
    if (!anchor.isValid())
        return std::nullopt;
    const qint64 origin = anchor.toMSecsSinceEpoch();
    const qint64 now = instant.toMSecsSinceEpoch();
    if (origin > now)
        return anchor;
    qint64 period = 0;
    if (!checkedMultiply(rule.everyHours, 3600LL * 1000LL, &period) || period <= 0)
        return std::nullopt;
    const qint64 elapsed = now - origin;
    const qint64 steps = elapsed / period + 1;
    qint64 delta = 0;
    qint64 at = 0;
    if (!checkedMultiply(steps, period, &delta) || !checkedAdd(origin, delta, &at))
        return std::nullopt;
    return QDateTime::fromMSecsSinceEpoch(at, QTimeZone::utc());
}

int missedInterval(const ScheduleRule& rule, qint64 start, qint64 end) {
    if (rule.everyHours < minimumEveryHours || rule.everyHours > maximumEveryHours)
        return 0;
    const QDateTime anchor = resolveLocal(rule.date, rule.time, rule.timeZone);
    if (!anchor.isValid())
        return 0;
    const qint64 origin = anchor.toMSecsSinceEpoch();
    if (end < origin)
        return 0;
    qint64 period = 0;
    if (!checkedMultiply(rule.everyHours, 3600LL * 1000LL, &period) || period <= 0)
        return 0;
    const qint64 first = start < origin ? 0 : (start - origin) / period + 1;
    const qint64 last = (end - origin) / period;
    if (last < first)
        return 0;
    const qint64 count = last - first + 1;
    if (count > maximumMissedRuns)
        return maximumMissedRuns;
    return static_cast<int>(count);
}

int missedCalendar(const ScheduleRule& rule, const QDateTime& start, const QDateTime& end) {
    const QDateTime localStart = start.toTimeZone(rule.timeZone);
    const QDateTime localEnd = end.toTimeZone(rule.timeZone);
    if (!localStart.isValid() || !localEnd.isValid())
        return 0;
    QDate date = localStart.date().addDays(-1);
    if (!date.isValid())
        date = localStart.date();
    const QDate last = localEnd.date();
    int fires = 0;
    int steps = 0;
    const qint64 startMs = start.toMSecsSinceEpoch();
    const qint64 endMs = end.toMSecsSinceEpoch();
    while (date.isValid() && date <= last) {
        // A weekly rule fires at least once a week, so this stays near the cap
        // instead of walking a multi-year window one day at a time past 1000 hits.
        if (++steps > maximumMissedRuns * 8)
            return fires;
        if (firesOn(rule, date)) {
            const QDateTime at = resolveLocal(date, rule.time, rule.timeZone);
            if (at.isValid()) {
                const qint64 ms = at.toMSecsSinceEpoch();
                if (ms > startMs && ms <= endMs) {
                    if (++fires >= maximumMissedRuns)
                        return maximumMissedRuns;
                }
            }
        }
        date = date.addDays(1);
    }
    return fires;
}

QString nextLabel(const ScheduleRule& rule, const QDateTime& now, const QDateTime& next) {
    const QDateTime localNow = now.toTimeZone(rule.timeZone);
    const QDateTime localNext = next.toTimeZone(rule.timeZone);
    if (!localNow.isValid() || !localNext.isValid())
        return QStringLiteral("choose a day");
    const qint64 delta = localNext.date().toJulianDay() - localNow.date().toJulianDay();
    const QString clock = formatClock(localNext.time());
    if (delta == 0)
        return QStringLiteral("today %1").arg(clock);
    if (delta == 1)
        return QStringLiteral("tomorrow %1").arg(clock);
    return QStringLiteral("%1/%2 %3")
        .arg(localNext.date().day(), 2, 10, QChar(u'0'))
        .arg(localNext.date().month(), 2, 10, QChar(u'0'))
        .arg(clock);
}

} // namespace

ParseResult parseRule(const QByteArray& json) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return failure(error::invalidJson, QStringLiteral("Rule JSON must be one object."));
    return parseRule(document.object());
}

ParseResult parseRule(const QJsonObject& object) {
    QStringList keys = object.keys();
    keys.sort();
    for (const QString& key : keys) {
        if (!knownKey(key))
            return failure(error::unknownField, QStringLiteral("Unknown field %1.").arg(key));
    }

    const QJsonValue kindValue = object.value(QStringLiteral("kind"));
    if (!object.contains(QStringLiteral("kind")))
        return failure(error::missingField, QStringLiteral("Missing kind."));
    if (!kindValue.isString())
        return failure(error::badKind, QStringLiteral("kind must be a string."));
    const auto kind = kindFrom(kindValue.toString());
    if (!kind)
        return failure(error::badKind,
                       QStringLiteral("kind must be once, daily, weekdays, weekly or interval."));

    if (!object.contains(QStringLiteral("time_zone")))
        return failure(error::missingField, QStringLiteral("Missing time_zone."));
    const auto zone = parseZone(object.value(QStringLiteral("time_zone")));
    if (!zone)
        return failure(error::badTimeZone, QStringLiteral("time_zone must be an IANA id."));

    for (const QString& key : keys) {
        if (!allowedFor(*kind, key))
            return failure(error::unexpectedField,
                           QStringLiteral("Field %1 is not part of a %2 rule.")
                               .arg(key, QString::fromUtf8(kindToken(*kind))));
    }

    ScheduleRule rule;
    rule.kind = *kind;
    rule.timeZone = *zone;

    const auto requireClock = [&](const char* field) -> std::optional<QTime> {
        const QJsonValue value = object.value(QString::fromUtf8(field));
        if (!object.contains(QString::fromUtf8(field)))
            return std::nullopt;
        if (!value.isString())
            return std::nullopt;
        return parseClock(value.toString());
    };

    switch (*kind) {
    case Kind::Once:
    case Kind::Interval: {
        const char* field = *kind == Kind::Once ? "at" : "anchor";
        if (!object.contains(QString::fromUtf8(field)))
            return failure(error::missingField,
                           QStringLiteral("Missing %1.").arg(QString::fromUtf8(field)));
        const QJsonValue value = object.value(QString::fromUtf8(field));
        if (!value.isString())
            return failure(
                error::badDatetime,
                QStringLiteral("%1 must be YYYY-MM-DDTHH:mm.").arg(QString::fromUtf8(field)));
        const auto civil = parseCivil(value.toString());
        if (!civil)
            return failure(
                error::badDatetime,
                QStringLiteral("%1 must be YYYY-MM-DDTHH:mm.").arg(QString::fromUtf8(field)));
        rule.date = civil->date;
        rule.time = civil->time;
        if (*kind == Kind::Interval) {
            const QJsonValue hours = object.value(QStringLiteral("every_hours"));
            if (!object.contains(QStringLiteral("every_hours")))
                return failure(error::missingField, QStringLiteral("Missing every_hours."));
            if (!hours.isDouble())
                return failure(error::badInterval,
                               QStringLiteral("every_hours must be a whole number from 1 to 24."));
            const double number = hours.toDouble();
            if (!std::isfinite(number) || number < minimumEveryHours ||
                number > maximumEveryHours || std::trunc(number) != number)
                return failure(error::badInterval,
                               QStringLiteral("every_hours must be a whole number from 1 to 24."));
            rule.everyHours = static_cast<int>(number);
        }
        break;
    }
    case Kind::Daily:
    case Kind::Weekdays:
    case Kind::Weekly: {
        if (!object.contains(QStringLiteral("time")))
            return failure(error::missingField, QStringLiteral("Missing time."));
        const auto clock = requireClock("time");
        if (!clock)
            return failure(error::badTime, QStringLiteral("time must be HH:mm."));
        rule.time = *clock;
        if (*kind == Kind::Weekly) {
            const QJsonValue days = object.value(QStringLiteral("days"));
            if (!object.contains(QStringLiteral("days")))
                return failure(error::missingField, QStringLiteral("Missing days."));
            if (!days.isArray() || days.toArray().isEmpty())
                return failure(error::badDays,
                               QStringLiteral("days must list at least one weekday."));
            const QJsonArray array = days.toArray();
            if (array.size() > 7)
                return failure(error::badDays,
                               QStringLiteral("days lists a weekday more than once."));
            for (const QJsonValue& entry : array) {
                if (!entry.isString())
                    return failure(
                        error::badDays,
                        QStringLiteral("days entries are sun, mon, tue, wed, thu, fri, sat."));
                const auto day = dayFromToken(entry.toString());
                if (!day)
                    return failure(
                        error::badDays,
                        QStringLiteral("days entries are sun, mon, tue, wed, thu, fri, sat."));
                if (rule.days.contains(*day))
                    return failure(error::badDays,
                                   QStringLiteral("days lists a weekday more than once."));
                rule.days.append(*day);
            }
            // Monday through Sunday, so ["fri","mon"] and ["mon","fri"] are one rule.
            std::sort(rule.days.begin(), rule.days.end());
        }
        break;
    }
    }
    return success(std::move(rule));
}

QJsonObject toJson(const ScheduleRule& rule) {
    QJsonObject object;
    object.insert(QStringLiteral("kind"), QString::fromUtf8(kindToken(rule.kind)));
    object.insert(QStringLiteral("time_zone"), QString::fromUtf8(rule.timeZone.id()));
    switch (rule.kind) {
    case Kind::Once:
        object.insert(QStringLiteral("at"), formatCivil(rule.date, rule.time));
        break;
    case Kind::Daily:
    case Kind::Weekdays:
        object.insert(QStringLiteral("time"), formatClock(rule.time));
        break;
    case Kind::Weekly: {
        object.insert(QStringLiteral("time"), formatClock(rule.time));
        QJsonArray days;
        for (int day : rule.days) {
            if (const char* token = dayToken(day))
                days.append(QString::fromUtf8(token));
        }
        object.insert(QStringLiteral("days"), days);
        break;
    }
    case Kind::Interval:
        object.insert(QStringLiteral("every_hours"), rule.everyHours);
        object.insert(QStringLiteral("anchor"), formatCivil(rule.date, rule.time));
        break;
    }
    return object;
}

QByteArray serialize(const ScheduleRule& rule) {
    return QJsonDocument(toJson(rule)).toJson(QJsonDocument::Compact);
}

std::optional<QDateTime> nextRunAfter(const ScheduleRule& rule, const QDateTime& instant) {
    if (!instant.isValid() || !rule.timeZone.isValid())
        return std::nullopt;
    const QDateTime after = instant.toUTC();
    switch (rule.kind) {
    case Kind::Once: {
        const QDateTime at = resolveLocal(rule.date, rule.time, rule.timeZone);
        if (!at.isValid() || at.toMSecsSinceEpoch() <= after.toMSecsSinceEpoch())
            return std::nullopt;
        return at;
    }
    case Kind::Daily:
    case Kind::Weekdays:
    case Kind::Weekly:
        return nextCalendarFire(rule, after);
    case Kind::Interval:
        return nextInterval(rule, after);
    }
    return std::nullopt;
}

int missedRuns(const ScheduleRule& rule, const QDateTime& from, const QDateTime& to) {
    if (!from.isValid() || !to.isValid() || !rule.timeZone.isValid())
        return 0;
    const QDateTime start = from.toUTC();
    const QDateTime end = to.toUTC();
    if (start.toMSecsSinceEpoch() >= end.toMSecsSinceEpoch())
        return 0;
    switch (rule.kind) {
    case Kind::Once: {
        const QDateTime at = resolveLocal(rule.date, rule.time, rule.timeZone);
        if (!at.isValid())
            return 0;
        const qint64 ms = at.toMSecsSinceEpoch();
        return ms > start.toMSecsSinceEpoch() && ms <= end.toMSecsSinceEpoch() ? 1 : 0;
    }
    case Kind::Interval:
        return missedInterval(rule, start.toMSecsSinceEpoch(), end.toMSecsSinceEpoch());
    case Kind::Daily:
    case Kind::Weekdays:
    case Kind::Weekly:
        return missedCalendar(rule, start, end);
    }
    return 0;
}

QString summary(const ScheduleRule& rule) {
    switch (rule.kind) {
    case Kind::Once:
        return QStringLiteral("Once · %1 %2").arg(formatDate(rule.date), formatClock(rule.time));
    case Kind::Daily:
        return QStringLiteral("Daily %1").arg(formatClock(rule.time));
    case Kind::Weekdays:
        return QStringLiteral("Weekdays %1").arg(formatClock(rule.time));
    case Kind::Weekly: {
        QStringList labels;
        for (int day : rule.days) {
            if (const char* label = dayLabel(day))
                labels.append(QString::fromLatin1(label));
        }
        return QStringLiteral("%1 %2").arg(labels.join(QStringLiteral(", ")),
                                           formatClock(rule.time));
    }
    case Kind::Interval:
        if (rule.everyHours == 1)
            return QStringLiteral("Every hour");
        return QStringLiteral("Every %1 hours").arg(rule.everyHours);
    }
    return {};
}

QString liveSummary(const ScheduleRule& rule, const QDateTime& now) {
    if (!now.isValid() || !rule.timeZone.isValid())
        return summary(rule);
    const auto next = nextRunAfter(rule, now);
    const QString when = next ? nextLabel(rule, now, *next) : QStringLiteral("choose a day");
    const QString offset = gmtOffset(rule.timeZone.offsetFromUtc(now));
    return summary(rule) + QStringLiteral(" (") + offset + QStringLiteral(") · next: ") + when;
}

} // namespace ketplus::daemon::schedule
