#include "control/ControlV2Client.h"

#include "control/RelayEventFeed.h"
#include "control/RelayFrameLink.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace ketplus::remote {
namespace {
const QString capabilitiesAccount = QStringLiteral("device-capabilities");

QStringList normalized(QStringList capabilities) {
    capabilities.removeDuplicates();
    capabilities.sort();
    return capabilities;
}

QJsonArray toArray(const QStringList& capabilities) {
    QJsonArray array;
    for (const QString& capability : capabilities)
        array.append(capability);
    return array;
}
} // namespace

QStringList ControlV2Client::desktopCapabilities() {
    return {QStringLiteral("events.forward_only/1")};
}

void ControlV2Client::updateCapabilities(const QStringList& capabilities, Callback done) {
    if (deviceId_.isEmpty()) {
        done({}, "Pair this device before it lists its capabilities.");
        return;
    }
    const auto set = normalized(capabilities);
    const auto device = deviceId_;
    authorized(
        "PUT", "/v2/mobile/device/capabilities", QJsonObject{{"capabilities", toArray(set)}},
        [this, set, device, done = std::move(done)](const QJsonValue& value,
                                                    const QString& failure) {
            if (!failure.isEmpty()) {
                done({}, failure);
                return;
            }
            QStringList accepted;
            for (const auto& entry : value.toObject().value("capabilities").toArray())
                accepted.append(entry.toString());
            if (normalized(accepted) != set) {
                done({}, "The relay stored other capabilities than this device sent.");
                return;
            }
            // A record of what was accepted, for this device only, so a restart or an
            // unchanged set sends nothing.
            if (deviceId_ == device)
                (void)credentials_.write(
                    capabilitiesAccount,
                    QJsonDocument(QJsonObject{{"deviceId", device}, {"capabilities", toArray(set)}})
                        .toJson(QJsonDocument::Compact));
            done(QJsonObject{{"capabilities", toArray(set)}}, {});
        });
}

void ControlV2Client::syncCapabilities(const QStringList& capabilities, Callback done) {
    const auto stored = QJsonDocument::fromJson(credentials_.read(capabilitiesAccount)).object();
    QStringList known;
    for (const auto& entry : stored.value("capabilities").toArray())
        known.append(entry.toString());
    if (!deviceId_.isEmpty() && stored.value("deviceId").toString() == deviceId_ &&
        normalized(known) == normalized(capabilities)) {
        done(QJsonObject{{"unchanged", true}}, {});
        return;
    }
    updateCapabilities(capabilities, std::move(done));
}

ControlV2Client::~ControlV2Client() = default;

RelayFrameLink* ControlV2Client::frameLink() {
    if (frameLink_)
        return frameLink_;
    frameLink_ = new RelayFrameLink(
        origin_,
        [this](std::function<void(const QString&, const QString&)> done) {
            connectionToken(std::move(done));
        },
        frameSocketFactory_ ? frameSocketFactory_ : RelayFrameLink::qtSockets(),
        RelayFrameLink::Options{frameBackoffInitial_, frameBackoffMaximum_, 30'000}, this);
    // Without a credential the link has nothing to open with; the next connection starts it.
    connect(this, &ControlV2Client::credentialRevoked, frameLink_, &RelayFrameLink::stop);
    return frameLink_;
}

EventCursorStore* ControlV2Client::eventCursors() {
    if (!eventCursors_)
        eventCursors_ = std::make_unique<CredentialEventCursors>(&credentials_);
    return eventCursors_.get();
}

void ControlV2Client::setFrameLinkBackoff(const int initial, const int maximum) {
    frameBackoffInitial_ = initial;
    frameBackoffMaximum_ = maximum;
}

void ControlV2Client::setFrameSocketFactory(std::function<FrameSocket*(QObject*)> factory) {
    frameSocketFactory_ = std::move(factory);
}

} // namespace ketplus::remote
