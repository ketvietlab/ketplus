#include "app/MainWindow.h"

#include "DaemonClient.h"
#include "agent/AgentWorkspaceView.h"
#include "schedules/ComposerSchedule.h"
#include "schedules/ScheduleClient.h"
#include "schedules/ScheduleWindows.h"

#include <QAction>
#include <QMenu>
#include <QMenuBar>

namespace ketplus {

void MainWindow::setupSchedules() {
    schedules_ = new ScheduleWindows(this);
    schedulesAction_ = schedules_->createAction(this);
    for (QAction* bar : menuBar()->actions()) {
        QMenu* menu = bar->menu();
        if (menu == nullptr || menu->title() != QStringLiteral("&View"))
            continue;
        const QList<QAction*> actions = menu->actions();
        const int index = actions.indexOf(remoteControlAction_);
        QAction* before =
            index >= 0 && index + 1 < actions.size() ? actions.at(index + 1) : nullptr;
        if (before != nullptr)
            menu->insertAction(before, schedulesAction_);
        else
            menu->addAction(schedulesAction_);
        break;
    }

    auto* daemon = accountDaemon();
    auto* client = schedules_->client();
    client->setTransport([daemon](const QString& type, const QJsonObject& payload,
                                  ScheduleClient::Reply reply) {
        daemon->call(type, payload, {}, -1,
                     [reply = std::move(reply)](const QJsonObject& result, const QString& error) {
                         if (reply)
                             reply(result, error);
                     });
    });
    const auto update = [client, daemon] {
        client->setAvailable(daemon->isReady() &&
                             daemon->hasFeature(QStringLiteral("schedules/1")));
    };
    connect(daemon, &remote::DaemonClient::readyChanged, client, update);
    connect(daemon, &remote::DaemonClient::scheduleEventReceived, client,
            &ScheduleClient::applyEvent);
    connect(schedules_, &ScheduleWindows::conversationRequested, this, &MainWindow::openTeamMember);
    update();
    attachComposerSchedule();
}

void MainWindow::attachComposerSchedule() {
    if (schedules_ == nullptr || agentWorkspace_ == nullptr)
        return;
    if (composerSchedule_ == nullptr) {
        composerSchedule_ = new ComposerSchedule(this);
        composerSchedule_->setClient(schedules_->client());
        connect(composerSchedule_, &ComposerSchedule::openRequested, this,
                &MainWindow::showSchedules);
    }
    agentWorkspace_->setScheduleBridge(composerSchedule_);
}

void MainWindow::showSchedules(const QString& scheduleId) {
    if (schedules_ != nullptr)
        schedules_->show(scheduleId);
}

} // namespace ketplus
