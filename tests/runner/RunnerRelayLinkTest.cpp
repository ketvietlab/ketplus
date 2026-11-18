// The runner's Relay socket link over a fake socket, token source and clock.
#include "RunnerPeerTest.h"

#include <QJsonDocument>
#include <QtTest>

namespace runner_peer_test {
namespace {

QString text(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString ready(const char* role = "runner") {
    return text({{QStringLiteral("type"), QStringLiteral("ready")},
                 {QStringLiteral("role"), QString::fromLatin1(role)},
                 {QStringLiteral("heartbeatSeconds"), 25}});
}

// Backoff without jitter: 1, 2, 4, … seconds.
struct LinkHarness {
    LinkHarness() {
        clock.fire = [this](PeerTimer timer) { link.timerFired(timer); };
    }
    // The socket opened and said ready.
    void becomeReady() {
        link.socketOpened();
        link.socketText(ready());
    }
    void serverClose(int code) {
        socket.live = false;
        link.socketClosed(code);
    }
    void serverRefuse(int status) {
        socket.live = false;
        link.socketRefused(status);
    }

    FakeClock clock;
    FakeRelaySocket socket;
    FakeTokens tokens;
    RecordingRelayListener listener;
    RelayLink link{socket, tokens, clock, app::Backoff([] { return 0.0; }), listener, listener};
};

} // namespace

void RunnerRelayLinkTest::readyNeedsTheRunnerRole() {
    LinkHarness h;
    h.link.start();
    QCOMPARE(h.tokens.calls, QStringList{QStringLiteral("current")});
    QCOMPARE(h.socket.tokens.size(), 1);
    QVERIFY(h.socket.tokens.first().startsWith(QLatin1StringView(tokenMarker)));
    h.link.socketOpened();
    QVERIFY(!h.link.isReady());
    // Nothing before ready counts, not even a wake-up.
    h.link.socketText(text({{QStringLiteral("type"), QStringLiteral("job.available")},
                            {QStringLiteral("jobId"), QStringLiteral("job_1")}}));
    QVERIFY(h.listener.wakeups.isEmpty());
    h.link.socketText(ready("desktop"));
    QVERIFY(!h.link.isReady());
    QVERIFY(!h.socket.live);
    QCOMPARE(h.clock.remaining(PeerTimer::RelayRetry), 1'000);
    h.clock.advance(1'000);
    QCOMPARE(h.socket.tokens.size(), 2);
    h.becomeReady();
    QVERIFY(h.link.isReady());
    QCOMPARE(h.listener.events, QStringList{QStringLiteral("ready")});
    QVERIFY(!h.clock.armed(PeerTimer::RelayReady));
    QVERIFY(h.link.send({{QStringLiteral("type"), QStringLiteral("x")}}));
    QCOMPARE(h.socket.openedWhileLive, 0);
}

void RunnerRelayLinkTest::readyMustComeInTime() {
    LinkHarness h;
    h.link.start();
    h.link.socketOpened();
    QCOMPARE(h.clock.remaining(PeerTimer::RelayReady), relayReadyTimeoutMs);
    h.clock.advance(relayReadyTimeoutMs);
    QVERIFY(!h.socket.live);
    QCOMPARE(h.socket.closes, 1);
    QVERIFY(h.listener.events.isEmpty());
    QCOMPARE(h.clock.remaining(PeerTimer::RelayRetry), 1'000);
    QVERIFY(!h.link.send({{QStringLiteral("type"), QStringLiteral("x")}}));
}

void RunnerRelayLinkTest::unauthorizedRenewsOnceThenBacksOff() {
    LinkHarness h;
    h.link.start();
    h.serverRefuse(401);
    QCOMPARE(h.listener.events, QStringList{QStringLiteral("refused:401")});
    // Renewed and reopened at once.
    QCOMPARE(h.tokens.calls, (QStringList{QStringLiteral("current"), QStringLiteral("renew")}));
    QCOMPARE(h.socket.tokens.size(), 2);
    QVERIFY(!h.clock.armed(PeerTimer::RelayRetry));
    h.serverRefuse(401);
    QCOMPARE(h.socket.tokens.size(), 2);
    QVERIFY(h.clock.armed(PeerTimer::RelayRetry));
    h.clock.advance(h.clock.remaining(PeerTimer::RelayRetry));
    QCOMPARE(h.tokens.calls.last(), QStringLiteral("current"));
    QCOMPARE(h.socket.tokens.size(), 3);
    // Still refused: still on the backoff, never at once.
    h.serverRefuse(401);
    QCOMPARE(h.socket.tokens.size(), 3);
    QVERIFY(h.clock.armed(PeerTimer::RelayRetry));
    QCOMPARE(h.socket.openedWhileLive, 0);
}

void RunnerRelayLinkTest::forbiddenAndUnknownRefusalsBackOff() {
    LinkHarness h;
    h.link.start();
    h.serverRefuse(403);
    QCOMPARE(h.listener.events, QStringList{QStringLiteral("refused:403")});
    QCOMPARE(h.clock.remaining(PeerTimer::RelayRetry), 1'000);
    h.clock.advance(1'000);
    QCOMPARE(h.tokens.calls.last(), QStringLiteral("current"));
    // The first unreadable refusal of a chain renews the token, if 403 started it or not.
    h.becomeReady();
    h.serverClose(1006);
    h.clock.advance(h.clock.remaining(PeerTimer::RelayRetry));
    const qsizetype before = h.tokens.calls.size();
    h.serverRefuse(0);
    QCOMPARE(h.listener.events.last(), QStringLiteral("refused:0"));
    h.clock.advance(h.clock.remaining(PeerTimer::RelayRetry));
    QCOMPARE(h.tokens.calls.mid(before), QStringList{QStringLiteral("current")});

    LinkHarness fresh;
    fresh.link.start();
    fresh.serverRefuse(0);
    QCOMPARE(fresh.clock.remaining(PeerTimer::RelayRetry), 1'000);
    fresh.clock.advance(1'000);
    QCOMPARE(fresh.tokens.calls.last(), QStringLiteral("renew"));
    fresh.serverRefuse(0);
    QCOMPARE(fresh.clock.remaining(PeerTimer::RelayRetry), 2'000);
    fresh.clock.advance(2'000);
    QCOMPARE(fresh.tokens.calls.last(), QStringLiteral("current"));
}

void RunnerRelayLinkTest::credentialExpiryRenewsAtOnceAfterAHealthySocket() {
    LinkHarness h;
    h.link.start();
    h.becomeReady();
    h.clock.advance(relayHealthyMs);
    h.serverClose(4001);
    QCOMPARE(h.listener.events, (QStringList{QStringLiteral("ready"), QStringLiteral("lost")}));
    QCOMPARE(h.tokens.calls.last(), QStringLiteral("renew"));
    QCOMPARE(h.socket.tokens.size(), 2);
    QVERIFY(!h.clock.armed(PeerTimer::RelayRetry));
}

void RunnerRelayLinkTest::credentialExpiryOnAYoungSocketBacksOff() {
    LinkHarness h;
    h.link.start();
    h.becomeReady();
    h.clock.advance(relayHealthyMs - 1);
    h.serverClose(4001);
    QCOMPARE(h.socket.tokens.size(), 1);
    QCOMPARE(h.clock.remaining(PeerTimer::RelayRetry), 1'000);
    h.clock.advance(1'000);
    QCOMPARE(h.tokens.calls.last(), QStringLiteral("renew"));
    QCOMPARE(h.socket.tokens.size(), 2);
}

void RunnerRelayLinkTest::removalStopsForGood() {
    LinkHarness h;
    h.link.start();
    h.becomeReady();
    h.serverClose(4003);
    QCOMPARE(h.listener.events, (QStringList{QStringLiteral("ready"), QStringLiteral("lost"),
                                             QStringLiteral("runnerRemoved")}));
    QVERIFY(h.clock.due.isEmpty());
    h.clock.advance(3'600'000);
    QCOMPARE(h.socket.tokens.size(), 1);
    QVERIFY(!h.link.isReady());
}

void RunnerRelayLinkTest::tokenSourceOutcomes() {
    LinkHarness h;
    h.tokens.autoAnswer = false;
    h.link.start();
    h.tokens.answer(TokenSource::Outcome::Retry);
    QVERIFY(h.socket.tokens.isEmpty());
    QCOMPARE(h.clock.remaining(PeerTimer::RelayRetry), 1'000);
    h.clock.advance(1'000);
    h.tokens.answer(TokenSource::Outcome::InvalidAccess);
    QCOMPARE(h.listener.events, QStringList{QStringLiteral("tokenRejected")});
    QVERIFY(h.clock.due.isEmpty());
    QVERIFY(h.socket.tokens.isEmpty());

    LinkHarness removed;
    removed.tokens.autoAnswer = false;
    removed.link.start();
    removed.tokens.answer(TokenSource::Outcome::RunnerRemoved);
    QCOMPARE(removed.listener.events, QStringList{QStringLiteral("runnerRemoved")});
    QVERIFY(removed.clock.due.isEmpty());
    // A stopped link starts again on request.
    removed.tokens.autoAnswer = true;
    removed.link.start();
    QCOMPARE(removed.socket.tokens.size(), 1);
}

void RunnerRelayLinkTest::backoffRestartsOnlyAfterAHealthySocket() {
    LinkHarness h;
    h.link.start();
    h.serverRefuse(502);
    QCOMPARE(h.clock.remaining(PeerTimer::RelayRetry), 1'000);
    h.clock.advance(1'000);
    h.serverClose(1006); // closed before ready
    QCOMPARE(h.clock.remaining(PeerTimer::RelayRetry), 2'000);
    h.clock.advance(2'000);
    h.becomeReady();
    h.clock.advance(relayHealthyMs - 1);
    h.serverClose(1000);
    QCOMPARE(h.clock.remaining(PeerTimer::RelayRetry), 4'000);
    h.clock.advance(4'000);
    h.becomeReady();
    h.clock.advance(relayHealthyMs);
    h.serverClose(1001);
    QCOMPARE(h.clock.remaining(PeerTimer::RelayRetry), 1'000);
    QCOMPARE(h.socket.openedWhileLive, 0);
}

void RunnerRelayLinkTest::stopAndStartKeepOneSocket() {
    LinkHarness h;
    h.link.start();
    h.link.start(); // already running
    QCOMPARE(h.socket.tokens.size(), 1);
    h.becomeReady();
    h.link.stop();
    QCOMPARE(h.listener.events, (QStringList{QStringLiteral("ready"), QStringLiteral("lost")}));
    QVERIFY(!h.socket.live);
    QVERIFY(h.clock.due.isEmpty());
    // Answers of the closed socket change nothing.
    h.link.socketClosed(1006);
    h.link.socketText(ready());
    QCOMPARE(h.listener.events.size(), 2);
    h.link.start();
    h.becomeReady();
    QCOMPARE(h.socket.tokens.size(), 2);
    QCOMPARE(h.socket.openedWhileLive, 0);
}

void RunnerRelayLinkTest::lateTokenAnswersAreIgnored() {
    LinkHarness h;
    h.tokens.autoAnswer = false;
    h.link.start();
    h.link.stop();
    h.tokens.answer(TokenSource::Outcome::Ok);
    QVERIFY(h.socket.tokens.isEmpty());
    {
        FakeTokens tokens;
        tokens.autoAnswer = false;
        FakeClock clock;
        FakeRelaySocket socket;
        RecordingRelayListener listener;
        {
            RelayLink link(socket, tokens, clock, app::Backoff([] { return 0.0; }), listener,
                           listener);
            link.start();
        }
        tokens.answer(TokenSource::Outcome::Ok); // the link is gone
        QVERIFY(socket.tokens.isEmpty());
    }
}

void RunnerRelayLinkTest::wakeupsAreCheckedThenHandedOver_data() {
    QTest::addColumn<QString>("message");
    QTest::addColumn<QString>("expected"); // empty: dropped
    const auto row = [](const char* name, const QJsonObject& message, const char* expected) {
        QTest::newRow(name) << text(message) << QString::fromLatin1(expected);
    };
    const QString t = QStringLiteral("type");
    row("job.available", {{t, "job.available"}, {"jobId", "job_1"}}, "job.available job_1");
    row("job.available extra field", {{t, "job.available"}, {"jobId", "job_1"}, {"x", 1}},
        "job.available job_1");
    row("job.available no id", {{t, "job.available"}}, "");
    row("job.available empty id", {{t, "job.available"}, {"jobId", ""}}, "");
    row("job.available numeric id", {{t, "job.available"}, {"jobId", 7}}, "");
    row("job.cancel", {{t, "job.cancel"}, {"jobId", "job_1"}, {"leaseId", "lease_1"}},
        "job.cancel job_1 lease_1");
    row("job.cancel no lease", {{t, "job.cancel"}, {"jobId", "job_1"}}, "");
    row("job.cancel null lease", {{t, "job.cancel"}, {"jobId", "job_1"}, {"leaseId", QJsonValue()}},
        "");
    row("runner.paired",
        {{t, "runner.paired"},
         {"ticketId", "rpt_1"},
         {"pairState", "claimed"},
         {"approvalSignature", "c2ln"}},
        "runner.paired rpt_1 claimed c2ln");
    row("runner.paired no signature",
        {{t, "runner.paired"}, {"ticketId", "rpt_1"}, {"pairState", "claimed"}}, "");
    row("runner.paired array state",
        {{t, "runner.paired"},
         {"ticketId", "rpt_1"},
         {"pairState", QJsonArray{"claimed"}},
         {"approvalSignature", "c2ln"}},
        "");
    row("runner.unpaired", {{t, "runner.unpaired"}, {"machineId", "m_1"}}, "runner.unpaired m_1");
    row("runner.unpaired bool", {{t, "runner.unpaired"}, {"machineId", true}}, "");
    row("unknown type", {{t, "job.changed"}, {"jobId", "job_1"}}, "");
    row("no type", {{"jobId", "job_1"}}, "");
    QTest::newRow("not json") << QStringLiteral("{nope") << QString();
    QTest::newRow("array") << QStringLiteral("[1,2]") << QString();
}

void RunnerRelayLinkTest::wakeupsAreCheckedThenHandedOver() {
    QFETCH(QString, message);
    QFETCH(QString, expected);
    LinkHarness h;
    h.link.start();
    h.becomeReady();
    h.link.socketText(message);
    QCOMPARE(h.listener.wakeups, expected.isEmpty() ? QStringList{} : QStringList{expected});
    // Nothing closes the socket, valid or not.
    QVERIFY(h.link.isReady());
    QVERIFY(h.socket.live);
    QCOMPARE(h.socket.closes, 0);
    QVERIFY(h.listener.signals_.isEmpty());
}

void RunnerRelayLinkTest::signalsGoToTheirListener() {
    LinkHarness h;
    h.link.start();
    h.becomeReady();
    const QJsonObject started{{QStringLiteral("type"), QStringLiteral("signal.started")},
                              {QStringLiteral("connectionId"), QStringLiteral("pc_x")}};
    h.link.socketText(text(started));
    QCOMPARE(h.listener.signals_.size(), 1);
    QCOMPARE(h.listener.signals_.first(), started);
    QVERIFY(h.listener.wakeups.isEmpty());
}

} // namespace runner_peer_test
