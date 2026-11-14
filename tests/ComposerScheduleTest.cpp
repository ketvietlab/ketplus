#include "schedules/ComposerSchedule.h"
#include "schedules/ScheduleClient.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest>

using namespace ketplus;

namespace {

QJsonObject morning() {
    return {{QStringLiteral("id"), QStringLiteral("morning")},
            {QStringLiteral("name"), QStringLiteral("Morning")},
            {QStringLiteral("enabled"), true},
            {QStringLiteral("prompt"), QStringLiteral("Say hello")},
            {QStringLiteral("target"),
             QJsonObject{{QStringLiteral("kind"), QStringLiteral("conversation")},
                         {QStringLiteral("session_id"), QStringLiteral("sess-1")}}},
            {QStringLiteral("rule"),
             QStringLiteral(
                 "{\"kind\":\"daily\",\"time\":\"08:00\",\"time_zone\":\"Asia/Ho_Chi_Minh\"}")},
            {QStringLiteral("busy_policy"), QStringLiteral("queue")},
            {QStringLiteral("missed_policy"), QStringLiteral("run_once")},
            {QStringLiteral("next_run_at"), QStringLiteral("2026-11-14T01:00:00.000Z")}};
}

} // namespace

class ComposerScheduleTest final : public QObject {
    Q_OBJECT

  private slots:
    void sendNowIsTheDefaultAndDoesNotCreate();
    void sendCreatesTheScheduleAndKeepsTheDraftOnError();
    void existingConversationShowsTheClockOnlyWithASchedule();
    void markerNamesTheScheduleAndToastReportsAFailure();
};

void ComposerScheduleTest::sendNowIsTheDefaultAndDoesNotCreate() {
    ComposerSchedule composer;
    composer.setContext(true, {}, QStringLiteral("/work"), QStringLiteral("codex"),
                        QStringLiteral("gpt"), QStringLiteral("medium"));
    QCOMPARE(composer.mode(), 0);
    QVERIFY(!composer.scheduleSet());
    QVERIFY(composer.clockVisible());
    QVERIFY(!composer.clockActive());
    QCOMPARE(composer.tooltip(), QStringLiteral("Schedule first message"));
    composer.setDraftPrompt(QStringLiteral("Hello"));
    QVERIFY(!composer.send());
    QCOMPARE(composer.draftPrompt(), QStringLiteral("Hello"));
}

void ComposerScheduleTest::sendCreatesTheScheduleAndKeepsTheDraftOnError() {
    ScheduleClient client;
    QString type;
    QJsonObject payload;
    QString replyError;
    QJsonObject replyResult;
    client.setTransport(
        [&](const QString& request, const QJsonObject& body, ScheduleClient::Reply reply) {
            type = request;
            payload = body;
            reply(replyResult, replyError);
        });
    client.setAvailable(true);
    ComposerSchedule composer;
    composer.setClient(&client);
    composer.setContext(true, {}, QStringLiteral("/work"), QStringLiteral("codex"),
                        QStringLiteral("gpt"), QStringLiteral("medium"));
    composer.setMode(2);
    composer.setRepeatKind(QStringLiteral("daily"));
    composer.setTime(QStringLiteral("08:00"));
    composer.setZone(QStringLiteral("Asia/Ho_Chi_Minh"));
    QVERIFY(composer.apply());
    QVERIFY(composer.clockActive());
    QCOMPARE(composer.summary(), QStringLiteral("Daily 08:00"));

    const QString draft = QStringLiteral("A scheduled first message");
    composer.setDraftPrompt(draft);
    replyError = QStringLiteral("provider_not_ready");
    QVERIFY(composer.send());
    QCOMPARE(type, QStringLiteral("schedule.create"));
    QCOMPARE(payload.value(QStringLiteral("prompt")).toString(), draft);
    QCOMPARE(payload.value(QStringLiteral("target")).toObject().value(QStringLiteral("kind")),
             QStringLiteral("new"));
    const QJsonObject rule =
        QJsonDocument::fromJson(payload.value(QStringLiteral("rule")).toString().toUtf8()).object();
    QCOMPARE(rule.value(QStringLiteral("kind")).toString(), QStringLiteral("daily"));
    QCOMPARE(rule.value(QStringLiteral("time_zone")).toString(),
             QStringLiteral("Asia/Ho_Chi_Minh"));
    QVERIFY(composer.draftKept());
    QCOMPARE(composer.draftPrompt(), draft);
    QVERIFY(composer.scheduleSet());

    replyError.clear();
    replyResult = {
        {QStringLiteral("schedule"), QJsonObject{{QStringLiteral("id"), QStringLiteral("made")}}}};
    QSignalSpy created(&composer, &ComposerSchedule::created);
    QVERIFY(composer.send());
    QCOMPARE(created.count(), 1);
    QCOMPARE(created.at(0).at(0).toString(), QStringLiteral("made"));
    QVERIFY(!composer.draftKept());
    QVERIFY(!composer.scheduleSet());
    QCOMPARE(composer.mode(), 0);
}

void ComposerScheduleTest::existingConversationShowsTheClockOnlyWithASchedule() {
    ScheduleClient client;
    client.setTransport([](const QString&, const QJsonObject&, ScheduleClient::Reply reply) {
        reply({{QStringLiteral("schedules"), QJsonArray{morning()}}}, {});
    });
    client.setAvailable(true);
    ComposerSchedule composer;
    composer.setClient(&client);
    composer.setContext(false, QStringLiteral("sess-1"), QStringLiteral("/work"), {}, {}, {});
    QVERIFY(composer.clockVisible());
    QVERIFY(composer.clockActive());
    QCOMPARE(composer.tooltip(), QStringLiteral("Next run: 2026-11-14 01:00"));
    composer.setContext(false, QStringLiteral("someone-else"), QStringLiteral("/work"), {}, {}, {});
    QVERIFY(!composer.clockVisible());
    composer.setContext(true, {}, QStringLiteral("/work"), {}, {}, {});
    QVERIFY(composer.clockVisible());
    QCOMPARE(composer.tooltip(), QStringLiteral("Schedule first message"));
}

void ComposerScheduleTest::markerNamesTheScheduleAndToastReportsAFailure() {
    ScheduleClient client;
    client.setTransport([](const QString&, const QJsonObject&, ScheduleClient::Reply reply) {
        reply({{QStringLiteral("schedules"), QJsonArray{morning()}}}, {});
    });
    client.setAvailable(true);
    ComposerSchedule composer;
    composer.setClient(&client);
    const QString payload = QStringLiteral(
        "{\"schedule_id\":\"morning\",\"scheduled_for\":\"2026-11-14T01:00:00.000Z\"}");
    QCOMPARE(composer.markerText(payload),
             QStringLiteral("Scheduled run: Morning · 2026-11-14 01:00"));
    QCOMPARE(composer.markerScheduleId(payload), QStringLiteral("morning"));
    QCOMPARE(composer.markerText(QStringLiteral("{\"schedule_id\":\"gone\"}")),
             QStringLiteral("Scheduled run: Schedule"));
    QCOMPARE(composer.markerText(QStringLiteral("{}")), QString{});

    QCOMPARE(composer.toastText(QStringLiteral("succeeded"), {}), QString{});
    QCOMPARE(composer.toastText(QStringLiteral("failed"), QStringLiteral("Provider is not ready")),
             QStringLiteral("Provider is not ready"));
    QCOMPARE(composer.actionLabel(), QStringLiteral("Open schedule"));

    QSignalSpy toasts(&composer, &ComposerSchedule::toastRequested);
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    composer.observeRun({{QStringLiteral("id"), QStringLiteral("run-ok")},
                         {QStringLiteral("schedule_id"), QStringLiteral("morning")},
                         {QStringLiteral("status"), QStringLiteral("succeeded")},
                         {QStringLiteral("finished_at"), now}});
    QCOMPARE(toasts.count(), 0);
    composer.observeRun({{QStringLiteral("id"), QStringLiteral("run-bad")},
                         {QStringLiteral("schedule_id"), QStringLiteral("morning")},
                         {QStringLiteral("status"), QStringLiteral("failed")},
                         {QStringLiteral("error_message"), QStringLiteral("Provider is not ready")},
                         {QStringLiteral("finished_at"), now}});
    QCOMPARE(toasts.count(), 1);
    QCOMPARE(toasts.at(0).at(0).toString(), QStringLiteral("Provider is not ready"));
    QCOMPARE(toasts.at(0).at(1).toString(), QStringLiteral("morning"));
    composer.observeRun({{QStringLiteral("id"), QStringLiteral("run-bad")},
                         {QStringLiteral("schedule_id"), QStringLiteral("morning")},
                         {QStringLiteral("status"), QStringLiteral("failed")},
                         {QStringLiteral("error_message"), QStringLiteral("Provider is not ready")},
                         {QStringLiteral("finished_at"), now}});
    QCOMPARE(toasts.count(), 1);
}

QTEST_MAIN(ComposerScheduleTest)
#include "ComposerScheduleTest.moc"
