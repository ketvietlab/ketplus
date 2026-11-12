#include "transport/WebRtcPeerLink.h"
#include "transport/SelectedPath.h"

#include <QDebug>
#include <QJsonObject>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <rtc/rtc.hpp>
#include <utility>

namespace ketplus::daemon::transport {
struct WebRtcPeerLink::Impl {
    struct Delivery {
        std::mutex mutex;
        WebRtcPeerLink* owner{nullptr};
        std::atomic<int> pending{0};
        std::atomic<int> channels{0};
        std::atomic<bool> overflow{false};
    };
    std::shared_ptr<Delivery> delivery = std::make_shared<Delivery>();
    std::shared_ptr<rtc::PeerConnection> peer;
    std::array<std::shared_ptr<rtc::DataChannel>, 3> channels;
    bool open{false};
    bool stopped{false};
    bool iceConnected{false};

    void reportPath(WebRtcPeerLink* owner, bool reopened) {
        rtc::Candidate local, remote;
        auto localType = rtc::Candidate::Type::Unknown;
        auto remoteType = rtc::Candidate::Type::Unknown;
        try {
            if (peer && peer->getSelectedCandidatePair(&local, &remote)) {
                localType = local.type();
                remoteType = remote.type();
            }
        } catch (const std::exception&) {
            // Native exceptions can contain private candidate data. Do not log them.
        }
        qInfo().noquote() << selectedPathLog(localType, remoteType, owner->diagnosticEpoch(),
                                             reopened, QDateTime::currentDateTimeUtc());
    }

    // Taking the mutex only protects the short enqueue operation against destruction;
    // no native callback waits for the Qt event loop. Queued callbacks are cancelled by Qt.
    static void post(const std::shared_ptr<Delivery>& state,
                     std::function<void(WebRtcPeerLink*)> fn) {
        std::lock_guard lock(state->mutex);
        if (!state->owner)
            return;
        QMetaObject::invokeMethod(
            state->owner,
            [state, fn = std::move(fn)] {
                auto* owner = state->owner; // Qt thread owns pointer mutation/destruction.
                if (owner && !owner->impl_->stopped)
                    fn(owner);
            },
            Qt::QueuedConnection);
    }
    void attach(WebRtcPeerLink* owner, const std::shared_ptr<rtc::DataChannel>& channel) {
        const auto label = channel->label();
        const int index = label == "control" ? 0 : label == "events" ? 1 : label == "bulk" ? 2 : -1;
        const auto reliability = channel->reliability();
        if (index < 0 || channels[size_t(index)] || reliability.unordered ||
            reliability.maxPacketLifeTime || reliability.maxRetransmits) {
            channel->close();
            owner->close();
            return;
        }
        channels[size_t(index)] = channel;
        const auto state = delivery;
        const auto ready = [state] {
            post(state, [](auto* target) {
                auto& impl = *target->impl_;
                if (!impl.open && std::all_of(impl.channels.begin(), impl.channels.end(),
                                              [](const auto& ch) { return ch && ch->isOpen(); })) {
                    impl.open = true;
                    impl.reportPath(target, false);
                    emit target->opened();
                }
            });
        };
        channel->onOpen(ready);
        channel->onClosed([state] { post(state, [](auto* target) { target->close(); }); });
        channel->setBufferedAmountLowThreshold(64 * 1024);
        channel->onBufferedAmountLow(
            [state] { post(state, [](auto* target) { emit target->writable(); }); });
        channel->onMessage([state, index](rtc::message_variant message) {
            const bool binary = std::holds_alternative<rtc::binary>(message);
            const auto size = std::visit([](const auto& bytes) { return bytes.size(); }, message);
            if (size > size_t(FrameCodec::messageLimit)) {
                if (!state->overflow.exchange(true))
                    post(state, [](auto* target) { target->close(); });
                return;
            }
            if (state->pending.fetch_add(1) >= 64) {
                state->pending.fetch_sub(1);
                // Only one overflow close notification can be pending at a time.
                if (!state->overflow.exchange(true))
                    post(state, [](auto* target) { target->close(); });
                return;
            }
            const auto bytes = std::visit(
                [](const auto& data) {
                    return QByteArray(reinterpret_cast<const char*>(data.data()),
                                      qsizetype(data.size()));
                },
                message);
            post(state, [state, index, bytes, binary](auto* target) {
                state->pending.fetch_sub(1);
                emit target->received(static_cast<Channel>(index), bytes, binary);
            });
        });
        ready();
    }
};

WebRtcPeerLink::WebRtcPeerLink(QObject* parent)
    : PeerLink(parent), impl_(std::make_unique<Impl>()) {
    impl_->delivery->owner = this;
}

WebRtcPeerLink::~WebRtcPeerLink() {
    {
        std::lock_guard lock(impl_->delivery->mutex);
        impl_->delivery->owner = nullptr;
    }
    close();
}

bool WebRtcPeerLink::answer(const QString& offer, const QJsonArray& iceServers) {
    if (impl_->peer || impl_->stopped || offer.toUtf8().size() > 16 * 1024)
        return false;
    try {
        rtc::Configuration config;
        config.disableFingerprintVerification = false;
        config.maxMessageSize = FrameCodec::messageLimit;
        if (iceServers.size() > 8)
            return false;
        for (const auto& value : iceServers) {
            const auto server = value.toObject();
            auto urls = server.value("urls").toArray();
            if (server.value("urls").isString())
                urls = QJsonArray{server.value("urls")};
            if (urls.size() > 8)
                return false;
            for (const auto& url : urls) {
                const auto text = url.toString();
                if (text.size() > 2048 || (!text.startsWith("stun:") && !text.startsWith("turn:")))
                    continue; // libjuice supports UDP only; the relay supplies host-specific URLs.
                rtc::IceServer ice(text.toStdString());
                ice.username = server.value("username").toString().toStdString();
                ice.password = server.value("credential").toString().toStdString();
                config.iceServers.push_back(std::move(ice));
            }
        }
        const auto state = impl_->delivery;
        impl_->peer = std::make_shared<rtc::PeerConnection>(config);
        impl_->peer->onLocalDescription([state](rtc::Description description) {
            const auto sdp = QString::fromStdString(std::string(description));
            Impl::post(state, [sdp](auto* target) { emit target->localDescription(sdp); });
        });
        impl_->peer->onLocalCandidate([state](rtc::Candidate candidate) {
            const auto text = QString::fromStdString(std::string(candidate));
            const auto mid = QString::fromStdString(candidate.mid());
            Impl::post(state,
                       [text, mid](auto* target) { emit target->localCandidate(text, mid); });
        });
        impl_->peer->onGatheringStateChange([state](rtc::PeerConnection::GatheringState value) {
            if (value == rtc::PeerConnection::GatheringState::Complete)
                Impl::post(state, [](auto* target) { emit target->localCandidate({}, {}); });
        });
        impl_->peer->onStateChange([state](rtc::PeerConnection::State value) {
            if (value == rtc::PeerConnection::State::Failed ||
                value == rtc::PeerConnection::State::Closed)
                Impl::post(state, [](auto* target) { target->close(); });
        });
        impl_->peer->onIceStateChange([state](rtc::PeerConnection::IceState value) {
            Impl::post(state, [value](auto* target) {
                auto& impl = *target->impl_;
                const bool connected = value == rtc::PeerConnection::IceState::Connected ||
                                       value == rtc::PeerConnection::IceState::Completed;
                if (connected && !impl.iceConnected && impl.open)
                    impl.reportPath(target, true);
                impl.iceConnected = connected;
            });
        });
        impl_->peer->onDataChannel([state](std::shared_ptr<rtc::DataChannel> channel) {
            if (state->channels.fetch_add(1) >= 3) {
                channel->close();
                if (state->channels.load() == 4)
                    Impl::post(state, [](auto* target) { target->close(); });
                return;
            }
            Impl::post(state, [channel](auto* target) { target->impl_->attach(target, channel); });
        });
        impl_->peer->setRemoteDescription(rtc::Description(offer.toStdString(), "offer"));
        return true;
    } catch (const std::exception&) {
        // Native exception messages can contain IPs, SDP or credentials. Never log them.
        close();
        return false;
    }
}

bool WebRtcPeerLink::candidate(const QString& candidate, const QString& mid) {
    if (!impl_->peer || impl_->stopped || candidate.size() > 1024 || mid.size() > 64)
        return false;
    if (candidate.isEmpty())
        return true;
    try {
        impl_->peer->addRemoteCandidate(rtc::Candidate(candidate.toStdString(), mid.toStdString()));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool WebRtcPeerLink::send(const Packet& packet) {
    const auto& channel = impl_->channels.at(size_t(packet.channel));
    if (impl_->stopped || !channel || !channel->isOpen())
        return false;
    try {
        if (packet.binary)
            channel->send(reinterpret_cast<const std::byte*>(packet.bytes.constData()),
                          size_t(packet.bytes.size()));
        else
            channel->send(packet.bytes.toStdString());
        // libdatachannel false means accepted into its send buffer, not rejected.
        return true;
    } catch (const std::exception&) {
        close();
        return false;
    }
}

quint64 WebRtcPeerLink::buffered(Channel channel) const {
    const auto& ch = impl_->channels.at(size_t(channel));
    return ch ? ch->bufferedAmount() : 0;
}

void WebRtcPeerLink::close() {
    if (std::exchange(impl_->stopped, true))
        return;
    if (impl_->peer)
        impl_->peer->close();
    emit closed();
}
} // namespace ketplus::daemon::transport
