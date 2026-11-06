#include "atlas/AtlasLiveRuntime.h"
#include "cloud/CloudControlBatching.h"
#include "cloud/CloudControlClient.h"
#include "cloud/CloudControlSupport.h"
#include "cloud/CloudEventProjection.h"
#include "cloud/ControlEndpoint.h"
#include "cloud/WorktreeBranches.h"
#include "cloud/WorktreeRepositories.h"
#include <QRandomGenerator>
#include <QUrlQuery>

#include "KetPlusProductVersion.h"
#include "protocol/DaemonProtocol.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include "workspace/ReadOnlyWorkspaceFiles.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QThreadPool>

#include <algorithm>
#include <utility>

// Relay commands from paired devices: polling, executing and acknowledging them.

namespace ketplus::daemon {
using cloud_control::opaquePathId;
using cloud_control::object;
using cloud_control::responseCode;
using cloud_control::sourceContext;

namespace {

// Scoped to a catalog worktree rather than a session, and sealed to this Mac.
bool isWorktreeCommand(const QJsonObject& command) {
    const QString kind = command.value(QStringLiteral("kind")).toString();
    return kind == QStringLiteral("session.create") ||
           kind == QStringLiteral("worktree.branches.read") ||
           kind == QStringLiteral("worktree.configuration.read");
}

} // namespace

void CloudControlClient::pollCommands() {
    if (machineId_.isEmpty() || commandPollInFlight_ || !commandPollRetryAt_.hasExpired())
        return;
    commandPollInFlight_ = true;
    ensureDaemonCredential([this](const bool ready, const QString& error) {
        if (!ready) {
            commandPollInFlight_ = false;
            lastError_ = error;
            noteCommandPollFailure(0);
            return;
        }
        request(
            "GET", QStringLiteral("/v2/commands:poll?limit=20"), {}, daemonToken_,
            {{QByteArray("X-KetPlus-Connection-Id"), QByteArray("daemon-") + machineId_.toUtf8()}},
            [this](const int status, const QJsonValue& value, const QString& transportError) {
                commandPollInFlight_ = false;
                if (std::exchange(commandPollAgain_, false))
                    QTimer::singleShot(0, this, &CloudControlClient::pollCommands);
                if (status != 200 || !transportError.isEmpty()) {
                    lastError_ = transportError.isEmpty()
                                     ? responseCode(value, QStringLiteral("command_poll_failed"))
                                     : transportError;
                    // The relay leased this poll's commands; they return once the lease lapses.
                    if (status == 200)
                        qWarning("command poll lost its leased commands: %s",
                                 qUtf8Printable(transportError));
                    if (status == 401)
                        daemonToken_.clear();
                    noteCommandPollFailure(status);
                    return;
                }
                commandPollFailures_ = 0;
                for (const QJsonValue& command :
                     object(value).value(QStringLiteral("commands")).toArray()) {
                    executeCommand(command.toObject());
                }
            });
    });
}

// A relay that is down or has revoked this machine is asked again after a growing wait, as
// uploads are, instead of every 2 seconds; jitter spreads the machines an outage disconnected.
void CloudControlClient::noteCommandPollFailure(const int status) {
    ++commandPollFailures_;
    const qint64 delay =
        eventUploadBackoffMilliseconds(commandPollFailures_, eventUploadRefused(status));
    commandPollRetryAt_.setRemainingTime(delay / 2 +
                                         QRandomGenerator::global()->bounded(delay / 2 + 1));
}

void CloudControlClient::wakeCommandPoll() {
    // A wake-up comes from the relay, so it is reachable again.
    commandPollFailures_ = 0;
    commandPollRetryAt_ = QDeadlineTimer(0);
    if (commandPollInFlight_)
        commandPollAgain_ = true;
    else
        pollCommands();
}

QByteArray CloudControlClient::commandAssociatedData(const QJsonObject& command) const {
    const QJsonObject content = command.value(QStringLiteral("content")).toObject();
    QJsonArray values{QStringLiteral("ketplus"),
                      2,
                      QStringLiteral("command"),
                      command.value(QStringLiteral("id")),
                      command.value(QStringLiteral("workspaceId")),
                      command.value(QStringLiteral("machineId")),
                      command.value(QStringLiteral("sessionId")),
                      command.value(QStringLiteral("sessionGeneration")),
                      command.value(QStringLiteral("idempotencyKey")),
                      command.value(QStringLiteral("capability")),
                      command.value(QStringLiteral("kind")),
                      content.value(QStringLiteral("keyId")),
                      content.value(QStringLiteral("keyEpoch")),
                      content.value(QStringLiteral("senderDeviceId")),
                      content.value(QStringLiteral("senderKeyVersion"))};
    if (command.value(QStringLiteral("kind")) == QStringLiteral("transcript.restore_window")) {
        values.append(command.value(QStringLiteral("credentialEpoch")));
        values.append(command.value(QStringLiteral("ownerSubjectId")));
        values.append(command.value(QStringLiteral("snapshotCursor")));
        values.append(command.value(QStringLiteral("requestedByteCap")));
        values.append(command.value(QStringLiteral("continuationOfRequestId")).toString());
        values.append(command.value(QStringLiteral("continuationSequence")).toInteger(0));
        values.append(command.value(QStringLiteral("continuationDigest")).toString());
        values.append(command.value(QStringLiteral("deadlineAt")));
    }
    if (command.value(QStringLiteral("kind")) == QStringLiteral("session.worktree.switch")) {
        values.append(command.value(QStringLiteral("repositoryId")));
        values.append(command.value(QStringLiteral("worktreeId")));
    }
    if (command.value(QStringLiteral("kind"))
            .toString()
            .startsWith(QStringLiteral("workspace.files."))) {
        values.append(command.value(QStringLiteral("repositoryId")));
        values.append(command.value(QStringLiteral("worktreeId")));
        values.append(command.value(QStringLiteral("workspaceRevision")));
    }
    if (isWorktreeCommand(command)) {
        values.append(command.value(QStringLiteral("ownerSubjectId")));
        values.append(command.value(QStringLiteral("repositoryId")));
        values.append(command.value(QStringLiteral("worktreeId")));
        values.append(command.value(QStringLiteral("expectedCatalogRevision")));
        values.append(command.value(QStringLiteral("expectedLifecycleRevision")));
        values.append(content.value(QStringLiteral("recipientDeviceId")));
        values.append(content.value(QStringLiteral("recipientKeyVersion")));
        values.append(content.value(QStringLiteral("ephemeralPublicKey")));
    }
    if (command.value(QStringLiteral("kind")) == QStringLiteral("session.delete")) {
        values.append(command.value(QStringLiteral("ownerSubjectId")));
        values.append(command.value(QStringLiteral("expectedLifecycleRevision")));
        values.append(command.value(QStringLiteral("deletionChallengeId")));
    }
    return CloudCrypto::canonical(values);
}

QByteArray CloudControlClient::restoreResponseAssociatedData(const QJsonObject& command,
                                                             const qint64 responseCursor,
                                                             const bool terminal,
                                                             const QString& responseDigest,
                                                             const QJsonObject& content) const {
    return CloudCrypto::canonical(
        {QStringLiteral("ketplus"), 2, QStringLiteral("transcript-restore-response"),
         command.value(QStringLiteral("id")), command.value(QStringLiteral("ownerSubjectId")),
         command.value(QStringLiteral("workspaceId")), command.value(QStringLiteral("machineId")),
         command.value(QStringLiteral("sessionId")),
         command.value(QStringLiteral("sessionGeneration")),
         command.value(QStringLiteral("credentialEpoch")),
         command.value(QStringLiteral("snapshotCursor")), responseCursor, terminal ? 1 : 0,
         responseDigest, content.value(QStringLiteral("keyId")),
         content.value(QStringLiteral("keyEpoch")), content.value(QStringLiteral("senderDeviceId")),
         content.value(QStringLiteral("senderKeyVersion"))});
}

QByteArray CloudControlClient::commandResultAssociatedData(const QJsonObject& command,
                                                           const int attempt, const QString& status,
                                                           const QString& resultCode,
                                                           const QJsonObject& content) const {
    QJsonArray values{QStringLiteral("ketplus"),
                      2,
                      QStringLiteral("command-result"),
                      command.value(QStringLiteral("id")),
                      command.value(QStringLiteral("workspaceId")),
                      command.value(QStringLiteral("machineId")),
                      command.value(QStringLiteral("sessionId")),
                      command.value(QStringLiteral("sessionGeneration")),
                      attempt,
                      status,
                      resultCode,
                      content.value(QStringLiteral("keyId")),
                      content.value(QStringLiteral("keyEpoch")),
                      content.value(QStringLiteral("senderDeviceId")),
                      content.value(QStringLiteral("senderKeyVersion"))};
    if (command.value(QStringLiteral("kind")) == QStringLiteral("session.worktree.switch")) {
        values.append(command.value(QStringLiteral("repositoryId")));
        values.append(command.value(QStringLiteral("worktreeId")));
    }
    if (command.value(QStringLiteral("kind"))
            .toString()
            .startsWith(QStringLiteral("workspace.files."))) {
        values.append(command.value(QStringLiteral("repositoryId")));
        values.append(command.value(QStringLiteral("worktreeId")));
        values.append(command.value(QStringLiteral("workspaceRevision")));
    }
    return CloudCrypto::canonical(values);
}

void CloudControlClient::withSenderKeys(const QString& senderId, const int version,
                                        SenderKeysCallback callback) {
    const QString cacheKey = QStringLiteral("%1/%2").arg(senderId).arg(version);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (const auto cached = senderKeys_.constFind(cacheKey);
        cached != senderKeys_.cend() && cached->second > now) {
        callback(cached->first, {});
        return;
    }
    const QString keyPath = QStringLiteral("/v2/device-keys/%1/%2")
                                .arg(QString::fromUtf8(QUrl::toPercentEncoding(senderId)))
                                .arg(version);
    request("GET", keyPath, {}, daemonToken_, {},
            [this, cacheKey, callback = std::move(callback)](
                const int status, const QJsonValue& value, const QString& transportError) {
                if (status != 200 || !transportError.isEmpty()) {
                    callback({}, transportError.isEmpty()
                                     ? QStringLiteral("The sender key is unavailable.")
                                     : transportError);
                    return;
                }
                const QJsonObject keys = object(value).value(QStringLiteral("keys")).toObject();
                if (senderKeys_.size() >= maximumCachedSenderKeys)
                    senderKeys_.clear();
                senderKeys_.insert(cacheKey, {keys, QDateTime::currentDateTimeUtc().addMSecs(
                                                        senderKeyCacheMilliseconds)});
                callback(keys, {});
            });
}

void CloudControlClient::executeCommand(const QJsonObject& command) {
    if (command.value(QStringLiteral("machineId")).toString() != machineId_)
        return;
    const QJsonObject content = command.value(QStringLiteral("content")).toObject();
    const QString senderId = content.value(QStringLiteral("senderDeviceId")).toString();
    const int senderVersion = content.value(QStringLiteral("senderKeyVersion")).toInt();
    const QString commandId = command.value(QStringLiteral("id")).toString();
    if (commandStartedAt_.size() >= 1024)
        commandStartedAt_.clear();
    commandStartedAt_.insert(commandId, QDateTime::currentMSecsSinceEpoch());
    qInfo("command %s %s claimed attempt=%d", qUtf8Printable(commandId),
          qUtf8Printable(command.value(QStringLiteral("kind")).toString()),
          command.value(QStringLiteral("attempt")).toInt());
    // Every command from a phone names the same few device keys; a relay read per command
    // was a large share of the relay's traffic.
    withSenderKeys(
        senderId, senderVersion,
        [this, command, senderId, senderVersion](const QJsonObject& senderKeys,
                                                 const QString& keyError) {
            const auto peerId = command.value("_peerConnection").toString();
            if (!peerId.isEmpty() && !peers_->authorized(peerId, command))
                return;
            QJsonObject localResult;
            if (!keyError.isEmpty()) {
                localResult = {
                    {QStringLiteral("ok"), false},
                    {QStringLiteral("error_code"), QStringLiteral("sender_key_unavailable")},
                    {QStringLiteral("error_message"), keyError}};
                acknowledgeCommand(command, localResult);
                return;
            }
            const QByteArray senderKey = CloudCrypto::fromBase64Url(
                senderKeys.value(QStringLiteral("signingPublicKey")).toString());
            const QByteArray senderAgreementKey = CloudCrypto::fromBase64Url(
                senderKeys.value(QStringLiteral("agreementPublicKey")).toString());
            QString cryptoError;
            const QString sessionId = command.value(QStringLiteral("sessionId")).toString();
            const qint64 generation =
                command.value(QStringLiteral("sessionGeneration")).toInteger();
            // A conversation's first command, and the branch list read to start one, come
            // before any session key exists, so they are sealed to this Mac.
            const bool creating = isWorktreeCommand(command);
            const QJsonObject content = command.value(QStringLiteral("content")).toObject();
            const QByteArray plaintext =
                creating ? crypto_.decryptSealed(content, commandAssociatedData(command), senderKey,
                                                 command.value(QStringLiteral("id")).toString(),
                                                 &cryptoError)
                         : crypto_.decrypt(content, commandAssociatedData(command), senderKey,
                                           sessionId, generation, &cryptoError);
            const QJsonDocument decoded = QJsonDocument::fromJson(plaintext);
            if (plaintext.isEmpty() || !decoded.isObject()) {
                // A cached key that no longer opens the sender's commands is read again next time.
                senderKeys_.remove(QStringLiteral("%1/%2").arg(senderId).arg(senderVersion));
                localResult = {
                    {QStringLiteral("ok"), false},
                    {QStringLiteral("error_code"), QStringLiteral("invalid_encrypted_command")},
                    {QStringLiteral("error_message"),
                     cryptoError.isEmpty() ? QStringLiteral("The command body is invalid.")
                                           : cryptoError}};
            } else {
                if (creating) {
                    executeWorktreeCommand(command, decoded.object(), senderAgreementKey);
                    return;
                }
                if (command.value(QStringLiteral("kind")) ==
                    QStringLiteral("transcript.restore_window")) {
                    executeRestoreWindow(command, decoded.object());
                    return;
                }
                const QString kind = command.value(QStringLiteral("kind")).toString();
                if (kind == QStringLiteral("session.delete")) {
                    const QJsonObject payload = decoded.object();
                    if (command.value(QStringLiteral("ownerSubjectId")).toString().isEmpty() ||
                        command.value(QStringLiteral("workspaceId")).toString() != workspaceId_ ||
                        command.value(QStringLiteral("machineId")).toString() != machineId_ ||
                        command.value(QStringLiteral("expectedLifecycleRevision")).toInteger(-1) <
                            0 ||
                        command.value(QStringLiteral("deletionChallengeId")).toString().isEmpty() ||
                        payload.value(QStringLiteral("confirmation")).toString() !=
                            QStringLiteral("DELETE")) {
                        localResult = {{QStringLiteral("ok"), false},
                                       {QStringLiteral("error_code"),
                                        QStringLiteral("delete_scope_forbidden")}};
                        acknowledgeCommand(command, localResult);
                        return;
                    }
                    QJsonObject localPayload = payload;
                    localPayload.insert(QStringLiteral("owner_subject_id"),
                                        command.value(QStringLiteral("ownerSubjectId")));
                    localPayload.insert(QStringLiteral("delete_operation_id"),
                                        command.value(QStringLiteral("idempotencyKey")));
                    localPayload.insert(QStringLiteral("expected_lifecycle_revision"),
                                        command.value(QStringLiteral("expectedLifecycleRevision")));
                    const QJsonObject local{
                        {QStringLiteral("protocol_version"), 0},
                        {QStringLiteral("message_id"), command.value(QStringLiteral("id"))},
                        {QStringLiteral("message_type"), QStringLiteral("session.delete")},
                        {QStringLiteral("session_id"), sessionId},
                        {QStringLiteral("session_generation"), generation},
                        {QStringLiteral("sent_at"), protocol::timestamp()},
                        {QStringLiteral("payload"), localPayload}};
                    localResult = host_->handleCommand(local, QStringLiteral("cloud-control-v2"));
                    acknowledgeCommand(command, localResult);
                    return;
                }
                if (kind.startsWith(QStringLiteral("workspace.files."))) {
                    executeWorkspaceFiles(command, decoded.object());
                    return;
                }
                if ((kind == QStringLiteral("session.configuration.read") &&
                     !decoded.object().value("effective_only").toBool()) ||
                    kind == QStringLiteral("session.configuration.apply")) {
                    host_->handleProviderQuery(
                        QJsonObject{{QStringLiteral("message_type"),
                                     QStringLiteral("provider.configuration_catalog")},
                                    {QStringLiteral("session_id"), sessionId},
                                    {QStringLiteral("payload"), QJsonObject{}}},
                        [this, command, decoded, sessionId,
                         generation](const QJsonObject& models, const QString& providerError) {
                            if (!providerError.isEmpty()) {
                                acknowledgeCommand(
                                    command, {{QStringLiteral("ok"), false},
                                              {QStringLiteral("error_code"),
                                               QStringLiteral("provider_capabilities_unavailable")},
                                              {QStringLiteral("error_message"),
                                               QStringLiteral(
                                                   "The available settings could not be read.")}});
                                return;
                            }
                            QJsonObject payload = decoded.object();
                            payload.insert(QStringLiteral("provider_catalog"),
                                           models.value(QStringLiteral("catalog")).toArray());
                            const QJsonObject local{
                                {QStringLiteral("protocol_version"), 0},
                                {QStringLiteral("message_id"), command.value(QStringLiteral("id"))},
                                {QStringLiteral("message_type"),
                                 command.value(QStringLiteral("kind"))},
                                {QStringLiteral("session_id"), sessionId},
                                {QStringLiteral("session_generation"), generation},
                                {QStringLiteral("sent_at"), protocol::timestamp()},
                                {QStringLiteral("payload"), payload}};
                            acknowledgeCommand(
                                command,
                                host_->handleCommand(local, QStringLiteral("cloud-control-v2")));
                        });
                    return;
                }
                if (kind == "atlas.list" || kind == "atlas.start" || kind == "atlas.detach") {
                    const auto payload = decoded.object();
                    const auto sessions = journal_->sessions();
                    const auto found =
                        std::find_if(sessions.cbegin(), sessions.cend(), [&](const auto& s) {
                            return s.id == sessionId && s.generation == generation;
                        });
                    const auto device =
                        command.value("content").toObject().value("senderDeviceId").toString();
                    if (found == sessions.cend() || command.value("workspaceId") != workspaceId_ ||
                        device.isEmpty()) {
                        acknowledgeCommand(
                            command, {{"ok", false}, {"error_code", "atlas_scope_forbidden"}});
                        return;
                    }
                    if (kind == "atlas.detach") {
                        for (auto* runtime : findChildren<AtlasLiveRuntime*>()) {
                            if (runtime->channel() == payload.value("channel").toString() &&
                                runtime->property("device").toString() == device &&
                                runtime->property("session").toString() == sessionId &&
                                runtime->property("generation").toLongLong() == generation) {
                                runtime->stop();
                                runtime->deleteLater();
                            }
                        }
                        acknowledgeCommand(command, {{"ok", true}});
                        return;
                    }
                    for (auto* runtime : findChildren<AtlasLiveRuntime*>()) {
                        if (runtime->property("command").toString() ==
                            command.value("id").toString()) {
                            const auto saved = runtime->property("attachment").toJsonObject();
                            if (!saved.isEmpty())
                                acknowledgeCommand(command, saved);
                            return;
                        }
                    }
                    if (kind == "atlas.start" && payload.contains("attach_channel")) {
                        for (auto* runtime : findChildren<AtlasLiveRuntime*>()) {
                            if (!runtime->reusable() ||
                                runtime->channel() != payload.value("attach_channel").toString() ||
                                runtime->property("device").toString() != device ||
                                runtime->property("session").toString() != sessionId ||
                                runtime->property("generation").toLongLong() != generation)
                                continue;
                            auto relay =
                                controlEndpoint(baseUrl_, QStringLiteral("/v2/atlas/tunnel"));
                            relay.setScheme(relay.scheme() == "http" ? "ws" : "wss");
                            QUrlQuery query;
                            query.addQueryItem("role", "daemon");
                            query.addQueryItem("deviceId", device);
                            query.addQueryItem("sessionId", sessionId);
                            query.addQueryItem("generation", QString::number(generation));
                            relay.setQuery(query);
                            runtime->setProperty("command", command.value("id").toString());
                            runtime->attach(relay, daemonToken_,
                                            [this, runtime, command](const QJsonObject& result) {
                                                runtime->setProperty("attachment", result);
                                                acknowledgeCommand(command, result);
                                            });
                            return;
                        }
                        acknowledgeCommand(
                            command, {{"ok", false}, {"error_code", "atlas_attachment_expired"}});
                        return;
                    }
                    if (kind == "atlas.start" &&
                        payload.value("expected_context_revision").toInteger(-1) !=
                            found->contextRevision) {
                        acknowledgeCommand(
                            command, {{"ok", false}, {"error_code", "context_revision_conflict"}});
                        return;
                    }
                    if (findChildren<AtlasLiveRuntime*>().size() >= 8 ||
                        workspaceFilesPending_.fetch_add(1) >= 8) {
                        // The counter is acquired only when the runtime limit did not
                        // short-circuit.
                        if (findChildren<AtlasLiveRuntime*>().size() < 8)
                            workspaceFilesPending_.fetch_sub(1);
                        acknowledgeCommand(command, {{"ok", false}, {"error_code", "busy"}});
                        return;
                    }
                    const auto source = *found;
                    const auto accountEpoch = accountEpoch_;
                    const QPointer<CloudControlClient> guard(this);
                    workspaceFilesPool_.start([guard, command, payload, kind, source, device,
                                               accountEpoch]() {
                        const auto projects = AtlasIndex::scan(source.worktreePath);
                        if (!guard)
                            return;
                        QMetaObject::invokeMethod(
                            guard,
                            [guard, command, payload, kind, source, device, projects,
                             accountEpoch] {
                                if (!guard)
                                    return;
                                guard->workspaceFilesPending_.fetch_sub(1);
                                if (guard->accountEpoch_ != accountEpoch)
                                    return;
                                for (auto* runtime : guard->findChildren<AtlasLiveRuntime*>()) {
                                    if (runtime->property("command").toString() !=
                                        command.value("id").toString())
                                        continue;
                                    const auto saved =
                                        runtime->property("attachment").toJsonObject();
                                    if (!saved.isEmpty())
                                        guard->acknowledgeCommand(command, saved);
                                    return;
                                }
                                if (guard->findChildren<AtlasLiveRuntime*>().size() >= 8) {
                                    guard->acknowledgeCommand(
                                        command, {{"ok", false}, {"error_code", "busy"}});
                                    return;
                                }

                                const auto current = guard->journal_->sessions();
                                if (std::none_of(
                                        current.cbegin(), current.cend(), [&](const auto& s) {
                                            return s.id == source.id &&
                                                   s.generation == source.generation &&
                                                   s.contextRevision == source.contextRevision;
                                        })) {
                                    guard->acknowledgeCommand(
                                        command, {{"ok", false},
                                                  {"error_code", "context_revision_conflict"}});
                                    return;
                                }
                                QJsonArray entries;
                                std::optional<AtlasDescriptor> selected;
                                for (const auto& atlas : projects) {
                                    const auto canonical =
                                        QFileInfo(atlas.path).canonicalFilePath();
                                    const auto root =
                                        QFileInfo(source.worktreePath).canonicalFilePath();
                                    if (root.isEmpty() || !canonical.startsWith(root + '/'))
                                        continue;
                                    const auto id =
                                        opaquePathId("atlas", canonical, guard->ensurePathIdSalt());
                                    if (id == payload.value("project_id").toString())
                                        selected = atlas;
                                    if (entries.size() < 200)
                                        entries.append(QJsonObject{{"id", id},
                                                                   {"title", atlas.title},
                                                                   {"path", atlas.relativePath}});
                                }
                                if (kind == "atlas.list") {
                                    guard->acknowledgeCommand(
                                        command, {{"ok", true},
                                                  {"projects", entries},
                                                  {"context_revision", source.contextRevision}});
                                    return;
                                }
                                if (!selected) {
                                    guard->acknowledgeCommand(
                                        command, {{"ok", false},
                                                  {"error_code", "atlas_project_unavailable"}});
                                    return;
                                }
                                AtlasLiveRuntime* runtime = nullptr;
                                for (auto* candidate : guard->findChildren<AtlasLiveRuntime*>()) {
                                    if (candidate->reusable() &&
                                        candidate->property("device").toString() == device &&
                                        candidate->property("session").toString() == source.id &&
                                        candidate->property("generation").toLongLong() ==
                                            source.generation &&
                                        candidate->property("project").toString() ==
                                            selected->path) {
                                        runtime = candidate;
                                        break;
                                    }
                                }
                                const bool reuse = runtime != nullptr;
                                if (!runtime)
                                    runtime = new AtlasLiveRuntime(guard);
                                const auto atlasSource =
                                    sourceContext(source.worktreePath, guard->ensurePathIdSalt());
                                runtime->setProperty("repositoryId",
                                                     atlasSource.value("repositoryId").toString());
                                runtime->setProperty("worktreeId",
                                                     atlasSource.value("worktreeId").toString());
                                runtime->setProperty("project", selected->path);
                                runtime->setProperty("device", device);
                                runtime->setProperty("session", source.id);
                                runtime->setProperty("generation", source.generation);
                                runtime->setProperty("command", command.value("id").toString());
                                auto relay = controlEndpoint(guard->baseUrl_,
                                                             QStringLiteral("/v2/atlas/tunnel"));
                                relay.setScheme(relay.scheme() == "http" ? "ws" : "wss");
                                QUrlQuery query;
                                query.addQueryItem("role", "daemon");
                                query.addQueryItem("deviceId", device);
                                query.addQueryItem("sessionId", source.id);
                                query.addQueryItem("generation",
                                                   QString::number(source.generation));
                                relay.setQuery(query);
                                auto completed = [guard, runtime,
                                                  command](const QJsonObject& result) {
                                    if (!guard)
                                        return;
                                    runtime->setProperty("attachment", result);
                                    guard->acknowledgeCommand(command, result);
                                };
                                if (reuse)
                                    runtime->attach(relay, guard->daemonToken_, completed);
                                else
                                    runtime->start(*selected, relay, guard->daemonToken_,
                                                   source.contextRevision, completed);
                            },
                            Qt::QueuedConnection);
                    });
                    return;
                }
                if (kind == QStringLiteral("session.resource.resolve")) {
                    auto result = host_->handleCommand({{"protocol_version", 0},
                                                        {"message_id", command.value("id")},
                                                        {"message_type", kind},
                                                        {"session_id", sessionId},
                                                        {"session_generation", generation},
                                                        {"sent_at", protocol::timestamp()},
                                                        {"payload", decoded.object()}},
                                                       "cloud-control-v2");
                    const auto root = result.take("worktree_path").toString();
                    if (!result.value("ok").toBool()) {
                        acknowledgeCommand(command, result);
                        return;
                    }
                    result.insert("source_path", decoded.object().value("path"));
                    const auto source = sourceContext(root, ensurePathIdSalt());
                    result.insert("repository_id", source.value("repositoryId"));
                    result.insert("worktree_id", source.value("worktreeId"));
                    result.insert("branch", source.value("branch"));
                    result.insert("repository_label", source.value("repositoryLabel"));
                    const WorkspaceFileScope scope{command.value("ownerSubjectId").toString(),
                                                   workspaceId_,
                                                   machineId_,
                                                   source.value("repositoryId").toString(),
                                                   source.value("worktreeId").toString(),
                                                   root};
                    ReadOnlyWorkspaceFiles::revisionAsync(
                        scope, this,
                        [this, command, result, sessionId,
                         generation](const QString& revision, const QString& error) mutable {
                            const auto sessions = journal_->sessions();
                            const bool valid =
                                std::any_of(sessions.cbegin(), sessions.cend(), [&](const auto& s) {
                                    return s.id == sessionId && s.generation == generation;
                                });
                            if (!valid || revision.isEmpty()) {
                                acknowledgeCommand(
                                    command, {{"ok", false},
                                              {"error_code",
                                               valid ? error : "session_generation_mismatch"}});
                                return;
                            }
                            result.insert("workspace_revision", revision);
                            acknowledgeCommand(command, result);
                        });
                    return;
                }
                if (kind == QStringLiteral("session.worktrees")) {
                    // Repository browsing by path is a trusted local IPC feature only.
                    auto listPayload = decoded.object();
                    listPayload.remove("repository_path");
                    auto result = host_->handleCommand({{"protocol_version", 0},
                                                        {"message_id", command.value("id")},
                                                        {"message_type", kind},
                                                        {"session_id", sessionId},
                                                        {"session_generation", generation},
                                                        {"sent_at", protocol::timestamp()},
                                                        {"payload", listPayload}},
                                                       "cloud-control-v2");
                    const auto current =
                        sourceContext(result.value("worktree_path").toString(), ensurePathIdSalt());
                    result.insert("current_worktree_id", current.value("worktreeId"));
                    QJsonArray entries;
                    for (const auto& v : result.value("worktrees").toArray()) {
                        auto entry = v.toObject();
                        const auto path = entry.take("worktree_path").toString();
                        const auto scope = sourceContext(path, ensurePathIdSalt());
                        entry.insert("worktree_id", scope.value("worktreeId"));
                        entry.insert("repository_id", scope.value("repositoryId"));
                        entry.insert("folder", QFileInfo(path).fileName());
                        entries.append(entry);
                    }
                    result.insert("worktrees", entries);
                    result.remove("contexts");
                    result.remove("repositories");
                    result.remove("worktree_path");
                    if (!result.value("ok").toBool()) {
                        acknowledgeCommand(command, result);
                        return;
                    }
                    // Publish discovered opaque targets before returning the picker page, so
                    // a newly created Git worktree already has a catalog/grant at selection.
                    const auto epoch = accountEpoch_;
                    const QJsonObject heartbeatBody{
                        {"machineId", machineId_},
                        {"controlProtocolVersion", 2},
                        {"localProtocolVersion", 0},
                        {"daemonVersion", QStringLiteral(KETPLUS_PRODUCT_VERSION)},
                        {"observedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                        {"sessions", QJsonArray{}},
                        {"worktrees", worktreeSnapshots()}};
                    request("POST", "/v2/daemon/heartbeat", heartbeatBody, daemonToken_, {},
                            [this, command, result, epoch](int status, const QJsonValue&,
                                                           const QString& error) {
                                if (epoch != accountEpoch_)
                                    return;
                                acknowledgeCommand(
                                    command, status == 202 && error.isEmpty()
                                                 ? result
                                                 : QJsonObject{{"ok", false},
                                                               {"error_code",
                                                                "worktree_catalog_unavailable"}});
                            });
                    return;
                }
                if (kind == QStringLiteral("session.worktree.switch")) {
                    QString target;
                    for (const QString& candidate : journal_->worktreePaths()) {
                        const auto scope = sourceContext(candidate, ensurePathIdSalt());
                        if (scope.value("repositoryId") == command.value("repositoryId") &&
                            scope.value("worktreeId") == command.value("worktreeId")) {
                            target = QFileInfo(candidate).canonicalFilePath();
                            break;
                        }
                    }
                    if (target.isEmpty() ||
                        command.value("workspaceId").toString() != workspaceId_) {
                        acknowledgeCommand(command,
                                           {{"ok", false}, {"error_code", "scope_forbidden"}});
                        return;
                    }
                    QJsonObject payload = decoded.object();
                    payload.insert("worktree_path", target);
                    auto result = host_->handleCommand({{"protocol_version", 0},
                                                        {"message_id", command.value("id")},
                                                        {"message_type", kind},
                                                        {"session_id", sessionId},
                                                        {"session_generation", generation},
                                                        {"sent_at", protocol::timestamp()},
                                                        {"payload", payload}},
                                                       "cloud-control-v2");
                    result.remove("worktree_path");
                    result.insert("repository_id", command.value("repositoryId"));
                    result.insert("worktree_id", command.value("worktreeId"));
                    acknowledgeCommand(command, result);
                    if (result.value("ok").toBool())
                        sync();
                    return;
                }
                // Cloud clients (mobile, web console) may only run explicit session operations.
                // Journal reads return unprojected payloads that can include command output, and
                // cursor acks move local retention, so those stay local-only. Transcripts reach
                // the cloud through transcript.restore_window, whose pages executeRestoreWindow
                // passes through projectTranscriptWindowForCloud (large items included).
                static const QSet<QString> remoteOperations{
                    QStringLiteral("session.configuration.read"),
                    QStringLiteral("session.start_turn"), QStringLiteral("session.rename"),
                    QStringLiteral("session.fork"), QStringLiteral("session.steer"),
                    QStringLiteral("session.interrupt"),
                    // One background task: stop it, or read its output tail.
                    QStringLiteral("session.task.stop"), QStringLiteral("session.task.output"),
                    QStringLiteral("session.pending_approvals"),
                    // Bounded, on-demand reads: one command's output tail or one image preview.
                    QStringLiteral("session.output.read"), QStringLiteral("session.media.read"),
                    QStringLiteral("session.execution_policy.apply"),
                    QStringLiteral("session.mode.apply"), QStringLiteral("approval.resolve"),
                    QStringLiteral("question.answer"), QStringLiteral("attachment.stage.begin"),
                    QStringLiteral("attachment.stage.chunk"),
                    QStringLiteral("attachment.stage.commit"),
                    QStringLiteral("attachment.stage.cancel")};
                if (!remoteOperations.contains(kind)) {
                    acknowledgeCommand(
                        command,
                        {{QStringLiteral("ok"), false},
                         {QStringLiteral("error_code"), QStringLiteral("unknown_operation")},
                         {QStringLiteral("error_message"),
                          QStringLiteral("The requested operation is not available "
                                         "remotely.")}});
                    return;
                }
                const QJsonObject local{
                    {QStringLiteral("protocol_version"), 0},
                    {QStringLiteral("message_id"), command.value(QStringLiteral("id"))},
                    {QStringLiteral("message_type"), kind},
                    {QStringLiteral("session_id"), sessionId},
                    {QStringLiteral("session_generation"), generation},
                    {QStringLiteral("sent_at"), protocol::timestamp()},
                    {QStringLiteral("payload"), decoded.object()}};
                // A steer is answered once the provider has taken or refused it: the phone
                // drops its draft on "completed".
                const QPointer<CloudControlClient> guard(this);
                if (kind == QStringLiteral("session.fork")) {
                    // Provider history is copied asynchronously; keep the source turn alive.
                    host_->forkSession(local, [guard, command](const QJsonObject& result) {
                        if (guard) {
                            guard->acknowledgeCommand(command, result);
                            if (result.value(QStringLiteral("ok")).toBool())
                                guard->sync();
                        }
                    });
                    return;
                }
                host_->handleCommandDeferred(local, QStringLiteral("cloud-control-v2"),
                                             [guard, command](const QJsonObject& result) {
                                                 if (guard)
                                                     guard->acknowledgeCommand(command, result);
                                             });
                return;
            }
            acknowledgeCommand(command, localResult);
        });
}

void CloudControlClient::notePublishedRevisions(const QList<QJsonArray>& batches) {
    constexpr qint64 keptMilliseconds = 10 * 60 * 1'000;
    constexpr qsizetype keptPerSession = 8;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSet<QString> current;
    for (const QJsonArray& batch : batches) {
        for (const QJsonValue& value : batch) {
            const QJsonObject snapshot = value.toObject();
            const QString sessionId = snapshot.value(QStringLiteral("id")).toString();
            const QString revision = snapshot.value(QStringLiteral("workspaceRevision")).toString();
            current.insert(sessionId);
            if (revision.isEmpty())
                continue;
            auto& published = publishedRevisions_[sessionId];
            published.removeIf([&](const auto& entry) {
                return entry.first == revision || now - entry.second > keptMilliseconds;
            });
            published.append({revision, now});
            while (published.size() > keptPerSession)
                published.removeFirst();
        }
    }
    for (auto it = publishedRevisions_.begin(); it != publishedRevisions_.end();) {
        if (current.contains(it.key()))
            ++it;
        else
            it = publishedRevisions_.erase(it);
    }
}

bool CloudControlClient::revisionPublished(const QString& sessionId,
                                           const QString& revision) const {
    constexpr qint64 keptMilliseconds = 10 * 60 * 1'000;
    if (revision.isEmpty())
        return false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto published = publishedRevisions_.constFind(sessionId);
    return published != publishedRevisions_.cend() &&
           std::any_of(published->cbegin(), published->cend(), [&](const auto& entry) {
               return entry.first == revision && now - entry.second <= keptMilliseconds;
           });
}

void CloudControlClient::acknowledgeCommand(const QJsonObject& command,
                                            const QJsonObject& localResult,
                                            const QByteArray& recipientAgreementKey) {
    const bool accepted = localResult.value(QStringLiteral("ok")).toBool();
    const QString status = accepted ? QStringLiteral("completed") : QStringLiteral("rejected");
    const QString resultCode = accepted ? QStringLiteral("ok")
                                        : localResult.value(QStringLiteral("error_code"))
                                              .toString(QStringLiteral("command_rejected"));
    const QString sessionId = command.value(QStringLiteral("sessionId")).toString();
    const qint64 generation = command.value(QStringLiteral("sessionGeneration")).toInteger();
    const int attempt = command.value(QStringLiteral("attempt")).toInt();
    const bool creating = isWorktreeCommand(command);
    const QString commandId = command.value(QStringLiteral("id")).toString();
    const qint64 startedAt = commandStartedAt_.take(commandId);
    qInfo("command %s %s %s/%s after %lldms", qUtf8Printable(commandId),
          qUtf8Printable(command.value(QStringLiteral("kind")).toString()), qUtf8Printable(status),
          qUtf8Printable(resultCode),
          startedAt > 0 ? QDateTime::currentMSecsSinceEpoch() - startedAt : -1LL);
    const QJsonObject shell{{QStringLiteral("keyId"),
                             creating ? QStringLiteral("session-create:%1")
                                            .arg(command.value(QStringLiteral("id")).toString())
                                      : QStringLiteral("%1:%2:1").arg(sessionId).arg(generation)},
                            {QStringLiteral("keyEpoch"), 1},
                            {QStringLiteral("senderDeviceId"), machineId_},
                            {QStringLiteral("senderKeyVersion"), machineKeyVersion_}};
    const QByteArray aad = commandResultAssociatedData(command, attempt, status, resultCode, shell);
    QString cryptoError;
    const QJsonObject encrypted =
        creating ? crypto_.encryptSealed(QJsonDocument(localResult).toJson(QJsonDocument::Compact),
                                         aad, recipientAgreementKey,
                                         command.value(QStringLiteral("id")).toString(),
                                         command.value(QStringLiteral("content"))
                                             .toObject()
                                             .value(QStringLiteral("senderDeviceId"))
                                             .toString(),
                                         command.value(QStringLiteral("content"))
                                             .toObject()
                                             .value(QStringLiteral("senderKeyVersion"))
                                             .toInt(),
                                         machineId_, machineKeyVersion_, &cryptoError)
                 : crypto_.encrypt(QJsonDocument(localResult).toJson(QJsonDocument::Compact), aad,
                                   sessionId, generation, 1, machineId_, machineKeyVersion_,
                                   &cryptoError);
    if (encrypted.isEmpty()) {
        lastError_ = cryptoError;
        return;
    }
    const QJsonObject body{
        {QStringLiteral("attempt"), attempt},
        {QStringLiteral("claimToken"), command.value(QStringLiteral("claimToken"))},
        {QStringLiteral("status"), status},
        {QStringLiteral("resultCode"), resultCode},
        {QStringLiteral("encryptedResult"), encrypted},
        {QStringLiteral("completedAt"), protocol::timestamp()}};
    const auto peerId = command.value("_peerConnection").toString();
    if (!peerId.isEmpty()) {
        if (peers_->authorized(peerId, command)) {
            auto result = body;
            result.remove("claimToken");
            result.insert("id", commandId);
            peers_->send(peerId, {{"type", "command.result"}, {"result", result}});
        }
        peers_->completed(peerId, commandId);
        return;
    }
    const QString path = QStringLiteral("/v2/commands/%1/ack")
                             .arg(QString::fromUtf8(QUrl::toPercentEncoding(
                                 command.value(QStringLiteral("id")).toString())));
    request("POST", path, body, daemonToken_, {},
            [this, commandId](const int statusCode, const QJsonValue& value,
                              const QString& transportError) {
                if (statusCode != 200 || !transportError.isEmpty()) {
                    lastError_ =
                        transportError.isEmpty()
                            ? responseCode(value, QStringLiteral("command_acknowledgement_failed"))
                            : transportError;
                    qWarning("command %s acknowledgement failed: %d %s", qUtf8Printable(commandId),
                             statusCode, qUtf8Printable(lastError_));
                } else {
                    uploadEvents();
                    // The relay runs one command per session at a time, so the next one
                    // (an upload's following chunk, a turn start, a phone's read) is leasable
                    // now. Waiting for the timer added a full tick to every sequential command.
                    wakeCommandPoll();
                }
            });
}

} // namespace ketplus::daemon
