// WebRTC DataChannel transport spike for ADR 0024.
//
//   p2p                      two peers in this process, direct candidates only
//   relay <turn-url>         two peers that signal only relayed candidates from that TURN server
//   serve [port]             answers a browser's offer over a local WebSocket (interop.html)
//
// Each peer pair opens a "control" and a "bulk" channel, measures control round trips idle and
// during a bulk transfer, sends the bulk transfer with backpressure and checks its SHA-256 at
// the receiver. One JSON line goes to stdout; the exit code says whether the path worked.
#include <rtc/rtc.hpp>

#include <openssl/evp.h>
#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr size_t mebibyte = 1024 * 1024;
constexpr size_t bulkChunkBytes = 64 * 1024;
constexpr size_t defaultBulkHighWater = 4 * mebibyte;
constexpr size_t pingBytes = 200;
constexpr size_t idlePings = 300;
constexpr size_t maximumPings = 20000;

double millisecondsSince(const Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

class Sha256 {
  public:
    Sha256() : context_(EVP_MD_CTX_new()) { EVP_DigestInit_ex(context_, EVP_sha256(), nullptr); }
    ~Sha256() { EVP_MD_CTX_free(context_); }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void add(const std::byte* data, const size_t size) { EVP_DigestUpdate(context_, data, size); }

    std::string hex() {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int size = 0;
        EVP_DigestFinal_ex(context_, digest, &size);
        static constexpr char digits[] = "0123456789abcdef";
        std::string text;
        for (unsigned int i = 0; i < size; ++i) {
            text += digits[digest[i] >> 4];
            text += digits[digest[i] & 15];
        }
        return text;
    }

  private:
    EVP_MD_CTX* context_;
};

// The byte at an offset of a bulk transfer: both ends produce or check it without keeping it.
void fillPattern(std::vector<std::byte>& chunk, const size_t offset) {
    for (size_t i = 0; i < chunk.size(); ++i)
        chunk[i] = std::byte(static_cast<unsigned char>(((offset + i) * 31 + 7) & 0xff));
}

class Event {
  public:
    void set() {
        {
            std::lock_guard lock(mutex_);
            set_ = true;
        }
        condition_.notify_all();
    }
    bool wait(const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return set_; });
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool set_{false};
};

// Hashes a bulk transfer as it arrives; `complete` runs once when the expected size is in.
class BulkSink {
  public:
    void expect(const size_t bytes, std::function<void(size_t, std::string)> complete = {}) {
        std::unique_lock lock(mutex_);
        expected_ = bytes;
        complete_ = std::move(complete);
        finishIfComplete(lock);
    }
    void add(const rtc::binary& data) {
        std::unique_lock lock(mutex_);
        digest_.add(data.data(), data.size());
        received_ += data.size();
        finishIfComplete(lock);
    }
    std::optional<std::string> wait(const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, timeout, [this] { return hex_.has_value(); }))
            return std::nullopt;
        return hex_;
    }

  private:
    void finishIfComplete(std::unique_lock<std::mutex>& lock) {
        if (!expected_ || received_ < *expected_ || hex_)
            return;
        hex_ = digest_.hex();
        auto complete = std::move(complete_);
        const size_t received = received_;
        const std::string hex = *hex_;
        lock.unlock();
        condition_.notify_all();
        if (complete)
            complete(received, hex);
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    Sha256 digest_;
    size_t received_{0};
    std::optional<size_t> expected_;
    std::optional<std::string> hex_;
    std::function<void(size_t, std::string)> complete_;
};

struct BulkSent {
    std::string hex;
    double milliseconds{0};
    size_t chunkBytes{0};
    size_t maximumBuffered{0};
};

// Sends `total` pattern bytes, waiting while more than `highWater` is buffered: without it a fast
// sender queues the whole transfer in memory ahead of the congestion window. The channel asks for
// more once a quarter of that is left.
BulkSent sendBulk(rtc::DataChannel& channel, const size_t total,
                  const size_t highWater = defaultBulkHighWater) {
    struct Drain {
        std::mutex mutex;
        std::condition_variable condition;
    };
    const auto drain = std::make_shared<Drain>();
    channel.setBufferedAmountLowThreshold(highWater / 4);
    channel.onBufferedAmountLow([drain] {
        {
            std::lock_guard lock(drain->mutex);
        }
        drain->condition.notify_all();
    });
    BulkSent sent;
    sent.chunkBytes = std::min(bulkChunkBytes, channel.maxMessageSize());
    Sha256 digest;
    std::vector<std::byte> chunk;
    const auto start = Clock::now();
    for (size_t offset = 0; offset < total; offset += chunk.size()) {
        chunk.resize(std::min(sent.chunkBytes, total - offset));
        fillPattern(chunk, offset);
        digest.add(chunk.data(), chunk.size());
        {
            std::unique_lock lock(drain->mutex);
            while (!drain->condition.wait_for(lock, 100ms, [&] {
                return channel.bufferedAmount() <= highWater || channel.isClosed();
            })) {
            }
        }
        if (channel.isClosed())
            throw std::runtime_error("bulk channel closed during the transfer");
        channel.send(chunk.data(), chunk.size());
        sent.maximumBuffered = std::max(sent.maximumBuffered, channel.bufferedAmount());
    }
    sent.milliseconds = millisecondsSince(start);
    sent.hex = digest.hex();
    channel.onBufferedAmountLow(nullptr);
    return sent;
}

// Numbered pings on the control channel; the far end echoes each one.
class Pinger {
  public:
    Pinger() : sent_(maximumPings), received_(maximumPings) {}

    void handle(const rtc::message_variant& message) {
        const auto* text = std::get_if<std::string>(&message);
        if (!text || text->rfind("ping ", 0) != 0)
            return;
        const size_t index = std::stoul(text->substr(5));
        {
            std::lock_guard lock(mutex_);
            if (index >= received_.size() || received_[index])
                return;
            received_[index] = Clock::now();
            ++answered_;
        }
        condition_.notify_all();
    }
    void ping(rtc::DataChannel& channel, const size_t index) {
        std::string text = "ping " + std::to_string(index) + " ";
        text.resize(pingBytes, 'x');
        {
            std::lock_guard lock(mutex_);
            sent_[index] = Clock::now();
        }
        channel.send(text);
    }
    bool waitFor(const size_t index, const std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] { return received_[index].has_value(); });
    }
    std::vector<double> roundTrips(const size_t begin, const size_t end) {
        std::lock_guard lock(mutex_);
        std::vector<double> values;
        for (size_t i = begin; i < end; ++i)
            if (sent_[i] && received_[i])
                values.push_back(
                    std::chrono::duration<double, std::milli>(*received_[i] - *sent_[i]).count());
        return values;
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::optional<Clock::time_point>> sent_;
    std::vector<std::optional<Clock::time_point>> received_;
    size_t answered_{0};
};

double percentile(std::vector<double> values, const double fraction) {
    if (values.empty())
        return -1;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<size_t>(fraction * double(values.size() - 1) + 0.5);
    return values[std::min(index, values.size() - 1)];
}

std::string roundTripJson(const std::vector<double>& values, const size_t sent) {
    std::ostringstream json;
    json.precision(3);
    json << std::fixed << "{\"sent\":" << sent << ",\"answered\":" << values.size()
         << ",\"p50Ms\":" << percentile(values, 0.5) << ",\"p95Ms\":" << percentile(values, 0.95)
         << ",\"p99Ms\":" << percentile(values, 0.99)
         << ",\"maxMs\":" << (values.empty() ? -1 : *std::max_element(values.begin(), values.end()))
         << "}";
    return json.str();
}

std::string candidateType(const rtc::Candidate& candidate) {
    switch (candidate.type()) {
    case rtc::Candidate::Type::Host:
        return "host";
    case rtc::Candidate::Type::ServerReflexive:
        return "srflx";
    case rtc::Candidate::Type::PeerReflexive:
        return "prflx";
    case rtc::Candidate::Type::Relayed:
        return "relay";
    default:
        return "unknown";
    }
}

// "candidate:<foundation> <component> <transport> ..." -> "udp" or "tcp".
std::string candidateTransport(const rtc::Candidate& candidate) {
    std::istringstream fields(candidate.candidate());
    std::string foundation, component, transport;
    fields >> foundation >> component >> transport;
    std::transform(transport.begin(), transport.end(), transport.begin(),
                   [](const unsigned char c) { return char(std::tolower(c)); });
    return transport;
}

std::string countsJson(const std::map<std::string, int>& counts) {
    std::ostringstream json;
    json << "{";
    bool first = true;
    for (const auto& [key, count] : counts) {
        json << (first ? "" : ",") << "\"" << key << "\":" << count;
        first = false;
    }
    json << "}";
    return json.str();
}

long maximumResidentKiB() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
    return usage.ru_maxrss / 1024;
#else
    return usage.ru_maxrss;
#endif
}

// An offerer (the host) and an answerer (a client) wired to each other in this process.
class Link {
  public:
    explicit Link(const rtc::Configuration& configuration)
        : offerer_(std::make_shared<rtc::PeerConnection>(configuration)),
          answerer_(std::make_shared<rtc::PeerConnection>(configuration)) {
        wire(offerer_, answerer_, offererCandidates_);
        wire(answerer_, offerer_, answererCandidates_);
        answerer_->onDataChannel(
            [this](std::shared_ptr<rtc::DataChannel> channel) { accept(std::move(channel)); });
    }
    ~Link() {
        for (const auto& channel : {control_, bulk_})
            if (channel)
                channel->resetCallbacks();
        {
            std::lock_guard lock(mutex_);
            for (const auto& channel : accepted_)
                channel->resetCallbacks();
        }
        offerer_->resetCallbacks();
        answerer_->resetCallbacks();
        offerer_->close();
        answerer_->close();
    }

    bool open(const std::chrono::milliseconds timeout) {
        control_ = offerer_->createDataChannel("control");
        bulk_ = offerer_->createDataChannel("bulk");
        control_->onOpen([this] { controlOpen_.set(); });
        bulk_->onOpen([this] { bulkOpen_.set(); });
        control_->onMessage([this](rtc::message_variant message) { pinger_.handle(message); });
        return controlOpen_.wait(timeout) && bulkOpen_.wait(timeout) &&
               answererReady_.wait(timeout);
    }

    std::string run(const size_t bulkBytes, const size_t highWater, const double openMilliseconds) {
        std::ostringstream json;
        json.precision(3);
        json << std::fixed;
        rtc::Candidate local, remote;
        if (offerer_->getSelectedCandidatePair(&local, &remote))
            json << "\"selected\":{\"local\":\"" << candidateType(local) << "/"
                 << candidateTransport(local) << "\",\"remote\":\"" << candidateType(remote) << "/"
                 << candidateTransport(remote) << "\"},";
        // Traffic crosses the TURN server when either end of the pair is relayed. A relay-only
        // policy signals only relayed candidates, yet both ICE backends may still reach the far
        // end's relay from a local host address when the network allows it.
        throughTurn_ = local.type() == rtc::Candidate::Type::Relayed ||
                       remote.type() == rtc::Candidate::Type::Relayed;
        json << "\"openMs\":" << openMilliseconds
             << ",\"maxMessageSize\":" << control_->maxMessageSize() << ",";

        for (size_t i = 0; i < idlePings; ++i) {
            pinger_.ping(*control_, i);
            if (!pinger_.waitFor(i, 5s))
                break;
        }
        json << "\"idleRoundTrip\":" << roundTripJson(pinger_.roundTrips(0, idlePings), idlePings)
             << ",";

        // Control pings every 10 ms while the bulk transfer runs, on their own channel.
        std::atomic<bool> transferring{true};
        std::atomic<size_t> busyPings{0};
        std::thread pings([&] {
            for (size_t i = idlePings; transferring && i < maximumPings; ++i) {
                pinger_.ping(*control_, i);
                busyPings = i + 1 - idlePings;
                std::this_thread::sleep_for(10ms);
            }
        });
        bulkSink_.expect(bulkBytes);
        BulkSent sent;
        std::optional<std::string> received;
        try {
            sent = sendBulk(*bulk_, bulkBytes, highWater);
            received = bulkSink_.wait(120s);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "bulk transfer failed: %s\n", error.what());
        }
        transferring = false;
        pings.join();
        std::this_thread::sleep_for(500ms);
        const double seconds = sent.milliseconds / 1000.0;
        bulkIntact_ = received && *received == sent.hex;
        json << "\"bulk\":{\"bytes\":" << bulkBytes << ",\"chunkBytes\":" << sent.chunkBytes
             << ",\"highWaterBytes\":" << highWater
             << ",\"maximumBufferedBytes\":" << sent.maximumBuffered
             << ",\"sendMs\":" << sent.milliseconds
             << ",\"mibPerSecond\":" << (seconds > 0 ? double(bulkBytes) / mebibyte / seconds : 0)
             << ",\"sha256Match\":" << (bulkIntact_ ? "true" : "false") << "},";
        json << "\"roundTripDuringBulk\":"
             << roundTripJson(pinger_.roundTrips(idlePings, idlePings + busyPings), busyPings)
             << ",";
        json << "\"bytesSent\":" << offerer_->bytesSent()
             << ",\"bytesReceived\":" << offerer_->bytesReceived();
        return json.str();
    }

    std::string candidatesJson() {
        std::lock_guard lock(mutex_);
        return "{\"offerer\":" + countsJson(offererCandidates_) +
               ",\"answerer\":" + countsJson(answererCandidates_) + "}";
    }
    bool bulkIntact() const { return bulkIntact_; }
    bool throughTurn() const { return throughTurn_; }

  private:
    void wire(const std::shared_ptr<rtc::PeerConnection>& from,
              const std::shared_ptr<rtc::PeerConnection>& to,
              std::map<std::string, int>& gathered) {
        from->onLocalDescription([peer = std::weak_ptr(to)](rtc::Description description) {
            if (const auto remote = peer.lock())
                remote->setRemoteDescription(std::move(description));
        });
        from->onLocalCandidate(
            [this, &gathered, peer = std::weak_ptr(to)](rtc::Candidate candidate) {
                {
                    std::lock_guard lock(mutex_);
                    ++gathered[candidateType(candidate) + "/" + candidateTransport(candidate)];
                }
                if (const auto remote = peer.lock()) {
                    try {
                        remote->addRemoteCandidate(std::move(candidate));
                    } catch (const std::exception& error) {
                        std::fprintf(stderr, "remote candidate refused: %s\n", error.what());
                    }
                }
            });
    }

    void accept(std::shared_ptr<rtc::DataChannel> channel) {
        if (channel->label() == "control") {
            channel->onMessage([echo = std::weak_ptr(channel)](rtc::message_variant message) {
                if (const auto control = echo.lock())
                    control->send(std::move(message));
            });
        } else if (channel->label() == "bulk") {
            channel->onMessage([this](rtc::message_variant message) {
                if (const auto* data = std::get_if<rtc::binary>(&message))
                    bulkSink_.add(*data);
            });
        }
        std::lock_guard lock(mutex_);
        accepted_.push_back(std::move(channel));
        if (accepted_.size() == 2)
            answererReady_.set();
    }

    std::shared_ptr<rtc::PeerConnection> offerer_;
    std::shared_ptr<rtc::PeerConnection> answerer_;
    std::shared_ptr<rtc::DataChannel> control_;
    std::shared_ptr<rtc::DataChannel> bulk_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<rtc::DataChannel>> accepted_;
    std::map<std::string, int> offererCandidates_;
    std::map<std::string, int> answererCandidates_;
    Event controlOpen_;
    Event bulkOpen_;
    Event answererReady_;
    Pinger pinger_;
    BulkSink bulkSink_;
    bool bulkIntact_{false};
    bool throughTurn_{false};
};

struct Options {
    std::string mode;
    std::string turnUrl;
    size_t bulkMiB{64};
    size_t highWaterKiB{defaultBulkHighWater / 1024};
    bool expectNoConnection{false};
    int connectSeconds{15};
    uint16_t port{8765};
    std::string bindAddress{"127.0.0.1"};
    int serveSeconds{300};
    bool verbose{false};
};

int runLink(const Options& options) {
    rtc::Configuration configuration;
    if (options.mode == "relay") {
        configuration.iceServers.emplace_back(options.turnUrl);
        configuration.iceTransportPolicy = rtc::TransportPolicy::Relay;
    }
    Link link(configuration);
    const auto start = Clock::now();
    const bool opened = link.open(std::chrono::seconds(options.connectSeconds));
    const double openMilliseconds = millisecondsSince(start);
    std::string result =
        "{\"mode\":\"" + options.mode + "\",\"opened\":" + (opened ? "true" : "false");
    if (opened)
        result += "," + link.run(options.bulkMiB * mebibyte, options.highWaterKiB * 1024,
                                 openMilliseconds);
    result += ",\"candidates\":" + link.candidatesJson() +
              ",\"maxRssKiB\":" + std::to_string(maximumResidentKiB()) + "}";
    std::printf("%s\n", result.c_str());
    std::fflush(stdout);
    if (options.expectNoConnection)
        return opened ? 1 : 0;
    if (!opened || !link.bulkIntact())
        return 1;
    return options.mode == "relay" && !link.throughTurn() ? 1 : 0;
}

// One browser tab: answers its offer once gathering is complete (no trickle over the socket),
// echoes control pings and runs the bulk commands interop.html sends.
class BrowserPeer {
  public:
    BrowserPeer(std::shared_ptr<rtc::WebSocket> socket, Event& finished, const size_t highWater)
        : socket_(std::move(socket)), finished_(finished), highWater_(highWater) {
        socket_->onMessage([this](rtc::message_variant message) {
            if (const auto* text = std::get_if<std::string>(&message);
                text && text->rfind("offer\n", 0) == 0)
                answer(text->substr(6));
        });
    }
    ~BrowserPeer() {
        socket_->resetCallbacks();
        std::lock_guard lock(mutex_);
        for (const auto& channel : channels_)
            channel->resetCallbacks();
        if (pc_) {
            pc_->resetCallbacks();
            pc_->close();
        }
    }

  private:
    void answer(const std::string& offer) {
        pc_ = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});
        pc_->onGatheringStateChange([this](const rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete)
                if (const auto description = pc_->localDescription())
                    socket_->send("answer\n" + std::string(*description));
        });
        pc_->onDataChannel(
            [this](std::shared_ptr<rtc::DataChannel> channel) { accept(std::move(channel)); });
        pc_->setRemoteDescription(rtc::Description(offer, "offer"));
    }

    void accept(std::shared_ptr<rtc::DataChannel> channel) {
        if (channel->label() == "bulk") {
            channel->onMessage([this](rtc::message_variant message) {
                if (const auto* data = std::get_if<rtc::binary>(&message))
                    sink_->add(*data);
            });
        } else if (channel->label() == "control") {
            channel->onMessage(
                [this, control = std::weak_ptr(channel)](rtc::message_variant message) {
                    if (const auto channel = control.lock())
                        command(channel, std::move(message));
                });
        }
        std::lock_guard lock(mutex_);
        if (channel->label() == "bulk")
            bulk_ = channel;
        channels_.push_back(std::move(channel));
    }

    void command(const std::shared_ptr<rtc::DataChannel>& control, rtc::message_variant message) {
        const auto* text = std::get_if<std::string>(&message);
        if (!text)
            return;
        if (text->rfind("ping ", 0) == 0) {
            control->send(std::move(message));
        } else if (text->rfind("expect ", 0) == 0) {
            // The bulk stream may deliver before this command: the sink already counts it.
            sink_->expect(std::stoull(text->substr(7)),
                          [weak = std::weak_ptr(control)](size_t bytes, std::string hex) {
                              if (const auto channel = weak.lock())
                                  channel->send("received " + std::to_string(bytes) + " " + hex);
                          });
        } else if (text->rfind("send ", 0) == 0) {
            const size_t bytes = std::stoull(text->substr(5));
            std::shared_ptr<rtc::DataChannel> bulk;
            {
                std::lock_guard lock(mutex_);
                bulk = bulk_;
            }
            std::thread([bulk, control, bytes, highWater = highWater_] {
                const BulkSent sent = sendBulk(*bulk, bytes, highWater);
                control->send("sent " + std::to_string(bytes) + " " + sent.hex + " " +
                              std::to_string(sent.milliseconds));
            }).detach();
        } else if (text->rfind("report ", 0) == 0) {
            rtc::Candidate local, remote;
            std::string selected;
            if (pc_->getSelectedCandidatePair(&local, &remote))
                selected = candidateType(local) + "/" + candidateTransport(local) + " -> " +
                           candidateType(remote) + "/" + candidateTransport(remote);
            std::printf(
                "{\"mode\":\"serve\",\"hostSelected\":\"%s\",\"maxRssKiB\":%ld,\"browser\":%s}\n",
                selected.c_str(), maximumResidentKiB(), text->substr(7).c_str());
            std::fflush(stdout);
            finished_.set();
        }
    }

    std::shared_ptr<rtc::WebSocket> socket_;
    Event& finished_;
    const size_t highWater_;
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<rtc::DataChannel>> channels_;
    std::shared_ptr<rtc::DataChannel> bulk_;
    const std::shared_ptr<BulkSink> sink_ = std::make_shared<BulkSink>();
};

int serve(const Options& options) {
    rtc::WebSocketServerConfiguration configuration;
    configuration.port = options.port;
    configuration.bindAddress = options.bindAddress;
    rtc::WebSocketServer server(configuration);
    Event finished;
    std::mutex mutex;
    std::vector<std::unique_ptr<BrowserPeer>> peers;
    server.onClient([&](std::shared_ptr<rtc::WebSocket> socket) {
        std::lock_guard lock(mutex);
        peers.push_back(std::make_unique<BrowserPeer>(std::move(socket), finished,
                                                      options.highWaterKiB * 1024));
    });
    std::fprintf(stderr, "waiting for interop.html on ws://%s:%u\n", options.bindAddress.c_str(),
                 unsigned(server.port()));
    const bool done = finished.wait(std::chrono::seconds(options.serveSeconds));
    server.stop();
    std::lock_guard lock(mutex);
    peers.clear();
    return done ? 0 : 1;
}

std::optional<Options> parse(const int argc, char** argv) {
    if (argc < 2)
        return std::nullopt;
    Options options;
    options.mode = argv[1];
    int next = 2;
    if (options.mode == "relay") {
        if (argc < 3)
            return std::nullopt;
        options.turnUrl = argv[next++];
    } else if (options.mode == "serve" && argc > 2 && argv[2][0] != '-') {
        options.port = uint16_t(std::stoi(argv[next++]));
    } else if (options.mode != "p2p" && options.mode != "serve") {
        return std::nullopt;
    }
    for (; next < argc; ++next) {
        const std::string flag = argv[next];
        if (flag == "--expect-no-connection")
            options.expectNoConnection = true;
        else if (flag == "--bulk-mib" && next + 1 < argc)
            options.bulkMiB = std::stoul(argv[++next]);
        else if (flag == "--high-water-kib" && next + 1 < argc)
            options.highWaterKiB = std::max<size_t>(64, std::stoul(argv[++next]));
        else if (flag == "--connect-seconds" && next + 1 < argc)
            options.connectSeconds = std::stoi(argv[++next]);
        else if (flag == "--serve-seconds" && next + 1 < argc)
            options.serveSeconds = std::stoi(argv[++next]);
        else if (flag == "--bind" && next + 1 < argc)
            options.bindAddress = argv[++next];
        else if (flag == "--verbose")
            options.verbose = true;
        else
            return std::nullopt;
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parse(argc, argv);
    if (!options) {
        std::fprintf(stderr,
                     "usage: %s p2p|relay <turn-url>|serve [port] [--bulk-mib N] [--high-water-kib "
                     "N] [--connect-seconds N]"
                     " [--expect-no-connection] [--serve-seconds N] [--bind ADDRESS] [--verbose]\n",
                     argv[0]);
        return 2;
    }
    rtc::InitLogger(options->verbose ? rtc::LogLevel::Verbose : rtc::LogLevel::Warning);
    rtc::Preload();
    const int status = options->mode == "serve" ? serve(*options) : runLink(*options);
    rtc::Cleanup().wait_for(5s);
    return status;
}
