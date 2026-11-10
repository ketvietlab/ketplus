#include "transport/Signaling.h"
#include "cloud/CloudCrypto.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QRegularExpression>
#include <openssl/evp.h>

namespace ketplus::daemon::transport {
namespace {
QString hash(const QString& value) {
    return QString::fromLatin1(
        QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
}
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
} // namespace
Signaling::Signaling(RemotePeers* peers, Keys keys, Sign sign, QObject* parent)
    : QObject(parent), peers_(peers), keys_(std::move(keys)), sign_(std::move(sign)),
      epoch_(now()) {
    connect(peers_, &RemotePeers::ended, this, [this](const QString& id, const QString& reason) {
        attempts_.remove(id);
        emit outgoing({{"type", "signal.end"}, {"connectionId", id}, {"reason", reason}});
    });
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, [this] {
        used_.removeIf([](const auto& value) { return value.value() < now(); });
        for (const auto& id : attempts_.keys()) {
            const auto& attempt = attempts_[id];
            if (attempt.pending && now() - attempt.created > 10'000)
                end(id, "offer_timeout");
        }
    });
    timer_.start();
}

void Signaling::configure(const QString& workspace, const QString& machine, int keyVersion,
                          bool enabled) {
    if (!enabled || workspace_ != workspace || machine_ != machine || version_ != keyVersion) {
        peers_->closeAll();
        attempts_.clear();
    }
    workspace_ = workspace;
    machine_ = machine;
    version_ = keyVersion;
    enabled_ = enabled && !workspace.isEmpty() && !machine.isEmpty();
}

QByteArray Signaling::offerBytes(const QJsonObject& offer) {
    const auto grant = offer.value("grant").toObject();
    return CloudCrypto::canonical({"ketplus", 2, "webrtc-offer", offer.value("connectionId"),
                                   grant.value("workspaceId"), grant.value("machineId"),
                                   offer.value("deviceId"), offer.value("deviceKeyVersion"),
                                   offer.value("issuedAt"), hash(offer.value("sdp").toString())});
}
QByteArray Signaling::answerBytes(const QJsonObject& offer, const QString& sdp, int machineVersion,
                                  const QString& issuedAt) {
    const auto grant = offer.value("grant").toObject();
    return CloudCrypto::canonical({"ketplus", 2, "webrtc-answer", offer.value("connectionId"),
                                   grant.value("workspaceId"), grant.value("machineId"),
                                   machineVersion, offer.value("deviceId"),
                                   hash(offer.value("sdp").toString()), issuedAt, hash(sdp)});
}
bool Signaling::verify(const QByteArray& bytes, const QByteArray& key,
                       const QByteArray& signature) {
    if (key.size() != 32 || signature.size() != 64)
        return false;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> publicKey(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                    reinterpret_cast<const unsigned char*>(key.constData()), 32),
        EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return publicKey && ctx &&
           EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, publicKey.get()) == 1 &&
           EVP_DigestVerify(ctx.get(),
                            reinterpret_cast<const unsigned char*>(signature.constData()), 64,
                            reinterpret_cast<const unsigned char*>(bytes.constData()),
                            size_t(bytes.size())) == 1;
}

void Signaling::receive(const QJsonObject& message) {
    const auto type = message.value("type").toString();
    const auto id = message.value("connectionId").toString();
    // A revocation ends the device's access whether or not peer transport is negotiated.
    if (!enabled_ && type != "device.revoked")
        return;
    if (type == "device.revoked") {
        const auto device = message.value("deviceId").toString();
        peers_->revoke(device);
        for (const auto& pending : attempts_.keys())
            if (attempts_[pending].offer.value("deviceId") == device)
                end(pending, "device_revoked");
        if (!device.isEmpty())
            emit deviceRevoked(device);
    } else if (type == "signal.offer") {
        offer(message);
    } else if (attempts_.contains(id)) {
        if (type == "signal.grant") {
            if (!peers_->renew(id, message.value("grant").toObject()))
                end(id, "invalid_grant");
        } else if (type == "signal.end" || type == "signal.error") {
            end(id, "remote_closed");
        } else if (type == "signal.candidate") {
            auto& attempt = attempts_[id];
            const auto candidate = message.value("candidate");
            if (attempt.remoteComplete || ++attempt.remoteCandidates > 65 ||
                (!candidate.isString() && !candidate.isNull()) ||
                candidate.toString().toUtf8().size() > 1024 ||
                message.value("sdpMid").toString().size() > 64) {
                end(id, "candidate_limit");
                return;
            }
            attempt.remoteComplete = candidate.isNull();
            if (attempt.link) {
                if (!attempt.link->candidate(candidate.toString(),
                                             message.value("sdpMid").toString()))
                    end(id, "invalid_candidate");
            } else
                attempt.candidates.append(message);
        }
    }
}

void Signaling::offer(const QJsonObject& message) {
    const auto id = message.value("connectionId").toString();
    static const QRegularExpression pattern("^pc_[A-Za-z0-9_-]{24}$");
    if (!pattern.match(id).hasMatch())
        return;
    const auto grant = message.value("grant").toObject();
    const auto device = message.value("deviceId").toString();
    const auto version = message.value("deviceKeyVersion").toInt();
    const auto issued =
        QDateTime::fromString(message.value("issuedAt").toString(), Qt::ISODateWithMs);
    const auto sdp = message.value("sdp").toString().toUtf8();
    if (used_.contains(id) || attempts_.size() >= 8 || used_.size() >= 4096 || !issued.isValid() ||
        qAbs(issued.toMSecsSinceEpoch() - now()) > 5 * 60'000 || sdp.isEmpty() ||
        sdp.size() > 16 * 1024 || grant.value("workspaceId") != workspace_ ||
        grant.value("machineId") != machine_ ||
        !RemotePeers::validGrant(grant, id, device, version, now())) {
        emit outgoing({{"type", "signal.end"}, {"connectionId", id}, {"reason", "invalid_offer"}});
        return;
    }
    used_.insert(id, now() + 10 * 60'000);
    Attempt attempt;
    attempt.offer = message;
    attempt.created = now();
    attempts_.insert(id, attempt);
    keys_(device, version, [self = QPointer<Signaling>(this), id](const QJsonObject& keys) {
        if (self)
            self->finishOffer(id, keys);
    });
}

void Signaling::finishOffer(const QString& id, const QJsonObject& keys) {
    if (!enabled_ || !attempts_.contains(id))
        return;
    auto& attempt = attempts_[id];
    const auto offer = attempt.offer;
    const auto device = offer.value("deviceId").toString();
    const auto version = offer.value("deviceKeyVersion").toInt();
    if (keys.value("id") != device || keys.value("keyVersion").toInt() != version ||
        (keys.contains("status") && keys.value("status") != "active") ||
        !verify(offerBytes(offer),
                CloudCrypto::fromBase64Url(
                    keys.value("keys").toObject().value("signingPublicKey").toString()),
                CloudCrypto::fromBase64Url(offer.value("signature").toString()))) {
        end(id, "invalid_signature");
        return;
    }
    auto* link = new WebRtcPeerLink;
    if (!peers_->accept(id, device, version, offer.value("grant").toObject(), link, ++epoch_)) {
        delete link;
        end(id, "peer_limit");
        return;
    }
    attempt.link = link;
    attempt.pending = false;
    connect(link, &WebRtcPeerLink::localDescription, this, [this, id, offer](const QString& sdp) {
        if (!attempts_.contains(id))
            return;
        const auto issued = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        const auto signature = sign_(answerBytes(offer, sdp, version_, issued));
        if (signature.size() != 64 || sdp.toUtf8().size() > 16 * 1024) {
            end(id, "answer_failed");
            return;
        }
        emit outgoing({{"type", "signal.answer"},
                       {"connectionId", id},
                       {"sdp", sdp},
                       {"issuedAt", issued},
                       {"machineKeyVersion", version_},
                       {"signature", QString::fromLatin1(CloudCrypto::base64Url(signature))}});
    });
    connect(link, &WebRtcPeerLink::localCandidate, this,
            [this, id](const QString& candidate, const QString& mid) {
                if (!attempts_.contains(id))
                    return;
                if (++attempts_[id].localCandidates > 65) {
                    end(id, "candidate_limit");
                    return;
                }
                emit outgoing({{"type", "signal.candidate"},
                               {"connectionId", id},
                               {"candidate", candidate.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                                 : QJsonValue(candidate)},
                               {"sdpMid", mid}});
            });
    const auto candidates = attempt.candidates;
    attempt.candidates.clear();
    if (!link->answer(offer.value("sdp").toString(), offer.value("iceServers").toArray())) {
        end(id, "answer_failed");
        return;
    }
    for (const auto& candidate : candidates)
        if (!link->candidate(candidate.value("candidate").toString(),
                             candidate.value("sdpMid").toString())) {
            end(id, "invalid_candidate");
            return;
        }
}

void Signaling::end(const QString& id, const QString& reason) {
    if (!attempts_.contains(id))
        return;
    const bool linked = bool(attempts_[id].link);
    attempts_.remove(id);
    if (linked)
        peers_->close(id, reason);
    else
        emit outgoing({{"type", "signal.end"}, {"connectionId", id}, {"reason", reason}});
}

void Signaling::refreshDevices(const QJsonArray& devices) {
    for (const auto& id : attempts_.keys()) {
        const auto offer = attempts_[id].offer;
        bool active = false;
        for (const auto& value : devices) {
            const auto device = value.toObject();
            if (device.value("id") == offer.value("deviceId") &&
                device.value("keyVersion") == offer.value("deviceKeyVersion") &&
                (!device.contains("status") || device.value("status") == "active"))
                active = true;
        }
        if (!active)
            end(id, "device_revoked");
    }
}
} // namespace ketplus::daemon::transport
