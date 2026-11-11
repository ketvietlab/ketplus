#include "DaemonClient.h"
#include "app/MainWindow.h"
#include "workspace/ActivityBar.h"

#include <QJsonObject>
#include <QPointer>

namespace ketplus {

remote::DaemonClient* MainWindow::accountDaemon() {
    // One connection serves the sidebar and Settings → Account & devices; Connections keeps
    // its own because it also streams pairing events.
    if (accountDaemon_ == nullptr)
        accountDaemon_ = new remote::DaemonClient(this);
    return accountDaemon_;
}

void MainWindow::setupSidebarAccount() {
    auto* daemon = accountDaemon();
    const QPointer<ActivityBar> sidebar(activityBar_);
    const auto read = [daemon, sidebar] {
        daemon->call(QStringLiteral("remote.status"), {}, {}, -1,
                     [sidebar](const QJsonObject& status, const QString& error) {
                         if (!sidebar || !error.isEmpty())
                             return;
                         if (!status.value(QStringLiteral("configured")).toBool()) {
                             sidebar->setAccount({});
                             return;
                         }
                         // Same order as Connections: the display name, then the username.
                         const QJsonObject account =
                             status.value(QStringLiteral("account")).toObject();
                         QString name = account.value(QStringLiteral("display_name")).toString();
                         if (name.isEmpty())
                             name = account.value(QStringLiteral("username")).toString();
                         sidebar->setAccount(name); // none before ADR 0109: the product name
                     });
    };
    // Later sign-ins and sign-outs reach the sidebar from Connections (accountChanged).
    connect(daemon, &remote::DaemonClient::readyChanged, this, [read](const bool ready) {
        if (ready)
            read();
    });
    if (daemon->isReady())
        read();
}

} // namespace ketplus
