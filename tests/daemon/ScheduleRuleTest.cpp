#include "schedule/ScheduleRule.h"

#include <QJsonObject>
#include <QTimeZone>

#include <QtTest>

namespace ketplus::daemon {
namespace {

QString iso(const QDateTime& datetime) { return datetime.toUTC().toString(Qt::ISODate); }

QDateTime utc(int year, int month, int day, int hour, int minute) {
    return QDateTime(QDate(year, month, day), QTime(hour, minute), QTimeZone::utc());
}

std::optional<schedule::ScheduleRule> parsed(const char* json) {
    const schedule::ParseResult result = schedule::parseRule(QByteArray(json));
    if (!result.ok()) {
        qWarning("%s", qPrintable(result.code + QLatin1Char(' ') + result.message));
        return std::nullopt;
    }
    return result.rule;
}

} // namespace

class ScheduleRuleTest final : public QObject {
    Q_OBJECT

  private slots:
    void roundTripsEveryKind();
    void rejectsInvalidInput_data();
    void rejectsInvalidInput();
    void nextRunAfterEachKind();
    void usesAZoneOtherThanTheSystemZone();
    void daylightSavingTransitions();
    void missedRunsCountsTheWindowAndCaps();
    void summariesMatchTheMock();
};

#define CHECK_ROUND_TRIP(json, canonical)                                                          \
    do {                                                                                           \
        const QString roundTripFailure = expect(json, canonical);                                  \
        QVERIFY2(roundTripFailure.isEmpty(), qPrintable(roundTripFailure));                        \
    } while (false)

void ScheduleRuleTest::roundTripsEveryKind() {
    const auto expect = [](const char* json, const char* canonical) {
        const schedule::ParseResult result = schedule::parseRule(QByteArray(json));
        if (!result.ok())
            return result.code + QLatin1Char(' ') + result.message;
        if (schedule::serialize(*result.rule) != QByteArray(canonical))
            return QStringLiteral("canonical ") +
                   QString::fromUtf8(schedule::serialize(*result.rule));
        const schedule::ParseResult again = schedule::parseRule(QByteArray(canonical));
        if (!again.ok() || !(*again.rule == *result.rule))
            return QStringLiteral("round trip lost a field");
        if (schedule::serialize(*again.rule) != QByteArray(canonical))
            return QStringLiteral("serialize was not stable");
        return QString();
    };

    CHECK_ROUND_TRIP(R"({"time_zone":"Asia/Ho_Chi_Minh","kind":"once","at":"2026-11-14T08:00"})",
                     R"({"at":"2026-11-14T08:00","kind":"once","time_zone":"Asia/Ho_Chi_Minh"})");
    CHECK_ROUND_TRIP(R"({ "kind" : "daily", "time_zone" : "Asia/Ho_Chi_Minh", "time" : "08:00" })",
                     R"({"kind":"daily","time":"08:00","time_zone":"Asia/Ho_Chi_Minh"})");
    CHECK_ROUND_TRIP(R"({"kind":"weekdays","time":"09:00","time_zone":"Asia/Ho_Chi_Minh"})",
                     R"({"kind":"weekdays","time":"09:00","time_zone":"Asia/Ho_Chi_Minh"})");
    CHECK_ROUND_TRIP(R"({"kind":"weekly","days":["fri","mon"],"time":"08:00","time_zone":"UTC"})",
                     R"({"days":["fri","mon"],"kind":"weekly","time":"08:00","time_zone":"UTC"})");
    CHECK_ROUND_TRIP(
        R"({"kind":"interval","every_hours":4.0,"anchor":"2026-11-13T08:00","time_zone":"UTC"})",
        R"({"anchor":"2026-11-13T08:00","every_hours":4,"kind":"interval","time_zone":"UTC"})");
    CHECK_ROUND_TRIP(
        R"({"kind":"interval","every_hours":1,"anchor":"2026-11-13T08:00","time_zone":"UTC"})",
        R"({"anchor":"2026-11-13T08:00","every_hours":1,"kind":"interval","time_zone":"UTC"})");
    CHECK_ROUND_TRIP(
        R"({"kind":"interval","every_hours":24,"anchor":"2026-11-13T00:00","time_zone":"UTC"})",
        R"({"anchor":"2026-11-13T00:00","every_hours":24,"kind":"interval","time_zone":"UTC"})");
#undef CHECK_ROUND_TRIP

    QJsonObject object{{QStringLiteral("kind"), QStringLiteral("daily")},
                       {QStringLiteral("time"), QStringLiteral("08:00")},
                       {QStringLiteral("time_zone"), QStringLiteral("UTC")}};
    const schedule::ParseResult fromObject = schedule::parseRule(object);
    QVERIFY(fromObject.ok());
    QCOMPARE(schedule::serialize(*fromObject.rule),
             QByteArray(R"({"kind":"daily","time":"08:00","time_zone":"UTC"})"));

    const auto weekly =
        parsed(R"({"kind":"weekly","days":["fri","mon"],"time":"08:00","time_zone":"UTC"})");
    QVERIFY(weekly);
    QCOMPARE(weekly->days, QList<int>({Qt::Friday, Qt::Monday}));
}

void ScheduleRuleTest::rejectsInvalidInput_data() {
    QTest::addColumn<QByteArray>("json");
    QTest::addColumn<QString>("code");
    const QString invalid = QStringLiteral("schedule_rule_invalid_json");
    const QString missing = QStringLiteral("schedule_rule_missing_field");
    const QString unknown = QStringLiteral("schedule_rule_unknown_field");
    const QString unexpected = QStringLiteral("schedule_rule_unexpected_field");
    const QString badKind = QStringLiteral("schedule_rule_bad_kind");
    const QString badZone = QStringLiteral("schedule_rule_bad_time_zone");
    const QString badTime = QStringLiteral("schedule_rule_bad_time");
    const QString badAt = QStringLiteral("schedule_rule_bad_datetime");
    const QString badDays = QStringLiteral("schedule_rule_bad_days");
    const QString badHours = QStringLiteral("schedule_rule_bad_interval");

    QTest::newRow("empty") << QByteArray() << invalid;
    QTest::newRow("null") << QByteArray("null") << invalid;
    QTest::newRow("array") << QByteArray("[]") << invalid;
    QTest::newRow("truncated") << QByteArray("{") << invalid;
    QTest::newRow("string") << QByteArray("\"once\"") << invalid;
    QTest::newRow("trailing comma") << QByteArray(R"({"kind":"daily",})") << invalid;
    QTest::newRow("no kind") << QByteArray("{}") << missing;
    QTest::newRow("kind only via zone") << QByteArray(R"({"time_zone":"UTC"})") << missing;
    QTest::newRow("capital kind") << QByteArray(
                                         R"({"kind":"Daily","time_zone":"UTC","time":"08:00"})")
                                  << badKind;
    QTest::newRow("numeric kind") << QByteArray(R"({"kind":1,"time_zone":"UTC","time":"08:00"})")
                                  << badKind;
    QTest::newRow("unknown kind") << QByteArray(R"({"kind":"hourly","time_zone":"UTC"})")
                                  << badKind;
    QTest::newRow("missing zone") << QByteArray(R"({"kind":"daily","time":"08:00"})") << missing;
    QTest::newRow("offset zone") << QByteArray(
                                        R"({"kind":"daily","time":"08:00","time_zone":"GMT+7"})")
                                 << badZone;
    QTest::newRow("empty zone") << QByteArray(R"({"kind":"daily","time":"08:00","time_zone":""})")
                                << badZone;
    QTest::newRow("lower case utc")
        << QByteArray(R"({"kind":"daily","time":"08:00","time_zone":"utc"})") << badZone;
    QTest::newRow("unknown field")
        << QByteArray(R"({"kind":"daily","time":"08:00","time_zone":"UTC","extra":1})") << unknown;
    QTest::newRow("daily days")
        << QByteArray(R"({"kind":"daily","time":"08:00","time_zone":"UTC","days":["mon"]})")
        << unexpected;
    QTest::newRow("missing time") << QByteArray(R"({"kind":"daily","time_zone":"UTC"})") << missing;
    QTest::newRow("hour 25") << QByteArray(R"({"kind":"daily","time":"25:00","time_zone":"UTC"})")
                             << badTime;
    QTest::newRow("unpadded time")
        << QByteArray(R"({"kind":"daily","time":"8:00","time_zone":"UTC"})") << badTime;
    QTest::newRow("seconds") << QByteArray(
                                    R"({"kind":"daily","time":"08:00:00","time_zone":"UTC"})")
                             << badTime;
    QTest::newRow("numeric time") << QByteArray(R"({"kind":"daily","time":8,"time_zone":"UTC"})")
                                  << badTime;
    QTest::newRow("missing at") << QByteArray(R"({"kind":"once","time_zone":"UTC"})") << missing;
    QTest::newRow("february 30")
        << QByteArray(R"({"kind":"once","time_zone":"UTC","at":"2026-02-30T08:00"})") << badAt;
    QTest::newRow("space instead of T")
        << QByteArray(R"({"kind":"once","time_zone":"UTC","at":"2026-11-14 08:00"})") << badAt;
    QTest::newRow("seconds in at")
        << QByteArray(R"({"kind":"once","time_zone":"UTC","at":"2026-11-14T08:00:00"})") << badAt;
    QTest::newRow("once also has time")
        << QByteArray(R"({"kind":"once","time_zone":"UTC","at":"2026-11-14T08:00","time":"08:00"})")
        << unexpected;
    QTest::newRow("weekly without days")
        << QByteArray(R"({"kind":"weekly","time":"08:00","time_zone":"UTC"})") << missing;
    QTest::newRow("weekly empty days")
        << QByteArray(R"({"kind":"weekly","time":"08:00","time_zone":"UTC","days":[]})") << badDays;
    QTest::newRow("duplicate day")
        << QByteArray(R"({"kind":"weekly","time":"08:00","time_zone":"UTC","days":["mon","mon"]})")
        << badDays;
    QTest::newRow("title case day")
        << QByteArray(R"({"kind":"weekly","time":"08:00","time_zone":"UTC","days":["Mon"]})")
        << badDays;
    QTest::newRow("long day name")
        << QByteArray(R"({"kind":"weekly","time":"08:00","time_zone":"UTC","days":["monday"]})")
        << badDays;
    QTest::newRow("days is a string")
        << QByteArray(R"({"kind":"weekly","time":"08:00","time_zone":"UTC","days":"mon"})")
        << badDays;
    QTest::newRow("missing every hours")
        << QByteArray(R"({"kind":"interval","time_zone":"UTC","anchor":"2026-11-13T08:00"})")
        << missing;
    QTest::newRow("zero hours")
        << QByteArray(
               R"({"kind":"interval","time_zone":"UTC","every_hours":0,"anchor":"2026-11-13T08:00"})")
        << badHours;
    QTest::newRow("25 hours")
        << QByteArray(
               R"({"kind":"interval","time_zone":"UTC","every_hours":25,"anchor":"2026-11-13T08:00"})")
        << badHours;
    QTest::newRow("fractional hours")
        << QByteArray(
               R"({"kind":"interval","time_zone":"UTC","every_hours":1.5,"anchor":"2026-11-13T08:00"})")
        << badHours;
    QTest::newRow("hours as string")
        << QByteArray(
               R"({"kind":"interval","time_zone":"UTC","every_hours":"4","anchor":"2026-11-13T08:00"})")
        << badHours;
    QTest::newRow("hours as bool")
        << QByteArray(
               R"({"kind":"interval","time_zone":"UTC","every_hours":true,"anchor":"2026-11-13T08:00"})")
        << badHours;
    QTest::newRow("weekdays days")
        << QByteArray(R"({"kind":"weekdays","time":"09:00","time_zone":"UTC","days":["mon"]})")
        << unexpected;
    QTest::newRow("interval time")
        << QByteArray(
               R"({"kind":"interval","time_zone":"UTC","every_hours":4,"anchor":"2026-11-13T08:00","time":"08:00"})")
        << unexpected;
}

void ScheduleRuleTest::rejectsInvalidInput() {
    QFETCH(QByteArray, json);
    QFETCH(QString, code);
    const schedule::ParseResult result = schedule::parseRule(json);
    QVERIFY2(!result.ok(), qPrintable(QString::fromUtf8(json)));
    QCOMPARE(result.code, code);
}

void ScheduleRuleTest::nextRunAfterEachKind() {
    const QDateTime fridayAfternoon(QDate(2026, 11, 13), QTime(15, 0),
                                    QTimeZone(QByteArrayLiteral("Asia/Ho_Chi_Minh")));

    const auto once =
        parsed(R"({"kind":"once","time_zone":"Asia/Ho_Chi_Minh","at":"2026-11-14T08:00"})");
    QVERIFY(once);
    const auto onceNext = schedule::nextRunAfter(*once, fridayAfternoon);
    QVERIFY(onceNext);
    QCOMPARE(iso(*onceNext), QStringLiteral("2026-11-14T01:00:00Z"));
    QVERIFY(!schedule::nextRunAfter(*once, utc(2026, 11, 14, 1, 0)));
    QVERIFY(!schedule::nextRunAfter(*once, utc(2026, 11, 14, 1, 1)));

    const auto daily = parsed(R"({"kind":"daily","time":"08:00","time_zone":"Asia/Ho_Chi_Minh"})");
    QVERIFY(daily);
    const auto dailyNext = schedule::nextRunAfter(*daily, fridayAfternoon);
    QVERIFY(dailyNext);
    QCOMPARE(iso(*dailyNext), QStringLiteral("2026-11-14T01:00:00Z"));
    const auto dailyBoundary = schedule::nextRunAfter(*daily, utc(2026, 11, 14, 1, 0));
    QVERIFY(dailyBoundary);
    QCOMPARE(iso(*dailyBoundary), QStringLiteral("2026-11-15T01:00:00Z"));

    const auto weekdays =
        parsed(R"({"kind":"weekdays","time":"09:00","time_zone":"Asia/Ho_Chi_Minh"})");
    QVERIFY(weekdays);
    const auto monday = schedule::nextRunAfter(*weekdays, fridayAfternoon);
    QVERIFY(monday);
    QCOMPARE(iso(*monday), QStringLiteral("2026-11-16T02:00:00Z"));
    const QDateTime saturday(QDate(2026, 11, 14), QTime(10, 0),
                             QTimeZone(QByteArrayLiteral("Asia/Ho_Chi_Minh")));
    const auto mondayAgain = schedule::nextRunAfter(*weekdays, saturday);
    QVERIFY(mondayAgain);
    QCOMPARE(iso(*mondayAgain), QStringLiteral("2026-11-16T02:00:00Z"));

    const auto weekly = parsed(
        R"({"kind":"weekly","time":"09:00","time_zone":"Asia/Ho_Chi_Minh","days":["mon","wed"]})");
    QVERIFY(weekly);
    const QDateTime wednesdayMorning(QDate(2026, 11, 18), QTime(8, 0),
                                     QTimeZone(QByteArrayLiteral("Asia/Ho_Chi_Minh")));
    const auto thisWednesday = schedule::nextRunAfter(*weekly, wednesdayMorning);
    QVERIFY(thisWednesday);
    QCOMPARE(iso(*thisWednesday), QStringLiteral("2026-11-18T02:00:00Z"));
    const QDateTime thursday(QDate(2026, 11, 19), QTime(10, 0),
                             QTimeZone(QByteArrayLiteral("Asia/Ho_Chi_Minh")));
    const auto nextMonday = schedule::nextRunAfter(*weekly, thursday);
    QVERIFY(nextMonday);
    QCOMPARE(iso(*nextMonday), QStringLiteral("2026-11-23T02:00:00Z"));

    const auto interval = parsed(
        R"({"kind":"interval","every_hours":4,"anchor":"2026-11-13T08:00","time_zone":"Asia/Ho_Chi_Minh"})");
    QVERIFY(interval);
    const auto beforeAnchor = schedule::nextRunAfter(*interval, utc(2026, 11, 13, 0, 30));
    QVERIFY(beforeAnchor);
    QCOMPARE(iso(*beforeAnchor), QStringLiteral("2026-11-13T01:00:00Z"));
    const auto onAnchor = schedule::nextRunAfter(*interval, utc(2026, 11, 13, 1, 0));
    QVERIFY(onAnchor);
    QCOMPARE(iso(*onAnchor), QStringLiteral("2026-11-13T05:00:00Z"));
    const auto between = schedule::nextRunAfter(*interval, fridayAfternoon);
    QVERIFY(between);
    QCOMPARE(iso(*between), QStringLiteral("2026-11-13T09:00:00Z"));

    schedule::ScheduleRule broken;
    broken.kind = schedule::Kind::Interval;
    broken.timeZone = QTimeZone::utc();
    broken.everyHours = 0;
    broken.date = QDate(2026, 11, 13);
    broken.time = QTime(8, 0);
    QVERIFY(!schedule::nextRunAfter(broken, fridayAfternoon));
    QVERIFY(!schedule::nextRunAfter(*daily, QDateTime()));
}

void ScheduleRuleTest::usesAZoneOtherThanTheSystemZone() {
    const QByteArray system = schedule::systemTimeZoneId();
    const QByteArray id = system == QByteArrayLiteral("Pacific/Auckland")
                              ? QByteArrayLiteral("America/Chicago")
                              : QByteArrayLiteral("Pacific/Auckland");
    QVERIFY(id != system);
    const QByteArray json = QByteArrayLiteral(R"({"kind":"daily","time":"09:00","time_zone":")") +
                            id + QByteArrayLiteral(R"("})");
    const schedule::ParseResult result = schedule::parseRule(json);
    QVERIFY(result.ok());
    QCOMPARE(result.rule->timeZone.id(), id);

    const bool auckland = id == QByteArrayLiteral("Pacific/Auckland");
    const QDateTime now = auckland ? utc(2026, 11, 13, 19, 0) : utc(2026, 11, 14, 14, 0);
    const auto next = schedule::nextRunAfter(*result.rule, now);
    QVERIFY(next);
    QCOMPARE(iso(*next), auckland ? QStringLiteral("2026-11-13T20:00:00Z")
                                  : QStringLiteral("2026-11-14T15:00:00Z"));
}

void ScheduleRuleTest::daylightSavingTransitions() {
    const auto newYork =
        parsed(R"({"kind":"daily","time":"02:30","time_zone":"America/New_York"})");
    QVERIFY(newYork);
    const auto spring = schedule::nextRunAfter(*newYork, utc(2026, 3, 8, 6, 0));
    QVERIFY(spring);
    QCOMPARE(iso(*spring), QStringLiteral("2026-03-08T07:00:00Z"));
    QCOMPARE(schedule::liveSummary(*newYork, utc(2026, 3, 8, 6, 0)),
             QStringLiteral("Daily 02:30 (GMT-5) · next: today 03:00"));
    const auto dayAfter = schedule::nextRunAfter(*newYork, utc(2026, 3, 8, 7, 0));
    QVERIFY(dayAfter);
    QCOMPARE(iso(*dayAfter), QStringLiteral("2026-03-09T06:30:00Z"));

    const auto fold = parsed(R"({"kind":"daily","time":"01:30","time_zone":"America/New_York"})");
    QVERIFY(fold);
    const auto firstOccurrence = schedule::nextRunAfter(*fold, utc(2026, 11, 1, 4, 0));
    QVERIFY(firstOccurrence);
    QCOMPARE(iso(*firstOccurrence), QStringLiteral("2026-11-01T05:30:00Z"));
    QCOMPARE(schedule::missedRuns(*fold, utc(2026, 11, 1, 4, 0), utc(2026, 11, 1, 7, 0)), 1);

    const auto berlinSpring =
        parsed(R"({"kind":"daily","time":"02:30","time_zone":"Europe/Berlin"})");
    QVERIFY(berlinSpring);
    const auto berlinGap = schedule::nextRunAfter(*berlinSpring, utc(2026, 3, 29, 0, 0));
    QVERIFY(berlinGap);
    QCOMPARE(iso(*berlinGap), QStringLiteral("2026-03-29T01:00:00Z"));

    const auto berlinFall =
        parsed(R"({"kind":"daily","time":"02:30","time_zone":"Europe/Berlin"})");
    QVERIFY(berlinFall);
    const auto berlinFold = schedule::nextRunAfter(*berlinFall, utc(2026, 10, 24, 22, 0));
    QVERIFY(berlinFold);
    QCOMPARE(iso(*berlinFold), QStringLiteral("2026-10-25T00:30:00Z"));

    const auto gapAnchor = parsed(
        R"({"kind":"interval","every_hours":1,"anchor":"2026-03-08T02:30","time_zone":"America/New_York"})");
    QVERIFY(gapAnchor);
    const auto resolvedAnchor = schedule::nextRunAfter(*gapAnchor, utc(2026, 3, 8, 6, 0));
    QVERIFY(resolvedAnchor);
    QCOMPARE(iso(*resolvedAnchor), QStringLiteral("2026-03-08T07:00:00Z"));
    const auto hourLater = schedule::nextRunAfter(*gapAnchor, utc(2026, 3, 8, 7, 0));
    QVERIFY(hourLater);
    QCOMPARE(iso(*hourLater), QStringLiteral("2026-03-08T08:00:00Z"));

    const auto across = parsed(
        R"({"kind":"interval","every_hours":4,"anchor":"2026-03-08T00:30","time_zone":"America/New_York"})");
    QVERIFY(across);
    const auto fourElapsed = schedule::nextRunAfter(*across, utc(2026, 3, 8, 5, 30));
    QVERIFY(fourElapsed);
    QCOMPARE(iso(*fourElapsed), QStringLiteral("2026-03-08T09:30:00Z"));

    const auto howeGap =
        parsed(R"({"kind":"daily","time":"02:15","time_zone":"Australia/Lord_Howe"})");
    QVERIFY(howeGap);
    const auto halfHourGap = schedule::nextRunAfter(*howeGap, utc(2026, 10, 3, 15, 0));
    QVERIFY(halfHourGap);
    QCOMPARE(iso(*halfHourGap), QStringLiteral("2026-10-03T15:30:00Z"));

    const auto howeFold =
        parsed(R"({"kind":"daily","time":"01:45","time_zone":"Australia/Lord_Howe"})");
    QVERIFY(howeFold);
    const auto halfHourFold = schedule::nextRunAfter(*howeFold, utc(2026, 4, 4, 14, 0));
    QVERIFY(halfHourFold);
    QCOMPARE(iso(*halfHourFold), QStringLiteral("2026-04-04T14:45:00Z"));
}

void ScheduleRuleTest::missedRunsCountsTheWindowAndCaps() {
    const auto daily = parsed(R"({"kind":"daily","time":"08:00","time_zone":"UTC"})");
    QVERIFY(daily);
    QCOMPARE(schedule::missedRuns(*daily, utc(2026, 11, 13, 8, 0), utc(2026, 11, 15, 8, 0)), 2);
    QCOMPARE(schedule::missedRuns(*daily, utc(2026, 11, 15, 8, 0), utc(2026, 11, 13, 8, 0)), 0);
    QCOMPARE(schedule::missedRuns(*daily, utc(2026, 11, 13, 8, 0), utc(2026, 11, 13, 8, 0)), 0);
    QCOMPARE(schedule::missedRuns(*daily, utc(2020, 1, 1, 0, 0), utc(2030, 1, 1, 0, 0)),
             schedule::maximumMissedRuns);

    const auto once = parsed(R"({"kind":"once","time_zone":"UTC","at":"2026-11-14T08:00"})");
    QVERIFY(once);
    QCOMPARE(schedule::missedRuns(*once, utc(2026, 11, 14, 7, 0), utc(2026, 11, 14, 8, 0)), 1);
    QCOMPARE(schedule::missedRuns(*once, utc(2026, 11, 14, 8, 0), utc(2026, 11, 14, 9, 0)), 0);

    const auto weekdays =
        parsed(R"({"kind":"weekdays","time":"09:00","time_zone":"Asia/Ho_Chi_Minh"})");
    QVERIFY(weekdays);
    // Friday 13:00Z is 20:00 in Ho Chi Minh, after 09:00. The window ends Monday 02:00Z,
    // which is 09:00 local, so Friday is excluded and Monday is included: Mon only
    // (Sat and Sun do not fire). From Friday 01:00Z (08:00 local, before 09:00) the
    // Friday fire is included too.
    QCOMPARE(schedule::missedRuns(*weekdays, utc(2026, 11, 13, 13, 0), utc(2026, 11, 16, 2, 0)), 1);
    QCOMPARE(schedule::missedRuns(*weekdays, utc(2026, 11, 13, 1, 0), utc(2026, 11, 16, 2, 0)), 2);

    const auto interval = parsed(
        R"({"kind":"interval","every_hours":4,"anchor":"2026-11-13T08:00","time_zone":"Asia/Ho_Chi_Minh"})");
    QVERIFY(interval);
    QCOMPARE(schedule::missedRuns(*interval, utc(2026, 11, 13, 0, 0), utc(2026, 11, 13, 9, 0)), 3);
    QCOMPARE(schedule::missedRuns(*interval, utc(2026, 11, 13, 1, 0), utc(2026, 11, 13, 9, 0)), 2);
    QCOMPARE(schedule::missedRuns(*interval, utc(2026, 11, 12, 0, 0), utc(2026, 11, 12, 12, 0)), 0);

    const auto hourly = parsed(
        R"({"kind":"interval","every_hours":1,"anchor":"2020-01-01T00:00","time_zone":"UTC"})");
    QVERIFY(hourly);
    QCOMPARE(schedule::missedRuns(*hourly, utc(2020, 1, 1, 0, 0), utc(2100, 1, 1, 0, 0)),
             schedule::maximumMissedRuns);

    schedule::ScheduleRule broken;
    broken.kind = schedule::Kind::Interval;
    broken.timeZone = QTimeZone::utc();
    broken.everyHours = 0;
    QCOMPARE(schedule::missedRuns(broken, utc(2026, 1, 1, 0, 0), utc(2026, 2, 1, 0, 0)), 0);
    QCOMPARE(schedule::missedRuns(*daily, QDateTime(), utc(2026, 1, 1, 0, 0)), 0);
}

void ScheduleRuleTest::summariesMatchTheMock() {
    const QDateTime clock(QDate(2026, 11, 13), QTime(15, 0),
                          QTimeZone(QByteArrayLiteral("Asia/Ho_Chi_Minh")));

    const auto daily = parsed(R"({"kind":"daily","time":"08:00","time_zone":"Asia/Ho_Chi_Minh"})");
    QVERIFY(daily);
    QCOMPARE(schedule::summary(*daily), QStringLiteral("Daily 08:00"));
    QCOMPARE(schedule::liveSummary(*daily, clock),
             QStringLiteral("Daily 08:00 (GMT+7) · next: tomorrow 08:00"));

    const auto weekdays =
        parsed(R"({"kind":"weekdays","time":"09:00","time_zone":"Asia/Ho_Chi_Minh"})");
    QVERIFY(weekdays);
    QCOMPARE(schedule::summary(*weekdays), QStringLiteral("Weekdays 09:00"));
    QCOMPARE(schedule::liveSummary(*weekdays, clock),
             QStringLiteral("Weekdays 09:00 (GMT+7) · next: 16/11 09:00"));

    const auto weekly = parsed(
        R"({"kind":"weekly","time":"17:00","time_zone":"Asia/Ho_Chi_Minh","days":["mon","fri"]})");
    QVERIFY(weekly);
    QCOMPARE(schedule::summary(*weekly), QStringLiteral("Mon, Fri 17:00"));
    QCOMPARE(schedule::liveSummary(*weekly, clock),
             QStringLiteral("Mon, Fri 17:00 (GMT+7) · next: today 17:00"));

    const auto hours = parsed(
        R"({"kind":"interval","every_hours":4,"anchor":"2026-11-13T08:00","time_zone":"Asia/Ho_Chi_Minh"})");
    QVERIFY(hours);
    QCOMPARE(schedule::summary(*hours), QStringLiteral("Every 4 hours"));
    QCOMPARE(schedule::liveSummary(*hours, clock),
             QStringLiteral("Every 4 hours (GMT+7) · next: today 16:00"));

    const auto oneHour = parsed(
        R"({"kind":"interval","every_hours":1,"anchor":"2026-11-13T08:00","time_zone":"UTC"})");
    QVERIFY(oneHour);
    QCOMPARE(schedule::summary(*oneHour), QStringLiteral("Every 1 hours"));

    const auto once =
        parsed(R"({"kind":"once","time_zone":"Asia/Ho_Chi_Minh","at":"2026-11-14T08:00"})");
    QVERIFY(once);
    QCOMPARE(schedule::summary(*once), QStringLiteral("Once · 2026-11-14 08:00"));
    QCOMPARE(schedule::liveSummary(*once, clock),
             QStringLiteral("Once · 2026-11-14 08:00 (GMT+7) · next: tomorrow 08:00"));

    const auto past =
        parsed(R"({"kind":"once","time_zone":"Asia/Ho_Chi_Minh","at":"2026-11-12T08:00"})");
    QVERIFY(past);
    QCOMPARE(schedule::liveSummary(*past, clock),
             QStringLiteral("Once · 2026-11-12 08:00 (GMT+7) · next: choose a day"));
    QCOMPARE(schedule::liveSummary(*past, QDateTime()), schedule::summary(*past));

    const auto utcDaily = parsed(R"({"kind":"daily","time":"09:00","time_zone":"UTC"})");
    QVERIFY(utcDaily);
    QCOMPARE(schedule::liveSummary(*utcDaily, utc(2026, 11, 13, 8, 0)),
             QStringLiteral("Daily 09:00 (GMT0) · next: today 09:00"));

    const auto kolkata = parsed(R"({"kind":"daily","time":"09:00","time_zone":"Asia/Kolkata"})");
    QVERIFY(kolkata);
    QCOMPARE(schedule::liveSummary(*kolkata, utc(2026, 1, 1, 0, 0)),
             QStringLiteral("Daily 09:00 (GMT+5:30) · next: today 09:00"));

    const auto kathmandu =
        parsed(R"({"kind":"daily","time":"09:00","time_zone":"Asia/Kathmandu"})");
    QVERIFY(kathmandu);
    QCOMPARE(schedule::liveSummary(*kathmandu, utc(2026, 1, 1, 0, 0)),
             QStringLiteral("Daily 09:00 (GMT+5:45) · next: today 09:00"));

    const auto eastern =
        parsed(R"({"kind":"daily","time":"08:00","time_zone":"America/New_York"})");
    QVERIFY(eastern);
    QCOMPARE(schedule::liveSummary(*eastern, utc(2026, 1, 15, 12, 0)),
             QStringLiteral("Daily 08:00 (GMT-5) · next: today 08:00"));
}

} // namespace ketplus::daemon

QTEST_GUILESS_MAIN(ketplus::daemon::ScheduleRuleTest)
#include "ScheduleRuleTest.moc"
