#include "schedules/ScheduleClient.h"
#include "schedules/ScheduleRules.h"

#include <QJsonArray>
#include <QtTest>

using namespace ketplus;

namespace {

QJsonObject conversationSchedule() {
    return {{QStringLiteral("id"), QStringLiteral("morning")},
            {QStringLiteral("name"), QStringLiteral("Morning")},
            {QStringLiteral("enabled"), true},
            {QStringLiteral("prompt"), QStringLiteral("Say hello")},
            {QStringLiteral("target"),
             QJsonObject{{QStringLiteral("kind"), QStringLiteral("conversation")},
                         {QStringLiteral("session_id"), QStringLiteral("sess-1")}}},
            {QStringLiteral("rule"),
             QStringLiteral(
                 "{\"kind\":\"daily\",\"time\":\"08:00\",\"timezone\":\"Asia/Ho_Chi_Minh\"}")},
            {QStringLiteral("busy_policy"), QStringLiteral("queue")},
            {QStringLiteral("missed_policy"), QStringLiteral("run_once")},
            {QStringLiteral("created_by"), QStringLiteral("user")},
            {QStringLiteral("created_at"), QStringLiteral("2026-11-13T01:00:00Z")},
            {QStringLiteral("updated_at"), QStringLiteral("2026-11-13T01:00:00Z")},
            {QStringLiteral("next_run_at"), QStringLiteral("2026-11-14T01:00:00Z")},
            {QStringLiteral("last_run_at"), QString{}}};
}

QJsonObject teamStartSchedule() {
    QJsonObject schedule = conversationSchedule();
    schedule.insert(QStringLiteral("id"), QStringLiteral("kickoff"));
    schedule.insert(QStringLiteral("name"), QStringLiteral("Kickoff"));
    schedule.insert(QStringLiteral("prompt"), QString{});
    schedule.insert(QStringLiteral("target"),
                    QJsonObject{{QStringLiteral("kind"), QStringLiteral("team_start")},
                                {QStringLiteral("team_id"), QStringLiteral("team-1")}});
    schedule.insert(
        QStringLiteral("rule"),
        QStringLiteral(
            "{\"at\":\"2026-11-14T22:00\",\"kind\":\"once\",\"timezone\":\"Asia/Ho_Chi_Minh\"}"));
    return schedule;
}

ScheduleEdit dailyEdit() {
    ScheduleEdit edit;
    edit.name = QStringLiteral("Morning");
    edit.prompt = QStringLiteral("Say hello");
    edit.rule.kind = QStringLiteral("daily");
    edit.rule.time = QStringLiteral("08:00");
    edit.rule.timezone = QStringLiteral("Asia/Ho_Chi_Minh");
    edit.busyPolicy = QStringLiteral("queue");
    edit.missedPolicy = QStringLiteral("run_once");
    return edit;
}

QJsonObject run(const QString& id) {
    return {{QStringLiteral("id"), id},
            {QStringLiteral("schedule_id"), QStringLiteral("morning")},
            {QStringLiteral("scheduled_for"), QStringLiteral("2026-11-14T01:00:00Z")},
            {QStringLiteral("started_at"), QString{}},
            {QStringLiteral("finished_at"), QString{}},
            {QStringLiteral("status"), QStringLiteral("succeeded")},
            {QStringLiteral("session_id"), QStringLiteral("sess-1")},
            {QStringLiteral("turn_id"), QString{}},
            {QStringLiteral("error_code"), QString{}},
            {QStringLiteral("error_message"), QString{}}};
}

} // namespace

class ScheduleClientTest final : public QObject {
    Q_OBJECT

  private slots:
    void summarizesDailyInTheRuleZone();
    void rejectsEmptyPromptBadIntervalAndTeamStartConstraints();
    void teamStartUpdateKeepsTheTargetAndClearsThePrompt();
    void leavesAnUnknownRuleUntouched();
    void eventsUpdateTheCacheWithoutAnotherList();
    void pagesRunsAndIgnoresADuplicateEvent();
    void notImplementedLeavesTheScheduleInPlace();
};

void ScheduleClientTest::summarizesDailyInTheRuleZone() {
    QCOMPARE(scheduleRuleSummary(conversationSchedule().value(QStringLiteral("rule")).toString()),
             QStringLiteral("Daily 08:00 (GMT+7)"));
    QCOMPARE(scheduleTargetLabel(conversationSchedule().value(QStringLiteral("target")).toObject()),
             QStringLiteral("#sess-1"));
}

void ScheduleClientTest::rejectsEmptyPromptBadIntervalAndTeamStartConstraints() {
    ScheduleEdit edit = dailyEdit();
    edit.prompt = QStringLiteral("  ");
    const EditIssue empty = validateScheduleEdit(conversationSchedule(), edit);
    QCOMPARE(empty.code, QStringLiteral("empty_prompt"));
    QCOMPARE(empty.field, QStringLiteral("prompt"));

    edit = dailyEdit();
    edit.rule.kind = QStringLiteral("interval");
    edit.rule.everyHours = 25;
    edit.rule.anchor = QStringLiteral("2026-11-13T15:00");
    QCOMPARE(validateScheduleEdit(conversationSchedule(), edit).code,
             QStringLiteral("invalid_rule"));

    edit = dailyEdit();
    edit.rule.kind = QStringLiteral("weekly");
    edit.rule.time = QStringLiteral("17:00");
    edit.rule.days.clear();
    QCOMPARE(validateScheduleEdit(conversationSchedule(), edit).message,
             QStringLiteral("Choose at least one weekday."));

    edit = dailyEdit();
    edit.prompt = QStringLiteral("nope");
    const EditIssue prompt = validateScheduleEdit(teamStartSchedule(), edit);
    QCOMPARE(prompt.code, QStringLiteral("prompt_not_allowed"));

    edit.prompt.clear();
    edit.rule.kind = QStringLiteral("daily");
    QCOMPARE(validateScheduleEdit(teamStartSchedule(), edit).code, QStringLiteral("invalid_rule"));

    edit.rule.kind = QStringLiteral("once");
    edit.rule.at = QStringLiteral("2026-11-14T22:00");
    edit.missedPolicy = QStringLiteral("skip");
    const EditIssue missed = validateScheduleEdit(teamStartSchedule(), edit);
    QCOMPARE(missed.code, QStringLiteral("team_start_constraint"));
    QCOMPARE(missed.field, QStringLiteral("missed_policy"));
}

void ScheduleClientTest::teamStartUpdateKeepsTheTargetAndClearsThePrompt() {
    ScheduleEdit edit = dailyEdit();
    edit.name = QStringLiteral("Kickoff");
    edit.prompt = QStringLiteral("   ");
    edit.rule.kind = QStringLiteral("once");
    edit.rule.at = QStringLiteral("2026-11-14T22:00");
    const QJsonObject payload = scheduleUpdatePayload(teamStartSchedule(), edit);
    QCOMPARE(payload.value(QStringLiteral("prompt")).toString(), QString{});
    QCOMPARE(
        payload.value(QStringLiteral("target")).toObject().value(QStringLiteral("kind")).toString(),
        QStringLiteral("team_start"));
    QVERIFY(!scheduleUpdatePayload(conversationSchedule(), dailyEdit())
                 .contains(QStringLiteral("target")));
}

void ScheduleClientTest::leavesAnUnknownRuleUntouched() {
    const QString raw =
        QStringLiteral("{\"kind\":\"daily\",\"time\":\"08:00\",\"timezone\":\"UTC\",\"extra\":1}");
    QVERIFY(!scheduleRuleFromText(raw));
    QCOMPARE(scheduleRuleSummary(raw), QStringLiteral("Custom rule"));
    ScheduleEdit edit = dailyEdit();
    edit.preservedRule = raw;
    QCOMPARE(scheduleUpdatePayload(conversationSchedule(), edit)
                 .value(QStringLiteral("rule"))
                 .toString(),
             raw);
    QVERIFY(validateScheduleEdit(conversationSchedule(), edit).ok());
}

void ScheduleClientTest::eventsUpdateTheCacheWithoutAnotherList() {
    ScheduleClient client;
    int lists = 0;
    client.setTransport([&](const QString& type, const QJsonObject&, ScheduleClient::Reply reply) {
        if (type == QLatin1String("schedule.list"))
            ++lists;
        reply({{QStringLiteral("ok"), true}, {QStringLiteral("schedules"), QJsonArray{}}}, {});
    });
    client.setAvailable(true);
    QCOMPARE(lists, 1);

    QJsonObject created = conversationSchedule();
    client.applyEvent(QStringLiteral("schedule.changed"),
                      {{QStringLiteral("change"), QStringLiteral("created")},
                       {QStringLiteral("schedule"), created}});
    QCOMPARE(client.schedules().size(), 1);
    QCOMPARE(lists, 1);

    created.insert(QStringLiteral("name"), QStringLiteral("Renamed"));
    client.applyEvent(QStringLiteral("schedule.changed"),
                      {{QStringLiteral("change"), QStringLiteral("updated")},
                       {QStringLiteral("schedule"), created}});
    QCOMPARE(client.schedule(QStringLiteral("morning"))->name, QStringLiteral("Renamed"));
    QCOMPARE(lists, 1);

    client.applyEvent(QStringLiteral("schedule.run"),
                      {{QStringLiteral("run"), run(QStringLiteral("r1"))}});
    QCOMPARE(client.latestRun(QStringLiteral("morning")).id, QStringLiteral("r1"));
    QCOMPARE(client.runs(QStringLiteral("morning")).size(), 0);

    client.applyEvent(QStringLiteral("schedule.changed"),
                      {{QStringLiteral("change"), QStringLiteral("deleted")},
                       {QStringLiteral("schedule"), conversationSchedule()}});
    QCOMPARE(client.schedules().size(), 0);
    QCOMPARE(lists, 1);
}

void ScheduleClientTest::pagesRunsAndIgnoresADuplicateEvent() {
    ScheduleClient client;
    QList<QJsonObject> requests;
    client.setTransport(
        [&](const QString& type, const QJsonObject& payload, ScheduleClient::Reply reply) {
            requests.append(payload);
            requests.last().insert(QStringLiteral("type"), type);
            if (type == QLatin1String("schedule.list")) {
                reply({{QStringLiteral("ok"), true},
                       {QStringLiteral("schedules"), QJsonArray{conversationSchedule()}}},
                      {});
                return;
            }
            const bool more = payload.contains(QStringLiteral("before_id"));
            const QJsonArray page =
                more ? QJsonArray{run(QStringLiteral("r1"))}
                     : QJsonArray{run(QStringLiteral("r3")), run(QStringLiteral("r2"))};
            reply({{QStringLiteral("ok"), true},
                   {QStringLiteral("runs"), page},
                   {QStringLiteral("has_more"), !more}},
                  {});
        });
    client.setAvailable(true);
    client.loadRuns(QStringLiteral("morning"), false);
    QCOMPARE(client.runs(QStringLiteral("morning")).size(), 2);
    QVERIFY(client.runsHaveMore(QStringLiteral("morning")));
    client.loadRuns(QStringLiteral("morning"), true);
    QCOMPARE(requests.last().value(QStringLiteral("before_id")).toString(), QStringLiteral("r2"));
    QCOMPARE(requests.last().value(QStringLiteral("limit")).toInt(), ScheduleClient::runsPageSize);
    QCOMPARE(client.runs(QStringLiteral("morning")).size(), 3);
    QVERIFY(!client.runsHaveMore(QStringLiteral("morning")));

    client.applyEvent(QStringLiteral("schedule.run"),
                      {{QStringLiteral("run"), run(QStringLiteral("r3"))}});
    QCOMPARE(client.runs(QStringLiteral("morning")).size(), 3);
    client.applyEvent(QStringLiteral("schedule.run"),
                      {{QStringLiteral("run"), run(QStringLiteral("r4"))}});
    QCOMPARE(client.runs(QStringLiteral("morning")).first().id, QStringLiteral("r4"));
}

void ScheduleClientTest::notImplementedLeavesTheScheduleInPlace() {
    ScheduleClient client;
    client.setTransport([&](const QString& type, const QJsonObject&, ScheduleClient::Reply reply) {
        if (type == QLatin1String("schedule.runNow")) {
            reply({{QStringLiteral("ok"), false},
                   {QStringLiteral("error_code"), QStringLiteral("not_implemented")},
                   {QStringLiteral("error_message"),
                    QStringLiteral("Run now is not available yet.")}},
                  QStringLiteral("Run now is not available yet."));
            return;
        }
        reply({{QStringLiteral("ok"), true},
               {QStringLiteral("schedules"), QJsonArray{conversationSchedule()}}},
              {});
    });
    client.setAvailable(true);
    QCOMPARE(client.schedules().size(), 1);
    QString error;
    QString code;
    client.request(QStringLiteral("schedule.runNow"),
                   {{QStringLiteral("id"), QStringLiteral("morning")}},
                   [&](const QJsonObject& result, const QString& failure) {
                       error = failure;
                       code = result.value(QStringLiteral("error_code")).toString();
                   });
    QCOMPARE(code, QStringLiteral("not_implemented"));
    QCOMPARE(scheduleFailureText({{QStringLiteral("error_code"), code}}, error),
             QStringLiteral("Run now is not available yet."));
    QCOMPARE(client.schedules().size(), 1);
    QCOMPARE(client.schedule(QStringLiteral("morning"))->name, QStringLiteral("Morning"));
}

QTEST_MAIN(ScheduleClientTest)
#include "ScheduleClientTest.moc"
