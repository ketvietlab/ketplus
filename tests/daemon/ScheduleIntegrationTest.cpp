// Cross-boundary schedule flow: the desktop client talks to a live SessionHost.
// The fake providers stand in for Codex and Claude, and the schedule clock is pinned.
#include "AgentTestFixture.h"
#include "schedules/ScheduleClient.h"

#include <QDateTime>
#include <QJsonArray>
#include <QMetaObject>
#include <QTest>

using namespace agent_test;
using namespace ketplus::daemon;

namespace {

QJsonObject command(SessionHost& host, const QString& type, const QJsonObject& payload) {
    return host.handleCommand(request(type, payload), QStringLiteral("desktop-test"));
}

bool pin(SessionHost& host, const QDateTime& when) {
    return QMetaObject::invokeMethod(&host, "setScheduleTimeForTest", Qt::DirectConnection,
                                     Q_ARG(QDateTime, when));
}

QString dailyRule(const QDateTime& due) {
    return QStringLiteral(R"({"kind":"daily","time_zone":"UTC","time":"%1"})")
        .arg(due.toUTC().toString(QStringLiteral("HH:mm")));
}

QJsonObject createPayload(const QString& name, const QJsonObject& target, const QString& prompt,
                          const QString& rule) {
    return {{QStringLiteral("name"), name},
            {QStringLiteral("enabled"), true},
            {QStringLiteral("target"), target},
            {QStringLiteral("prompt"), prompt},
            {QStringLiteral("rule"), rule},
            {QStringLiteral("busy_policy"), QStringLiteral("queue")},
            {QStringLiteral("missed_policy"), QStringLiteral("run_once")}};
}

void attach(ketplus::ScheduleClient& client, SessionHost& host) {
    QObject::connect(&host, &SessionHost::scheduleEvent, &client,
                     &ketplus::ScheduleClient::applyEvent);
    client.setTransport([&host](const QString& type, const QJsonObject& payload,
                                ketplus::ScheduleClient::Reply reply) {
        const QJsonObject result = command(host, type, payload);
        if (reply)
            reply(result, result.value(QStringLiteral("ok")).toBool()
                              ? QString{}
                              : result.value(QStringLiteral("error_message")).toString());
    });
    client.setAvailable(true);
}

QString scheduleNamed(const ketplus::ScheduleClient& client, const QString& name) {
    for (const ketplus::schedule::Schedule& schedule : client.schedules())
        if (schedule.name == name)
            return schedule.id;
    return {};
}

bool userMessageCarries(LocalJournal& journal, const QString& sessionId, const QString& scheduleId,
                        const QString& runId) {
    for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
        const QJsonObject event = value.toObject();
        if (event.value(QStringLiteral("event_type")).toString() != QLatin1String("user.message"))
            continue;
        const QJsonObject payload = event.value(QStringLiteral("payload")).toObject();
        if (payload.value(QStringLiteral("schedule_id")).toString() == scheduleId &&
            payload.value(QStringLiteral("run_id")).toString() == runId)
            return true;
    }
    return false;
}

bool eventsContain(LocalJournal& journal, const QString& sessionId, const QString& needle) {
    for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0))
        if (QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact))
                .contains(needle))
            return true;
    return false;
}

QJsonObject teamPlan() {
    return {{QStringLiteral("leader"),
             QJsonObject{{QStringLiteral("provider"), QStringLiteral("claude")},
                         {QStringLiteral("model"), QStringLiteral("opus")},
                         {QStringLiteral("effort"), QStringLiteral("high")}}},
            {QStringLiteral("developers"),
             QJsonObject{{QStringLiteral("count"), 1},
                         {QStringLiteral("provider"), QStringLiteral("codex")},
                         {QStringLiteral("model"), QStringLiteral("gpt-5")},
                         {QStringLiteral("effort"), QStringLiteral("medium")},
                         {QStringLiteral("members"),
                          QJsonArray{QJsonObject{{QStringLiteral("paths"),
                                                  QJsonArray{QStringLiteral("apps/web")}}}}}}},
            {QStringLiteral("reviewer"),
             QJsonObject{{QStringLiteral("provider"), QStringLiteral("claude")},
                         {QStringLiteral("model"), QStringLiteral("sonnet")}}}};
}

} // namespace

class ScheduleIntegrationTest final : public QObject {
    Q_OBJECT

  private slots:
    void aConversationScheduleFiresThroughTheClient() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QDateTime base = QDateTime::currentDateTimeUtc();
        QVERIFY(pin(*fixture.host, base));
        const QString session = fixture.start(QStringLiteral("codex"), QStringLiteral("Say hello"));
        QVERIFY(!session.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(session, QStringLiteral("turn.completed")), 1,
                                  8000);

        ketplus::ScheduleClient client;
        attach(client, *fixture.host);
        const QDateTime due = base.addSecs(180);
        QString refused;
        client.request(QStringLiteral("schedule.create"),
                       createPayload(QStringLiteral("Morning note"),
                                     {{QStringLiteral("kind"), QStringLiteral("conversation")},
                                      {QStringLiteral("session_id"), session}},
                                     QStringLiteral("Summarise the inbox."), dailyRule(due)),
                       [&](const QJsonObject&, const QString& error) { refused = error; });
        QVERIFY2(refused.isEmpty(), qPrintable(refused));
        const QString id = scheduleNamed(client, QStringLiteral("Morning note"));
        QVERIFY(!id.isEmpty());

        QVERIFY(pin(*fixture.host, due.addSecs(90)));
        QTRY_VERIFY_WITH_TIMEOUT(
            client.latestRun(id).status == ketplus::schedule::RunStatus::Succeeded ||
                client.latestRun(id).status == ketplus::schedule::RunStatus::Failed,
            15000);
        const ketplus::schedule::Run run = client.latestRun(id);
        QVERIFY2(run.status == ketplus::schedule::RunStatus::Succeeded,
                 qPrintable(run.errorCode + QLatin1Char(' ') + run.errorMessage));
        QTRY_VERIFY_WITH_TIMEOUT(userMessageCarries(*fixture.journal, session, id, run.id), 8000);
        client.loadRuns(id, false);
        QVERIFY(client.runsLoaded(id));
        QCOMPARE(client.runs(id).size(), 1);
        QCOMPARE(client.runs(id).constFirst().id, run.id);
    }

    void aScheduledTeamStartsThenDeliversADailyRun() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QDateTime base = QDateTime::currentDateTimeUtc();
        QVERIFY(pin(*fixture.host, base));
        const QDateTime start = base.addSecs(180);
        const QJsonObject created =
            command(*fixture.host, QStringLiteral("team.create"),
                    {{QStringLiteral("repository"), fixture.directory.path()},
                     {QStringLiteral("name"), QStringLiteral("Checkout")},
                     {QStringLiteral("goal"), QStringLiteral("Ship the checkout")},
                     {QStringLiteral("integration_branch"), QStringLiteral("integration")},
                     {QStringLiteral("merge_policy"), QStringLiteral("leader")},
                     {QStringLiteral("plan"), teamPlan()},
                     {QStringLiteral("start_at"),
                      QJsonObject{{QStringLiteral("at"),
                                   start.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm"))},
                                  {QStringLiteral("timezone"), QStringLiteral("UTC")}}}});
        QVERIFY2(created.value(QStringLiteral("ok")).toBool(),
                 qPrintable(created.value(QStringLiteral("error_code")).toString() +
                            QLatin1Char(' ') +
                            created.value(QStringLiteral("error_message")).toString()));
        const QJsonValue teamId =
            created.value(QStringLiteral("team")).toObject().value(QStringLiteral("id"));
        const QString teamKey =
            teamId.isString() ? teamId.toString() : QString::number(teamId.toInteger());
        QVERIFY(!created.value(QStringLiteral("team"))
                     .toObject()
                     .value(QStringLiteral("leader_started"))
                     .toBool());

        QVERIFY(pin(*fixture.host, start.addSecs(90)));
        auto leaderStarted = [&] {
            return command(*fixture.host, QStringLiteral("team.get"),
                           {{QStringLiteral("team_id"), teamId}})
                .value(QStringLiteral("team"))
                .toObject()
                .value(QStringLiteral("leader_started"))
                .toBool();
        };
        QTRY_VERIFY_WITH_TIMEOUT(leaderStarted(), 15000);
        const QJsonObject team = command(*fixture.host, QStringLiteral("team.get"),
                                         {{QStringLiteral("team_id"), teamId}})
                                     .value(QStringLiteral("team"))
                                     .toObject();
        QString leader;
        for (const QJsonValue& member : team.value(QStringLiteral("members")).toArray()) {
            const QJsonObject row = member.toObject();
            if (row.value(QStringLiteral("role")).toString() == QLatin1String("leader"))
                leader = row.value(QStringLiteral("session_id")).toString();
        }
        QVERIFY(!leader.isEmpty());

        ketplus::ScheduleClient client;
        attach(client, *fixture.host);
        const QDateTime due = start.addSecs(240);
        const QString prompt = QStringLiteral("Write a post for the web");
        QString refused;
        client.request(QStringLiteral("schedule.create"),
                       createPayload(QStringLiteral("Web post"),
                                     {{QStringLiteral("kind"), QStringLiteral("team")},
                                      {QStringLiteral("team_id"), teamKey}},
                                     prompt, dailyRule(due)),
                       [&](const QJsonObject&, const QString& error) { refused = error; });
        QVERIFY2(refused.isEmpty(), qPrintable(refused));
        const QString id = scheduleNamed(client, QStringLiteral("Web post"));
        QVERIFY(!id.isEmpty());

        QVERIFY(pin(*fixture.host, due.addSecs(90)));
        QTRY_VERIFY_WITH_TIMEOUT(
            client.latestRun(id).status == ketplus::schedule::RunStatus::Succeeded ||
                client.latestRun(id).status == ketplus::schedule::RunStatus::Failed,
            15000);
        const ketplus::schedule::Run run = client.latestRun(id);
        QVERIFY2(run.status == ketplus::schedule::RunStatus::Succeeded,
                 qPrintable(run.errorCode + QLatin1Char(' ') + run.errorMessage));
        QTRY_VERIFY_WITH_TIMEOUT(eventsContain(*fixture.journal, leader, prompt), 8000);
    }
};

QTEST_GUILESS_MAIN(ScheduleIntegrationTest)
#include "ScheduleIntegrationTest.moc"
