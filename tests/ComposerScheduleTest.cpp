#include "schedules/ComposerSchedule.h"
#include "schedules/ScheduleClient.h"

#include "KetAppearance.h"
#include "KetStyle.h"
#include "agent/AgentWorkspaceView.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
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
            {QStringLiteral("created_by"), QStringLiteral("user")},
            {QStringLiteral("created_at"), QStringLiteral("2026-11-13T01:00:00Z")},
            {QStringLiteral("updated_at"), QStringLiteral("2026-11-13T01:00:00Z")},
            {QStringLiteral("next_run_at"), QStringLiteral("2026-11-14T01:00:00Z")},
            {QStringLiteral("last_run_at"), QString{}}};
}

QString localClock(const QString& iso) {
    QDateTime when = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!when.isValid())
        when = QDateTime::fromString(iso, Qt::ISODate);
    return when.toLocalTime().toString(QStringLiteral("dd/MM HH:mm"));
}

bool saveScene(QQuickWindow* window, const QString& folder, const QString& name) {
    if (window == nullptr)
        return false;
    QApplication::processEvents();
    const QImage image = window->grabWindow();
    if (image.isNull() || image.width() < 200)
        return false;
    int ink = 0;
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4) {
            if (image.pixelColor(x, y).alpha() > 0)
                ++ink;
        }
    }
    return ink > 50 && image.save(QDir(folder).filePath(name + QStringLiteral(".png")));
}

} // namespace

class ComposerScheduleTest final : public QObject {
    Q_OBJECT

  private slots:
    void sendNowIsTheDefaultAndDoesNotCreate();
    void sendCreatesTheScheduleAndKeepsTheDraftOnError();
    void existingConversationShowsTheClockOnlyWithASchedule();
    void markerNamesTheScheduleAndToastReportsAFailure();
    void captureLightAndDark();
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
    QVERIFY(composer.liveSummary().contains(QStringLiteral("Daily 08:00 (GMT+7)")));
    QVERIFY(composer.liveSummary().contains(QStringLiteral("next:")));

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
    QCOMPARE(
        composer.tooltip(),
        QStringLiteral("Next run: %1").arg(localClock(QStringLiteral("2026-11-14T01:00:00Z"))));
    QSignalSpy edits(&composer, &ComposerSchedule::editRequested);
    QSignalSpy opens(&composer, &ComposerSchedule::openRequested);
    composer.edit();
    QCOMPARE(edits.count(), 1);
    QCOMPARE(edits.at(0).at(0).toString(), QStringLiteral("morning"));
    QCOMPARE(opens.count(), 0);
    composer.openSchedules();
    QCOMPARE(opens.count(), 1);
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
             QStringLiteral("Scheduled run: Morning · %1")
                 .arg(localClock(QStringLiteral("2026-11-14T01:00:00.000Z"))));
    client.applyEvent(
        QStringLiteral("schedule.run"),
        {{QStringLiteral("run"),
          QJsonObject{{QStringLiteral("id"), QStringLiteral("run-earlier")},
                      {QStringLiteral("schedule_id"), QStringLiteral("morning")},
                      {QStringLiteral("scheduled_for"), QStringLiteral("2026-11-13T01:00:00.000Z")},
                      {QStringLiteral("started_at"), QStringLiteral("2026-11-13T01:00:00.000Z")},
                      {QStringLiteral("finished_at"), QStringLiteral("2026-11-13T01:01:00.000Z")},
                                    {QStringLiteral("status"), QStringLiteral("succeeded")},
                                    {QStringLiteral("session_id"), QStringLiteral("sess-1")},
                                    {QStringLiteral("turn_id"), QStringLiteral("turn-1")},
                                    {QStringLiteral("error_code"), QString{}},
                                    {QStringLiteral("error_message"), QString{}}}}});
    QCOMPARE(composer.markerText(
                 QStringLiteral("{\"schedule_id\":\"morning\",\"run_id\":\"run-earlier\"}")),
             QStringLiteral("Scheduled run: Morning · %1")
                 .arg(localClock(QStringLiteral("2026-11-13T01:00:00.000Z"))));
    QCOMPARE(composer.markerScheduleId(payload), QStringLiteral("morning"));
    QCOMPARE(composer.markerText(QStringLiteral("{\"schedule_id\":\"gone\"}")),
             QStringLiteral("Scheduled run: Schedule"));
    QCOMPARE(composer.markerText(QStringLiteral("{}")), QString{});

    QCOMPARE(composer.toastText(QStringLiteral("succeeded"), {}), QString{});
    QCOMPARE(composer.toastText(QStringLiteral("failed"), QStringLiteral("Provider is not ready")),
             QStringLiteral("Schedule “Schedule” failed — Provider is not ready"));
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
    QCOMPARE(toasts.at(0).at(0).toString(),
             QStringLiteral("Schedule “Morning” failed — Provider is not ready"));
    QCOMPARE(toasts.at(0).at(1).toString(), QStringLiteral("morning"));
    composer.observeRun({{QStringLiteral("id"), QStringLiteral("run-bad")},
                         {QStringLiteral("schedule_id"), QStringLiteral("morning")},
                         {QStringLiteral("status"), QStringLiteral("failed")},
                         {QStringLiteral("error_message"), QStringLiteral("Provider is not ready")},
                         {QStringLiteral("finished_at"), now}});
    QCOMPARE(toasts.count(), 1);
}

// Offscreen grabs of the production composer. Absent directory: the slot does nothing.
void ComposerScheduleTest::captureLightAndDark() {
    const QString folder = qEnvironmentVariable("KETPLUS_COMPOSER_SCHEDULE_CAPTURE");
    if (folder.isEmpty())
        return;
    QDir().mkpath(folder);
    ket::Style::install(*qApp);
    ket::Appearance::instance().setZoom(1.0);

    const auto waitFor = [](const auto& ready) {
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (ready())
                return true;
            QTest::qWait(50);
        }
        return false;
    };
    const auto shoot = [&](ket::Appearance::Mode mode, const QString& suffix) -> QString {
        ket::Appearance::instance().setMode(mode);
        ScheduleClient client;
        client.setTransport([](const QString&, const QJsonObject&, ScheduleClient::Reply reply) {
            reply({{QStringLiteral("schedules"), QJsonArray{morning()}}}, {});
        });
        client.setAvailable(true);
        ComposerSchedule composer;
        composer.setClient(&client);

        AgentWorkspaceView view;
        view.resize(1100, 780);
        view.setScheduleBridge(&composer);
        view.show();
        if (!QTest::qWaitForWindowExposed(view.quickView()))
            return QStringLiteral("workspace window was not exposed");
        auto* window = view.quickView();
        if (window == nullptr || !waitFor([&] { return window->rootObject() != nullptr; }))
            return QStringLiteral("workspace did not load");
        auto* root = window->rootObject();
        auto* clock = root->findChild<QQuickItem*>(QStringLiteral("composerScheduleClock"));
        if (clock == nullptr || !waitFor([&] { return clock->isVisible(); }))
            return QStringLiteral("composer clock is not visible");
        QTest::qWait(80);
        if (!saveScene(window, folder, QStringLiteral("01-composer-clock-") + suffix))
            return QStringLiteral("composer clock grab failed");

        auto* popover = root->findChild<QObject*>(QStringLiteral("scheduleFirstMessagePopover"));
        if (popover == nullptr || !QMetaObject::invokeMethod(popover, "open") ||
            !waitFor([&] { return popover->property("visible").toBool(); }))
            return QStringLiteral("schedule popover did not open");
        if (composer.mode() != 0)
            return QStringLiteral("Send now is not the default");
        QTest::qWait(80);
        if (!saveScene(window, folder, QStringLiteral("02-popover-send-now-") + suffix))
            return QStringLiteral("send-now grab failed");

        composer.setMode(1);
        composer.applyPreset(QStringLiteral("tomorrow"));
        QTest::qWait(80);
        if (!saveScene(window, folder, QStringLiteral("08-popover-at-time-") + suffix))
            return QStringLiteral("at-a-time grab failed");

        composer.setMode(2);
        composer.setRepeatKind(QStringLiteral("daily"));
        composer.setTime(QStringLiteral("08:00"));
        QTest::qWait(80);
        if (!saveScene(window, folder, QStringLiteral("03-popover-repeat-") + suffix))
            return QStringLiteral("repeat grab failed");
        QMetaObject::invokeMethod(popover, "close");

        view.setConversations({QVariantMap{{QStringLiteral("id"), QStringLiteral("sess-1")},
                                           {QStringLiteral("sessionId"), QStringLiteral("sess-1")},
                                           {QStringLiteral("active"), true},
                                           {QStringLiteral("title"), QStringLiteral("Morning")}}});
        view.resetMessages(
            {QVariantMap{{QStringLiteral("kind"), QStringLiteral("user")},
                         {QStringLiteral("eventType"), QStringLiteral("user.message")},
                         {QStringLiteral("body"), QStringLiteral("Say hello")},
                         {QStringLiteral("messageKey"), QStringLiteral("plain")}}});
        if (!waitFor([&] { return !composer.newConversation() && clock->isVisible(); }))
            return QStringLiteral("existing conversation clock is not visible");
        QTest::qWait(80);
        if (!saveScene(window, folder, QStringLiteral("04-existing-clock-") + suffix))
            return QStringLiteral("existing conversation clock grab failed");
        view.resetMessages(
            {QVariantMap{{QStringLiteral("kind"), QStringLiteral("user")},
                         {QStringLiteral("eventType"), QStringLiteral("user.message")},
                         {QStringLiteral("body"), QStringLiteral("Say hello")},
                         {QStringLiteral("messageKey"), QStringLiteral("scheduled")},
                         {QStringLiteral("sanitizedPayload"),
                          QVariantMap{{QStringLiteral("schedule_id"), QStringLiteral("morning")},
                                      {QStringLiteral("run_id"), QStringLiteral("run-shown")},
                                      {QStringLiteral("scheduled_for"),
                                       QStringLiteral("2026-11-14T01:00:00.000Z")},
                                      {QStringLiteral("text"), QStringLiteral("Say hello")}}}}});
        auto* transcript = root->findChild<QObject*>(QStringLiteral("agentTranscript"));
        if (transcript != nullptr) {
            if (auto* windowModel = transcript->property("windowModel").value<QObject*>()) {
                // A range recorded before the row existed stays empty until it is set again.
                QMetaObject::invokeMethod(windowModel, "setRange", Q_ARG(int, -1), Q_ARG(int, -1));
                QMetaObject::invokeMethod(windowModel, "setRange", Q_ARG(int, 0), Q_ARG(int, 0));
            }
            QMetaObject::invokeMethod(transcript, "forceLayout");
        }
        const auto findVisual = [](auto&& self, QQuickItem* item,
                                   const QString& name) -> QQuickItem* {
            if (item == nullptr)
                return nullptr;
            if (item->objectName() == name)
                return item;
            for (auto* child : item->childItems()) {
                if (auto* found = self(self, child, name))
                    return found;
            }
            return nullptr;
        };
        QQuickItem* marker = nullptr;
        if (!waitFor([&] {
                marker = findVisual(findVisual, root, QStringLiteral("scheduledRunMarker"));
                return marker != nullptr && marker->isVisible();
            })) {
            if (marker == nullptr)
                return QStringLiteral("scheduled-run marker item was not created");
            return QStringLiteral("scheduled-run marker is hidden: %1")
                .arg(marker->property("text").toString());
        }
        QTest::qWait(80);
        if (!saveScene(window, folder, QStringLiteral("06-scheduled-marker-") + suffix))
            return QStringLiteral("scheduled-run marker grab failed");

        auto* menu = root->findChild<QObject*>(QStringLiteral("existingScheduleMenu"));
        if (menu == nullptr || !QMetaObject::invokeMethod(menu, "open") ||
            !waitFor([&] { return menu->property("visible").toBool(); }))
            return QStringLiteral("existing schedule menu did not open");
        QTest::qWait(80);
        if (!saveScene(window, folder, QStringLiteral("05-existing-menu-") + suffix))
            return QStringLiteral("existing menu grab failed");
        QMetaObject::invokeMethod(menu, "close");

        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        composer.observeRun(
            {{QStringLiteral("id"), QStringLiteral("run-") + suffix},
             {QStringLiteral("schedule_id"), QStringLiteral("morning")},
             {QStringLiteral("status"), QStringLiteral("failed")},
             {QStringLiteral("error_message"), QStringLiteral("Provider is not ready")},
             {QStringLiteral("finished_at"), now}});
        auto* toast = root->findChild<QObject*>(QStringLiteral("scheduleFailureToast"));
        if (toast == nullptr || !waitFor([&] { return toast->property("visible").toBool(); }))
            return QStringLiteral("failure toast did not open");
        QTest::qWait(80);
        if (!saveScene(window, folder, QStringLiteral("07-failure-toast-") + suffix))
            return QStringLiteral("toast grab failed");
        return {};
    };

    const QString light = shoot(ket::Appearance::Mode::Light, QStringLiteral("light"));
    QVERIFY2(light.isEmpty(), qPrintable(light));
    const QString dark = shoot(ket::Appearance::Mode::Dark, QStringLiteral("dark"));
    QVERIFY2(dark.isEmpty(), qPrintable(dark));
}

QTEST_MAIN(ComposerScheduleTest)
#include "ComposerScheduleTest.moc"
