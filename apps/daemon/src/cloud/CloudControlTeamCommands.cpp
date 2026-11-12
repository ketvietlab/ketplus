// ADR 0116: team commands from paired phones, sealed to this Mac and answered sealed to the
// device that sent them. The rules are the session host's; this file only checks that the
// command is for this Mac and its owner, and names repositories by their catalog ids.
#include "cloud/CloudControlClient.h"
#include "cloud/CloudControlSupport.h"
#include "protocol/DaemonProtocol.h"
#include "runtime/SessionHost.h"

#include <QPointer>

namespace ketplus::daemon {
using cloud_control::sourceContext;

namespace {

QJsonObject refusal(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

} // namespace

void CloudControlClient::executeTeamCommand(const QJsonObject& command, const QJsonObject& payload,
                                            const QByteArray& senderAgreementKey) {
    const QString kind = command.value(QStringLiteral("kind")).toString();
    const QJsonObject content = command.value(QStringLiteral("content")).toObject();
    QJsonObject scoped = command;
    scoped.insert(QStringLiteral("_teamId"), payload.value(QStringLiteral("team_id")).toString());
    const auto answer = [this, senderAgreementKey](QJsonObject sent, const QJsonObject& result) {
        // A created team is named by its result.
        if (sent.value(QStringLiteral("_teamId")).toString().isEmpty())
            sent.insert(QStringLiteral("_teamId"), result.value(QStringLiteral("team"))
                                                       .toObject()
                                                       .value(QStringLiteral("id"))
                                                       .toString());
        acknowledgeCommand(sent, result, senderAgreementKey);
    };
    if (!SessionHost::isRemoteTeamCommand(kind)) {
        answer(scoped, refusal(QStringLiteral("unknown_operation"),
                               QStringLiteral("The requested daemon operation is not supported.")));
        return;
    }
    // §5: only for this Mac and the account that owns it, under the capability the kind
    // needs, through the mailbox. The relay checked the device and its key version; the
    // sender's keys were matched against a device pinned here before the command opened.
    const QString capability = SessionHost::isRemoteTeamRead(kind) ? QStringLiteral("teams.read")
                                                                   : QStringLiteral("teams.write");
    if (!command.value(QStringLiteral("_peerConnection")).toString().isEmpty() ||
        command.value(QStringLiteral("workspaceId")).toString() != workspaceId_ ||
        command.value(QStringLiteral("machineId")).toString() != machineId_ ||
        command.value(QStringLiteral("ownerSubjectId")).toString().isEmpty() ||
        !command.value(QStringLiteral("sessionId")).toString().isEmpty() ||
        command.value(QStringLiteral("sessionGeneration")).toInteger(-1) != 0 ||
        command.value(QStringLiteral("capability")).toString() != capability ||
        content.value(QStringLiteral("recipientDeviceId")).toString() != machineId_ ||
        content.value(QStringLiteral("recipientKeyVersion")).toInt() != machineKeyVersion_) {
        answer(scoped, refusal(QStringLiteral("team_scope_forbidden"),
                               QStringLiteral("This device may not act on this Mac's teams.")));
        return;
    }
    QJsonObject localPayload = payload;
    const QByteArray salt = ensurePathIdSalt();
    if (kind == QStringLiteral("team.create")) {
        // The phone never names a local path: the repository is the catalog's, at the catalog
        // revision the phone drew it from (ADR 0116 §4). The catalog is served at revision 0,
        // as the worktree commands require.
        constexpr qint64 currentCatalogRevision = 0;
        const QString repositoryId = command.value(QStringLiteral("repositoryId")).toString();
        QString repository;
        if (!payload.contains(QStringLiteral("repository")) && !repositoryId.isEmpty() &&
            !salt.isEmpty() &&
            command.value(QStringLiteral("expectedCatalogRevision")).toInteger(-1) ==
                currentCatalogRevision) {
            for (const QString& candidate : catalogWorktreePaths()) {
                if (sourceContext(candidate, salt).value(QStringLiteral("repositoryId")) ==
                    repositoryId) {
                    repository = candidate;
                    break;
                }
            }
        }
        if (repository.isEmpty()) {
            answer(scoped, refusal(QStringLiteral("repository_not_found"),
                                   QStringLiteral("The repository is no longer on this Mac.")));
            return;
        }
        localPayload.insert(QStringLiteral("repository"), repository);
    } else if (payload.contains(QStringLiteral("repository"))) {
        answer(scoped, refusal(QStringLiteral("invalid_payload"),
                               QStringLiteral("A paired device cannot name a repository path.")));
        return;
    }
    teams::remote::Ids ids;
    ids.repository = [salt](const QString& path) {
        return sourceContext(path, salt).value(QStringLiteral("repositoryId")).toString();
    };
    ids.worktree = [salt](const QString& path) {
        return sourceContext(path, salt).value(QStringLiteral("worktreeId")).toString();
    };
    const QJsonObject local{{QStringLiteral("protocol_version"), 0},
                            {QStringLiteral("message_id"), command.value(QStringLiteral("id"))},
                            {QStringLiteral("message_type"), kind},
                            {QStringLiteral("sent_at"), protocol::timestamp()},
                            {QStringLiteral("payload"), localPayload}};
    const quint64 requestEpoch = accountEpoch_;
    host_->remoteTeamCommand(local, ids,
                             [guard = QPointer<CloudControlClient>(this), scoped, requestEpoch,
                              answer](const QJsonObject& result) {
                                 // Answered only while the Mac is still in that account.
                                 if (guard && guard->accountEpoch_ == requestEpoch)
                                     answer(scoped, result);
                             });
}

void CloudControlClient::noteTeamChanged() {
    if (teamSyncTimer_.isActive())
        return;
    const qint64 since =
        lastTeamSync_.isValid() ? lastTeamSync_.elapsed() : teamHeartbeatMilliseconds;
    teamSyncTimer_.start(int(std::max<qint64>(300, teamHeartbeatMilliseconds - since)));
}

} // namespace ketplus::daemon
