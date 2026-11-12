#include "transport/RemotePeers.h"
#include "KetPlusProductVersion.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

namespace ketplus::daemon::transport {
namespace {
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
qint64 timeOf(const QJsonValue& value) {
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs).toMSecsSinceEpoch();
}
} // namespace
RemotePeers::RemotePeers(Reader reader, QObject* parent)
    : QObject(parent), reader_(std::move(reader)) {
    timer_.setInterval(50);
    connect(&timer_, &QTimer::timeout, this, &RemotePeers::tick);
}

bool RemotePeers::validGrant(const QJsonObject& grant, const QString& id, const QString& device,
                             int version, qint64 at) {
    const auto expires = timeOf(grant.value("expiresAt"));
    const auto issued = timeOf(grant.value("issuedAt"));
    return grant.value("connectionId") == id && !id.isEmpty() &&
           grant.value("deviceId") == device && !device.isEmpty() && version > 0 &&
           grant.value("credentialEpoch").toInteger() == version &&
           !grant.value("workspaceId").toString().isEmpty() &&
           !grant.value("machineId").toString().isEmpty() &&
           !grant.value("ownerSubjectId").toString().isEmpty() &&
           grant.value("capabilities").isArray() && issued > 0 && issued <= at + 30'000 &&
           expires > at && expires - issued <= 10 * 60'000 && expires > issued;
}

bool RemotePeers::accept(const QString& id, const QString& device, int version,
                         const QJsonObject& grant, PeerLink* link, qint64 epoch) {
    if (!link || peers_.contains(id) || peers_.size() >= 8 ||
        !validGrant(grant, id, device, version, now()))
        return false;
    for (const auto& peer : peers_)
        if (peer->device == device && !peer->hello)
            return false; // One pending replacement per device.
    auto peer = std::make_shared<Peer>();
    peer->device = device;
    peer->version = version;
    peer->grant = grant;
    peer->link = link;
    peer->epoch = epoch;
    link->setDiagnosticEpoch(epoch);
    peer->created = now();
    peer->lastPing = now();
    peers_.insert(id, peer);
    link->setParent(this);
    connect(link, &PeerLink::received, this,
            [this, id](Channel ch, const QByteArray& bytes, bool binary) {
                receive(id, ch, bytes, binary);
            });
    connect(link, &PeerLink::closed, this, [this, id] { close(id, "link_closed"); });
    connect(link, &PeerLink::writable, this, &RemotePeers::tick);
    timer_.start();
    return true;
}

bool RemotePeers::renew(const QString& id, const QJsonObject& grant) {
    const auto peer = peers_.value(id);
    if (!peer || !validGrant(grant, id, peer->device, peer->version, now()) ||
        grant.value("workspaceId") != peer->grant.value("workspaceId") ||
        grant.value("machineId") != peer->grant.value("machineId") ||
        grant.value("ownerSubjectId") != peer->grant.value("ownerSubjectId") ||
        grant.value("capabilities") != peer->grant.value("capabilities"))
        return false;
    peer->grant = grant;
    return true;
}

bool RemotePeers::authorized(const QString& id, const QJsonObject& command) const {
    const auto peer = peers_.value(id);
    if (!peer || !peer->hello || !validGrant(peer->grant, id, peer->device, peer->version, now()))
        return false;
    const auto kind = command.value("kind").toString();
    const auto expected = kind.startsWith("workspace.files.") || kind == "session.resource.resolve"
                              ? QStringLiteral("workspace.files.read")
                          : kind == "worktree.branches.read" ? QStringLiteral("session.create")
                                                             : kind;
    const auto content = command.value("content").toObject();
    // ADR 0116 §2: team commands travel only through the mailbox in this version.
    return !kind.isEmpty() && kind != "session.delete" && !kind.startsWith("team.") &&
           command.value("id").toString().size() <= 128 &&
           !command.value("id").toString().isEmpty() &&
           !command.value("idempotencyKey").toString().isEmpty() &&
           command.value("capability") == expected &&
           peer->grant.value("capabilities").toArray().contains(expected) &&
           command.value("workspaceId") == peer->grant.value("workspaceId") &&
           command.value("machineId") == peer->grant.value("machineId") &&
           content.value("senderDeviceId") == peer->device &&
           content.value("senderKeyVersion").toInt() == peer->version &&
           (!command.contains("ownerSubjectId") ||
            command.value("ownerSubjectId") == peer->grant.value("ownerSubjectId")) &&
           (!command.contains("credentialEpoch") ||
            command.value("credentialEpoch") == peer->grant.value("credentialEpoch"));
}

void RemotePeers::receive(const QString& id, Channel channel, const QByteArray& bytes,
                          bool binary) {
    auto peer = peers_.value(id);
    if (!peer)
        return;
    if (!validGrant(peer->grant, id, peer->device, peer->version, now())) {
        close(id, "grant_expired");
        return;
    }
    if (now() - peer->inputWindow >= 1000) {
        peer->inputWindow = now();
        peer->inputFrames = 0;
    }
    if (++peer->inputFrames > 256) {
        close(id, "frame_rate_limit");
        return;
    }
    QString error;
    const auto frame = peer->codec.receive({channel, bytes, binary}, now(), &error);
    if (!error.isEmpty()) {
        close(id, error);
        return;
    }
    if (frame.isEmpty())
        return;
    const auto type = frame.value("type").toString();
    if ((!binary && channel != Channel::Control) || (binary && type != "command")) {
        close(id, "wrong_channel");
        return;
    }
    if (!peer->hello) {
        if (type != "hello" || frame.value("connectionId") != id ||
            !frame.value("capabilities").toArray().contains("transport.webrtc/1")) {
            close(id, "hello_required");
            return;
        }
        peer->hello = true;
        for (const auto& otherId : peers_.keys())
            if (otherId != id && peers_.value(otherId)->device == peer->device)
                close(otherId, "superseded");
        send(id, {{"type", "welcome"},
                  {"connectionId", id},
                  {"capabilities", QJsonArray{"transport.webrtc/1"}},
                  {"daemonVersion", KETPLUS_PRODUCT_VERSION}});
    } else if (type == "command") {
        auto command = frame.value("command").toObject();
        if (!authorized(id, command)) {
            close(id, "command_forbidden");
            return;
        }
        const auto commandId = command.value("id").toString();
        if (peer->commands.contains(commandId))
            return;
        if (peer->commands.size() >= 32) {
            close(id, "command_limit");
            return;
        }
        // Routing metadata is generated here, never accepted from the peer.
        command.remove("_peerConnection");
        command.remove("claimToken");
        command.insert("attempt", 0);
        command.insert("ownerSubjectId", peer->grant.value("ownerSubjectId"));
        command.insert("credentialEpoch", peer->grant.value("credentialEpoch"));
        peer->commands.insert(commandId);
        emit this->command(id, command);
    } else if (type == "subscribe") {
        const auto session = frame.value("sessionId").toString();
        const auto generation = frame.value("sessionGeneration").toInteger(-1);
        const auto cursor = frame.value("afterCursor").toInteger(-1);
        if (!peer->grant.value("capabilities").toArray().contains("session.read") ||
            session.isEmpty() || session.size() > 128 || generation < 1 || cursor < 0 ||
            (!peer->subscriptions.contains(session) && peer->subscriptions.size() >= 16)) {
            close(id, "subscription_forbidden");
            return;
        }
        peer->subscriptions.insert(session, {generation, cursor, false});
    } else if (type == "unsubscribe") {
        peer->subscriptions.remove(frame.value("sessionId").toString());
    } else if (type == "ping") {
        send(id, {{"type", "pong"}, {"nonce", frame.value("nonce").toString().left(128)}});
    } else if (type == "pong" && frame.value("nonce") == peer->pingNonce) {
        peer->missedPongs = 0;
        peer->pingNonce.clear();
    } else if (type == "close") {
        close(id, "client_closed");
    } else if (type != "pong") {
        close(id, "unsupported_frame");
    }
}

bool RemotePeers::send(const QString& id, QJsonObject frame, Channel channel) {
    const auto peer = peers_.value(id);
    if (!peer || !peer->link || !peer->hello ||
        !validGrant(peer->grant, id, peer->device, peer->version, now()))
        return false;
    frame.insert("epoch", peer->epoch);
    const auto packets = FrameCodec::encode(frame, channel, peer->nextTransfer++);
    if (packets.isEmpty() || !peer->queue.append(packets)) {
        close(id, "send_queue_full");
        return false;
    }
    peer->queue.flush(*peer->link);
    return true;
}

void RemotePeers::completed(const QString& id, const QString& commandId) {
    if (auto peer = peers_.value(id))
        peer->commands.remove(commandId);
}

void RemotePeers::tick() {
    for (const auto& id : peers_.keys()) {
        const auto peer = peers_.value(id);
        if (!peer || !peer->link)
            continue;
        if (!validGrant(peer->grant, id, peer->device, peer->version, now()) ||
            (!peer->hello && now() - peer->created > 10'000) || peer->codec.expire(now())) {
            close(id, "peer_expired");
            continue;
        }
        if (!peer->hello)
            continue;
        peer->queue.flush(*peer->link);
        if (now() - peer->lastPing >= 15'000) {
            if (peer->missedPongs >= 3) {
                close(id, "ping_timeout");
                continue;
            }
            peer->lastPing = now();
            ++peer->missedPongs;
            peer->pingNonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
            send(id, {{"type", "ping"}, {"nonce", peer->pingNonce}});
        }
        // One bounded page per tick; never accumulate the journal behind a slow transport.
        if (!peer->queue.empty() || peer->link->buffered(Channel::Bulk) != 0)
            continue;
        const auto sessions = peer->subscriptions.keys();
        if (!sessions.isEmpty()) {
            const auto session = sessions.at(peer->nextSubscription++ % sessions.size());
            auto& sub = peer->subscriptions[session];
            if (sub.live && !sub.dirty)
                continue;
            sub.dirty = false;
            const auto page = reader_(session, sub.generation, sub.cursor);
            if (page.reset) {
                send(id, {{"type", "session.reset"},
                          {"sessionId", session},
                          {"sessionGeneration", page.generation}});
                peer->subscriptions.remove(session);
            } else if (!page.events.isEmpty() || (!sub.live && page.live)) {
                const QJsonObject body{{"events", page.events},
                                       {"nextCursor", page.nextCursor},
                                       {"hasMore", !page.live}};
                if (send(id,
                         {{"type", "events"},
                          {"sessionId", session},
                          {"page", body},
                          {"live", page.live}},
                         Channel::Events)) {
                    sub.cursor = page.nextCursor;
                    sub.live = page.live;
                }
            }
        }
    }
}

void RemotePeers::eventsChanged(const QString& sessionId) {
    for (const auto& peer : peers_) {
        auto sub = peer->subscriptions.find(sessionId);
        if (sub != peer->subscriptions.end())
            sub->dirty = true;
    }
}

void RemotePeers::close(const QString& id, const QString& reason) {
    const auto peer = peers_.take(id);
    if (!peer)
        return;
    if (peer->link) {
        peer->link->disconnect(this);
        peer->link->close();
        peer->link->deleteLater();
    }
    if (peers_.isEmpty())
        timer_.stop();
    emit ended(id, reason);
}
void RemotePeers::revoke(const QString& device) {
    for (const auto& id : peers_.keys())
        if (peers_.value(id)->device == device)
            close(id, "device_revoked");
}
void RemotePeers::closeAll() {
    for (const auto& id : peers_.keys())
        close(id, "host_closed");
}
} // namespace ketplus::daemon::transport
