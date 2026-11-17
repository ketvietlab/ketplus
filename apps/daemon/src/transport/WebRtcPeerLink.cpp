#include "transport/WebRtcPeerLink.h"
#include "transport/SelectedPath.h"

#include <QDebug>
#include <QJsonObject>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <rtc/rtc.hpp>
#include <utility>
#include <vector>

namespace ketplus::daemon::transport {
namespace {
// Only SHA-2 digests of at least 256 bits are accepted as an expected fingerprint.
QString fingerprintText(const rtc::CertificateFingerprint& value) {
    using Algorithm = rtc::CertificateFingerprint::Algorithm;
    if (!value.isValid() ||
        (value.algorithm != Algorithm::Sha256 && value.algorithm != Algorithm::Sha384 &&
         value.algorithm != Algorithm::Sha512))
        return {};
    return QString::fromStdString(
               rtc::CertificateFingerprint::AlgorithmIdentifier(value.algorithm)) +
           QLatin1Char(' ') + QString::fromStdString(value.value).toUpper();
}
} // namespace

struct WebRtcPeerLink::Impl {
    enum class Role { None, Answerer, Offerer };
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
    SelectedPathDecision pathDecision;
    Role role{Role::None};
    QString localFingerprint;
    // Offerer only: the caller's expected fingerprint, checked against the answer and then
    // against the certificate DTLS actually verified before any frame passes either way.
    QString expectedFingerprint;
    bool answered{false};
    bool verified{false};
    std::vector<rtc::Candidate> pendingCandidates;

    bool trusted(WebRtcPeerLink* owner) {
        if (role != Role::Offerer || verified)
            return true;
        QString actual;
        try {
            if (peer && answered)
                actual = fingerprintText(peer->remoteFingerprint());
        } catch (const std::exception&) {
        }
        if (actual.isEmpty() || actual != expectedFingerprint) {
            owner->close();
            return false;
        }
        verified = true;
        return true;
    }

    // Both roles use the same bounded configuration. Returns nothing for refused ICE servers.
    static std::optional<rtc::Configuration> configuration(const QJsonArray& iceServers) {
        rtc::Configuration config;
        config.disableFingerprintVerification = false;
        config.maxMessageSize = FrameCodec::messageLimit;
        if (iceServers.size() > 8)
            return std::nullopt;
        for (const auto& value : iceServers) {
            const auto server = value.toObject();
            auto urls = server.value("urls").toArray();
            if (server.value("urls").isString())
                urls = QJsonArray{server.value("urls")};
            if (urls.size() > 8)
                return std::nullopt;
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
        return config;
    }

    // Connection-level callbacks shared by both roles. Each only enqueues onto the Qt thread.
    void wire() {
        const auto state = delivery;
        peer->onLocalDescription([state](rtc::Description description) {
            const auto sdp = QString::fromStdString(std::string(description));
            const auto own = description.fingerprint();
            const auto print = own ? fingerprintText(*own) : QString();
            post(state, [sdp, print](auto* target) {
                target->impl_->localFingerprint = print;
                emit target->localDescription(sdp);
            });
        });
        peer->onLocalCandidate([state](rtc::Candidate candidate) {
            const auto text = QString::fromStdString(std::string(candidate));
            const auto mid = QString::fromStdString(candidate.mid());
            post(state, [text, mid](auto* target) { emit target->localCandidate(text, mid); });
        });
        peer->onGatheringStateChange([state](rtc::PeerConnection::GatheringState value) {
            if (value == rtc::PeerConnection::GatheringState::Complete)
                post(state, [](auto* target) { emit target->localCandidate({}, {}); });
        });
        peer->onStateChange([state](rtc::PeerConnection::State value) {
            if (value == rtc::PeerConnection::State::Failed ||
                value == rtc::PeerConnection::State::Closed)
                post(state, [](auto* target) { target->close(); });
        });
        peer->onIceStateChange([state](rtc::PeerConnection::IceState value) {
            post(state, [value](auto* target) {
                auto& impl = *target->impl_;
                const bool connected = value == rtc::PeerConnection::IceState::Connected ||
                                       value == rtc::PeerConnection::IceState::Completed;
                if (connected && impl.open)
                    impl.reportPath(target, !impl.iceConnected,
                                    value == rtc::PeerConnection::IceState::Completed);
                impl.iceConnected = connected;
            });
        });
    }

    void reportPath(WebRtcPeerLink* owner, bool reopened, bool completed = false) {
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
        const bool changed = pathDecision.logged && completed && !reopened;
        if (!pathDecision.shouldLog(localType, remoteType, reopened, completed))
            return;
        qInfo().noquote() << selectedPathLog(localType, remoteType, owner->diagnosticEpoch(),
                                             reopened, QDateTime::currentDateTimeUtc(), changed);
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
                    if (!impl.trusted(target))
                        return;
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
                if (!target->impl_->trusted(target))
                    return;
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
        auto config = Impl::configuration(iceServers);
        if (!config)
            return false;
        const auto state = impl_->delivery;
        impl_->role = Impl::Role::Answerer;
        impl_->peer = std::make_shared<rtc::PeerConnection>(*config);
        impl_->wire();
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

bool WebRtcPeerLink::offer(const QJsonArray& iceServers) {
    if (impl_->peer || impl_->stopped)
        return false;
    try {
        auto config = Impl::configuration(iceServers);
        if (!config)
            return false;
        // One explicit offer after all three channels exist, not one per channel.
        config->disableAutoNegotiation = true;
        const auto state = impl_->delivery;
        impl_->role = Impl::Role::Offerer;
        impl_->peer = std::make_shared<rtc::PeerConnection>(*config);
        impl_->wire();
        // The answering side never opens channels of its own.
        impl_->peer->onDataChannel([state](std::shared_ptr<rtc::DataChannel> channel) {
            channel->close();
            Impl::post(state, [](auto* target) { target->close(); });
        });
        for (const char* label : {"control", "events", "bulk"}) {
            impl_->attach(this, impl_->peer->createDataChannel(label));
            if (impl_->stopped)
                return false;
        }
        impl_->peer->setLocalDescription(rtc::Description::Type::Offer);
        return true;
    } catch (const std::exception&) {
        close();
        return false;
    }
}

bool WebRtcPeerLink::acceptAnswer(const QString& answer, const QString& expectedRemoteFingerprint) {
    if (impl_->role != Impl::Role::Offerer || impl_->answered || impl_->stopped)
        return false;
    const auto expected = normalizedFingerprint(expectedRemoteFingerprint);
    const auto refuse = [this] {
        close();
        return false;
    };
    if (expected.isEmpty() || answer.toUtf8().size() > 16 * 1024)
        return refuse();
    try {
        rtc::Description description(answer.toStdString(), "answer");
        const auto offered = description.fingerprint();
        // Refused before the answer is applied, so ICE and DTLS never start with that peer.
        if (!offered || fingerprintText(*offered) != expected)
            return refuse();
        impl_->expectedFingerprint = expected;
        impl_->answered = true;
        impl_->peer->setRemoteDescription(std::move(description));
        for (auto& pending : std::exchange(impl_->pendingCandidates, {}))
            impl_->peer->addRemoteCandidate(std::move(pending));
        return true;
    } catch (const std::exception&) {
        // Native exception messages can contain IPs, SDP or credentials. Never log them.
        return refuse();
    }
}

bool WebRtcPeerLink::candidate(const QString& candidate, const QString& mid) {
    if (!impl_->peer || impl_->stopped || candidate.size() > 1024 || mid.size() > 64)
        return false;
    if (candidate.isEmpty())
        return true;
    try {
        rtc::Candidate remote(candidate.toStdString(), mid.toStdString());
        // An offerer may hear candidates before the answer; keep a bounded few until then.
        if (impl_->role == Impl::Role::Offerer && !impl_->answered) {
            if (impl_->pendingCandidates.size() >= 32)
                return false;
            impl_->pendingCandidates.push_back(std::move(remote));
            return true;
        }
        impl_->peer->addRemoteCandidate(std::move(remote));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool WebRtcPeerLink::send(const Packet& packet) {
    const auto& channel = impl_->channels.at(size_t(packet.channel));
    if (impl_->stopped || !channel || !channel->isOpen())
        return false;
    // The offerer sends nothing before the peer's certificate matched the expected fingerprint.
    if (impl_->role == Impl::Role::Offerer && !impl_->open)
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

QString WebRtcPeerLink::localFingerprint() const { return impl_->localFingerprint; }

QString WebRtcPeerLink::normalizedFingerprint(const QString& fingerprint) {
    const auto parts = fingerprint.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() != 2 || parts.at(1).size() > 256)
        return {};
    using Algorithm = rtc::CertificateFingerprint::Algorithm;
    const auto name = parts.at(0).toLower().toStdString();
    for (const auto algorithm : {Algorithm::Sha256, Algorithm::Sha384, Algorithm::Sha512})
        if (name == rtc::CertificateFingerprint::AlgorithmIdentifier(algorithm))
            return fingerprintText({algorithm, parts.at(1).toStdString()});
    return {};
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
