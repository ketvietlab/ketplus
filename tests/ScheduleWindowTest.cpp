#include "schedules/ScheduleWindow.h"
#include "schedules/ScheduleClient.h"
#include "schedules/ScheduleWindows.h"

#include "KetButton.h"
#include "KetDialog.h"
#include "KetTextArea.h"

#include <QAbstractButton>
#include <QAction>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QtTest>

using namespace ketplus;

namespace {

QJsonObject schedule() {
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

} // namespace

class ScheduleWindowTest final : public QObject {
    Q_OBJECT

  private slots:
    void menuOpensAndRaisesOneWindow();
    void emptyStateHasNoCreateButton();
    void editShowsValidationWithoutSending();
    void runNowShowsNotImplemented();
    void deleteWaitsForConfirmation();
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
                   {QStringLiteral("schedules"), QJsonArray{schedule()}},
                   {QStringLiteral("schedule"), schedule()}},
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
    QCOMPARE(target->text(), QStringLiteral("Conversation · sess-1"));
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
        reply({{QStringLiteral("ok"), true}, {QStringLiteral("schedules"), QJsonArray{schedule()}}},
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
        reply({{QStringLiteral("ok"), true}, {QStringLiteral("schedules"), QJsonArray{schedule()}}},
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

QTEST_MAIN(ScheduleWindowTest)
#include "ScheduleWindowTest.moc"
