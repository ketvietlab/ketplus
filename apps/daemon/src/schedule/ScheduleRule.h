#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QTimeZone>

#include <optional>

namespace ketplus::daemon::schedule {

// v1 recurrence for a scheduled agent run. The daemon stores the canonical JSON
// of a rule as an opaque string; this unit parses it, finds the next fire time
// and counts missed fires. It does not read a journal, start a timer or run a turn.
//
// A new rule that the user did not pin to a zone stores systemTimeZoneId().
// Parsing never fills a missing time_zone in: that is an error.
//
// Canonical JSON (one object, no unknown fields):
//   {"kind":"once","time_zone":"<IANA>","at":"YYYY-MM-DDTHH:mm"}
//   {"kind":"daily","time_zone":"<IANA>","time":"HH:mm"}
//   {"kind":"weekdays","time_zone":"<IANA>","time":"HH:mm"}
//   {"kind":"weekly","time_zone":"<IANA>","time":"HH:mm","days":["mon","fri"]}
//   {"kind":"interval","time_zone":"<IANA>","every_hours":4,"anchor":"YYYY-MM-DDTHH:mm"}
// Days are sun, mon, tue, wed, thu, fri, sat. weekdays is Monday–Friday in the
// rule's zone and does not take a days array. every_hours is an integer from 1 to 24.
// Interval fires begin at the anchor and then every N elapsed hours; nothing before
// the anchor is a fire. Clock times are minute precision.
//
// Daylight-saving gaps and folds, for ADR 0126:
// A civil time skipped by a spring-forward gap fires at the first valid instant
// after the gap, the zone transition itself (02:30 in a 02:00–03:00 gap fires at
// 03:00 local). A civil time repeated by a fall-back fold fires once, at the first
// occurrence (the earlier UTC instant, still on the pre-transition offset). An
// interval steps by elapsed hours from the anchor's resolved instant, so a
// transition does not insert or drop a slot.
//
// nextRunAfter is exclusive: a fire at exactly `instant` is not the next one.
// missedRuns counts fires t with from < t <= to. The count saturates at
// maximumMissedRuns, so 1000 means "1000 or more" (enough for run-once versus skip).

inline constexpr int maximumMissedRuns = 1000;
inline constexpr int minimumEveryHours = 1;
inline constexpr int maximumEveryHours = 24;

enum class Kind { Once, Daily, Weekdays, Weekly, Interval };

namespace error {
inline const QString invalidJson = QStringLiteral("schedule_rule_invalid_json");
inline const QString unknownField = QStringLiteral("schedule_rule_unknown_field");
inline const QString missingField = QStringLiteral("schedule_rule_missing_field");
inline const QString badKind = QStringLiteral("schedule_rule_bad_kind");
inline const QString badTimeZone = QStringLiteral("schedule_rule_bad_time_zone");
inline const QString badTime = QStringLiteral("schedule_rule_bad_time");
inline const QString badDatetime = QStringLiteral("schedule_rule_bad_datetime");
inline const QString badDays = QStringLiteral("schedule_rule_bad_days");
inline const QString badInterval = QStringLiteral("schedule_rule_bad_interval");
inline const QString unexpectedField = QStringLiteral("schedule_rule_unexpected_field");
} // namespace error

struct ScheduleRule final {
    Kind kind{Kind::Once};
    QTimeZone timeZone;
    // once: the civil date of `at`. interval: the civil date of `anchor`. Unused otherwise.
    QDate date;
    // Clock time for daily, weekdays and weekly. For once and interval, the clock
    // half of `at` / `anchor`.
    QTime time;
    // weekly only, Qt::DayOfWeek values (Monday = 1 … Sunday = 7), sorted Monday through Sunday.
    QList<int> days;
    // interval only. Zero on every other kind.
    int everyHours{0};
};

[[nodiscard]] inline bool operator==(const ScheduleRule& left, const ScheduleRule& right) {
    return left.kind == right.kind && left.timeZone.id() == right.timeZone.id() &&
           left.date == right.date && left.time == right.time && left.days == right.days &&
           left.everyHours == right.everyHours;
}

struct ParseResult final {
    std::optional<ScheduleRule> rule;
    QString code;
    QString message;
    [[nodiscard]] bool ok() const { return rule.has_value(); }
};

// The IANA id a newly created rule stores when the user does not pick a zone.
[[nodiscard]] inline QByteArray systemTimeZoneId() { return QTimeZone::systemTimeZoneId(); }

[[nodiscard]] ParseResult parseRule(const QByteArray& json);
[[nodiscard]] ParseResult parseRule(const QJsonObject& object);
[[nodiscard]] QJsonObject toJson(const ScheduleRule& rule);
[[nodiscard]] QByteArray serialize(const ScheduleRule& rule);

// The next fire strictly after `instant`, as a UTC datetime. None when a once
// rule is already due or in the past, or when the rule cannot fire.
[[nodiscard]] std::optional<QDateTime> nextRunAfter(const ScheduleRule& rule,
                                                    const QDateTime& instant);

// How many fires were missed while the daemon was down. 1000 means at least 1000.
[[nodiscard]] int missedRuns(const ScheduleRule& rule, const QDateTime& from, const QDateTime& to);

// Short label, matching the schedules mock: "Daily 08:00", "Weekdays 09:00",
// "Mon, Fri 17:00", "Every 4 hours", "Once · 2026-11-14 08:00". One hour is
// "Every hour"; every other interval stays "Every N hours".
[[nodiscard]] QString summary(const ScheduleRule& rule);
// The mock's live line: summary, the zone's offset at `now`, and the next fire.
// "Daily 08:00 (GMT+7) · next: tomorrow 08:00". A rule with no next fire says
// "next: choose a day", the mock's own fallback. An invalid `now` returns summary().
[[nodiscard]] QString liveSummary(const ScheduleRule& rule, const QDateTime& now);

} // namespace ketplus::daemon::schedule
