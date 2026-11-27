#include "PeerSessionFakes.h"

#include <QTest>

#include <type_traits>
#include <variant>

using namespace peer_session_test;
using protocol::Channel;

class PeerSessionLinkTest : public QObject {
    Q_OBJECT

  private slots:
    void aValidHelloIsWelcomedAndGetsTheSpec() {
        Host host;
        host.connect();
        QCOMPARE(host.sentTypes(), (QStringList{"welcome", "job.spec"}));
        const auto welcome = host.sentOf<protocol::Welcome>().first();
        QCOMPARE(welcome.connectionId, QStringLiteral("c1"));
        QCOMPARE(welcome.epoch, 1);
        QCOMPARE(welcome.capabilities, QStringList{"runner.protocol/1"});
        QCOMPARE(welcome.daemonVersion, daemonVersion);
        QCOMPARE(host.links.sent.first().channel, Channel::Control);
        const auto spec = host.sentOf<protocol::JobSpec>().first();
        protocol::JobSpec expected = makeSpec();
        expected.epoch = 1;
        QCOMPARE(spec, expected);
        QCOMPARE(host.records.calls,
                 (QStringList{"offer runner-1 1", "peerOpened runner-1", "dispatch runner-1"}));
        QVERIFY(!host.clock.armed(PeerTimer::Hello));
        QCOMPARE(host.session->phase(), JobPeerSession::Phase::Accepting);
    }

    void noHelloWithinTenSecondsClosesTheConnection() {
        Host host;
        host.open();
        QVERIFY(host.clock.armed(PeerTimer::Hello));
        host.advance(9'999);
        QVERIFY(host.links.closes.isEmpty());
        host.advance(1);
        QCOMPARE(host.links.closes, QStringList{"c1 hello_timeout"});
        QCOMPARE(host.records.calls, QStringList{"offer runner-1 1"});
        host.deliver(makeHello("c1"));
        QVERIFY(host.links.sent.isEmpty());
    }

    void aHelloOnTimeAfterNineSecondsIsWelcomed() {
        Host host;
        host.open();
        host.advance(9'999);
        host.deliver(makeHello("c1"));
        QCOMPARE(host.sentTypes().first(), QStringLiteral("welcome"));
        host.advance(60'000);
        QVERIFY(host.links.closes.isEmpty());
    }

    void aBadHelloClosesWithoutTouchingTheRecord_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("reason");
        QTest::newRow("connection") << "connectionId" << "hello_mismatch";
        QTest::newRow("runner") << "runnerId" << "hello_mismatch";
        QTest::newRow("job") << "jobId" << "hello_mismatch";
        QTest::newRow("attempt") << "attempt" << "hello_mismatch";
        QTest::newRow("labels") << "labels" << "label_mismatch";
        QTest::newRow("capability") << "capabilities" << "unsupported_protocol";
        QTest::newRow("oversize") << "snapshots" << "frame_too_large";
    }
    void aBadHelloClosesWithoutTouchingTheRecord() {
        QFETCH(QString, field);
        QFETCH(QString, reason);
        Host host;
        host.open();
        protocol::Hello hello = makeHello("c1");
        QByteArray message;
        if (field == "connectionId")
            hello.connectionId = "c9";
        else if (field == "runnerId")
            hello.runnerId = "runner-9";
        else if (field == "jobId")
            hello.jobId = "job-9";
        else if (field == "attempt")
            hello.attempt = 2;
        else if (field == "labels")
            hello.labels = {"gpu"};
        if (field == "capabilities") {
            message = encodeFromRunner(hello).replace("runner.protocol/1", "other.protocol/1");
        } else if (field == "snapshots") {
            message = encodeFromRunner(hello);
            message.chop(1);
            message += ",\"pad\":\"" + QByteArray(64 * 1024, 'x') + "\"}";
        } else {
            message = encodeFromRunner(hello);
        }
        host.deliverRaw(Channel::Control, message);
        QCOMPARE(host.links.closes, QStringList{"c1 " + reason});
        QVERIFY(host.links.sent.isEmpty());
        QCOMPARE(host.records.calls, QStringList{"offer runner-1 1"});
        QVERIFY(!host.clock.armed(PeerTimer::Hello));
    }

    void beforeHelloOnlyHelloAndCloseAreTaken_data() {
        QTest::addColumn<QString>("frame");
        QTest::addColumn<QString>("closed");
        QTest::newRow("ping") << "ping" << "c1 frame_before_hello";
        QTest::newRow("pong") << "pong" << "c1 frame_before_hello";
        QTest::newRow("accepted") << "accepted" << "c1 frame_before_hello";
        QTest::newRow("broken") << "broken" << "c1 invalid_frame";
        QTest::newRow("close") << "close" << "";
        QTest::newRow("unknown type") << "unknown" << "";
    }
    void beforeHelloOnlyHelloAndCloseAreTaken() {
        QFETCH(QString, frame);
        QFETCH(QString, closed);
        Host host;
        host.open();
        if (frame == "ping")
            host.deliver(protocol::Ping{std::nullopt, "n1"});
        else if (frame == "pong")
            host.deliver(protocol::Pong{std::nullopt, "n1"});
        else if (frame == "accepted")
            host.deliver(protocol::JobAccepted{jobId, 1});
        else if (frame == "broken")
            host.deliverRaw(Channel::Control, "{\"type\":");
        else if (frame == "close")
            host.deliver(protocol::Close{std::nullopt, "bye"});
        else
            host.deliverRaw(Channel::Control, R"({"type":"job.future"})");
        QCOMPARE(host.links.closes, closed.isEmpty() ? QStringList{} : QStringList{closed});
        QVERIFY(host.links.sent.isEmpty());
        QCOMPARE(host.records.calls, QStringList{"offer runner-1 1"});
        QCOMPARE(host.session->owns("c1"), frame == "unknown");
        QCOMPARE(host.clock.armed(PeerTimer::Hello), frame == "unknown");
    }

    void refusedOffersOpenNothing_data() {
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("answer");
        QTest::newRow("mismatch") << "job_mismatch" << "job_mismatch";
        QTest::newRow("unpaired") << "not_paired" << "not_paired";
        QTest::newRow("dispatched") << "already_dispatched" << "already_dispatched";
        QTest::newRow("store") << "storage_error" << "job_mismatch";
    }
    void refusedOffersOpenNothing() {
        QFETCH(QString, code);
        QFETCH(QString, answer);
        Host host;
        host.records.offerCode = code;
        QCOMPARE(host.session->offered("c1", runnerId, 1).code, answer);
        QVERIFY(!host.session->owns("c1"));
        host.session->channelsOpened("c1");
        host.deliver(makeHello("c1"));
        QVERIFY(host.links.sent.isEmpty());
        QVERIFY(!host.clock.armed(PeerTimer::Hello));
    }

    void aNewConnectionReplacesTheOldOneAtItsHello() {
        Host host;
        host.connect("c1");
        host.records.calls.clear();
        host.links.sent.clear();
        host.open("c2");
        host.deliver(protocol::JobAccepted{jobId, 1}, "c1"); // the old one still talks
        QCOMPARE(host.records.calls, (QStringList{"offer runner-1 1", "accepted runner-1"}));
        host.deliver(makeHello("c2"), "c2");
        QCOMPARE(host.links.closes, QStringList{"c1 superseded"});
        QVERIFY(!host.records.calls.join(' ').contains("peerLost"));
        QCOMPARE(host.session->openConnection(), QStringLiteral("c2"));
        const QList<SentFrame> sent = host.links.sent;
        QCOMPARE(sent.at(sent.size() - 2).connectionId, QStringLiteral("c2"));
        QCOMPARE(host.sentTypes().mid(host.sentTypes().size() - 2),
                 (QStringList{"welcome", "job.resume"}));
        QCOMPARE(host.sentOf<protocol::Welcome>().last().epoch, 2);
        QCOMPARE(host.sentOf<protocol::JobResume>().last().epoch, 2);
        QCOMPARE(host.snapshots.cancels, 1);
        const int calls = host.records.calls.size();
        host.deliver(protocol::JobStarted{jobId, "2026-11-18T09:00:00Z"}, "c1");
        host.session->connectionLost("c1");
        QCOMPARE(host.records.calls.size(), calls);
    }

    void aBadHelloOnANewConnectionClosesOnlyIt() {
        Host host;
        host.connect("c1");
        host.open("c2");
        host.deliver(makeHello("c9"), "c2");
        QCOMPARE(host.links.closes, QStringList{"c2 hello_mismatch"});
        QCOMPARE(host.session->openConnection(), QStringLiteral("c1"));
        host.deliver(protocol::JobAccepted{jobId, 1}, "c1");
        QVERIFY(host.records.calls.contains("accepted runner-1"));
    }

    void aLostConnectionIsReportedWithItsLastContact() {
        Host host;
        host.connect();
        host.advance(5'000);
        host.deliver(protocol::JobAccepted{jobId, 1});
        const QDateTime contact = host.clock.time;
        host.advance(7'000);
        host.session->connectionLost("c1");
        QVERIFY(host.records.calls.contains("peerLost runner-1"));
        QCOMPARE(host.records.lostAt, contact);
        QCOMPARE(host.snapshots.cancels, 1);
        QVERIFY(host.session->openConnection().isEmpty());
        host.session->connectionLost("c1");
        QCOMPARE(host.records.calls.count("peerLost runner-1"), 1);
    }

    void onlyTheLatestOfferIsKept() {
        Host host;
        QVERIFY(host.session->offered("c1", runnerId, 1).ok());
        QVERIFY(host.session->offered("c2", runnerId, 1).ok());
        QCOMPARE(host.links.closes, QStringList{"c1 superseded"});
        QVERIFY(!host.session->owns("c1"));
        host.session->offerEnded("c2");
        QVERIFY(!host.session->owns("c2"));
        host.session->channelsOpened("c2");
        QVERIFY(!host.clock.armed(PeerTimer::Hello));
        QVERIFY(host.session->offered("c3", runnerId, 1).ok());
        host.session->channelsOpened("c3");
        host.session->offerEnded("c3"); // too late: the channels are open
        QVERIFY(host.session->owns("c3"));
    }

    void untrustedFramesAfterHello_data() {
        QTest::addColumn<QString>("frame");
        QTest::addColumn<QString>("closed");
        QTest::addColumn<bool>("lost");
        QTest::newRow("not json") << "notjson" << "c1 invalid_frame" << true;
        QTest::newRow("bad base64") << "base64" << "c1 invalid_frame" << true;
        QTest::newRow("oversize") << "oversize" << "c1 frame_too_large" << true;
        QTest::newRow("wrong channel") << "channel" << "c1 wrong_route" << true;
        QTest::newRow("wrong direction") << "direction" << "c1 wrong_route" << true;
        QTest::newRow("unknown type") << "unknown" << "" << false;
        QTest::newRow("pong") << "pong" << "" << false;
        QTest::newRow("close") << "close" << "" << true;
    }
    void untrustedFramesAfterHello() {
        QFETCH(QString, frame);
        QFETCH(QString, closed);
        QFETCH(bool, lost);
        Host host;
        host.accept();
        const qsizetype sent = host.links.sent.size();
        const int writes = host.spool.writes;
        if (frame == "notjson") {
            host.deliverRaw(Channel::Events, "not json");
        } else if (frame == "base64") {
            host.deliverRaw(Channel::Events,
                            R"({"type":"job.output","jobId":"job-1","offset":0,"data":"*!*"})");
        } else if (frame == "oversize") {
            QByteArray message = encodeFromRunner(protocol::JobOutput{jobId, 0, "x"});
            message.chop(1);
            message += ",\"pad\":\"" + QByteArray(64 * 1024, 'x') + "\"}";
            host.deliverRaw(Channel::Events, message);
        } else if (frame == "channel") {
            host.deliverRaw(Channel::Control, encodeFromRunner(protocol::JobOutput{jobId, 0, "x"}));
        } else if (frame == "direction") {
            host.deliverRaw(Channel::Control,
                            protocol::encode(protocol::JobCancel{1, jobId, "user"},
                                             protocol::Direction::DesktopToRunner)
                                .message);
        } else if (frame == "unknown") {
            host.deliverRaw(Channel::Control, R"({"type":"job.future"})");
        } else if (frame == "pong") {
            host.deliver(protocol::Pong{std::nullopt, "n1"});
        } else {
            host.deliver(protocol::Close{std::nullopt, "bye"});
        }
        QCOMPARE(host.links.closes, closed.isEmpty() ? QStringList{} : QStringList{closed});
        QCOMPARE(host.records.calls.contains("peerLost runner-1"), lost);
        QCOMPARE(host.links.sent.size(), sent);
        QCOMPARE(host.spool.writes, writes);
        QCOMPARE(host.session->openConnection().isEmpty(), lost);
    }

    void everyFrameCarriesTheConnectionsEpoch() {
        Host host;
        host.accept();
        host.deliver(protocol::Ping{std::nullopt, "n1"});
        QCOMPARE(host.sentOf<protocol::Pong>(),
                 (QList<protocol::Pong>{{std::optional<qint64>(1), "n1"}}));
        host.open("c2");
        host.deliver(makeHello("c2"), "c2");
        host.deliver(protocol::Ping{std::nullopt, "n2"}, "c2");
        QCOMPARE(host.sentOf<protocol::Pong>().last().epoch, 2);
        QCOMPARE(host.links.undecodable, 0);
        for (const SentFrame& sent : host.links.sent) {
            const qint64 expected = sent.connectionId == "c1" ? 1 : 2;
            std::visit(
                [&](const auto& f) {
                    if constexpr (requires { f.epoch; }) {
                        if constexpr (std::is_same_v<std::decay_t<decltype(f.epoch)>, qint64>)
                            QCOMPARE(f.epoch, expected);
                        else
                            QCOMPARE(f.epoch, std::optional<qint64>(expected));
                    }
                },
                sent.frame);
        }
        // A new session after a restart starts again from 1.
        Host restarted;
        restarted.records.record = host.records.record;
        restarted.connect("c3");
        QCOMPARE(restarted.sentOf<protocol::Welcome>().first().epoch, 1);
    }

    void theLastContactIsTheHelloWhenNothingFollows() {
        Host host;
        host.open();
        host.advance(3'000);
        host.deliver(makeHello("c1"));
        const QDateTime hello = host.clock.time;
        host.advance(60'000);
        host.session->connectionLost("c1");
        QCOMPARE(host.records.lostAt, hello);
    }

    void aPendingConnectionLostIsNotReported() {
        Host host;
        host.open();
        host.session->connectionLost("c1");
        QCOMPARE(host.records.calls, QStringList{"offer runner-1 1"});
        QVERIFY(!host.clock.armed(PeerTimer::Hello));
    }
};

QTEST_GUILESS_MAIN(PeerSessionLinkTest)
#include "PeerSessionLinkTest.moc"
