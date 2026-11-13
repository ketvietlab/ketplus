#include "KetPlusProductVersion.h"
#include "cloud/CloudControlClient.h"
#include "cloud/CloudControlSupport.h"
#include "storage/LocalJournal.h"

#include <QDateTime>
#include <QJsonArray>
#include <QPointer>

namespace ketplus::daemon {

// Publish routing and the creator's existing-format key wrap before returning success.
// Normal heartbeats/key distribution enumerate only active journal sessions, so an inactive
// draft is never listed on another device or wrapped to every paired device.
void CloudControlClient::publishPreparedSession(const QJsonObject& command,
                                                const QJsonObject& result,
                                                const QByteArray& senderAgreementKey) {
    const QString id = result.value("session_id").toString();
    const auto content = command.value("content").toObject();
    const QString creator = content.value("senderDeviceId").toString();
    const int creatorVersion = content.value("senderKeyVersion").toInt();
    const auto epoch = accountEpoch_;
    const auto machine = machineId_;
    const auto workspace = workspaceId_;
    const auto keyVersion = machineKeyVersion_;
    const QPointer<CloudControlClient> guard(this);
    const auto current = [guard, epoch, machine, workspace, keyVersion] {
        return guard && guard->accountEpoch_ == epoch && guard->machineId_ == machine &&
               guard->workspaceId_ == workspace && guard->machineKeyVersion_ == keyVersion;
    };
    const auto available = [guard, current, id, creator] {
        if (!current())
            return false;
        const auto draft = guard->journal_->sessionPreparation(id);
        return draft && draft->state == "prepared" && draft->owner == "relay:" + creator &&
               draft->expiresAt > QDateTime::currentMSecsSinceEpoch();
    };
    const auto finish = [guard, current, command, senderAgreementKey](const QJsonObject& reply) {
        if (current())
            guard->acknowledgeCommand(command, reply, senderAgreementKey);
    };
    const auto failed = [finish] {
        finish({{"ok", false},
                {"error_code", "preparation_publish_failed"},
                {"error_message",
                 "The private attachment scope could not be published. Retry the draft."}});
    };
    if (!available()) {
        failed();
        return;
    }
    const auto draft = journal_->sessionPreparation(id);
    const auto salt = ensurePathIdSalt();
    if (salt.isEmpty()) {
        failed();
        return;
    }
    QJsonObject snapshot{
        {"sessionId", id},
        {"processInstanceId", id},
        {"generation", 1},
        {"state", "prepared"},
        {"adapter", draft->options.value("provider").toString("codex")},
        {"lastEventCursor", 0},
        {"currentTurnSequence", 0},
        {"preparedDeviceId", creator},
        {"preparedExpiresAt",
         QDateTime::fromMSecsSinceEpoch(draft->expiresAt, Qt::UTC).toString(Qt::ISODateWithMs)}};
    const auto scope = cloud_control::sourceContext(draft->worktreePath, salt);
    for (const auto* key : {"repositoryId", "worktreeId", "repositoryLabel", "worktreeLabel"})
        snapshot.insert(QLatin1String(key), scope.value(QLatin1String(key)));
    const QJsonObject body{
        {"machineId", machineId_},
        {"daemonVersion", KETPLUS_PRODUCT_VERSION},
        {"capabilities", QJsonArray{"transport.webrtc/1", "teams.remote/1", "session.prepare/1"}},
        {"worktrees", QJsonArray{}},
        {"sessions", QJsonArray{snapshot}}};
    request("POST", "/v2/daemon/heartbeat", body, daemonToken_, {},
            [guard, id, creator, creatorVersion, result, available, finish,
             failed](int status, const QJsonValue& value, const QString& error) {
                if (!available() || status != 202 || !error.isEmpty()) {
                    failed();
                    return;
                }
                for (const auto& rejected : value.toObject().value("rejectedSessions").toArray())
                    if (rejected.toObject().value("sessionId").toString() == id) {
                        failed();
                        return;
                    }
                guard->request(
                    "GET", "/v2/devices", {}, guard->daemonToken_, {},
                    [guard, id, creator, creatorVersion, result, available, finish, failed](
                        int deviceStatus, const QJsonValue& devices, const QString& deviceError) {
                        if (!available() || deviceStatus != 200 || !devices.isArray() ||
                            !deviceError.isEmpty()) {
                            failed();
                            return;
                        }
                        QJsonObject recipient;
                        for (const auto& candidate : guard->trustedDevices(devices.toArray())) {
                            const auto device = candidate.toObject();
                            if (device.value("id").toString() == creator &&
                                device.value("keyVersion").toInt() == creatorVersion)
                                recipient = device;
                        }
                        if (recipient.isEmpty()) {
                            failed();
                            return;
                        }
                        QString cryptoError;
                        const auto envelope =
                            guard->crypto_.wrapSessionKey(id, 1, 1, recipient, guard->machineId_,
                                                          guard->machineKeyVersion_, &cryptoError);
                        if (envelope.isEmpty()) {
                            failed();
                            return;
                        }
                        guard->request(
                            "POST", "/v2/session-keys:append",
                            QJsonObject{{"envelopes", QJsonArray{envelope}}}, guard->daemonToken_,
                            {},
                            [result, available, finish, failed](int keyStatus, const QJsonValue&,
                                                                const QString& keyError) {
                                if (!available() || keyStatus != 202 || !keyError.isEmpty())
                                    failed();
                                else
                                    finish(result);
                            });
                    });
            });
}
} // namespace ketplus::daemon
