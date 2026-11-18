#include "RunnerRunTest.h"

#include "peer/WebRtcTransport.h"
#include "protocol/Capabilities.h"
#include "protocol/FrameRoutes.h"
#include "transport/WebRtcPeerLink.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
#include <QTimer>

// A job through the run loop over a real WebRTC connection on this machine: the test is the
// Relay and the desktop, answering the runner's signed offer with a real peer link.
namespace runner_run_test {

using ketplus::daemon::transport::Packet;
using ketplus::daemon::transport::WebRtcPeerLink;

namespace {

class LoopDesktop {
  public:
    explicit LoopDesktop(Run& run) : m_run(run) {}

    void attach() {
        m_run.socket->onSend = [this](const QJsonObject& message) {
            QTimer::singleShot(0, &m_context, [this, message] { handle(message); });
        };
    }

    void send(const protocol::Frame& frame) {
        const protocol::EncodeResult encoded =
            protocol::encode(frame, protocol::Direction::DesktopToRunner);
        const protocol::Route* route = protocol::routeFor(protocol::frameType(frame));
        QVERIFY(encoded.ok() && route);
        QVERIFY(link->send({route->channel == protocol::Channel::Control ? peer::Channel::Control
                                                                         : peer::Channel::Events,
                            encoded.message, false}));
    }

    [[nodiscard]] QStringList receivedTypes() const {
        QStringList types;
        for (const Packet& packet : received) {
            const auto decoded = protocol::decode(packet.bytes,
                                                  packet.channel == peer::Channel::Control
                                                      ? protocol::Channel::Control
                                                      : protocol::Channel::Events,
                                                  protocol::Direction::RunnerToDesktop);
            if (decoded.ok())
                types.append(QString(protocol::frameType(*decoded.frame)));
        }
        return types;
    }

    std::unique_ptr<WebRtcPeerLink> link;
    QList<Packet> received;
    QString connectionId = QStringLiteral("pc_loopbackloopbackloopback");

  private:
    void toRunner(const QJsonObject& message) {
        m_run.loop->socketText(
            QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }

    void handle(const QJsonObject& message) {
        const QString type = message.value("type").toString();
        if (type == "signal.start") {
            m_jobId = message.value("jobId").toString();
            toRunner({{"type", "signal.started"},
                      {"connectionId", connectionId},
                      {"iceServers", QJsonArray()}});
        } else if (type == "signal.offer") {
            offer(message.value("sdp").toString());
        } else if (type == "signal.candidate" && link) {
            link->candidate(message.value("candidate").toString(),
                            message.value("sdpMid").toString());
        }
    }

    void offer(const QString& offerSdp) {
        link = std::make_unique<WebRtcPeerLink>();
        QObject::connect(
            link.get(), &WebRtcPeerLink::localDescription, link.get(),
            [this, offerSdp](const QString& sdp) {
                toRunner(signedAnswerFor(connectionId, m_jobId, offerSdp, sdp, m_run.hub.now()));
            });
        QObject::connect(
            link.get(), &WebRtcPeerLink::localCandidate, link.get(),
            [this](const QString& candidate, const QString& mid) {
                toRunner({{"type", "signal.candidate"},
                          {"connectionId", connectionId},
                          {"candidate", candidate.isEmpty() ? QJsonValue() : QJsonValue(candidate)},
                          {"sdpMid", mid}});
            });
        QObject::connect(link.get(), &WebRtcPeerLink::received, link.get(),
                         [this](peer::Channel channel, const QByteArray& bytes, bool binary) {
                             received.append({channel, bytes, binary});
                         });
        link->answer(offerSdp, {});
    }

    Run& m_run;
    QString m_jobId;
    QObject m_context;
};

} // namespace

void RunnerRunJobTest::jobOverARealPeerConnection() {
    Run run;
    peer::WebRtcTransportFactory transports;
    run.realTransports = &transports;
    run.start();
    run.socketReady();
    LoopDesktop desktop(run);
    desktop.attach();
    run.acquire();
    QTRY_VERIFY_WITH_TIMEOUT(desktop.receivedTypes().contains(QStringLiteral("hello")), 15'000);
    desktop.send(protocol::Welcome{desktop.connectionId,
                                   7,
                                   {QString(protocol::capability::runnerProtocol)},
                                   QStringLiteral("9.9.9")});
    desktop.send(makeSpec());
    QTRY_COMPARE_WITH_TIMEOUT(run.receiver.begun.size(), 1, 5'000);
    run.receiver.ready(QStringLiteral("/work/space"), {});
    run.settle();
    QCOMPARE(run.command.launches.size(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(desktop.receivedTypes().contains(QStringLiteral("job.started")),
                             5'000);
    QVERIFY(desktop.receivedTypes().contains(QStringLiteral("job.accepted")));

    run.command.events.output("built\n");
    session::CommandExit exit;
    exit.exitCode = 0;
    exit.cleanupStatus = QStringLiteral("verified");
    run.command.events.finished(exit);
    run.settle();
    QTRY_VERIFY_WITH_TIMEOUT(desktop.receivedTypes().contains(QStringLiteral("job.result")), 5'000);
    QVERIFY(desktop.receivedTypes().contains(QStringLiteral("job.output")));
    desktop.send(protocol::JobResultAck{7, QStringLiteral("job_42")});
    QTRY_COMPARE_WITH_TIMEOUT(run.leaseKinds().value(2), QStringLiteral("finish succeeded exited"),
                              5'000);
    QCOMPARE(run.leaseKinds(), (QStringList{"acquire", "start", "finish succeeded exited"}));
    run.loop.reset();
    desktop.link.reset();
}

} // namespace runner_run_test
