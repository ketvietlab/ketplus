#include "DaemonClient.h"
#include "agent/CodexAppServer.h"

namespace ketplus {
bool CodexAppServer::hasDaemonActivityGroups() const noexcept {
    return daemon_ && daemon_->hasFeature("transcript.activity_groups/1");
}

void CodexAppServer::readDaemonActivityGroup(const QString& sessionId, const QJsonObject& params,
                                             ResponseCallback callback) {
    const auto found = daemonSessions_.constFind(sessionId);
    if (!daemon_ || !daemon_->hasFeature("transcript.activity_groups/1") ||
        found == daemonSessions_.cend()) {
        callback({}, "This daemon does not support activity group details.");
        return;
    }
    const auto generation = found->generation;
    const auto epoch = found->windowEpoch;
    daemon_->call("session.activity_group_page", params, sessionId, generation,
                  [this, sessionId, generation, epoch, callback = std::move(callback)](
                      const QJsonObject& page, const QString& error) mutable {
                      const auto current = daemonSessions_.constFind(sessionId);
                      if (current == daemonSessions_.cend() || current->generation != generation ||
                          current->windowEpoch != epoch) {
                          callback({}, "Conversation changed while loading group details.");
                          return;
                      }
                      if (!error.isEmpty() || !page.value("ok").toBool()) {
                          callback(page, error);
                          return;
                      }
                      if (page.value("session_id").toString() != sessionId ||
                          page.value("session_generation").toInteger() != generation) {
                          callback({}, "Invalid activity group session.");
                          return;
                      }
                      resolveDaemonLargeEvents(sessionId, generation, epoch, page,
                                               std::move(callback));
                  });
}
} // namespace ketplus
