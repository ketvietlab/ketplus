#include "atlas/AtlasLiveRuntime.h"
#include "cloud/CloudControlBatching.h"
#include "cloud/CloudControlClient.h"
#include "cloud/CloudControlSupport.h"
#include "cloud/CloudEventProjection.h"
#include "cloud/ControlEndpoint.h"
#include "cloud/WorktreeBranches.h"
#include "cloud/WorktreeRepositories.h"
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

// Publishing to the relay: heartbeats with session snapshots, journal event uploads and
// distributing session keys to paired devices.

namespace ketplus::daemon {
using cloud_control::object;
using cloud_control::responseCode;

void CloudControlClient::sync() {
    if (machineId_.isEmpty())
        return;
    heartbeat();
}

void CloudControlClient::heartbeat() {
    if (heartbeatInFlight_)
        return;
    heartbeatInFlight_ = true;
    ensureDaemonCredential([this](const bool ready, const QString& error) {
        if (!ready) {
            heartbeatInFlight_ = false;
            lastError_ = error;
            return;
        }
        QString saltError;
        const QByteArray salt = ensurePathIdSalt(&saltError);
        if (salt.isEmpty()) {
            heartbeatInFlight_ = false;
            lastError_ = saltError;
            return;
        }
        const QList<StoredSession> stored = journal_->sessions();
        const QHash<QString, QString> repositories = resolveWorktreeRepositories(*journal_);
        QHash<QString, QJsonObject> agentMetadata;
        // Only the sealed metadata carries this lifecycle hint. Explicit false clears a
        // previous true on the next heartbeat; no message body or recipient is exported.
        const QSet<QString> awaiting = journal_->sessionsAwaitingAgentReply();
        for (const StoredSession& session : stored)
            agentMetadata[session.id].insert(QStringLiteral("awaitingAgentReply"),
                                             awaiting.contains(session.id));
        const QHash<QString, QString> numbers = journal_->conversationNumbers();
        for (auto it = numbers.cbegin(); it != numbers.cend(); ++it)
            agentMetadata[it.key()].insert(QStringLiteral("number"), it.value());
        // #429: the phone shows each conversation's tags in the catalog's colours.
        QHash<QString, QString> tagColors;
        for (const auto& value : journal_->tagCatalog()) {
            const QJsonObject tag = value.toObject();
            tagColors.insert(tag.value(QStringLiteral("label")).toString().toLower(),
                             tag.value(QStringLiteral("color")).toString());
        }
        const QHash<QString, QJsonObject> senders =
            host_ ? host_->agentTurnSenders() : QHash<QString, QJsonObject>{};
        for (auto it = senders.cbegin(); it != senders.cend(); ++it)
            agentMetadata[it.key()].insert(QStringLiteral("agentTurn"), it.value());
        // ADR 0116 §3: a member's team, its revision and the member's role; a conversation
        // on no team has no `team` field.
        const QHash<QString, QJsonObject> teams =
            host_ ? host_->teamMetadata() : QHash<QString, QJsonObject>{};
        for (auto it = teams.cbegin(); it != teams.cend(); ++it)
            agentMetadata[it.key()].insert(QStringLiteral("team"), it.value());
        const quint64 requestEpoch = accountEpoch_;
        const QString workspaceId = workspaceId_;
        const QString machineId = machineId_;
        const QPointer<CloudControlClient> guard(this);
        // Only one heartbeat runs at a time (heartbeatInFlight_), so the cache is handed to its
        // thread and back without sharing.
        WorktreeRevisionCache revisionCache = std::move(revisionCache_);
        revisionCache_ = {};
        heartbeatPool_.start([guard, stored, repositories, agentMetadata, tagColors, requestEpoch,
                              workspaceId, machineId, salt,
                              revisionCache = std::move(revisionCache)]() mutable {
            QHash<QString, QJsonObject> metadata;
            revisionCache.retain(stored);
            const QList<QJsonArray> batches =
                heartbeatSessionBatches(CloudControlClient::sessionSnapshots(
                    stored, workspaceId, machineId, salt, repositories, &metadata, agentMetadata,
                    tagColors, &revisionCache, QDateTime::currentMSecsSinceEpoch()));
            QMetaObject::invokeMethod(
                guard,
                [guard, batches, metadata, requestEpoch, workspaceId, machineId,
                 revisionCache = std::move(revisionCache)]() mutable {
                    if (!guard)
                        return;
                    guard->revisionCache_ = std::move(revisionCache);
                    if (guard->accountEpoch_ != requestEpoch ||
                        guard->workspaceId_ != workspaceId || guard->machineId_ != machineId) {
                        guard->heartbeatInFlight_ = false;
                        return;
                    }
                    guard->refusalsThisHeartbeat_.clear();
                    guard->notePublishedRevisions(batches);
                    guard->sendHeartbeatBatches(guard->withSessionMetadata(batches, metadata), 0,
                                                requestEpoch, machineId, workspaceId);
                },
                Qt::QueuedConnection);
        });
    });
}

void CloudControlClient::sendHeartbeatBatches(const QList<QJsonArray>& batches,
                                              const qsizetype index, const quint64 requestEpoch,
                                              const QString& machineId,
                                              const QString& workspaceId) {
    if (accountEpoch_ != requestEpoch || machineId_ != machineId || workspaceId_ != workspaceId) {
        heartbeatInFlight_ = false;
        return;
    }
    if (index >= batches.size()) {
        heartbeatInFlight_ = false;
        lastError_.clear();
        refusedSessions_ = refusalsThisHeartbeat_;
        // The relay answers again: uploads that only failed to reach it need not wait.
        eventUploadBackoff_.removeIf([](const QHash<QString, EventUploadBackoff>::iterator entry) {
            return !entry->refused;
        });
        uploadEvents();
        distributeKeys();
        return;
    }
    QJsonObject body{
        {QStringLiteral("machineId"), machineId},
        {QStringLiteral("controlProtocolVersion"), 2},
        // ADR 0116 §1: this daemon serves team commands and writes the `team` metadata.
        {QStringLiteral("capabilities"), QJsonArray{"transport.webrtc/1", "teams.remote/1"}},
        {QStringLiteral("localProtocolVersion"), 0},
        {QStringLiteral("daemonVersion"), QStringLiteral(KETPLUS_PRODUCT_VERSION)},
        {QStringLiteral("observedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("sessions"), batches.at(index)},
        {QStringLiteral("worktrees"), worktreeSnapshots()}};
    QJsonArray deletions;
    for (const auto& value : journal_->deletionWork(64, true)) {
        const auto work = value.toObject();
        if (!work.value("cloud_acked").toBool())
            deletions.append(work.value("deletion"));
    }
    if (!deletions.isEmpty())
        body.insert("deletions", deletions);
    request("POST", QStringLiteral("/v2/daemon/heartbeat"), body, daemonToken_,
            {{QByteArray("X-KetPlus-Daemon-Version"), QByteArray(KETPLUS_PRODUCT_VERSION)}},
            [this, batches, index, requestEpoch, machineId, workspaceId](
                const int status, const QJsonValue& value, const QString& transportError) {
                if (accountEpoch_ != requestEpoch || machineId_ != machineId ||
                    workspaceId_ != workspaceId) {
                    heartbeatInFlight_ = false;
                    return;
                }
                if (status != 202 || !transportError.isEmpty()) {
                    heartbeatInFlight_ = false;
                    lastError_ = transportError.isEmpty()
                                     ? responseCode(value, QStringLiteral("heartbeat_failed"))
                                     : transportError;
                    if (status == 401) {
                        daemonToken_.clear();
                    }
                    // A relay that cannot read the sealed metadata refuses the whole heartbeat;
                    // later heartbeats go without it rather than leave the Mac offline.
                    if (lastError_ == QStringLiteral("invalid_session_metadata"))
                        sessionMetadataRefused_ = true;
                    return;
                }
                negotiatePeerTransport(value);
                refusalsThisHeartbeat_.unite(heartbeatRefusedSessions(value));
                for (const auto& entry : value.toObject().value("acceptedDeletions").toArray()) {
                    const auto deletion = entry.toObject();
                    (void)journal_->acknowledgeCloudDeletion(
                        deletion.value("sessionId").toString(),
                        deletion.value("sessionGeneration").toInteger(),
                        deletion.value("operationId").toString());
                }
                sendHeartbeatBatches(batches, index + 1, requestEpoch, machineId, workspaceId);
            });
}

QByteArray CloudControlClient::eventAssociatedData(const QJsonObject& event,
                                                   const QJsonObject& content) const {
    return CloudCrypto::canonical(
        {QStringLiteral("ketplus"), 2, QStringLiteral("event"), workspaceId_,
         event.value(QStringLiteral("session_id")),
         event.value(QStringLiteral("session_generation")), event.value(QStringLiteral("cursor")),
         event.value(QStringLiteral("turn_sequence")), event.value(QStringLiteral("event_type")),
         content.value(QStringLiteral("keyId")), content.value(QStringLiteral("keyEpoch")),
         content.value(QStringLiteral("senderDeviceId")),
         content.value(QStringLiteral("senderKeyVersion"))});
}

bool CloudControlClient::isLiveGeneration(const QString& sessionId, const qint64 generation) const {
    return generation > 0 && journal_->sessionHead(sessionId).generation == generation;
}

void CloudControlClient::uploadSessionEvents(const QString& sessionId, const qint64 generation,
                                             qint64 afterCursor) {
    if (!isLiveGeneration(sessionId, generation))
        return;
    const QString scope = uploadScope();
    const qint64 durable = journal_->cloudUploadCursor(scope, sessionId, generation);
    if (durable < 0) {
        lastError_ = journal_->errorString();
        return;
    }
    afterCursor = qMax(afterCursor, durable);
    const QString uploadKey = QStringLiteral("%1:%2").arg(sessionId).arg(generation);
    if (eventUploadsInFlight_.contains(uploadKey))
        return;
    if (const auto backoff = eventUploadBackoff_.constFind(uploadKey);
        backoff != eventUploadBackoff_.cend() && !backoff->retryAt.hasExpired())
        return;
    const int countLimit = eventUploadLimits_.value(uploadKey, maximumEventUploadCount);
    const QJsonArray local = journal_->eventsAfter(sessionId, generation, afterCursor, countLimit);
    if (local.isEmpty())
        return;
    QJsonArray cloud;
    qsizetype cloudBytes = 0;
    // A batch carries as many events as fit the relay's request limit; the rest wait for the
    // next request. Counting alone sent 100 large chunks at once, which the relay refused on
    // every retry.
    // Plaintext item metadata for relay compaction, outside the signed content; an envelope
    // cached before it existed gains it when sent.
    const auto withItemMetadata = [&sessionId, generation](QJsonObject cloudEvent,
                                                           const QJsonObject& event) {
        const QJsonObject metadata = cloudItemMetadata(
            sessionId, generation, event.value(QStringLiteral("event_type")).toString(),
            event.value(QStringLiteral("payload")).toObject());
        for (auto field = metadata.begin(); field != metadata.end(); ++field)
            cloudEvent.insert(field.key(), field.value());
        return cloudEvent;
    };
    const auto join = [&cloud, &cloudBytes, countLimit,
                       &withItemMetadata](const QJsonObject& cached, const QJsonObject& event) {
        const QJsonObject cloudEvent = withItemMetadata(cached, event);
        const qsizetype bytes = QJsonDocument(cloudEvent).toJson(QJsonDocument::Compact).size();
        if (!eventJoinsUpload(cloudBytes, int(cloud.size()), bytes, countLimit))
            return false;
        cloud.append(cloudEvent);
        cloudBytes += bytes + 1;
        return true;
    };
    QString cryptoError;
    for (const QJsonValue& value : local) {
        const QJsonObject event = value.toObject();
        const qint64 cursor = event.value(QStringLiteral("cursor")).toInteger();
        QJsonObject cloudEvent = journal_->cloudEvent(sessionId, generation, cursor);
        if (!cloudEvent.isEmpty() && cloudEvent.value(QStringLiteral("turnSequence")) !=
                                         event.value(QStringLiteral("turn_sequence"))) {
            if (!journal_->discardCloudEvent(sessionId, generation, cursor)) {
                lastError_ = journal_->errorString();
                return;
            }
            cloudEvent = {};
        }
        if (!cloudEvent.isEmpty()) {
            const QJsonObject content = cloudEvent.value(QStringLiteral("content")).toObject();
            const QString expectedKeyId = QStringLiteral("%1:%2:1").arg(sessionId).arg(generation);
            const bool metadataMatches =
                journal_->cloudEventProjectionPolicyVersion(sessionId, generation, cursor) ==
                    LocalJournal::currentCloudProjectionPolicyVersion &&
                cloudEvent.value(QStringLiteral("sessionId")).toString() == sessionId &&
                cloudEvent.value(QStringLiteral("sessionGeneration")).toInteger(-1) == generation &&
                cloudEvent.value(QStringLiteral("cursor")).toInteger(-1) == cursor &&
                cloudEvent.value(QStringLiteral("turnSequence")) ==
                    event.value(QStringLiteral("turn_sequence")) &&
                cloudEvent.value(QStringLiteral("eventType")) ==
                    event.value(QStringLiteral("event_type")) &&
                content.value(QStringLiteral("keyId")).toString() == expectedKeyId &&
                content.value(QStringLiteral("keyEpoch")).toInt(-1) == 1 &&
                content.value(QStringLiteral("senderDeviceId")).toString() == machineId_ &&
                content.value(QStringLiteral("senderKeyVersion")).toInt(-1) == machineKeyVersion_;
            QString validationError;
            const QJsonObject publicKeys = crypto_.publicKeys(&validationError);
            const QByteArray signingPublic = CloudCrypto::fromBase64Url(
                publicKeys.value(QStringLiteral("signingPublicKey")).toString());
            const QByteArray plaintext =
                metadataMatches && signingPublic.size() == 32
                    ? crypto_.decrypt(content, eventAssociatedData(event, content), signingPublic,
                                      sessionId, generation, &validationError)
                    : QByteArray{};
            const QByteArray expectedPlaintext =
                QJsonDocument(projectEventPayloadForCloud(
                                  event.value(QStringLiteral("event_type")).toString(),
                                  event.value(QStringLiteral("payload")).toObject()))
                    .toJson(QJsonDocument::Compact);
            if (!metadataMatches || plaintext.isEmpty() || plaintext != expectedPlaintext) {
                // Fail this upload closed, but make the condition recoverable.
                // The local journal remains authoritative, so the next retry
                // will recreate this envelope using the current projection policy
                // and current key scope.
                if (!journal_->discardCloudEvent(sessionId, generation, cursor)) {
                    lastError_ = journal_->errorString();
                    return;
                }
                lastError_ = QStringLiteral("cloud_event_cache_requires_reconciliation");
                return;
            }
            if (!join(cloudEvent, event))
                break;
            continue;
        }
        cloudEvent = sealEvent(event, sessionId, generation, &cryptoError);
        if (cloudEvent.isEmpty()) {
            lastError_ = cryptoError;
            return;
        }
        if (!journal_->storeCloudEvent(sessionId, generation, cursor, cloudEvent)) {
            lastError_ = journal_->errorString();
            return;
        }
        if (!join(cloudEvent, event))
            break;
    }
    qint64 highestSent = 0;
    for (const QJsonValue& sent : cloud)
        highestSent =
            qMax(highestSent, sent.toObject().value(QStringLiteral("cursor")).toInteger());
    if (!journal_->prepareCloudEventUpload(cloud)) {
        lastError_ = journal_->errorString();
        return;
    }
    const quint64 requestEpoch = accountEpoch_;
    eventUploadsInFlight_.insert(uploadKey);
    request(
        "POST", QStringLiteral("/v2/events:append"),
        QJsonObject{{QStringLiteral("machineId"), machineId_}, {QStringLiteral("events"), cloud}},
        daemonToken_, {},
        [this, sessionId, generation, afterCursor, uploadKey, highestSent, cloud, scope,
         requestEpoch, sent = int(cloud.size())](const int status, const QJsonValue& value,
                                                 const QString& transportError) {
            eventUploadsInFlight_.remove(uploadKey);
            // This request owns the slot until its callback; stale account replies
            // release that slot but must not mutate progress, receipts or backoff.
            if (requestEpoch != accountEpoch_ || scope != uploadScope())
                return;
            // Deleted while in flight: no retry, receipt or progress for its history.
            if (!isLiveGeneration(sessionId, generation)) {
                eventUploadLimits_.remove(uploadKey);
                eventUploadBackoff_.remove(uploadKey);
                return;
            }
            // The batch was too large for the relay, or for a proxy or tunnel in front of it that
            // answers in HTML: send half as many events rather than the same refused batch again.
            if (status == 413 && sent > 1) {
                eventUploadLimits_.insert(uploadKey, sent / 2);
                uploadSessionEvents(sessionId, generation, afterCursor);
                return;
            }
            if (status != 202 || !transportError.isEmpty()) {
                lastError_ = status == 413
                                 ? responseCode(value, QStringLiteral("request_too_large"))
                             : transportError.isEmpty()
                                 ? responseCode(value, QStringLiteral("event_upload_failed"))
                                 : transportError;
                if (status == 401) {
                    // Uploads resume with the next credential, not after a wait.
                    daemonToken_.clear();
                    return;
                }
                EventUploadBackoff& backoff = eventUploadBackoff_[uploadKey];
                backoff.failures += 1;
                backoff.refused = eventUploadRefused(status);
                backoff.gap =
                    status == 409 && responseCode(value, {}) == QStringLiteral("event_cursor_gap");
                const qint64 delay =
                    eventUploadBackoffMilliseconds(backoff.failures, backoff.refused, backoff.gap);
                backoff.retryAt.setRemainingTime(delay);
                qWarning("event upload for session %s failed: %d %s; next attempt in %llds",
                         qUtf8Printable(sessionId), status, qUtf8Printable(lastError_),
                         delay / 1000);
                return;
            }
            eventUploadBackoff_.remove(uploadKey);
            qint64 accepted = 0;
            for (const QJsonValue& item :
                 object(value).value(QStringLiteral("acceptedThrough")).toArray()) {
                const QJsonObject cursor = item.toObject();
                if (cursor.value(QStringLiteral("sessionId")).toString() == sessionId &&
                    cursor.value(QStringLiteral("sessionGeneration")).toInteger() == generation) {
                    accepted = qMax(accepted, cursor.value(QStringLiteral("cursor")).toInteger());
                }
            }
            // The relay can only accept what this request carried. Never acknowledge or delete
            // local cloud events beyond the highest cursor actually sent.
            if (accepted > highestSent) {
                lastError_ = QStringLiteral("cloud_event_ack_beyond_upload");
                accepted = highestSent;
            }
            if (const int limit = eventUploadLimits_.value(uploadKey, maximumEventUploadCount);
                limit < maximumEventUploadCount) {
                if (limit * 2 >= maximumEventUploadCount)
                    eventUploadLimits_.remove(uploadKey);
                else
                    eventUploadLimits_.insert(uploadKey, limit * 2);
            }
            if (accepted > 0) {
                knownUploadedCursors_.insert(uploadKey, accepted);
                QString storageError;
                if (!journal_->acknowledgeCloudEventUpload(scope, sessionId, generation, cloud,
                                                           accepted) ||
                    !storeString(
                        QStringLiteral("uploaded-cursor-%1-%2").arg(sessionId).arg(generation),
                        QString::number(accepted), &storageError) ||
                    !journal_->acknowledgeCursor(QStringLiteral("cloud-control-v2"), sessionId,
                                                 generation, accepted) ||
                    !journal_->pruneAcknowledgedCloudEvents()) {
                    registeredCloudCursors_.remove(uploadKey);
                    knownUploadedCursors_.remove(uploadKey);
                    lastError_ = storageError.isEmpty() ? journal_->errorString() : storageError;
                    return;
                }
                uploadSessionEvents(sessionId, generation, accepted);
            }
        });
}

QJsonArray CloudControlClient::trustedDevices(const QJsonArray& devices) {
    QString error;
    if (!pins_.adopt(devices, &error))
        lastError_ = error;
    return pins_.trusted(devices);
}

void CloudControlClient::distributeKeys() {
    if (machineId_.isEmpty() || daemonToken_.isEmpty())
        return;
    request("GET", QStringLiteral("/v2/devices"), {}, daemonToken_, {},
            [this](const int status, const QJsonValue& value, const QString& transportError) {
                if (status != 200 || !value.isArray() || !transportError.isEmpty()) {
                    if (!transportError.isEmpty())
                        lastError_ = transportError;
                    return;
                }
                // Only devices pinned on this Mac receive session keys (#87).
                const QJsonArray devices = trustedDevices(value.toArray());
                signaling_->refreshDevices(devices);
                QJsonArray envelopes;
                QString cryptoError;
                for (const QJsonValue& deviceValue : devices) {
                    const QJsonObject device = deviceValue.toObject();
                    for (const StoredSession& session : journal_->sessions()) {
                        if (refusedSessions_.contains(session.id))
                            continue;
                        const QString account =
                            QStringLiteral("wrapped-%1-%2-%3-%4")
                                .arg(session.id)
                                .arg(session.generation)
                                .arg(device.value(QStringLiteral("id")).toString())
                                .arg(device.value(QStringLiteral("keyVersion")).toInt());
                        QJsonObject envelope =
                            QJsonDocument::fromJson(credentials_.read(account)).object();
                        // A cached malformed wrap is discarded and re-created, never re-sent.
                        if (!CloudCrypto::validWrappedSessionKey(envelope)) {
                            envelope = crypto_.wrapSessionKey(session.id, session.generation, 1,
                                                              device, machineId_,
                                                              machineKeyVersion_, &cryptoError);
                            if (envelope.isEmpty() ||
                                !credentials_.write(
                                    account, QJsonDocument(envelope).toJson(QJsonDocument::Compact),
                                    &cryptoError)) {
                                lastError_ = cryptoError;
                                return;
                            }
                        }
                        envelopes.append(envelope);
                        if (envelopes.size() == 500)
                            break;
                    }
                    if (envelopes.size() == 500)
                        break;
                }
                if (envelopes.isEmpty())
                    return;
                request("POST", QStringLiteral("/v2/session-keys:append"),
                        QJsonObject{{QStringLiteral("envelopes"), envelopes}}, daemonToken_, {},
                        [this](const int appendStatus, const QJsonValue& appendValue,
                               const QString& appendError) {
                            if (appendStatus != 202 || !appendError.isEmpty()) {
                                lastError_ =
                                    appendError.isEmpty()
                                        ? responseCode(appendValue,
                                                       QStringLiteral("session_key_upload_failed"))
                                        : appendError;
                            }
                        });
            });
}

} // namespace ketplus::daemon
