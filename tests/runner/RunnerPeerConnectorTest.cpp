// Signaling attempts of the peer connector over fake signals, transports, pins and clock.
#include "RunnerPeerTest.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingSignatures.h"

#include <QtTest>

namespace runner_peer_test {
namespace {

namespace cc = ketplus::control_crypto;

const QString kId = QString::fromLatin1(vectors::connectionId);
const QString kOtherId = QStringLiteral("pc_zzzzzzzzzzzzzzzzzzzzzzzz");

QJsonObject error(const QString& code, const QString& id = {}) {
    QJsonObject message{{QStringLiteral("type"), QStringLiteral("signal.error")},
                        {QStringLiteral("code"), code}};
    if (!id.isEmpty())
        message.insert(QStringLiteral("connectionId"), id);
    return message;
}

QJsonObject candidate(const QString& id, const QJsonValue& value,
                      const QString& mid = QStringLiteral("0")) {
    return {{QStringLiteral("type"), QStringLiteral("signal.candidate")},
            {QStringLiteral("connectionId"), id},
            {QStringLiteral("candidate"), value},
            {QStringLiteral("sdpMid"), mid}};
}

QStringList endReasons(const FakeSender& sender) {
    QStringList reasons;
    for (const auto& end : sender.ofType(QStringLiteral("signal.end")))
        reasons.append(end.value(QStringLiteral("reason")).toString());
    return reasons;
}

} // namespace

void RunnerPeerConnectorTest::offerIsSignedOverTheVectorBytes() {
    PeerHarness h;
    h.request();
    const auto starts = h.sender.ofType(QStringLiteral("signal.start"));
    QCOMPARE(starts.size(), 1);
    QCOMPARE(starts.first(), (QJsonObject{{QStringLiteral("type"), QStringLiteral("signal.start")},
                                          {QStringLiteral("jobId"), QStringLiteral("job_42")},
                                          {QStringLiteral("leaseId"), QStringLiteral("lease_1")}}));
    h.started();
    QCOMPARE(h.transports.records.size(), 1);
    QVERIFY(h.transports.last().offered);
    QCOMPARE(h.transports.last().iceServers, iceServers());
    h.describe();
    const auto offers = h.sender.ofType(QStringLiteral("signal.offer"));
    QCOMPARE(offers.size(), 1);
    const QJsonObject offer = offers.first();
    QCOMPARE(offer.keys(), (QStringList{"attempt", "connectionId", "issuedAt", "jobId",
                                        "runnerKeyVersion", "sdp", "signature", "type"}));
    QCOMPARE(offer.value("connectionId").toString(), kId);
    QCOMPARE(offer.value("sdp").toString(), QString::fromLatin1(vectors::offerSdp));
    QCOMPARE(offer.value("jobId").toString(), QStringLiteral("job_42"));
    QCOMPARE(offer.value("attempt").toInt(), 1);
    QCOMPARE(offer.value("runnerKeyVersion").toInt(), 1);
    QCOMPARE(offer.value("issuedAt").toString(), QString::fromLatin1(vectors::issuedAt));

    // The signed bytes are the vector's, and the signature verifies with the public key.
    pairing::OfferPayload payload{kId,
                                  QString::fromLatin1(vectors::workspaceId),
                                  QString::fromLatin1(vectors::machineId),
                                  QString::fromLatin1(vectors::runnerId),
                                  1,
                                  QStringLiteral("job_42"),
                                  1,
                                  offer.value("issuedAt").toString(),
                                  offer.value("sdp").toString()};
    QCOMPARE(payload.bytes(), QByteArray(vectors::offerBytes));
    const QByteArray signature = cc::fromBase64Url(offer.value("signature").toString());
    QCOMPARE(signature,
             QByteArray::fromBase64(vectors::offerSignature, QByteArray::Base64UrlEncoding));
    QVERIFY(!pairing::verify(payload, signature, {QByteArray::fromHex(vectors::runnerPublicKey), 1},
                             vectorTime()));
}

void RunnerPeerConnectorTest::eachSignalErrorCode_data() {
    QTest::addColumn<QString>("code");
    QTest::addColumn<bool>("retried");
    QTest::addColumn<bool>("afterStarted");
    for (const bool after : {false, true}) {
        const char* when = after ? " after started" : " before started";
        QTest::addRow("host_offline%s", when) << "host_offline" << true << after;
        QTest::addRow("rate_limited%s", when) << "rate_limited" << true << after;
        QTest::addRow("not_paired%s", when) << "not_paired" << false << after;
        QTest::addRow("lease_required%s", when) << "lease_required" << false << after;
        QTest::addRow("unsupported%s", when) << "unsupported" << false << after;
        QTest::addRow("unknown%s", when) << "made_up" << true << after;
    }
}

void RunnerPeerConnectorTest::eachSignalErrorCode() {
    QFETCH(QString, code);
    QFETCH(bool, retried);
    QFETCH(bool, afterStarted);
    PeerHarness h;
    h.request();
    if (afterStarted) {
        h.started();
        h.describe();
    }
    h.connector.signal(error(code, afterStarted ? kId : QString()));
    const QString reported = code == "made_up" ? QStringLiteral("signal_error") : code;
    QCOMPARE(h.listener.events, QStringList{(retried ? "failed " : "gaveUp ") + reported});
    QCOMPARE(endReasons(h.sender), afterStarted ? QStringList{reported} : QStringList{});
    if (afterStarted) {
        QVERIFY(h.transports.last().closed);
        QVERIFY(h.transports.last().destroyed);
    }
    QCOMPARE(h.clock.armed(PeerTimer::AttemptRetry), retried);
    // A retry starts once after its delay; a stop starts nothing more.
    h.clock.advance(retried ? attemptRetryBaseMs : 60'000);
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), retried ? 2 : 1);
}

void RunnerPeerConnectorTest::overlappingRequestsEndTheOldAttempt() {
    PeerHarness h;
    h.request(1);
    h.started();
    h.describe();
    h.clock.advance(1'000);
    h.request(2);
    QCOMPARE(endReasons(h.sender), QStringList{QStringLiteral("replaced")});
    QVERIFY(h.transports.last().destroyed);
    // The next start keeps 2.5 seconds from the last one.
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), 1);
    QCOMPARE(h.clock.remaining(PeerTimer::AttemptRetry), 1'500);
    h.clock.advance(1'500);
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), 2);
    // Signals for the replaced connection are ignored.
    h.connector.signal(signedAnswer(kId, h.offerSdp, answerSdp(), h.clock.now()));
    QCOMPARE(h.transports.records.size(), 1);
    h.started(kOtherId);
    h.describe();
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.offer")).last().value("attempt").toInt(), 2);
    QVERIFY(h.listener.events.isEmpty());
}

void RunnerPeerConnectorTest::otherConnectionsAreIgnored() {
    PeerHarness h;
    h.request();
    h.started();
    h.describe();
    h.connector.signal(signedAnswer(kOtherId, h.offerSdp, answerSdp(), h.clock.now()));
    h.connector.signal(
        candidate(kOtherId, QStringLiteral("candidate:1 1 udp 1 1.2.3.4 1 typ host")));
    h.connector.signal({{"type", "signal.end"}, {"connectionId", kOtherId}});
    h.connector.signal(error(QStringLiteral("not_paired"), kOtherId));
    h.connector.signal({{"type", "signal.started"}, {"connectionId", kOtherId}});
    QVERIFY(h.transports.last().answers.isEmpty());
    QVERIFY(h.transports.last().candidates.isEmpty());
    QVERIFY(!h.transports.last().closed);
    QVERIFY(h.listener.events.isEmpty());
    QVERIFY(endReasons(h.sender).isEmpty());
}

void RunnerPeerConnectorTest::desktopEndReasons_data() {
    QTest::addColumn<QJsonValue>("reason");
    QTest::addColumn<QString>("reported");
    QTest::addColumn<bool>("retried");
    QTest::newRow("already_dispatched")
        << QJsonValue(QStringLiteral("already_dispatched")) << "already_dispatched" << false;
    QTest::newRow("not_paired") << QJsonValue(QStringLiteral("not_paired")) << "not_paired"
                                << false;
    QTest::newRow("job_mismatch") << QJsonValue(QStringLiteral("job_mismatch")) << "job_mismatch"
                                  << true;
    QTest::newRow("other") << QJsonValue(QStringLiteral("busy")) << "remote_ended" << true;
    QTest::newRow("empty") << QJsonValue(QString()) << "remote_ended" << true;
    QTest::newRow("missing") << QJsonValue(QJsonValue::Undefined) << "remote_ended" << true;
    QTest::newRow("not a string") << QJsonValue(7) << "remote_ended" << true;
    QTest::newRow("too long") << QJsonValue(QString(65, 'a')) << "remote_ended" << true;
    QTest::newRow("unprintable") << QJsonValue(QStringLiteral("not_paired\n")) << "remote_ended"
                                 << true;
}

void RunnerPeerConnectorTest::desktopEndReasons() {
    QFETCH(QJsonValue, reason);
    QFETCH(QString, reported);
    QFETCH(bool, retried);
    PeerHarness h;
    h.request();
    h.started();
    h.describe();
    QJsonObject end{{"type", "signal.end"}, {"connectionId", kId}};
    if (!reason.isUndefined())
        end.insert(QStringLiteral("reason"), reason);
    h.connector.signal(end);
    QCOMPARE(h.listener.events, QStringList{(retried ? "failed " : "gaveUp ") + reported});
    // The desktop already ended the connection: no signal.end goes back.
    QVERIFY(endReasons(h.sender).isEmpty());
    QVERIFY(h.transports.last().destroyed);
    QCOMPARE(h.pins.pin->state, pairing::RunnerPin::State::Active);
    QCOMPARE(h.clock.armed(PeerTimer::AttemptRetry), retried);
    if (retried)
        QCOMPARE(h.clock.remaining(PeerTimer::AttemptRetry), attemptRetryBaseMs);
    // A retry starts once after its delay; a stop starts nothing more.
    h.clock.advance(retried ? attemptRetryBaseMs : 60'000);
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), retried ? 2 : 1);
}

void RunnerPeerConnectorTest::desktopEndsOutsideAnAttemptAreIgnored() {
    const QJsonObject end{
        {"type", "signal.end"}, {"connectionId", kId}, {"reason", "already_dispatched"}};
    PeerHarness idle;
    idle.connector.signal(end);
    QVERIFY(idle.listener.events.isEmpty());

    PeerHarness waiting;
    waiting.request();
    waiting.connector.signal(error(QStringLiteral("host_offline")));
    waiting.connector.signal(end);
    QCOMPARE(waiting.listener.events, QStringList{QStringLiteral("failed host_offline")});
    QVERIFY(waiting.clock.armed(PeerTimer::AttemptRetry));

    PeerHarness open;
    open.open();
    open.connector.signal(end);
    QVERIFY(open.connector.isOpen());
    QVERIFY(!open.transports.last().closed);
    QCOMPARE(open.listener.events, QStringList{"opened " + kId});
}

void RunnerPeerConnectorTest::answerFailures_data() {
    QTest::addColumn<QString>("reason");
    QTest::newRow("wrong signature") << "invalid_signature";
    QTest::newRow("tampered sdp") << "invalid_signature";
    QTest::newRow("other job") << "invalid_signature";
    QTest::newRow("key version") << "key_version_mismatch";
    QTest::newRow("no pin") << "not_paired";
    QTest::newRow("pending pin") << "not_paired";
    QTest::newRow("too old") << "issued_at_invalid";
    QTest::newRow("too new") << "issued_at_invalid";
    QTest::newRow("no fingerprint") << "invalid_fingerprint";
    QTest::newRow("missing field") << "invalid_answer";
    QTest::newRow("sdp too large") << "invalid_answer";
}

void RunnerPeerConnectorTest::answerFailures() {
    QFETCH(QString, reason);
    const QString row = QString::fromLatin1(QTest::currentDataTag());
    PeerHarness h;
    h.request();
    h.started();
    h.describe();
    const QDateTime now = h.clock.now();
    QJsonObject answer = signedAnswer(kId, h.offerSdp, answerSdp(), now);
    if (row == "wrong signature") {
        answer = signedAnswer(kId, QStringLiteral("v=0\r\n"), answerSdp(), now);
    } else if (row == "tampered sdp") {
        answer["sdp"] = answerSdp() + QStringLiteral("a=x\r\n");
    } else if (row == "other job") {
        answer = signedAnswer(kId, h.offerSdp, answerSdp(), now, 1, QStringLiteral("job_9"));
    } else if (row == "key version") {
        answer = signedAnswer(kId, h.offerSdp, answerSdp(), now, 2);
    } else if (row == "no pin") {
        h.pins.pin.reset();
    } else if (row == "pending pin") {
        h.pins.pin->state = pairing::RunnerPin::State::Pending;
    } else if (row == "too old") {
        answer = signedAnswer(kId, h.offerSdp, answerSdp(), now.addSecs(-301));
    } else if (row == "too new") {
        answer = signedAnswer(kId, h.offerSdp, answerSdp(), now.addSecs(301));
    } else if (row == "no fingerprint") {
        answer = signedAnswer(kId, h.offerSdp, answerSdp(false), now);
    } else if (row == "missing field") {
        answer.remove(QStringLiteral("issuedAt"));
    } else if (row == "sdp too large") {
        answer = signedAnswer(kId, h.offerSdp, answerSdp() + QString(sdpLimit, 'x'), now);
    }
    h.connector.signal(answer);
    QCOMPARE(h.listener.events, QStringList{"failed " + reason});
    QCOMPARE(endReasons(h.sender), QStringList{reason});
    QVERIFY(h.transports.last().answers.isEmpty());
    QVERIFY(h.transports.last().destroyed);
    QVERIFY(h.clock.armed(PeerTimer::AttemptRetry));
}

void RunnerPeerConnectorTest::aReplayedAnswerIsRefused() {
    PeerHarness h;
    h.open();
    QCOMPARE(h.listener.events, QStringList{"opened " + kId});
    const QJsonObject first = signedAnswer(kId, h.offerSdp, answerSdp(), h.clock.now());
    h.request(2);
    h.clock.advance(h.clock.remaining(PeerTimer::AttemptRetry));
    // A Relay that hands out the same connection id again cannot replay the answer.
    h.started(kId);
    h.describe();
    h.connector.signal(first);
    QCOMPARE(h.listener.events.last(), QStringLiteral("failed answer_replayed"));
    QVERIFY(h.transports.last().answers.isEmpty());
    // Ten minutes later the id is forgotten.
    h.clock.advance(10 * 60 * 1000 + 1);
    h.started(kId);
    h.describe();
    h.answer(kId);
    QCOMPARE(h.transports.last().answers.size(), 1);
}

void RunnerPeerConnectorTest::anAnswerBeforeTheOfferEndsTheAttempt() {
    PeerHarness h;
    h.request();
    h.started();
    // The transport has not published its offer yet.
    h.connector.signal(
        signedAnswer(kId, QString::fromLatin1(vectors::offerSdp), answerSdp(), h.clock.now()));
    QCOMPARE(h.listener.events, QStringList{QStringLiteral("failed answer_before_offer")});
    QCOMPARE(endReasons(h.sender), QStringList{QStringLiteral("answer_before_offer")});
    QVERIFY(h.transports.last().answers.isEmpty());
    // A late offer of the dropped transport is not sent.
    h.describe();
    QVERIFY(h.sender.ofType(QStringLiteral("signal.offer")).isEmpty());
}

void RunnerPeerConnectorTest::noActivePinStopsBeforeAnyStart() {
    for (const bool pending : {false, true}) {
        PeerHarness h;
        if (pending)
            h.pins.pin->state = pairing::RunnerPin::State::Pending;
        else
            h.pins.pin.reset();
        h.request();
        QCOMPARE(h.listener.events, QStringList{QStringLiteral("gaveUp not_paired")});
        QVERIFY(h.sender.sent.isEmpty());
        QVERIFY(h.transports.records.isEmpty());
        QVERIFY(h.clock.due.isEmpty());
        h.clock.advance(600'000);
        QVERIFY(h.sender.sent.isEmpty());
        QCOMPARE(h.listener.events.size(), 1);
    }

    // A pin that goes away between signal.start and signal.started stops there as well.
    PeerHarness late;
    late.request();
    late.pins.pin.reset();
    late.started();
    QCOMPARE(late.listener.events, QStringList{QStringLiteral("gaveUp not_paired")});
    QCOMPARE(endReasons(late.sender), QStringList{QStringLiteral("not_paired")});
    QVERIFY(late.transports.records.isEmpty());
    QVERIFY(late.clock.due.isEmpty());
}

void RunnerPeerConnectorTest::aPinRemovedDuringAnAttemptStopsTheRetry() {
    PeerHarness h;
    h.request();
    h.started();
    h.describe();
    h.pins.pin.reset();
    h.answer();
    // The answer fails its check and the attempt ends as any failed answer does.
    QCOMPARE(h.listener.events, QStringList{QStringLiteral("failed not_paired")});
    QVERIFY(h.transports.last().answers.isEmpty());
    QCOMPARE(h.clock.remaining(PeerTimer::AttemptRetry), attemptRetryBaseMs);
    // The one retry finds no pin: no signal.start, and the request stops once.
    h.clock.advance(attemptRetryBaseMs);
    QCOMPARE(h.listener.events, (QStringList{QStringLiteral("failed not_paired"),
                                             QStringLiteral("gaveUp not_paired")}));
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), 1);
    QVERIFY(h.clock.due.isEmpty());
    h.clock.advance(600'000);
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), 1);
    QCOMPARE(h.listener.events.size(), 2);
}

void RunnerPeerConnectorTest::theFingerprintComesFromTheSignedAnswer() {
    PeerHarness h;
    h.request();
    h.started();
    h.describe();
    h.answer();
    QCOMPARE(h.transports.last().answers, QStringList{answerSdp()});
    QCOMPARE(h.transports.last().fingerprints, QStringList{answerFingerprint()});
    // A second answer for the same connection changes nothing.
    h.answer();
    QCOMPARE(h.transports.last().answers.size(), 1);
    QVERIFY(h.listener.events.isEmpty());
    h.connector.opened();
    QCOMPARE(h.listener.events, QStringList{"opened " + kId});
    QVERIFY(!h.clock.armed(PeerTimer::AttemptDeadline));
}

void RunnerPeerConnectorTest::localCandidatesFollowTheOffer() {
    PeerHarness h;
    h.request();
    h.started();
    h.connector.localCandidate(QStringLiteral("candidate:a"), QStringLiteral("0"));
    h.connector.localCandidate(QStringLiteral("candidate:b"), QStringLiteral("0"));
    QVERIFY(h.sender.ofType(QStringLiteral("signal.candidate")).isEmpty());
    h.describe();
    h.connector.localCandidate(QStringLiteral("candidate:c"), QStringLiteral("0"));
    h.connector.localCandidate({}, {});
    QStringList order;
    for (const auto& message : h.sender.sent) {
        const QString type = message.value("type").toString();
        if (type == "signal.candidate")
            order.append(message.value("candidate").isNull()
                             ? QStringLiteral("end")
                             : message.value("candidate").toString());
        else
            order.append(type);
    }
    QCOMPARE(order, (QStringList{"signal.start", "signal.offer", "candidate:a", "candidate:b",
                                 "candidate:c", "end"}));
    QCOMPARE(h.sender.sent.last().value("connectionId").toString(), kId);
}

void RunnerPeerConnectorTest::remoteCandidatesWaitForTheCheckedAnswer() {
    PeerHarness h;
    h.request();
    h.started();
    h.connector.signal(candidate(kId, QStringLiteral("candidate:1")));
    h.describe();
    h.connector.signal(candidate(kId, QStringLiteral("candidate:2"), QStringLiteral("1")));
    QVERIFY(h.transports.last().candidates.isEmpty());
    h.answer();
    QCOMPARE(h.transports.last().candidates,
             (QStringList{QStringLiteral("0|candidate:1"), QStringLiteral("1|candidate:2")}));
    h.connector.signal(candidate(kId, QJsonValue()));
    QCOMPARE(h.transports.last().candidates.last(), QStringLiteral("0|"));

    // Candidates held for an answer that fails the check are never applied.
    PeerHarness refused;
    refused.request();
    refused.started();
    refused.describe();
    refused.connector.signal(candidate(kId, QStringLiteral("candidate:1")));
    refused.pins.pin.reset();
    refused.answer();
    QVERIFY(refused.transports.last().candidates.isEmpty());
}

void RunnerPeerConnectorTest::candidateLimits_data() {
    QTest::addColumn<QString>("kind");
    QTest::newRow("65 remote") << "remote-count";
    QTest::newRow("remote too long") << "remote-size";
    QTest::newRow("remote mid too long") << "remote-mid";
    QTest::newRow("remote after end") << "remote-after-end";
    QTest::newRow("remote not a string") << "remote-type";
    QTest::newRow("65 local") << "local-count";
    QTest::newRow("local too long") << "local-size";
}

void RunnerPeerConnectorTest::candidateLimits() {
    QFETCH(QString, kind);
    PeerHarness h;
    h.request();
    h.started();
    h.describe();
    const QString one = QStringLiteral("candidate:x");
    if (kind == "remote-count") {
        for (int i = 0; i < candidateLimit; ++i)
            h.connector.signal(candidate(kId, one));
        QVERIFY(h.listener.events.isEmpty());
        h.connector.signal(candidate(kId, one));
    } else if (kind == "remote-size") {
        h.connector.signal(candidate(kId, QString(candidateSizeLimit, 'c')));
        QVERIFY(h.listener.events.isEmpty());
        h.connector.signal(candidate(kId, QString(candidateSizeLimit + 1, 'c')));
    } else if (kind == "remote-mid") {
        h.connector.signal(candidate(kId, one, QString(candidateMidLimit + 1, 'm')));
    } else if (kind == "remote-after-end") {
        h.connector.signal(candidate(kId, QJsonValue()));
        h.connector.signal(candidate(kId, one));
    } else if (kind == "remote-type") {
        h.connector.signal(candidate(kId, 42));
    } else if (kind == "local-count") {
        for (int i = 0; i < candidateLimit; ++i)
            h.connector.localCandidate(one, QStringLiteral("0"));
        QVERIFY(h.listener.events.isEmpty());
        h.connector.localCandidate(one, QStringLiteral("0"));
    } else if (kind == "local-size") {
        h.connector.localCandidate(QString(candidateSizeLimit + 1, 'c'), QStringLiteral("0"));
    }
    QCOMPARE(h.listener.events, QStringList{QStringLiteral("failed candidate_limit")});
    QCOMPARE(endReasons(h.sender), QStringList{QStringLiteral("candidate_limit")});
    QVERIFY(h.transports.last().destroyed);
}

void RunnerPeerConnectorTest::deadlineAndRetryDelays() {
    PeerHarness h;
    h.request();
    QList<qint64> delays;
    for (int attempt = 0; attempt < 5; ++attempt) {
        h.started(QStringLiteral("pc_%1").arg(attempt, 24, 10, QLatin1Char('0')));
        h.describe();
        QCOMPARE(h.clock.remaining(PeerTimer::AttemptDeadline), attemptDeadlineMs);
        h.clock.advance(attemptDeadlineMs - 1);
        QCOMPARE(h.listener.events.size(), attempt);
        h.clock.advance(1);
        QCOMPARE(h.listener.events.last(), QStringLiteral("failed attempt_timeout"));
        QCOMPARE(endReasons(h.sender).size(), attempt + 1);
        QVERIFY(h.transports.last().destroyed);
        delays.append(h.clock.remaining(PeerTimer::AttemptRetry));
        h.clock.advance(delays.last());
        QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), attempt + 2);
    }
    QCOMPARE(delays, (QList<qint64>{2'500, 5'000, 10'000, 15'000, 15'000}));
    // A new request starts the delays over.
    h.request();
    h.clock.advance(h.clock.remaining(PeerTimer::AttemptRetry));
    h.connector.signal(error(QStringLiteral("host_offline")));
    QCOMPARE(h.clock.remaining(PeerTimer::AttemptRetry), 2'500);
}

void RunnerPeerConnectorTest::expiresAtShortensTheDeadline() {
    PeerHarness h;
    h.request();
    h.clock.advance(1'000);
    h.started(kId, 4'000);
    QCOMPARE(h.clock.remaining(PeerTimer::AttemptDeadline), 4'000);
    h.clock.advance(4'000);
    QCOMPARE(h.listener.events, QStringList{QStringLiteral("failed attempt_timeout")});

    // A later expiresAt does not stretch the 20 seconds from signal.start.
    PeerHarness late;
    late.request();
    late.clock.advance(5'000);
    late.started(kId, 600'000);
    QCOMPARE(late.clock.remaining(PeerTimer::AttemptDeadline), attemptDeadlineMs - 5'000);
}

void RunnerPeerConnectorTest::startsKeepTheirSpacingAcrossRequests() {
    PeerHarness h;
    h.request();
    h.connector.close(QStringLiteral("done"));
    QVERIFY(h.clock.due.isEmpty());
    h.clock.advance(2'000);
    h.request();
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), 1);
    QCOMPARE(h.clock.remaining(PeerTimer::AttemptRetry), 500);
    h.clock.advance(500);
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), 2);
    h.clock.advance(startSpacingMs);
    h.request();
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), 3);
}

void RunnerPeerConnectorTest::relayLossDuringAnAttemptWaitsForReady() {
    PeerHarness h;
    h.sender.ready = false;
    h.request();
    QVERIFY(h.sender.sent.isEmpty());
    h.sender.ready = true;
    h.connector.relayReady();
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), 1);
    h.started();
    h.describe();
    h.sender.ready = false;
    h.connector.relayLost();
    QCOMPARE(h.listener.events, QStringList{QStringLiteral("failed relay_lost")});
    QVERIFY(h.transports.last().destroyed);
    h.clock.advance(600'000);
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), 1);
    QVERIFY(h.clock.due.isEmpty());
    h.sender.ready = true;
    h.connector.relayReady();
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), 2);
    QCOMPARE(h.clock.remaining(PeerTimer::AttemptDeadline), attemptDeadlineMs);

    // The retry delay does not grow with a Relay loss; only the 2.5-second spacing between two
    // signal.start applies. Ready after 1 second: the next start is at 2.5 seconds exactly.
    PeerHarness quick;
    quick.request();
    quick.started();
    quick.describe();
    quick.sender.ready = false;
    quick.connector.relayLost();
    quick.clock.advance(1'000);
    quick.sender.ready = true;
    quick.connector.relayReady();
    QCOMPARE(quick.clock.remaining(PeerTimer::AttemptRetry), startSpacingMs - 1'000);
    quick.clock.advance(startSpacingMs - 1'000 - 1);
    QCOMPARE(quick.sender.ofType(QStringLiteral("signal.start")).size(), 1);
    quick.clock.advance(1);
    QCOMPARE(quick.sender.ofType(QStringLiteral("signal.start")).size(), 2);

    // Ready after 10 seconds: the next start goes at once.
    PeerHarness slow;
    slow.request();
    slow.started();
    slow.describe();
    slow.sender.ready = false;
    slow.connector.relayLost();
    slow.clock.advance(10'000);
    QCOMPARE(slow.sender.ofType(QStringLiteral("signal.start")).size(), 1);
    slow.sender.ready = true;
    slow.connector.relayReady();
    QCOMPARE(slow.sender.ofType(QStringLiteral("signal.start")).size(), 2);
    QVERIFY(!slow.clock.armed(PeerTimer::AttemptRetry));
    // The loss did not move the delay step: a failure after it takes the first retry delay,
    // and one more loss keeps the step where that failure left it.
    slow.connector.signal(error(QStringLiteral("host_offline")));
    QCOMPARE(slow.clock.remaining(PeerTimer::AttemptRetry), attemptRetryBaseMs);
    slow.clock.advance(attemptRetryBaseMs);
    slow.started(QStringLiteral("pc_cccccccccccccccccccccccc"));
    slow.sender.ready = false;
    slow.connector.relayLost();
    slow.clock.advance(10'000);
    slow.sender.ready = true;
    slow.connector.relayReady();
    QCOMPARE(slow.sender.ofType(QStringLiteral("signal.start")).size(), 4);
    slow.connector.signal(error(QStringLiteral("host_offline")));
    QCOMPARE(slow.clock.remaining(PeerTimer::AttemptRetry), 2 * attemptRetryBaseMs);
}

void RunnerPeerConnectorTest::relayLossLeavesAnOpenConnection() {
    PeerHarness h;
    h.open();
    h.sender.ready = false;
    h.connector.relayLost();
    h.connector.runnerRemoved();
    QVERIFY(h.connector.isOpen());
    QVERIFY(!h.transports.last().closed);
    QCOMPARE(h.listener.events, QStringList{"opened " + kId});
    // Signals arriving through the Relay no longer touch the open connection.
    h.connector.signal({{"type", "signal.end"}, {"connectionId", kId}});
    QVERIFY(h.connector.isOpen());
}

void RunnerPeerConnectorTest::runnerRemovalStopsAttempts() {
    PeerHarness h;
    h.request();
    h.started();
    h.describe();
    h.connector.runnerRemoved();
    QCOMPARE(h.listener.events, QStringList{QStringLiteral("gaveUp runner_removed")});
    QVERIFY(h.transports.last().destroyed);
    QVERIFY(h.clock.due.isEmpty());
    h.clock.advance(600'000);
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), 1);
}

void RunnerPeerConnectorTest::anOpenConnectionThatDropsIsNotRetried() {
    PeerHarness h;
    h.open();
    h.connector.closed();
    QCOMPARE(h.listener.events, (QStringList{"opened " + kId, QStringLiteral("lost peer_closed")}));
    h.connector.closed();
    h.clock.advance(600'000);
    QCOMPARE(h.listener.events.size(), 2);
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), 1);
    QVERIFY(h.transports.last().destroyed);
}

void RunnerPeerConnectorTest::aTransportThatClosesBeforeOpeningIsRetried() {
    PeerHarness h;
    h.request();
    h.started();
    h.describe();
    h.answer();
    h.connector.closed();
    QCOMPARE(h.listener.events, QStringList{QStringLiteral("failed peer_closed")});
    QCOMPARE(endReasons(h.sender), QStringList{QStringLiteral("peer_closed")});
    QCOMPARE(h.clock.remaining(PeerTimer::AttemptRetry), 2'500);

    PeerHarness refused;
    refused.transports.offerOk = false;
    refused.request();
    refused.started();
    QCOMPARE(refused.listener.events, QStringList{QStringLiteral("failed offer_failed")});
}

} // namespace runner_peer_test
