// Candidates, deadlines, replacement and limits of runner attempts, over fakes with simulated
// time.
#include "RunnerLinkTest.h"

#include <QtTest>

namespace runner_link_test {
namespace {

const QString kId = QString::fromLatin1(vectors::connectionId);
const QString kRunner = QString::fromLatin1(vectors::runnerId);

QJsonObject candidate(const QString& id, const QJsonValue& text,
                      const QJsonValue& mid = QStringLiteral("0")) {
    QJsonObject message{{"type", "signal.candidate"}, {"connectionId", id}, {"candidate", text}};
    if (!mid.isUndefined())
        message.insert("sdpMid", mid);
    return message;
}

QString offeredLine(const QString& id, const QString& runner = kRunner) {
    return QStringLiteral("offered %1 %2 job_42 1").arg(id, runner);
}

} // namespace

void RunnerLinkLifecycleTest::passesCandidatesBothWays() {
    LinkHarness h;
    h.deliver(offer());
    TransportRecord& record = h.transports.last();
    h.deliver(candidate(kId, "candidate:1 1 udp 1 127.0.0.1 5000 typ host"));
    h.deliver(candidate(kId, "candidate:2 1 udp 1 127.0.0.1 5001 typ host", QJsonValue()));
    h.deliver(candidate(kId, QJsonValue::Null));
    h.deliver(candidate(connectionId(9), "candidate:3")); // another id: ignored
    QCOMPARE(record.candidates.size(), 3);
    QCOMPARE(record.candidates[0].second, QStringLiteral("0"));
    QCOMPARE(record.candidates[1].second, QString());
    QCOMPARE(record.candidates[2].first, QString());

    record.events->localCandidate(QStringLiteral("candidate:9 1 udp 1 10.0.0.1 6000 typ host"),
                                  QStringLiteral("0"));
    record.events->localCandidate(QString(), QStringLiteral("0"));
    record.events->localCandidate(QStringLiteral("candidate:after"), QStringLiteral("0"));
    const auto sent = h.out.ofType("signal.candidate");
    QCOMPARE(sent.size(), 2);
    QCOMPARE(sent[0], (QJsonObject{{"type", "signal.candidate"},
                                   {"connectionId", kId},
                                   {"candidate", "candidate:9 1 udp 1 10.0.0.1 6000 typ host"},
                                   {"sdpMid", "0"}}));
    QVERIFY(sent[1].value("candidate").isNull());
    QVERIFY(h.out.ends().isEmpty());

    // 64 of 1,024 bytes each are taken.
    LinkHarness full;
    full.deliver(offer());
    for (int n = 0; n < candidateLimit; ++n)
        full.deliver(candidate(kId, QString(candidateSizeLimit, 'c')));
    QCOMPARE(full.transports.last().candidates.size(), candidateLimit);
    QVERIFY(full.out.sent.isEmpty());
}

void RunnerLinkLifecycleTest::holdsCandidatesUntilTheOfferIsApplied() {
    LinkHarness h;
    // Candidates that arrive while the offer is being applied wait for it, in order.
    h.transports.duringAnswer = [&h] {
        QVERIFY(h.transports.last().candidates.isEmpty());
        h.deliver(candidate(kId, "candidate:1"));
        h.deliver(candidate(kId, "candidate:2"));
        h.deliver(candidate(kId, QJsonValue::Null));
        QVERIFY(h.transports.last().candidates.isEmpty());
    };
    h.deliver(offer());
    const auto& applied = h.transports.last().candidates;
    QCOMPARE(applied.size(), 3);
    QCOMPARE(applied[0].first, QStringLiteral("candidate:1"));
    QCOMPARE(applied[1].first, QStringLiteral("candidate:2"));
    QCOMPARE(applied[2].first, QString());

    // A held candidate the transport refuses ends the attempt.
    LinkHarness refused;
    refused.transports.duringAnswer = [&refused] {
        refused.transports.last().candidateOk = false;
        refused.deliver(candidate(kId, "candidate:1"));
    };
    refused.deliver(offer());
    QCOMPARE(refused.out.ends(), QStringList{"invalid_candidate"});
    QVERIFY(!refused.links.owns(kId));

    // Candidates of an id with no verified offer are dropped, never applied later.
    LinkHarness early;
    early.deliver(candidate(kId, "candidate:0"));
    early.deliver(offer());
    QVERIFY(early.transports.last().candidates.isEmpty());
    QVERIFY(early.out.sent.isEmpty());
}

void RunnerLinkLifecycleTest::refusesCandidates_data() {
    QTest::addColumn<QList<QJsonValue>>("texts");
    QTest::addColumn<QJsonValue>("mid");
    QTest::addColumn<bool>("transportOk");
    QTest::addColumn<QString>("reason");
    const QString invalid = QStringLiteral("invalid_candidate");
    const QJsonValue mid = QStringLiteral("0");
    QTest::newRow("a number") << QList<QJsonValue>{7} << mid << true << invalid;
    QTest::newRow("mid a number") << QList<QJsonValue>{"candidate:1"} << QJsonValue(0) << true
                                  << invalid;
    QTest::newRow("mid too long") << QList<QJsonValue>{"candidate:1"}
                                  << QJsonValue(QString(65, 'm')) << true << invalid;
    QTest::newRow("after the end")
        << QList<QJsonValue>{QJsonValue::Null, "candidate:1"} << mid << true << invalid;
    QTest::newRow("refused by the transport")
        << QList<QJsonValue>{"candidate:1"} << mid << false << invalid;
    QTest::newRow("1,025 bytes") << QList<QJsonValue>{QString(candidateSizeLimit + 1, 'c')} << mid
                                 << true << QStringLiteral("candidate_limit");
    QList<QJsonValue> many;
    for (int n = 0; n <= candidateLimit; ++n)
        many.append(QStringLiteral("candidate:%1").arg(n));
    QTest::newRow("65 candidates") << many << mid << true << QStringLiteral("candidate_limit");
}

void RunnerLinkLifecycleTest::refusesCandidates() {
    QFETCH(QList<QJsonValue>, texts);
    QFETCH(QJsonValue, mid);
    QFETCH(bool, transportOk);
    QFETCH(QString, reason);
    LinkHarness h;
    h.deliver(offer());
    h.transports.last().candidateOk = transportOk;
    for (const QJsonValue& text : texts)
        h.deliver(candidate(kId, text, mid));
    QCOMPARE(h.out.ends(), QStringList{reason});
    QCOMPARE(h.events.events.last(), "ended " + kId);
    QVERIFY(h.transports.last().closed);
    QVERIFY(!h.links.owns(kId));
}

void RunnerLinkLifecycleTest::endsAtLocalCandidateLimit() {
    LinkHarness h;
    h.deliver(offer());
    for (int n = 0; n <= candidateLimit; ++n)
        h.transports.last().events->localCandidate(QStringLiteral("candidate:%1").arg(n),
                                                   QStringLiteral("0"));
    QCOMPARE(h.out.ofType("signal.candidate").size(), candidateLimit);
    QCOMPARE(h.out.ends(), QStringList{"candidate_limit"});
    QCOMPARE(h.events.events.last(), "ended " + kId);
}

void RunnerLinkLifecycleTest::aBadCandidateAfterOpenIsLost() {
    LinkHarness h;
    h.open();
    h.deliver(candidate(kId, 7));
    QCOMPARE(h.events.events.last(), "lost " + kId + " invalid_candidate");
    QVERIFY(h.out.ends().isEmpty());
}

void RunnerLinkLifecycleTest::endsAtTheDeadline() {
    LinkHarness h;
    h.deliver(offer());
    h.clock.advance(attemptDeadlineMs - 1);
    QVERIFY(h.links.owns(kId));
    h.clock.advance(1);
    QCOMPARE(h.out.ends(), QStringList{"attempt_timeout"});
    QCOMPARE(h.events.events.last(), "ended " + kId);
    QVERIFY(h.transports.last().closed);

    // Opening disarms the deadline.
    LinkHarness open;
    open.open();
    QVERIFY(open.clock.due.isEmpty());
    open.clock.advance(10 * attemptDeadlineMs);
    QCOMPARE(open.events.events.last(), "opened " + kId);
}

void RunnerLinkLifecycleTest::newestOfferReplacesThePendingAttempt() {
    LinkHarness h;
    h.deliver(offer());
    h.clock.advance(5000);
    const QString second = connectionId(2);
    h.deliver(offer({.connectionId = second}));
    QCOMPARE(h.events.events, (QStringList{offeredLine(kId), "ended " + kId, offeredLine(second)}));
    QCOMPARE(h.out.sent.size(), 1);
    QCOMPARE(
        h.out.sent[0],
        (QJsonObject{{"type", "signal.end"}, {"connectionId", kId}, {"reason", "superseded"}}));
    QVERIFY(h.transports.at(0).closed);
    QVERIFY(!h.links.owns(kId));
    QVERIFY(h.links.owns(second));
    // The new attempt has its own 20 seconds.
    QCOMPARE(h.clock.due.value(second), 5000 + attemptDeadlineMs);

    // Another runner's attempt is left alone.
    h.deliver(offer({.connectionId = connectionId(3), .runnerId = runnerId(3)}));
    QVERIFY(h.links.owns(second));
}

void RunnerLinkLifecycleTest::aRefusedNewestOfferStillEndsTheOldAttempt() {
    LinkHarness h;
    h.deliver(offer());
    h.events.code = QStringLiteral("job_mismatch");
    h.deliver(offer({.connectionId = connectionId(2)}));
    QCOMPARE(h.out.ends(), (QStringList{"superseded", "job_mismatch"}));
    QCOMPARE(h.events.events,
             (QStringList{offeredLine(kId), "ended " + kId, offeredLine(connectionId(2))}));
    QVERIFY(!h.links.owns(kId));
}

void RunnerLinkLifecycleTest::anOpenConnectionStaysUntilTheNewOneOpens() {
    LinkHarness h;
    TransportRecord& first = h.open();
    const QString second = connectionId(2);
    h.deliver(offer({.connectionId = second}));
    QVERIFY(h.links.owns(kId));
    QVERIFY(!first.closed);
    TransportRecord& next = h.transports.last();
    next.events->localDescription(QString::fromLatin1(vectors::answerSdp));
    next.events->opened();
    QVERIFY(first.closed);
    QVERIFY(!h.links.owns(kId));
    QCOMPARE(h.events.events, (QStringList{offeredLine(kId), "opened " + kId, offeredLine(second),
                                           "opened " + second}));
    QVERIFY(h.out.ends().isEmpty());
}

void RunnerLinkLifecycleTest::aFailedNewAttemptLeavesTheOpenConnection() {
    LinkHarness h;
    TransportRecord& first = h.open();
    h.deliver(offer({.connectionId = connectionId(2)}));
    h.clock.advance(attemptDeadlineMs);
    QCOMPARE(h.out.ends(), QStringList{"attempt_timeout"});
    QVERIFY(h.links.owns(kId));
    QVERIFY(!first.closed);
    QCOMPARE(h.events.events.last(), "ended " + connectionId(2));
}

void RunnerLinkLifecycleTest::limitsAttemptsAndConnections() {
    LinkHarness h;
    for (int n = 0; n < pendingAttemptLimit; ++n)
        h.deliver(offer({.connectionId = connectionId(n), .runnerId = runnerId(n)}));
    QVERIFY(h.out.sent.isEmpty());
    h.deliver(offer({.connectionId = connectionId(100), .runnerId = runnerId(100)}));
    QCOMPARE(h.out.ends(), QStringList{"peer_limit"});
    QCOMPARE(h.events.events.size(), pendingAttemptLimit); // not asked
    QVERIFY(!h.links.owns(connectionId(100)));
    // A runner's newest offer still replaces its pending one.
    h.deliver(offer({.connectionId = connectionId(101), .runnerId = runnerId(0)}));
    QVERIFY(h.links.owns(connectionId(101)));

    LinkHarness open;
    for (int n = 0; n < openConnectionLimit; ++n)
        open.open({.connectionId = connectionId(n), .runnerId = runnerId(n)});
    QVERIFY(open.out.ends().isEmpty());
    open.deliver(offer({.connectionId = connectionId(100), .runnerId = runnerId(100)}));
    QCOMPARE(open.out.ends(), QStringList{"peer_limit"});
    // A runner with an open connection may offer again: it replaces its own connection.
    open.deliver(offer({.connectionId = connectionId(101), .runnerId = runnerId(0)}));
    QVERIFY(open.links.owns(connectionId(101)));
}

void RunnerLinkLifecycleTest::endsOnTheOtherSidesEnd_data() {
    QTest::addColumn<QJsonObject>("message");
    QTest::addColumn<QString>("logged");
    QTest::newRow("end") << QJsonObject{{"type", "signal.end"}, {"reason", "label_mismatch"}}
                         << QStringLiteral("label_mismatch");
    QTest::newRow("error") << QJsonObject{{"type", "signal.error"}, {"code", "rate_limited"}}
                           << QStringLiteral("rate_limited");
    QTest::newRow("error with spaces")
        << QJsonObject{{"type", "signal.error"}, {"code", "two words"}}
        << QStringLiteral("unknown");
    QTest::newRow("error too long")
        << QJsonObject{{"type", "signal.error"}, {"code", QString(65, 'x')}}
        << QStringLiteral("unknown");
    QTest::newRow("end without reason")
        << QJsonObject{{"type", "signal.end"}} << QStringLiteral("unknown");
}

void RunnerLinkLifecycleTest::endsOnTheOtherSidesEnd() {
    QFETCH(QJsonObject, message);
    QFETCH(QString, logged);
    LinkHarness h;
    h.deliver(offer());
    logLines().clear();
    message.insert("connectionId", kId);
    h.deliver(message);
    QVERIFY(h.out.sent.isEmpty()); // no end of its own
    QCOMPARE(h.events.events.last(), "ended " + kId);
    QVERIFY(h.transports.last().closed);
    QVERIFY(logLines().join('\n').contains("runner peer attempt ended by the other side: \"" +
                                           logged + "\""));
}

void RunnerLinkLifecycleTest::ignoresAnEndAfterOpen() {
    LinkHarness h;
    TransportRecord& record = h.open();
    h.deliver({{"type", "signal.end"}, {"connectionId", kId}, {"reason", "x"}});
    h.deliver({{"type", "signal.error"}, {"connectionId", kId}, {"code", "host_offline"}});
    QVERIFY(h.links.owns(kId));
    QVERIFY(!record.closed);
    QCOMPARE(h.events.events.last(), "opened " + kId);
}

void RunnerLinkLifecycleTest::reportsATransportThatCloses() {
    LinkHarness h;
    h.deliver(offer());
    h.transports.last().events->closed();
    QCOMPARE(h.out.ends(), QStringList{"peer_closed"});
    QCOMPARE(h.events.events.last(), "ended " + kId);

    LinkHarness open;
    open.open().events->closed();
    QVERIFY(open.out.ends().isEmpty());
    QCOMPARE(open.events.events.last(), "lost " + kId + " peer_closed");
    QVERIFY(!open.links.owns(kId));
}

void RunnerLinkLifecycleTest::closeReportsNothing() {
    LinkHarness h;
    h.deliver(offer());
    logLines().clear();
    h.links.close(kId, QStringLiteral("cancelled"));
    QCOMPARE(h.out.ends(), QStringList{"closed"});
    QCOMPARE(h.events.events.size(), 1);
    QVERIFY(h.transports.last().closed);
    QVERIFY(h.clock.due.isEmpty());
    QVERIFY(logLines().join('\n').contains("cancelled"));
    h.links.close(kId, QStringLiteral("again"));
    QCOMPARE(h.out.sent.size(), 1);

    LinkHarness open;
    TransportRecord& record = open.open();
    open.links.close(kId, QStringLiteral("done"));
    QVERIFY(record.closed);
    QVERIFY(!record.alive);
    QVERIFY(open.out.ends().isEmpty());
    QCOMPARE(open.events.events.last(), "opened " + kId);
    QVERIFY(!open.links.owns(kId));
}

void RunnerLinkLifecycleTest::destructionReportsNothing() {
    FakeOut out;
    FakeClock clock;
    FakeTransports transports;
    FakeIdentity identity;
    FakePins pins;
    RecordingEvents events;
    {
        RunnerLinks links(out, clock, transports, identity, pins, events);
        links.signal(offer());
        transports.last().events->localDescription(QString::fromLatin1(vectors::answerSdp));
        transports.last().events->opened();
        links.signal(offer({.connectionId = connectionId(2), .runnerId = runnerId(2)}));
    }
    QCOMPARE(events.events, (QStringList{offeredLine(kId), "opened " + kId,
                                         offeredLine(connectionId(2), runnerId(2))}));
    QCOMPARE(out.ofType("signal.end").size(), 0);
    for (const auto& record : transports.records) {
        QVERIFY(record->closed);
        QVERIFY(!record->alive);
    }
}

} // namespace runner_link_test
