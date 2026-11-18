#include "runners/RunnersDaemonLink.h"

#include "DaemonClient.h"
#include "runners/SettingsRunnersPage.h"

#include <QCoreApplication>
#include <QPointer>

#include <utility>

namespace ketplus::runners {

Requester daemonRequester(remote::DaemonClient* daemon) {
    return [daemon = QPointer<remote::DaemonClient>(daemon)](
               const QString& type, const QJsonObject& payload, Reply reply) {
        if (!daemon || !daemon->isReady()) {
            reply({}, QCoreApplication::translate("SettingsRunnersPage",
                                                  "KetPlus Daemon is not running."));
            return;
        }
        daemon->call(type, payload, {}, -1, std::move(reply));
    };
}

void followDaemon(remote::DaemonClient* daemon, SettingsRunnersPage* page) {
    if (!daemon || !page)
        return;
    QObject::connect(daemon, &remote::DaemonClient::noticeReceived, page,
                     [page](const QString& type, const QJsonObject& payload) {
                         if (type == QStringLiteral("runner.pairing.changed"))
                             page->pairingChanged(payload);
                     });
    QObject::connect(daemon, &remote::DaemonClient::readyChanged, page, [page](const bool ready) {
        if (ready && page->isVisible())
            page->refresh();
    });
}

} // namespace ketplus::runners
