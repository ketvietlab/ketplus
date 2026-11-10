#include "cloud/CloudControlBatching.h"
#include "cloud/CloudControlClient.h"
#include "cloud/CloudEventProjection.h"
#include "storage/LocalJournal.h"
#include <QJsonArray>
#include <QJsonDocument>

namespace ketplus::daemon {
void CloudControlClient::initializePeerTransport() {
    peers_ = new transport::RemotePeers(
        [this](const QString& session, qint64 generation, qint64 cursor) {
            return peerEvents(session, generation, cursor);
        },
        this);
    signaling_ = new transport::Signaling(
        peers_,
        [this](const QString& device, int version, auto callback) {
            const auto epoch = accountEpoch_;
            // The active device list is the pairing trust boundary; never admit a peer from
            // a historical device-key endpoint or from a key embedded in an untrusted offer.
            request("GET", "/v2/devices", {}, daemonToken_, {},
                    [this, epoch, device, version, callback](int status, const QJsonValue& value,
                                                             const QString& error) {
                        if (epoch != accountEpoch_ || status != 200 || !error.isEmpty() ||
                            !value.isArray()) {
                            callback({});
                            return;
                        }
                        const auto devices = trustedDevices(value.toArray());
                        signaling_->refreshDevices(devices);
                        for (const auto& row : devices) {
                            const auto keys = row.toObject();
                            if (keys.value("id") == device &&
                                keys.value("keyVersion").toInt() == version &&
                                (!keys.contains("status") || keys.value("status") == "active")) {
                                callback(keys);
                                return;
                            }
                        }
                        callback({});
                    });
        },
        [this](const QByteArray& bytes) { return crypto_.sign(bytes); }, this);
    connect(&wakeups_, &RelayWakeups::authorizationRevoked, this, [this] {
        signaling_->configure({}, {}, 1, false);
        daemonToken_.clear();
        // A revoked machine keeps no Atlas tunnel open for any device.
        revokeAtlasAccess();
    });
    // Revoking a device here or on another client closes the Atlas it was viewing.
    connect(signaling_, &transport::Signaling::deviceRevoked, this,
            [this](const QString& device) { revokeAtlasAccess(device); });
    connect(&wakeups_, &RelayWakeups::message, signaling_, &transport::Signaling::receive);
    connect(signaling_, &transport::Signaling::outgoing, this, [this](const QJsonObject& message) {
        if (!wakeups_.send(message) && message.value("type") != "signal.end")
            signaling_->receive(
                {{"type", "signal.end"}, {"connectionId", message.value("connectionId")}});
    });
    connect(peers_, &transport::RemotePeers::command, this,
            [this](const QString& id, QJsonObject command) {
                const auto kind = command.value("kind").toString();
                if (kind != "session.create" && kind != "worktree.branches.read") {
                    const auto sessionId = command.value("sessionId").toString();
                    const auto head = journal_->sessionHead(sessionId);
                    const bool found =
                        head.generation > 0 &&
                        head.generation == command.value("sessionGeneration").toInteger();
                    if (!found || refusedSessions_.contains(sessionId)) {
                        peers_->close(id, "session_forbidden");
                        return;
                    }
                }
                command.insert("_peerConnection", id);
                executeCommand(command);
            });
}

void CloudControlClient::negotiatePeerTransport(const QJsonValue& heartbeat) {
    const auto capabilities =
        heartbeat.toObject().value("compatibility").toObject().value("capabilities").toArray();
    signaling_->configure(workspaceId_, machineId_, machineKeyVersion_,
                          capabilities.contains("transport.webrtc/1"));
}

QJsonObject CloudControlClient::sealEvent(const QJsonObject& event, const QString& sessionId,
                                          qint64 generation, QString* error) {
    const QJsonObject shell{{"keyId", QStringLiteral("%1:%2:1").arg(sessionId).arg(generation)},
                            {"keyEpoch", 1},
                            {"senderDeviceId", machineId_},
                            {"senderKeyVersion", machineKeyVersion_}};
    const auto plaintext =
        QJsonDocument(projectEventPayloadForCloud(event.value("event_type").toString(),
                                                  event.value("payload").toObject()))
            .toJson(QJsonDocument::Compact);
    const auto content = crypto_.encrypt(plaintext, eventAssociatedData(event, shell), sessionId,
                                         generation, 1, machineId_, machineKeyVersion_, error);
    if (content.isEmpty())
        return {};
    QJsonObject envelope{{"sessionId", sessionId},
                         {"sessionGeneration", generation},
                         {"cursor", event.value("cursor")},
                         {"turnSequence", event.value("turn_sequence")},
                         {"eventType", event.value("event_type")},
                         {"occurredAt", event.value("sent_at")},
                         {"content", content}};
    const auto metadata =
        cloudItemMetadata(sessionId, generation, event.value("event_type").toString(),
                          event.value("payload").toObject());
    for (auto it = metadata.begin(); it != metadata.end(); ++it)
        envelope.insert(it.key(), it.value());
    return envelope;
}

transport::EventPage CloudControlClient::peerEvents(const QString& sessionId, qint64 generation,
                                                    qint64 cursor) {
    transport::EventPage page;
    page.nextCursor = cursor;
    page.generation = generation;
    const auto selected = journal_->sessionHead(sessionId);
    if (selected.generation == 0 || selected.generation != generation ||
        refusedSessions_.contains(sessionId) || cursor > selected.cursor ||
        selected.cursor - cursor > 2000) {
        page.reset = true;
        page.generation = selected.generation == 0 ? generation : selected.generation;
        return page;
    }
    const auto local =
        journal_->compactedEventsAfter(sessionId, generation, cursor, maximumEventUploadCount);
    qsizetype bytes = 0;
    bool full = false;
    const auto add = [&](const QJsonObject& value) {
        QString error;
        const auto event = sealEvent(value, sessionId, generation, &error);
        if (event.isEmpty()) {
            page.reset = true;
            return false;
        }
        const auto size = QJsonDocument(event).toJson(QJsonDocument::Compact).size();
        if (!page.events.isEmpty() && bytes + size > maximumEventUploadBytes) {
            full = true;
            return false;
        }
        bytes += size;
        page.events.append(event);
        page.nextCursor = event.value("cursor").toInteger();
        return true;
    };
    // The compacted projection keeps a large item's manifest but not its chunks, which sit at
    // the cursors just before it. A reader cannot materialize the manifest without them: it
    // dropped the peer on every large item. They are read back from the journal and sent first;
    // one item may span pages, which end after a chunk and never before the manifest's chunks.
    const auto addChunksBefore = [&](const QJsonObject& value) {
        const auto manifest = value.value("payload").toObject().value("large_event").toObject();
        const qint64 at = value.value("cursor").toInteger();
        const qint64 count = manifest.value("chunk_count").toInteger();
        if (manifest.isEmpty() || count <= 0 || manifest.value("revision").toInteger() != at)
            return true;
        qint64 after = std::max(page.nextCursor, at - count - 1);
        while (after < at - 1) {
            const auto chunks =
                journal_->eventsAfter(sessionId, generation, after, int(at - 1 - after));
            qint64 reached = after;
            for (const auto& chunk : chunks) {
                const auto object = chunk.toObject();
                const qint64 position = object.value("cursor").toInteger();
                if (position >= at || object.value("event_type") != "transcript.large_chunk")
                    break;
                if (!add(object))
                    return false;
                reached = position;
            }
            if (reached == after)
                break; // The journal no longer holds them; the manifest goes as before.
            after = reached;
        }
        return true;
    };
    qsizetype consumed = 0;
    for (const auto& value : local.events) {
        if (!addChunksBefore(value.toObject()) || !add(value.toObject()))
            break;
        ++consumed;
    }
    if (page.reset)
        return page;
    if (!full && consumed == local.events.size())
        page.nextCursor = local.nextCursor;
    page.live = page.nextCursor >= selected.cursor;
    return page;
}
} // namespace ketplus::daemon
