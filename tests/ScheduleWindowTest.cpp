#include "schedules/ScheduleWindow.h"
#include "schedules/ScheduleClient.h"
#include "schedules/ScheduleWindows.h"

#include "KetAppearance.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetStyle.h"
#include "KetTextArea.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QtTest>

using namespace ketplus;

namespace {

QJsonObject oneSchedule() {
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

bool labelContains(QWidget* root, const QString& text) {
    for (QLabel* label : root->findChildren<QLabel*>())
        if (label->text().contains(text))
            return true;
    return false;
}

QJsonObject scheduleDocument(const QString& id, const QString& name, bool enabled,
                             const QString& prompt, const QJsonObject& target, const QString& rule,
                             const QString& createdBy, const QString& nextRun) {
    return {{QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("enabled"), enabled},
            {QStringLiteral("prompt"), prompt},
            {QStringLiteral("target"), target},
            {QStringLiteral("rule"), rule},
            {QStringLiteral("busy_policy"), QStringLiteral("queue")},
            {QStringLiteral("missed_policy"), QStringLiteral("run_once")},
            {QStringLiteral("created_by"), createdBy},
            {QStringLiteral("created_at"), QStringLiteral("2026-11-01T01:00:00Z")},
            {QStringLiteral("updated_at"), QStringLiteral("2026-11-13T01:00:00Z")},
            {QStringLiteral("next_run_at"), nextRun},
            {QStringLiteral("last_run_at"), QStringLiteral("2026-11-13T01:00:00Z")}};
}

QJsonObject runDocument(const QString& id, const QString& scheduleId, const QString& when,
                        const QString& status, const QString& session, const QString& error) {
    return {{QStringLiteral("id"), id},
            {QStringLiteral("schedule_id"), scheduleId},
            {QStringLiteral("scheduled_for"), when},
            {QStringLiteral("started_at"), when},
            {QStringLiteral("finished_at"), when},
            {QStringLiteral("status"), status},
            {QStringLiteral("session_id"), session},
            {QStringLiteral("turn_id"), QString{}},
            {QStringLiteral("error_code"), QString{}},
            {QStringLiteral("error_message"), error}};
}

QJsonArray sampleSchedules() {
    const QString zone = QStringLiteral("Asia/Ho_Chi_Minh");
    const QString prompt =
        QStringLiteral("Write a new website article: choose a useful topic, prepare an English "
                       "draft and send it to the leader for approval before publishing.");
    return {scheduleDocument(QStringLiteral("daily"), QStringLiteral("Daily website article"), true,
                             prompt,
                             {{QStringLiteral("kind"), QStringLiteral("team")},
                              {QStringLiteral("team_id"), QStringLiteral("Release.1.4")}},
                             QStringLiteral("{\"kind\":\"daily\",\"time\":\"08:00\",\"timezone\":"
                                            "\"Asia/Ho_Chi_Minh\"}"),
                             QStringLiteral("user"), QStringLiteral("2026-11-14T01:00:00Z")),
            scheduleDocument(QStringLiteral("ci"), QStringLiteral("Summarize failing CI"), true,
                             QStringLiteral("Summarize the failures."),
                             {{QStringLiteral("kind"), QStringLiteral("new")},
                              {QStringLiteral("workspace"), QStringLiteral("~/dev/ketplus")},
                              {QStringLiteral("provider"), QStringLiteral("codex")},
                              {QStringLiteral("model"), QStringLiteral("GPT-5.6 Sol")},
                              {QStringLiteral("effort"), QStringLiteral("medium")},
                              {QStringLiteral("isolated_worktree"), true},
                              {QStringLiteral("title_template"), QString{}}},
                             QStringLiteral("{\"kind\":\"weekdays\",\"time\":\"08:30\","
                                            "\"timezone\":\"%1\"}")
                                 .arg(zone),
                             QStringLiteral("user"), QStringLiteral("2026-11-16T01:30:00Z")),
            scheduleDocument(QStringLiteral("recheck"), QStringLiteral("Recheck CI"), true,
                             QStringLiteral("Recheck the build."),
                             {{QStringLiteral("kind"), QStringLiteral("conversation")},
                              {QStringLiteral("session_id"), QStringLiteral("8116")}},
                             QStringLiteral("{\"kind\":\"interval\",\"every_hours\":2,\"anchor\":"
                                            "\"2026-11-13T10:00\",\"timezone\":\"%1\"}")
                                 .arg(zone),
                             QStringLiteral("8116"), QStringLiteral("2026-11-13T10:00:00Z")),
            scheduleDocument(QStringLiteral("start"), QStringLiteral("Start Website"), true, {},
                             {{QStringLiteral("kind"), QStringLiteral("team_start")},
                              {QStringLiteral("team_id"), QStringLiteral("Release.1.4")}},
                             QStringLiteral("{\"kind\":\"once\",\"at\":\"2026-11-13T22:00\","
                                            "\"timezone\":\"%1\"}")
                                 .arg(zone),
                             QStringLiteral("user"), QStringLiteral("2026-11-13T15:00:00Z")),
            scheduleDocument(QStringLiteral("weekly"), QStringLiteral("Weekly report"), false,
                             QStringLiteral("Send the weekly report."),
                             {{QStringLiteral("kind"), QStringLiteral("team")},
                              {QStringLiteral("team_id"), QStringLiteral("Release.1.4")}},
                             QStringLiteral("{\"kind\":\"weekly\",\"days\":[\"fri\"],\"time\":"
                                            "\"17:00\",\"timezone\":\"%1\"}")
                                 .arg(zone),
                             QStringLiteral("user"), QStringLiteral("2026-11-13T10:00:00Z"))};
}

QJsonArray historyFor(const QString& id) {
    if (id != QLatin1String("daily"))
        return {};
    return {runDocument(QStringLiteral("r4"), id, QStringLiteral("2026-11-13T01:00:00Z"),
                        QStringLiteral("succeeded"), QStringLiteral("4821"), {}),
            runDocument(QStringLiteral("r3"), id, QStringLiteral("2026-11-12T01:00:00Z"),
                        QStringLiteral("skipped"), {}, {}),
            runDocument(QStringLiteral("r2"), id, QStringLiteral("2026-11-11T01:00:00Z"),
                        QStringLiteral("missed"), QStringLiteral("4821"), {}),
            runDocument(QStringLiteral("r1"), id, QStringLiteral("2026-11-10T01:00:00Z"),
                        QStringLiteral("failed"), {}, QStringLiteral("Codex is not signed in."))};
}

void rememberLatest(ScheduleClient* client, const QString& id, const QString& status,
                    const QString& error) {
    client->applyEvent(QStringLiteral("schedule.run"),
                       {{QStringLiteral("run"),
                         runDocument(id + QStringLiteral("-last"), id,
                                     QStringLiteral("2026-11-13T01:00:00Z"), status, {}, error)}});
}

void serveSamples(ScheduleWindows& windows, const QJsonArray& schedules) {
    windows.client()->setTransport(
        [schedules](const QString& type, const QJsonObject& payload, ScheduleClient::Reply reply) {
            if (type == QLatin1String("schedule.runs")) {
                const QJsonArray runs = historyFor(payload.value(QStringLiteral("id")).toString());
                reply({{QStringLiteral("ok"), true},
                       {QStringLiteral("runs"), runs},
                       {QStringLiteral("has_more"), !runs.isEmpty()}},
                      {});
                return;
            }
            reply({{QStringLiteral("ok"), true}, {QStringLiteral("schedules"), schedules}}, {});
        });
    windows.client()->setAvailable(true);
    rememberLatest(windows.client(), QStringLiteral("ci"), QStringLiteral("failed"),
                   QStringLiteral("Provider is not signed in."));
    rememberLatest(windows.client(), QStringLiteral("recheck"), QStringLiteral("skipped"), {});
    rememberLatest(windows.client(), QStringLiteral("weekly"), QStringLiteral("succeeded"), {});
}

void saveGrab(QWidget* widget, const QString& folder, const QString& name) {
    if (widget == nullptr)
        return;
    widget->repaint();
    QApplication::processEvents();
    const QImage image = widget->grab().toImage();
    if (!image.isNull())
        image.save(QDir(folder).filePath(name + QStringLiteral(".png")));
}

void clickText(QWidget* root, const QString& text) {
    for (QAbstractButton* button : root->findChildren<QAbstractButton*>()) {
        if (button->isVisible() && button->text() == text) {
            button->click();
            return;
        }
    }
}

} // namespace

class ScheduleWindowTest final : public QObject {
    Q_OBJECT

  private slots:
    void menuOpensAndRaisesOneWindow();
    void emptyStateHasNoCreateButton();
    void editShowsValidationWithoutSending();
    void runNowShowsNotImplemented();
    void deleteWaitsForConfirmation();
    void captureLightAndDark();
};

void ScheduleWindowTest::menuOpensAndRaisesOneWindow() {
    ScheduleWindows windows;
    windows.client()->setTransport(
        [](const QString&, const QJsonObject&, ScheduleClient::Reply reply) {
            reply({{QStringLiteral("ok"), true}, {QStringLiteral("schedules"), QJsonArray{}}}, {});
        });
    windows.client()->setAvailable(true);
    auto* menu = new QMenu;
    auto* action = windows.createAction(menu);
    menu->addAction(action);
    QCOMPARE(action->objectName(), QStringLiteral("schedulesMenuAction"));
    QCOMPARE(action->text(), QStringLiteral("Schedules…"));
    QCOMPARE(action->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+J")));

    action->trigger();
    ScheduleWindow* first = windows.window();
    QVERIFY(first != nullptr);
    QVERIFY(first->isVisible());
    QCOMPARE(first->objectName(), QStringLiteral("schedulesWindow"));
    const int raised = first->raiseCount();
    action->trigger();
    QCOMPARE(windows.window(), first);
    QVERIFY(first->raiseCount() > raised);
    delete menu;
}

void ScheduleWindowTest::emptyStateHasNoCreateButton() {
    ScheduleWindows windows;
    windows.client()->setTransport(
        [](const QString&, const QJsonObject&, ScheduleClient::Reply reply) {
            reply({{QStringLiteral("ok"), true}, {QStringLiteral("schedules"), QJsonArray{}}}, {});
        });
    windows.client()->setAvailable(true);
    windows.show();
    ScheduleWindow* window = windows.window();
    QVERIFY(labelContains(window, QStringLiteral("No schedules yet")));
    QVERIFY(labelContains(window, QStringLiteral("New conversation or New team")));
    for (QAbstractButton* button : window->findChildren<QAbstractButton*>()) {
        if (!button->isVisible())
            continue;
        QVERIFY(!button->text().contains(QStringLiteral("Create")));
        QVERIFY(!button->text().contains(QStringLiteral("New schedule")));
    }
}

void ScheduleWindowTest::editShowsValidationWithoutSending() {
    int updates = 0;
    ScheduleWindows windows;
    windows.client()->setTransport(
        [&](const QString& type, const QJsonObject&, ScheduleClient::Reply reply) {
            if (type == QLatin1String("schedule.update"))
                ++updates;
            if (type == QLatin1String("schedule.runs")) {
                reply({{QStringLiteral("ok"), true},
                       {QStringLiteral("runs"), QJsonArray{}},
                       {QStringLiteral("has_more"), false}},
                      {});
                return;
            }
            reply({{QStringLiteral("ok"), true},
                   {QStringLiteral("schedules"), QJsonArray{oneSchedule()}},
                   {QStringLiteral("schedule"), oneSchedule()}},
                  {});
        });
    windows.client()->setAvailable(true);
    windows.show();
    ScheduleWindow* window = windows.window();
    window->findChild<QAbstractButton*>(QStringLiteral("scheduleEdit"))->click();
    auto* prompt = window->findChild<ket::TextArea*>(QStringLiteral("schedulePromptEdit"));
    QVERIFY(prompt != nullptr);
    prompt->setText(QStringLiteral("  "));
    window->findChild<QAbstractButton*>(QStringLiteral("scheduleSave"))->click();
    QCOMPARE(updates, 0);
    QVERIFY(labelContains(window, QStringLiteral("Prompt is required.")));
    auto* target = window->findChild<QLabel*>(QStringLiteral("scheduleTargetReadOnly"));
    QVERIFY(target != nullptr);
    QCOMPARE(target->text(), QStringLiteral("#sess-1"));
}

void ScheduleWindowTest::runNowShowsNotImplemented() {
    ScheduleWindows windows;
    windows.client()->setTransport([&](const QString& type, const QJsonObject&,
                                       ScheduleClient::Reply reply) {
        if (type == QLatin1String("schedule.runNow")) {
            reply({{QStringLiteral("ok"), false},
                   {QStringLiteral("error_code"), QStringLiteral("not_implemented")},
                   {QStringLiteral("error_message"),
                    QStringLiteral("Run now is not available yet.")}},
                  QStringLiteral("Run now is not available yet."));
            return;
        }
        if (type == QLatin1String("schedule.runs")) {
            reply({{QStringLiteral("ok"), true},
                   {QStringLiteral("runs"), QJsonArray{}},
                   {QStringLiteral("has_more"), false}},
                  {});
            return;
        }
        reply({{QStringLiteral("ok"), true}, {QStringLiteral("schedules"), QJsonArray{oneSchedule()}}},
              {});
    });
    windows.client()->setAvailable(true);
    windows.show();
    ScheduleWindow* window = windows.window();
    window->findChild<QAbstractButton*>(QStringLiteral("scheduleRunNow"))->click();
    auto* banner = window->findChild<QLabel*>(QStringLiteral("schedulesBanner"));
    QVERIFY(banner != nullptr);
    QVERIFY(banner->isVisible());
    QCOMPARE(banner->text(), QStringLiteral("Run now is not available yet."));
    QCOMPARE(windows.client()->schedules().size(), 1);
}

void ScheduleWindowTest::deleteWaitsForConfirmation() {
    int deletes = 0;
    ScheduleWindows windows;
    windows.client()->setTransport([&](const QString& type, const QJsonObject&,
                                       ScheduleClient::Reply reply) {
        if (type == QLatin1String("schedule.delete")) {
            ++deletes;
            reply({{QStringLiteral("ok"), true}, {QStringLiteral("id"), QStringLiteral("morning")}},
                  {});
            return;
        }
        if (type == QLatin1String("schedule.runs")) {
            reply({{QStringLiteral("ok"), true},
                   {QStringLiteral("runs"), QJsonArray{}},
                   {QStringLiteral("has_more"), false}},
                  {});
            return;
        }
        reply({{QStringLiteral("ok"), true}, {QStringLiteral("schedules"), QJsonArray{oneSchedule()}}},
              {});
    });
    windows.client()->setAvailable(true);
    windows.show();
    ScheduleWindow* window = windows.window();
    window->findChild<QAbstractButton*>(QStringLiteral("scheduleDelete"))->click();
    auto* dialog = window->findChild<ket::Dialog*>(QStringLiteral("scheduleDeleteDialog"));
    QVERIFY(dialog != nullptr);
    QVERIFY(dialog->isOpen());
    QCOMPARE(deletes, 0);
    window->findChild<QAbstractButton*>(QStringLiteral("scheduleDeleteConfirm"))->click();
    QCOMPARE(deletes, 1);
    QCOMPARE(windows.client()->schedules().size(), 0);
}

// Offscreen grabs of the production window. Absent directory: the slot does nothing.
void ScheduleWindowTest::captureLightAndDark() {
    const QString folder = qEnvironmentVariable("KETPLUS_SCHEDULE_CAPTURE");
    if (folder.isEmpty())
        return;
    QDir().mkpath(folder);
    ket::Style::install(*qApp);
    ket::Appearance::instance().setZoom(1.0);

    const auto shoot = [&](ket::Appearance::Mode mode, const QString& suffix) {
        ket::Appearance::instance().setMode(mode);
        QApplication::processEvents();

        ScheduleWindows populated;
        serveSamples(populated, sampleSchedules());
        populated.show();
        ScheduleWindow* window = populated.window();
        window->resize(1160, 800);
        QTest::qWait(50);
        saveGrab(window, folder, QStringLiteral("01-schedules-") + suffix);

        window->resize(900, 600);
        QTest::qWait(50);
        saveGrab(window, folder, QStringLiteral("10-minimum-") + suffix);
        window->resize(1160, 800);
        QTest::qWait(50);

        window->findChild<QAbstractButton*>(QStringLiteral("scheduleEdit"))->click();
        QTest::qWait(50);
        saveGrab(window, folder, QStringLiteral("02-editor-team-") + suffix);
        window->findChild<ket::TextArea*>(QStringLiteral("schedulePromptEdit"))
            ->setText(QStringLiteral("  "));
        window->findChild<QAbstractButton*>(QStringLiteral("scheduleSave"))->click();
        QTest::qWait(50);
        saveGrab(window, folder, QStringLiteral("02-editor-validation-") + suffix);
        clickText(window, QStringLiteral("Cancel"));
        QTest::qWait(50);

        window->findChild<QAbstractButton*>(QStringLiteral("scheduleDelete"))->click();
        QTest::qWait(50);
        saveGrab(window, folder, QStringLiteral("03-delete-") + suffix);
        clickText(window, QStringLiteral("Cancel"));
        window->close();

        ScheduleWindows empty;
        empty.client()->setTransport([](const QString&, const QJsonObject&,
                                        ScheduleClient::Reply reply) {
            reply({{QStringLiteral("ok"), true}, {QStringLiteral("schedules"), QJsonArray{}}}, {});
        });
        empty.client()->setAvailable(true);
        empty.show();
        empty.window()->resize(1160, 800);
        QTest::qWait(50);
        saveGrab(empty.window(), folder, QStringLiteral("09-empty-") + suffix);
        empty.window()->close();

        QWidget menuHost;
        menuHost.resize(360, 240);
        auto* bar = new QMenuBar(&menuHost);
        auto* view = bar->addMenu(QStringLiteral("&View"));
        view->addAction(QStringLiteral("Command Palette"));
        view->addAction(populated.createAction(view));
        view->addAction(QStringLiteral("Enter Full Screen"));
        bar->resize(360, bar->sizeHint().height());
        bar->move(0, 16);
        menuHost.show();
        view->setParent(&menuHost);
        view->setWindowFlags(Qt::Widget);
        view->move(8, bar->y() + bar->height());
        view->setFixedWidth(260);
        view->show();
        QTest::qWait(50);
        saveGrab(&menuHost, folder, QStringLiteral("01-view-menu-") + suffix);
        menuHost.close();
    };
    shoot(ket::Appearance::Mode::Light, QStringLiteral("light"));
    shoot(ket::Appearance::Mode::Dark, QStringLiteral("dark"));
    ket::Appearance::instance().setMode(ket::Appearance::Mode::Light);
}

QTEST_MAIN(ScheduleWindowTest)
#include "ScheduleWindowTest.moc"
