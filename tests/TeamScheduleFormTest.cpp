// New team schedule payload, the waiting-state actions, and a team-filtered list.
// Run with QT_QPA_PLATFORM=offscreen.
#include "schedules/ScheduleClient.h"
#include "teams/TeamDialogs.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPage.h"
#include "teams/TeamScheduleFields.h"

#include "KetAppearance.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetSelect.h"
#include "KetStyle.h"
#include "KetTextArea.h"
#include "KetTextField.h"
#include "KetToggle.h"
#include "ui/Theme.h"

#include <QDir>
#include <QJsonArray>
#include <QTimeZone>
#include <QtTest>

using namespace ketplus;

namespace {

QVariantMap model(const QString& provider, const QString& id) {
    return {
        {"provider", provider},
        {"model", id},
        {"displayName", id},
        {"isDefault", true},
        {"defaultReasoningEffort", "medium"},
        {"supportedReasoningEfforts", QVariantList{QVariantMap{{"reasoningEffort", "medium"}}}}};
}

struct FakeDaemon {
    QList<QPair<QString, QJsonObject>> sent;
    QHash<QString, QJsonObject> answers;
    TeamDirectory::Transport transport() {
        return [this](const QString& type, const QJsonObject& payload, TeamDirectory::Reply reply) {
            if (type != "team.list" && type != "team.settings.get" && type != "team.get")
                sent.append({type, payload});
            const QJsonObject answer = answers.value(type, {{"ok", true}});
            reply(answer, answer.value("ok").toBool() ? QString{}
                                                      : answer.value("error_message").toString());
        };
    }
};

QJsonObject teamSchedule(const QString& id, const QString& teamId) {
    return {{QStringLiteral("id"), id},
            {QStringLiteral("name"), QStringLiteral("Daily website article")},
            {QStringLiteral("enabled"), true},
            {QStringLiteral("prompt"), QStringLiteral("Write a new article")},
            {QStringLiteral("target"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("team")},
                                                   {QStringLiteral("team_id"), teamId}}},
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

QJsonObject waitingTeam() {
    QJsonArray members{QJsonObject{{"session_id", "lead"}, {"role", "leader"}},
                       QJsonObject{{"session_id", "dev"}, {"role", "developer"}}};
    return {{"id", "7"},
            {"name", "Website"},
            {"repository", "/repo"},
            {"members", members},
            {"start_at", QJsonObject{{"at", "2026-11-20T08:00"}, {"timezone", "Asia/Ho_Chi_Minh"}}},
            {"updates", QJsonArray{}}};
}

} // namespace

class TeamScheduleFormTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { ket::Style::install(*qApp); }

    void immediateTeamOmitsTheSchedule() {
        FakeDaemon daemon;
        TeamDirectory teams;
        teams.setTransport(daemon.transport());
        teams.setAvailable(true);
        NewTeamForm form(&teams, "/repo", {model("claude", "opus"), model("codex", "gpt-5")});
        form.nameField()->setText("Website");
        const QJsonObject sent = form.payload();
        QVERIFY(!sent.contains(QStringLiteral("start_at")));
        QVERIFY(!sent.contains(QStringLiteral("repeat")));
    }

    void scheduledRepeatSendsStartAtAndWorkOrder() {
        FakeDaemon daemon;
        TeamDirectory teams;
        teams.setTransport(daemon.transport());
        teams.setAvailable(true);
        NewTeamForm form(&teams, "/repo", {model("claude", "opus"), model("codex", "gpt-5")});
        form.goalField()->setText("Write a new article for the website");
        form.findChild<ket::Radio*>(QStringLiteral("teamStartScheduled"))->setChecked(true);
        form.findChild<ket::TextField*>(QStringLiteral("teamStartDate"))
            ->setText(QStringLiteral("2026-11-20"));
        form.findChild<ket::TextField*>(QStringLiteral("teamStartTime"))
            ->setText(QStringLiteral("08:00"));
        auto* repeat = form.findChild<ket::Select*>(QStringLiteral("teamRepeat"));
        repeat->setCurrentIndex(repeat->findData(QStringLiteral("daily")));
        const QJsonObject sent = form.payload();
        QCOMPARE(sent.value(QStringLiteral("start_at")).toObject().value(QStringLiteral("at")),
                 QStringLiteral("2026-11-20T08:00"));
        QCOMPARE(sent.value(QStringLiteral("start_at"))
                     .toObject()
                     .value(QStringLiteral("timezone"))
                     .toString(),
                 QStringLiteral("Asia/Ho_Chi_Minh"));
        const QJsonObject repeatPayload = sent.value(QStringLiteral("repeat")).toObject();
        const QJsonObject rule = repeatPayload.value(QStringLiteral("rule")).toObject();
        QCOMPARE(rule.value(QStringLiteral("kind")).toString(), QStringLiteral("daily"));
        QCOMPARE(rule.value(QStringLiteral("time")).toString(), QStringLiteral("08:00"));
        QCOMPARE(rule.value(QStringLiteral("time_zone")).toString(),
                 QStringLiteral("Asia/Ho_Chi_Minh"));
        QCOMPARE(repeatPayload.value(QStringLiteral("work_order")).toString(),
                 QStringLiteral("Write a new article for the website"));
    }

    void startWindowIsOneMinuteToThirtyDays() {
        TeamScheduleFields fields;
        fields.setScheduled(true);
        fields.setStart(QStringLiteral("2026-11-14"), QStringLiteral("08:00"),
                        QStringLiteral("UTC"));
        const QDateTime almost = QDateTime(QDate(2026, 11, 14), QTime(7, 59, 30), QTimeZone::UTC);
        QVERIFY(fields.problem(almost).contains(QStringLiteral("one minute")));
        const QDateTime early = QDateTime(QDate(2026, 10, 1), QTime(8, 0), QTimeZone::UTC);
        QVERIFY(fields.problem(early).contains(QStringLiteral("30 days")));
        const QDateTime fine = QDateTime(QDate(2026, 11, 14), QTime(7, 0), QTimeZone::UTC);
        QVERIFY(fields.problem(fine).isEmpty());
    }

    void listAsksForTheTeamAndKeepsOtherSchedules() {
        QList<QPair<QString, QJsonObject>> sent;
        ScheduleClient client;
        client.setTransport([&](const QString& type, const QJsonObject& payload,
                                ScheduleClient::Reply reply) {
            sent.append({type, payload});
            if (payload.value(QStringLiteral("team_id")).toString() == QLatin1String("7"))
                reply({{QStringLiteral("ok"), true},
                       {QStringLiteral("schedules"),
                        QJsonArray{teamSchedule(QStringLiteral("web"), QStringLiteral("7"))}}},
                      {});
            else
                reply({{QStringLiteral("ok"), true},
                       {QStringLiteral("schedules"),
                        QJsonArray{teamSchedule(QStringLiteral("other"), QStringLiteral("9"))}}},
                      {});
        });
        client.setAvailable(true);
        client.refreshTeam(QStringLiteral("7"));
        QCOMPARE(sent.last().first, QStringLiteral("schedule.list"));
        QCOMPARE(sent.last().second.value(QStringLiteral("team_id")).toString(),
                 QStringLiteral("7"));
        QCOMPARE(client.schedules().size(), 2);
    }

    void waitingBannerStartsNow() {
        FakeDaemon daemon;
        daemon.answers.insert("team.list", {{"ok", true}, {"teams", QJsonArray{waitingTeam()}}});
        daemon.answers.insert(
            "team.get",
            {{"ok", true},
             {"team", waitingTeam()},
             {"updates",
              QJsonArray{QJsonObject{{"text", "Scheduled run: Daily website article"}}}}});
        TeamDirectory teams;
        teams.setTransport(daemon.transport());
        teams.setAvailable(true);
        TeamPage page(&teams, QStringLiteral("7"), QStringLiteral("/repo"));
        page.resize(1024, 700);
        page.show();
        QTest::qWait(0);
        QVERIFY(page.findChild<QWidget*>(QStringLiteral("teamStartBanner")) != nullptr);
        QVERIFY(page.findChild<QWidget*>(QStringLiteral("teamSchedulesEmpty")) != nullptr);
        QTest::mouseClick(page.findChild<ket::Button*>(QStringLiteral("teamPageStartNow")),
                          Qt::LeftButton);
        QCOMPARE(daemon.sent.last().first, QStringLiteral("team.start"));
        QCOMPARE(page.updates().first().toObject().value(QStringLiteral("text")).toString(),
                 QStringLiteral("Scheduled run: Daily website article"));
    }

    // Offscreen grabs. Absent directory: this slot does nothing.
    void lightAndDarkEvidence() {
        const QString folder = qEnvironmentVariable("KETPLUS_TEAM_SCHEDULE_CAPTURE");
        if (folder.isEmpty())
            return;
        QDir().mkpath(folder);
        const auto save = [](QWidget* widget, const QString& path) {
            widget->repaint();
            QApplication::processEvents();
            QVERIFY(widget->grab().save(path));
        };
        const QVariantList models{model("claude", "opus"), model("codex", "gpt-5")};
        for (const bool dark : {false, true}) {
            ThemeManager theme(*qApp);
            theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
            ket::Appearance::instance().setZoom(1.0);
            const QString suffix = dark ? QStringLiteral("dark") : QStringLiteral("light");

            FakeDaemon created;
            TeamDirectory creating;
            creating.setTransport(created.transport());
            creating.setAvailable(true);
            QWidget host;
            host.resize(1440, 900);
            host.show();
            ket::Dialog* dialog = NewTeamForm::present(&host, &creating, "/repo", models);
            QTest::qWait(0);
            save(dialog,
                 folder + QStringLiteral("/07-new-team-") + suffix + QStringLiteral(".png"));
            dialog->reject();

            FakeDaemon waitingDaemon;
            const QJsonObject waiting = waitingTeam();
            waitingDaemon.answers.insert("team.list",
                                         {{"ok", true}, {"teams", QJsonArray{waiting}}});
            waitingDaemon.answers.insert(
                "team.get",
                {{"ok", true},
                 {"team", waiting},
                 {"updates",
                  QJsonArray{QJsonObject{{"text", "Scheduled run: Daily website article"}}}}});
            TeamDirectory waitingTeams;
            waitingTeams.setTransport(waitingDaemon.transport());
            waitingTeams.setAvailable(true);
            TeamPage waitingPage(&waitingTeams, QStringLiteral("7"), QStringLiteral("/repo"));
            waitingPage.resize(1100, 800);
            waitingPage.show();
            QTest::qWait(0);
            save(&waitingPage,
                 folder + QStringLiteral("/08-team-waiting-") + suffix + QStringLiteral(".png"));

            QJsonObject listed = waiting;
            listed.remove(QStringLiteral("start_at"));
            FakeDaemon listedDaemon;
            listedDaemon.answers.insert("team.list", {{"ok", true}, {"teams", QJsonArray{listed}}});
            listedDaemon.answers.insert(
                "team.get", {{"ok", true}, {"team", listed}, {"updates", QJsonArray{}}});
            TeamDirectory listedTeams;
            listedTeams.setTransport(listedDaemon.transport());
            listedTeams.setAvailable(true);
            TeamPage listedPage(&listedTeams, QStringLiteral("7"), QStringLiteral("/repo"));
            ScheduleClient client;
            client.setTransport(
                [&](const QString& type, const QJsonObject&, ScheduleClient::Reply reply) {
                    if (type == QLatin1String("schedule.runs")) {
                        reply({{QStringLiteral("ok"), true},
                               {QStringLiteral("runs"),
                                QJsonArray{QJsonObject{
                                    {QStringLiteral("id"), QStringLiteral("weblast")},
                                    {QStringLiteral("schedule_id"), QStringLiteral("web")},
                                    {QStringLiteral("scheduled_for"),
                                     QStringLiteral("2026-11-13T01:00:00Z")},
                                    {QStringLiteral("started_at"),
                                     QStringLiteral("2026-11-13T01:00:00Z")},
                                    {QStringLiteral("finished_at"),
                                     QStringLiteral("2026-11-13T01:00:05Z")},
                                    {QStringLiteral("status"), QStringLiteral("skipped")},
                                    {QStringLiteral("session_id"), QString{}},
                                    {QStringLiteral("turn_id"), QString{}},
                                    {QStringLiteral("error_code"), QStringLiteral("paused")},
                                    {QStringLiteral("error_message"),
                                     QStringLiteral("Skipped: the team is paused.")}}}},
                               {QStringLiteral("has_more"), false}},
                              {});
                        return;
                    }
                    reply({{QStringLiteral("ok"), true},
                           {QStringLiteral("schedules"),
                            QJsonArray{teamSchedule(QStringLiteral("web"), QStringLiteral("7"))}}},
                          {});
                });
            client.setAvailable(true);
            listedPage.setScheduleClient(&client);
            listedPage.resize(1100, 800);
            listedPage.show();
            QTest::qWait(50);
            save(&listedPage,
                 folder + QStringLiteral("/08-team-schedules-") + suffix + QStringLiteral(".png"));
        }
    }
};

QTEST_MAIN(TeamScheduleFormTest)
#include "TeamScheduleFormTest.moc"
