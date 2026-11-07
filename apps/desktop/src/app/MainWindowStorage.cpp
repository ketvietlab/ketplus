#include "agent/AgentSessionManager.h"
#include "agent/CodexAppServer.h"
#include "app/MainWindow.h"
#include "workspace/ActivityBar.h"

#include <QPointer>
#include <QTimer>
#include <memory>

namespace ketplus {
void MainWindow::setupSidebarStorage() {
    auto* server = agentManager_->appServer();
    struct RequestState {
        bool pending = false;
        quint64 generation = 0;
    };
    const auto state = std::make_shared<RequestState>();
    auto* poll = new QTimer(this);
    poll->setInterval(60000);
    auto* deadline = new QTimer(this);
    deadline->setSingleShot(true);
    deadline->setInterval(15000);
    const QPointer<ActivityBar> sidebar(activityBar_);
    connect(deadline, &QTimer::timeout, this, [state, sidebar] {
        ++state->generation;
        state->pending = false;
        if (sidebar)
            sidebar->setStorageStatus({});
    });
    const auto refresh = [this, server, state, sidebar, deadline] {
        if (!isVisible() || !server->isReady() || state->pending)
            return;
        state->pending = true;
        const auto generation = ++state->generation;
        deadline->start();
        const QPointer<QTimer> timer(deadline);
        server->call(
            QStringLiteral("daemon/storageSize/read"), {},
            [state, generation, sidebar, timer](const QJsonObject& result, const QString& error) {
                if (!sidebar || !timer || generation != state->generation)
                    return;
                timer->stop();
                state->pending = false;
                sidebar->setStorageStatus(error.isEmpty() ? result.toVariantMap() : QVariantMap{});
            });
    };
    connect(poll, &QTimer::timeout, this, refresh);
    connect(server, &CodexAppServer::readyChanged, this,
            [refresh, poll, deadline, state, sidebar](bool ready) {
                ++state->generation;
                state->pending = false;
                deadline->stop();
                if (ready) {
                    poll->start();
                    refresh();
                } else {
                    poll->stop();
                    if (sidebar)
                        sidebar->setStorageStatus({});
                }
            });
    if (server->isReady()) {
        poll->start();
        refresh();
    }
}
} // namespace ketplus
