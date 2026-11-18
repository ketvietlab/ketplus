#include "RunnerSessionTest.h"

#include <QTest>

namespace runner_session_test {

using namespace protocol;
using Phase = session::JobSession::Phase;

void RunnerSessionHandshakeTest::helloCarriesTheJobAndSnapshots() {
    Host host;
    host.snapshots.held = {{QString(32, QLatin1Char('w')), QString(40, QLatin1Char('c'))}};
    QVERIFY(host.session->assign(assignment(QStringLiteral("job-9"), 2)));
    QCOMPARE(host.channel.connectionRequests, 1);
    QVERIFY(!host.session->assign(assignment(QStringLiteral("job-10")))); // one job at a time
    host.open(QStringLiteral("conn-7"));
    QCOMPARE(host.sentTypes(), QStringList{"hello"});
    const Hello hello = host.sentOf<Hello>().first();
    QCOMPARE(hello.connectionId, QStringLiteral("conn-7"));
    QCOMPARE(hello.capabilities, QStringList{"runner.protocol/1"});
    QCOMPARE(hello.runnerId, QStringLiteral("runner-1"));
    QCOMPARE(hello.runnerVersion, QStringLiteral("1.2.3"));
    QCOMPARE(hello.os, QStringLiteral("linux"));
    QCOMPARE(hello.arch, QStringLiteral("x86_64"));
    QCOMPARE(hello.labels, QStringList{"gpu"});
    QCOMPARE(hello.jobId, QStringLiteral("job-9"));
    QCOMPARE(hello.attempt, 2);
    QCOMPARE(hello.snapshots, host.snapshots.held);
    QCOMPARE(host.channel.sent.first().first, Channel::Control);
    host.welcome();
    QCOMPARE(host.clock.remaining(SessionTimer::PeerOpen), 60'000); // now for the spec
    QVERIFY(host.channel.closes.isEmpty());
    host.deliver(makeSpec(QStringLiteral("job-9"), 2));
    QVERIFY(!host.clock.armed(SessionTimer::PeerOpen));
}

void RunnerSessionHandshakeTest::welcomeWithoutTheProtocolReleases() {
    Host host;
    QVERIFY(host.session->assign(assignment()));
    host.open();
    host.welcome({QStringLiteral("runner.protocol/2")});
    QCOMPARE(host.channel.closes, QStringList{"unsupported_protocol"});
    QCOMPARE(host.lease.calls, QStringList{"release peer_unreachable"});
    QCOMPARE(host.session->phase(), Phase::Idle);
    QVERIFY(host.clock.deadlines.isEmpty());
}

void RunnerSessionHandshakeTest::framesBeforeWelcomeClose() {
    Host host;
    QVERIFY(host.session->assign(assignment()));
    host.open();
    host.deliver(makeSpec());
    QCOMPARE(host.channel.closes, QStringList{"frame_before_welcome"});
    QCOMPARE(host.lease.calls, QStringList{}); // nothing started
    QCOMPARE(host.session->phase(), Phase::Assigned);
    // The job keeps its lease and asks for a new connection 10 seconds later.
    QCOMPARE(host.clock.remaining(SessionTimer::Reconnect), 10'000);
    // A `ping` before `welcome` is fine.
    host.open(QStringLiteral("c2"));
    host.clearSent();
    host.deliver(Ping{7, QStringLiteral("n1")});
    QCOMPARE(host.sentTypes(), QStringList{"pong"});
    QCOMPARE(host.channel.closes.size(), 1);
}

void RunnerSessionHandshakeTest::brokenFramesClose_data() {
    QTest::addColumn<int>("channel");
    QTest::addColumn<QByteArray>("message");
    QTest::addColumn<QString>("reason");
    const QByteArray big =
        R"({"type":"job.cancel","epoch":7,"jobId":"job-1","reason":"user","pad":")" +
        QByteArray(2000, 'x') + R"("})";
    QTest::newRow("oversize") << int(Channel::Control) << big << "frame_too_large";
    QTest::newRow("wrong channel")
        << int(Channel::Events)
        << QByteArray(R"({"type":"job.cancel","epoch":7,"jobId":"job-1","reason":"user"})")
        << "wrong_route";
    QTest::newRow("wrong direction")
        << int(Channel::Control) << QByteArray(R"({"type":"job.accepted","jobId":"j","attempt":1})")
        << "wrong_route";
    QTest::newRow("invalid field")
        << int(Channel::Control)
        << QByteArray(R"({"type":"job.cancel","epoch":7,"jobId":"job-1","reason":"bored"})")
        << "invalid_frame";
    QTest::newRow("not json") << int(Channel::Control) << QByteArray("{") << "invalid_frame";
    QTest::newRow("spec without a readable id")
        << int(Channel::Control) << QByteArray(R"({"type":"job.spec","epoch":7,"attempt":1})")
        << "invalid_frame";
}

void RunnerSessionHandshakeTest::brokenFramesClose() {
    QFETCH(int, channel);
    QFETCH(QByteArray, message);
    QFETCH(QString, reason);
    Host host;
    host.connect();
    host.deliverRaw(Channel(channel), message);
    QCOMPARE(host.channel.closes, QStringList{reason});
    QCOMPARE(host.session->phase(), Phase::Assigned);
    QVERIFY(host.command.launches.isEmpty());
}

void RunnerSessionHandshakeTest::unknownTypeOnControlIsRejected() {
    Host host;
    host.connect();
    host.clearSent();
    host.deliverRaw(Channel::Control, R"({"type":"job.teleport","epoch":7})");
    QCOMPARE(host.sentTypes(), QStringList{"job.rejected"});
    const JobRejected rejected = host.sentOf<JobRejected>().first();
    QCOMPARE(rejected.code, QStringLiteral("unsupported_frame"));
    QCOMPARE(rejected.jobId, QStringLiteral("job-1"));
    QVERIFY(host.channel.closes.isEmpty()); // the connection stays
    // Elsewhere an unknown type is ignored.
    host.clearSent();
    host.deliverRaw(Channel::Events, R"({"type":"job.teleport","epoch":7})");
    QVERIFY(host.channel.sent.isEmpty());
    QVERIFY(host.channel.closes.isEmpty());
}

void RunnerSessionHandshakeTest::noWelcomeWithinAMinuteReleases() {
    Host host;
    QVERIFY(host.session->assign(assignment()));
    host.advance(59'999);
    QCOMPARE(host.session->phase(), Phase::Assigned);
    host.open();
    host.advance(1);
    QCOMPARE(host.channel.closes, QStringList{"peer_unreachable"});
    QCOMPARE(host.lease.calls, QStringList{"release peer_unreachable"});
    QCOMPARE(host.session->phase(), Phase::Idle);
}

void RunnerSessionHandshakeTest::noSpecWithinAMinuteOfWelcomeReleases() {
    Host host;
    QVERIFY(host.session->assign(assignment()));
    host.advance(30'000);
    host.open();
    host.welcome(); // the window starts again from here
    host.advance(59'999);
    host.deliver(makeSpec(QStringLiteral("job-2"))); // not the leased job: the clock runs on
    QCOMPARE(host.sentOf<JobRejected>().first().code, QStringLiteral("job_mismatch"));
    QCOMPARE(host.session->phase(), Phase::Assigned);
    host.advance(1);
    QCOMPARE(host.channel.closes, QStringList{"peer_unreachable"});
    QCOMPARE(host.lease.calls, QStringList{"release peer_unreachable"});
    QCOMPARE(host.session->phase(), Phase::Idle);
    QVERIFY(host.clock.deadlines.isEmpty());
}

void RunnerSessionHandshakeTest::pingIsAnswered() {
    Host host;
    host.connect();
    host.clearSent();
    host.deliver(Ping{7, QStringLiteral("nonce-1")});
    QCOMPARE(host.sentOf<Pong>().first().nonce, QStringLiteral("nonce-1"));
    QVERIFY(!host.sentOf<Pong>().first().epoch); // the runner sends no epoch
}

} // namespace runner_session_test
