// Real links on this machine: the runner's connector and channels (offer role) against the
// desktop's runner links (answer role) through an in-process Relay, with real DTLS.
#include "RunnerLinkTest.h"

#include "pairing/PairingSignatures.h"
#include "peer/PeerChannels.h"
#include "peer/PeerConnector.h"
#include "peer/QtPeerClock.h"
#include "peer/WebRtcTransport.h"
#include "runner/link/LinkAdapters.h"

#include <QtTest>

namespace runner_link_test {
namespace {

namespace peer = ketplus::runner::peer;
namespace protocol = ketplus::runner::protocol;
const QString kId = QString::fromLatin1(vectors::connectionId);

class RunnerSigner final : public peer::OfferSigner {
  public:
    [[nodiscard]] QString runnerId() const override {
        return QString::fromLatin1(vectors::runnerId);
    }
    [[nodiscard]] int runnerKeyVersion() const override { return vectors::runnerKeyVersion; }
    [[nodiscard]] QByteArray sign(const QByteArray& bytes) const override {
        return pairing::signPayload(QByteArray::fromHex(vectors::runnerSeed), bytes);
    }
};

class RunnerPinsOfDesktop final : public peer::PinLookup {
  public:
    [[nodiscard]] std::optional<pairing::RunnerPin>
    activePin(const QString& machineId) const override {
        if (machineId != QString::fromLatin1(vectors::machineId))
            return std::nullopt;
        pairing::RunnerPin pin;
        pin.machineId = machineId;
        pin.workspaceId = QString::fromLatin1(vectors::workspaceId);
        pin.machineKeyVersion = vectors::machineKeyVersion;
        pin.signingPublicKey = QByteArray::fromHex(vectors::desktopPublicKey);
        pin.state = pairing::RunnerPin::State::Active;
        pin.pairedAt = QDateTime::currentDateTimeUtc();
        return pin;
    }
};

class RunnerListener final : public peer::PeerChannelListener {
  public:
    void channelOpened(const QString& connectionId) override {
        events.append(QStringLiteral("opened ") + connectionId);
    }
    void channelLost(const QString& reason) override {
        events.append(QStringLiteral("lost ") + reason);
    }
    void frameReceived(protocol::Channel channel, const QByteArray& message) override {
        frames.append({channel, message});
    }
    void transferReceived(quint64 transferId, const QByteArray& bytes) override {
        transfers.append({transferId, bytes});
    }
    void eventsWritable() override {}
    void attemptFailed(const QString& reason) override {
        events.append(QStringLiteral("failed ") + reason);
    }
    void gaveUp(const QString& reason) override {
        events.append(QStringLiteral("gave up ") + reason);
    }

    QStringList events;
    QList<std::pair<protocol::Channel, QByteArray>> frames;
    QList<std::pair<quint64, QByteArray>> transfers;
};

// The Relay: answers signal.start, adds runnerId and iceServers to the runner's offer and
// passes every other message across, each on a later turn of the event loop.
class LoopRelay final : public peer::SignalSender {
  public:
    [[nodiscard]] bool relayReady() const override { return true; }
    bool sendSignal(const QJsonObject& message) override {
        const QJsonObject copy = message;
        QTimer::singleShot(0, &m_context, [this, copy] { fromRunner(copy); });
        return true;
    }

    std::function<void(const QJsonObject&)> toRunner;
    std::function<void(const QJsonObject&)> toDesktop;
    std::function<void(QJsonObject&)> rewriteOffer = [](QJsonObject&) {};
    QStringList desktopEnds;
    int answers = 0;
    QObject m_context;

  private:
    void fromRunner(QJsonObject message) {
        const QString type = message.value("type").toString();
        if (type == "signal.start") {
            toRunner({{"type", "signal.started"},
                      {"connectionId", kId},
                      {"iceServers", QJsonArray()},
                      {"expiresAt",
                       QDateTime::currentDateTimeUtc().addSecs(60).toString(Qt::ISODateWithMs)}});
            return;
        }
        if (type == "signal.offer") {
            message.insert("runnerId", QString::fromLatin1(vectors::runnerId));
            message.insert("iceServers", QJsonArray());
            rewriteOffer(message);
        }
        toDesktop(message);
    }
};

// Desktop to Relay.
class DesktopOut final : public SignalOut {
  public:
    explicit DesktopOut(LoopRelay& relay) : m_relay(relay) {}
    void sendSignal(const QJsonObject& message) override {
        const QJsonObject copy = message;
        QTimer::singleShot(0, &m_relay.m_context, [this, copy] {
            const QString type = copy.value("type").toString();
            if (type == "signal.end")
                m_relay.desktopEnds.append(copy.value("reason").toString());
            if (type == "signal.answer")
                ++m_relay.answers;
            m_relay.toRunner(copy);
        });
    }

  private:
    LoopRelay& m_relay;
};

struct Loop {
    Loop() {
        channels.attach(connector);
        relay.toRunner = [this](const QJsonObject& message) { connector.signal(message); };
        relay.toDesktop = [this](const QJsonObject& message) { links.signal(message); };
        desktopClock.setFired([this](const QString& id) { links.timerFired(id); });
    }
    void request() {
        const peer::PeerRequest request{QString::fromLatin1(vectors::jobId),
                                        QStringLiteral("lease_1"), 1,
                                        QString::fromLatin1(vectors::machineId)};
        channels.setRequest(request);
        connector.request(request);
    }

    // Runner side.
    RunnerListener runner;
    LoopRelay relay;
    peer::QtPeerClock runnerClock{[this](peer::PeerTimer timer) { connector.timerFired(timer); }};
    peer::WebRtcTransportFactory runnerTransports;
    RunnerSigner signer;
    RunnerPinsOfDesktop desktopPins;
    peer::PeerChannels channels{runnerClock, runner};
    peer::PeerConnector connector{relay,  runnerClock, runnerTransports,
                                  signer, desktopPins, channels};

    // Desktop side, with the production adapters.
    DesktopOut out{relay};
    QtLinkClock desktopClock;
    WebRtcAnswerTransportFactory desktopTransports;
    FunctionMachineIdentity identity{
        QString::fromLatin1(vectors::workspaceId), QString::fromLatin1(vectors::machineId),
        vectors::machineKeyVersion, [](const QByteArray& bytes) {
            return pairing::signPayload(QByteArray::fromHex(vectors::desktopSeed), bytes);
        }};
    FunctionRunnerPins pins{
        [](const QString& runnerId) -> std::optional<pairing::DesktopRunnerPin> {
            if (runnerId != QString::fromLatin1(vectors::runnerId))
                return std::nullopt;
            return runnerPin(runnerId);
        }};
    RecordingEvents desktop;
    RunnerLinks links{out, desktopClock, desktopTransports, identity, pins, desktop};
};

QByteArray pattern(const qsizetype size) {
    QByteArray bytes(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i)
        bytes[i] = static_cast<char>(i * 7 + 3);
    return bytes;
}

} // namespace

void RunnerLinkLoopbackTest::runnerAndDesktopExchangeFramesAndATransfer() {
    Loop loop;
    loop.request();
    QTRY_VERIFY_WITH_TIMEOUT(loop.connector.isOpen(), 15'000);
    QTRY_VERIFY_WITH_TIMEOUT(loop.desktop.events.contains("opened " + kId), 5'000);
    QCOMPARE(loop.desktop.events.first(),
             "offered " + kId + " " + QString::fromLatin1(vectors::runnerId) + " job_42 1");
    QCOMPARE(loop.runner.events, QStringList{"opened " + kId});
    QCOMPARE(loop.relay.answers, 1);
    QVERIFY(loop.relay.desktopEnds.isEmpty());

    // Runner to desktop on control and events.
    loop.channels.send(protocol::Channel::Control, "{\"type\":\"hello\"}");
    loop.channels.send(protocol::Channel::Events, "{\"type\":\"event\"}");
    QTRY_COMPARE_WITH_TIMEOUT(loop.desktop.frames.size(), 2, 5'000);
    QCOMPARE(std::get<1>(loop.desktop.frames[0]), FrameChannel::Control);
    QCOMPARE(std::get<2>(loop.desktop.frames[0]), QByteArray("{\"type\":\"hello\"}"));
    QCOMPARE(std::get<1>(loop.desktop.frames[1]), FrameChannel::Events);
    QCOMPARE(std::get<2>(loop.desktop.frames[1]), QByteArray("{\"type\":\"event\"}"));

    // Desktop to runner: frames, then a 4 MiB transfer.
    loop.links.send(kId, FrameChannel::Control, "{\"type\":\"welcome\"}");
    loop.links.send(kId, FrameChannel::Events, "{\"type\":\"tick\"}");
    QTRY_COMPARE_WITH_TIMEOUT(loop.runner.frames.size(), 2, 5'000);
    QCOMPARE(loop.runner.frames[0].first, protocol::Channel::Control);
    QCOMPARE(loop.runner.frames[0].second, QByteArray("{\"type\":\"welcome\"}"));
    QCOMPARE(loop.runner.frames[1].first, protocol::Channel::Events);

    const QByteArray bytes = pattern(transferSizeLimit);
    QVERIFY(loop.links.bulkWritable(kId));
    loop.links.sendBulk(kId, QStringLiteral("000000000000004d"), bytes);
    QTRY_COMPARE_WITH_TIMEOUT(loop.runner.transfers.size(), 1, 30'000);
    QCOMPARE(loop.runner.transfers[0].first, quint64(0x4d));
    QVERIFY(loop.runner.transfers[0].second == bytes);
    QCOMPARE(loop.runner.events, QStringList{"opened " + kId});
    QVERIFY(!loop.desktop.events.last().startsWith("lost"));

    // The desktop closing is reported once on the runner, never on the desktop.
    loop.links.close(kId, QStringLiteral("done"));
    QTRY_COMPARE_WITH_TIMEOUT(loop.runner.events.size(), 2, 10'000);
    QCOMPARE(loop.runner.events.last(), QStringLiteral("lost peer_closed"));
    QTest::qWait(200);
    QVERIFY(!loop.desktop.events.last().startsWith("lost"));
    QVERIFY(!loop.links.owns(kId));
    loop.connector.close(QStringLiteral("done"));
}

void RunnerLinkLoopbackTest::aTamperedOfferNeverOpens() {
    Loop loop;
    loop.relay.rewriteOffer = [](QJsonObject& offer) {
        offer["sdp"] = offer.value("sdp").toString() + QStringLiteral("a=x-tampered\r\n");
    };
    loop.request();
    QTRY_VERIFY_WITH_TIMEOUT(!loop.relay.desktopEnds.isEmpty(), 15'000);
    QCOMPARE(loop.relay.desktopEnds.first(), QStringLiteral("invalid_signature"));
    QTest::qWait(500);
    QCOMPARE(loop.relay.answers, 0);
    QVERIFY(loop.desktop.events.isEmpty());
    QVERIFY(!loop.connector.isOpen());
    QVERIFY(!loop.runner.events.contains("opened " + kId));
    QVERIFY(!loop.links.owns(kId));
    loop.connector.close(QStringLiteral("done"));
}

} // namespace runner_link_test
