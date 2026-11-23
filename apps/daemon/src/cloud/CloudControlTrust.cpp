#include "cloud/CloudControlClient.h"
#include "cloud/CloudControlSupport.h"
#include "storage/LocalJournal.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QUrl>

namespace ketplus::daemon {
using cloud_control::object;
using cloud_control::responseCode;
namespace {
QString encodedPathSegment(const QString& value) {
    return QString::fromUtf8(QUrl::toPercentEncoding(value, QByteArrayLiteral("-._~")));
}

} // namespace

CloudControlClient::AccountFence CloudControlClient::accountFence() const {
    return {accountEpoch_, enrollmentRevision_, machineId_, workspaceId_};
}

bool CloudControlClient::accountCurrent(const AccountFence& fence) const {
    return !machineId_.isEmpty() && !workspaceId_.isEmpty() && fence.epoch == accountEpoch_ &&
           fence.enrollmentRevision == enrollmentRevision_ && fence.machine == machineId_ &&
           fence.workspace == workspaceId_;
}

QJsonArray CloudControlClient::trustedDevices(const QJsonArray& devices,
                                              const AccountFence& fence) {
    if (!accountCurrent(fence))
        return {};
    if (legacyPinAdoptionPending_) {
        QString error;
        if (!pins_.adopt(devices, true, &error)) {
            lastError_ = error;
            return {};
        }
        legacyPinAdoptionPending_ = false;
    }
    QString error;
    const auto trusted = pins_.trusted(devices, &error);
    if (!error.isEmpty())
        lastError_ = error;
    return trusted;
}

void CloudControlClient::invalidateSenderKeys() {
    ++senderKeysRevision_;
    senderKeys_.clear();
}

void CloudControlClient::withSenderKeys(const QString& senderId, const int version,
                                        SenderKeysCallback callback) {
    const auto fence = accountFence();
    const auto revision = senderKeysRevision_;
    const auto current = [this, fence, revision] {
        return accountCurrent(fence) && revision == senderKeysRevision_;
    };
    if (!current()) {
        callback({}, QStringLiteral("The account changed before the operation completed."));
        return;
    }
    const QString cacheKey = QStringLiteral("%1/%2").arg(senderId).arg(version);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (const auto cached = senderKeys_.constFind(cacheKey); cached != senderKeys_.cend()) {
        if (cached->expiresAt > now && accountCurrent(cached->account) &&
            pins_.trustsKeys(senderId, version, cached->keys)) {
            const auto keys = cached->keys;
            callback(keys, {});
            return;
        }
        senderKeys_.remove(cacheKey);
    }
    const QString keyPath = QStringLiteral("/v2/device-keys/%1/%2")
                                .arg(QString::fromUtf8(QUrl::toPercentEncoding(senderId)))
                                .arg(version);
    request(
        "GET", keyPath, {}, daemonToken_, {},
        [this, fence, current, senderId, version, cacheKey, callback = std::move(callback)](
            const int status, const QJsonValue& value, const QString& transportError) mutable {
            if (!current()) {
                callback({}, QStringLiteral("The account changed before the operation completed."));
                return;
            }

            if (status != 200 || !transportError.isEmpty()) {
                callback({}, transportError.isEmpty()
                                 ? QStringLiteral("The sender key is unavailable.")
                                 : transportError);
                return;
            }
            const QJsonObject keys = object(value).value(QStringLiteral("keys")).toObject();
            // The relay names the keys; only a device pinned on this Mac may use them (#87).
            QString pinError;
            const bool initialized = pins_.initialized(&pinError);
            if (!pinError.isEmpty()) {
                lastError_ = pinError;
                callback({}, pinError);
                return;
            }
            auto accept = [this, fence, current, senderId, version, cacheKey, keys,
                           callback = std::move(callback)]() {
                if (!current()) {
                    callback({},
                             QStringLiteral("The account changed before the operation completed."));
                    return;
                }

                if (!pins_.trustsKeys(senderId, version, keys)) {
                    callback({}, QStringLiteral("The sender is not paired with this Mac."));
                    return;
                }
                if (senderKeys_.size() >= maximumCachedSenderKeys)
                    senderKeys_.clear();
                senderKeys_.insert(
                    cacheKey,
                    {keys, QDateTime::currentDateTimeUtc().addMSecs(senderKeyCacheMilliseconds),
                     fence});
                callback(keys, {});
            };
            if (initialized || !legacyPinAdoptionPending_) {
                accept();
                return;
            }
            // Before the first device list, the devices already active are adopted.
            request("GET", QStringLiteral("/v2/devices"), {}, daemonToken_, {},
                    [this, fence, current, accept = std::move(accept)](
                        const int listStatus, const QJsonValue& list, const QString& listError) {
                        if (current() && listStatus == 200 && list.isArray() && listError.isEmpty())
                            (void)trustedDevices(list.toArray(), fence);
                        accept();
                    });
        });
}

void CloudControlClient::createPairing(LocalCallback callback) {
    const auto fence = accountFence();
    ensureDaemonCredential([this, fence, callback = std::move(callback)](const bool ready,
                                                                         const QString& error) {
        if (!accountCurrent(fence)) {
            callback({}, QStringLiteral("The account changed before the operation completed."));
            return;
        }
        if (!ready) {
            callback({}, error);
            return;
        }
        // The QR code is the one channel the relay cannot touch: it names this Mac's keys, so
        // the phone can refuse host keys the relay substitutes (#89).
        QString keyError;
        const QString fingerprint = DevicePins::hostFingerprint(crypto_.publicKeys(&keyError));
        if (fingerprint.isEmpty()) {
            callback({},
                     keyError.isEmpty() ? QStringLiteral("This Mac has no valid keys.") : keyError);
            return;
        }
        const QString host = QStringLiteral("&machine=%1&kv=%2&fp=%3")
                                 .arg(QString::fromUtf8(QUrl::toPercentEncoding(machineId_)))
                                 .arg(machineKeyVersion_)
                                 .arg(fingerprint);
        request(
            "POST", QStringLiteral("/v2/pairing/tickets"), QJsonObject{}, daemonToken_, {},
            [this, fence, host, callback = std::move(callback)](
                const int status, const QJsonValue& value, const QString& transportError) {
                if (!accountCurrent(fence)) {
                    callback({},
                             QStringLiteral("The account changed before the operation completed."));
                    return;
                }

                const QJsonObject pairing = object(value);
                const QJsonObject ticket = pairing.value(QStringLiteral("ticket")).toObject();
                const QString secret = pairing.value(QStringLiteral("secret")).toString();
                if (status != 201 || ticket.isEmpty() || secret.isEmpty() ||
                    !transportError.isEmpty()) {
                    callback({}, transportError.isEmpty()
                                     ? responseCode(value, QStringLiteral("pairing_failed"))
                                     : transportError);
                    return;
                }
                const QString qr = QStringLiteral("ketplus://pair?ticket=%1&secret=%2")
                                       .arg(QString::fromUtf8(QUrl::toPercentEncoding(
                                                ticket.value(QStringLiteral("id")).toString())),
                                            QString::fromUtf8(QUrl::toPercentEncoding(secret))) +
                                   host;
                callback({{QStringLiteral("ok"), true},
                          {QStringLiteral("ticket"), ticket},
                          {QStringLiteral("pairing_code"), qr}},
                         {});
            });
    });
}

void CloudControlClient::pairingStatus(const QJsonObject& payload, LocalCallback callback) {
    const auto fence = accountFence();
    const QString ticketId = payload.value(QStringLiteral("ticket_id")).toString();
    if (ticketId.isEmpty()) {
        callback({}, QStringLiteral("A pairing ticket is required."));
        return;
    }
    ensureDaemonCredential([this, fence, ticketId, callback = std::move(callback)](
                               const bool ready, const QString& error) {
        if (!accountCurrent(fence)) {
            callback({}, QStringLiteral("The account changed before the operation completed."));
            return;
        }
        if (!ready) {
            callback({}, error);
            return;
        }
        request(
            "GET", QStringLiteral("/v2/pairing/tickets/%1").arg(encodedPathSegment(ticketId)), {},
            daemonToken_, {},
            [this, fence, ticketId, callback = std::move(callback)](
                const int status, const QJsonValue& value, const QString& transportError) {
                if (!accountCurrent(fence)) {
                    callback({},
                             QStringLiteral("The account changed before the operation completed."));
                    return;
                }

                if (status != 200 || !transportError.isEmpty()) {
                    callback({}, transportError.isEmpty()
                                     ? responseCode(value, QStringLiteral("pairing_not_found"))
                                     : transportError);
                    return;
                }
                const QJsonObject ticket = object(value);
                QJsonObject result{{QStringLiteral("ok"), true},
                                   {QStringLiteral("ticket"), ticket}};
                // The code to compare comes from this daemon, not the relay: a digest the
                // claimed keys do not reproduce gives no code (#89).
                const QJsonObject device = ticket.value(QStringLiteral("claimedDevice")).toObject();
                const QString digest = ticket.value(QStringLiteral("claimDigest")).toString();
                if (!device.isEmpty() || !digest.isEmpty()) {
                    const bool verified = !device.isEmpty() && !digest.isEmpty() &&
                                          DevicePins::claimDigest(ticketId, device) == digest &&
                                          !DevicePins::confirmationCode(digest).isEmpty();
                    result.insert(QStringLiteral("claim_verified"), verified);
                    if (verified) {
                        result.insert(QStringLiteral("claim_digest"), digest);
                        result.insert(QStringLiteral("confirmation_code"),
                                      DevicePins::confirmationCode(digest));
                    }
                }
                callback(result, {});
            });
    });
}

void CloudControlClient::approvePairing(const QJsonObject& payload, LocalCallback callback) {
    const auto fence = accountFence();
    const QString ticketId = payload.value(QStringLiteral("ticket_id")).toString();
    // The claim the user confirmed on this Mac; the ticket is read again below, and the relay
    // must not swap keys and digest together in between.
    const QString confirmed = payload.value(QStringLiteral("claim_digest")).toString();
    if (ticketId.isEmpty() || confirmed.isEmpty()) {
        callback({}, QStringLiteral("A pairing ticket and its confirmed claim are required."));
        return;
    }
    ensureDaemonCredential([this, fence, ticketId, confirmed, callback = std::move(callback)](
                               const bool ready, const QString& error) {
        if (!accountCurrent(fence)) {
            callback({}, QStringLiteral("The account changed before the operation completed."));
            return;
        }
        if (!ready) {
            callback({}, error);
            return;
        }
        const QString path =
            QStringLiteral("/v2/pairing/tickets/%1").arg(encodedPathSegment(ticketId));
        request(
            "GET", path, {}, daemonToken_, {},
            [this, fence, path, confirmed, callback = std::move(callback)](
                const int status, const QJsonValue& value, const QString& transportError) {
                if (!accountCurrent(fence)) {
                    callback({},
                             QStringLiteral("The account changed before the operation completed."));
                    return;
                }

                const QJsonObject ticket = object(value);
                const QJsonObject device = ticket.value(QStringLiteral("claimedDevice")).toObject();
                const QString digest = ticket.value(QStringLiteral("claimDigest")).toString();
                if (status != 200 ||
                    ticket.value(QStringLiteral("status")).toString() !=
                        QStringLiteral("claimed") ||
                    device.isEmpty() || digest.isEmpty() || !transportError.isEmpty()) {
                    callback({}, transportError.isEmpty()
                                     ? QStringLiteral("The mobile key claim is not ready.")
                                     : transportError);
                    return;
                }
                // The code the user compared is this digest. Recomputed from the claimed keys, it
                // proves those keys are the phone's, not ones the relay substituted (#87).
                if (digest != confirmed ||
                    DevicePins::claimDigest(ticket.value(QStringLiteral("id")).toString(),
                                            device) != digest) {
                    callback({}, QStringLiteral("The device claim does not match its keys."));
                    return;
                }
                QString cryptoError;
                const auto envelopes =
                    sessionKeyEnvelopes(QJsonArray{device}, false, 0, nullptr, &cryptoError);
                if (!cryptoError.isEmpty()) {
                    callback({}, cryptoError);
                    return;
                }
                QJsonObject body{{QStringLiteral("claimDigest"), digest}};
                if (!envelopes.isEmpty())
                    body.insert(QStringLiteral("sessionKeys"), envelopes);
                request("POST", path + QStringLiteral("/approve"), body, daemonToken_, {},
                        [this, fence, device, callback = std::move(callback)](
                            const int approveStatus, const QJsonValue& approveValue,
                            const QString& approveError) {
                            if (!accountCurrent(fence)) {
                                callback(
                                    {}, QStringLiteral(
                                            "The account changed before the operation completed."));
                                return;
                            }

                            QString pinError;
                            if (approveStatus != 200 || !approveError.isEmpty()) {
                                callback({}, approveError.isEmpty()
                                                 ? responseCode(
                                                       approveValue,
                                                       QStringLiteral("pairing_approval_failed"))
                                                 : approveError);
                            } else if (!pins_.pin(device, &pinError)) {
                                callback({}, pinError.isEmpty()
                                                 ? QStringLiteral("Unable to remember the device.")
                                                 : pinError);
                            } else {
                                legacyPinAdoptionPending_ = false;
                                invalidateSenderKeys();
                                signaling_->devicePinChanged(device.value("id").toString());
                                callback({{QStringLiteral("ok"), true},
                                          {QStringLiteral("ticket"), object(approveValue)}},
                                         {});
                            }
                        });
            });
    });
}

void CloudControlClient::revokePairing(const QJsonObject& payload, LocalCallback callback) {
    const auto fence = accountFence();
    const QString ticketId = payload.value(QStringLiteral("ticket_id")).toString();
    if (ticketId.isEmpty()) {
        callback({}, QStringLiteral("A pairing ticket is required."));
        return;
    }
    ensureDaemonCredential([this, fence, ticketId, callback = std::move(callback)](
                               const bool ready, const QString& error) {
        if (!accountCurrent(fence)) {
            callback({}, QStringLiteral("The account changed before the operation completed."));
            return;
        }
        if (!ready) {
            callback({}, error);
            return;
        }
        request(
            "POST",
            QStringLiteral("/v2/pairing/tickets/%1/revoke").arg(encodedPathSegment(ticketId)),
            QJsonObject{}, daemonToken_, {},
            [this, fence, callback = std::move(callback)](const int status, const QJsonValue& value,
                                                          const QString& transportError) {
                if (!accountCurrent(fence)) {
                    callback({},
                             QStringLiteral("The account changed before the operation completed."));
                    return;
                }

                if (status != 200 || !transportError.isEmpty()) {
                    callback({}, transportError.isEmpty()
                                     ? responseCode(value, QStringLiteral("pairing_revoke_failed"))
                                     : transportError);
                    return;
                }
                callback({{QStringLiteral("ok"), true}}, {});
            });
    });
}

void CloudControlClient::revokeDevice(const QJsonObject& payload, LocalCallback callback) {
    const auto fence = accountFence();
    const QString deviceId = payload.value(QStringLiteral("device_id")).toString();
    if (deviceId.isEmpty()) {
        callback({}, QStringLiteral("A device is required."));
        return;
    }
    ensureDaemonCredential([this, fence, deviceId, callback = std::move(callback)](
                               const bool ready, const QString& error) {
        if (!accountCurrent(fence)) {
            callback({}, QStringLiteral("The account changed before the operation completed."));
            return;
        }
        if (!ready) {
            callback({}, error);
            return;
        }
        request(
            "POST", QStringLiteral("/v2/devices/%1/revoke").arg(encodedPathSegment(deviceId)),
            QJsonObject{}, daemonToken_, {},
            [this, fence, deviceId, callback = std::move(callback)](
                const int status, const QJsonValue& value, const QString& transportError) {
                if (!accountCurrent(fence)) {
                    callback({},
                             QStringLiteral("The account changed before the operation completed."));
                    return;
                }

                if (status != 200 || !transportError.isEmpty()) {
                    callback({}, transportError.isEmpty()
                                     ? responseCode(value, QStringLiteral("device_revoke_failed"))
                                     : transportError);
                    return;
                }
                legacyPinAdoptionPending_ = false;
                signaling_->receive({{"type", "device.revoked"}, {"deviceId", deviceId}});
                QString pinError;
                const bool unpinned = pins_.unpin(deviceId, &pinError);
                signaling_->devicePinChanged(deviceId);
                if (!unpinned) {
                    callback({}, pinError);
                    return;
                }
                callback({{QStringLiteral("ok"), true}}, {});
            });
    });
}

} // namespace ketplus::daemon
