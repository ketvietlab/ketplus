// Offers the desktop checks and the answers it signs, over fakes with simulated time.
#include "RunnerLinkTest.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingSignatures.h"

#include <QtTest>

namespace runner_link_test {
namespace {

namespace cc = ketplus::control_crypto;
const QString kId = QString::fromLatin1(vectors::connectionId);
const QString kRunner = QString::fromLatin1(vectors::runnerId);

QJsonObject with(QJsonObject message, const QString& key, const QJsonValue& value) {
    if (value.isUndefined())
        message.remove(key);
    else
        message.insert(key, value);
    return message;
}

} // namespace

void RunnerLinkOfferTest::takesTheVectorOffer() {
    LinkHarness h;
    const QJsonObject message = offer();
    QCOMPARE(message.value("signature").toString(), QString::fromLatin1(vectors::offerSignature));
    h.deliver(message);
    QCOMPARE(h.events.events, QStringList{"offered " + kId + " " + kRunner + " job_42 1"});
    QCOMPARE(h.transports.records.size(), 1);
    QCOMPARE(h.transports.last().offerSdp, QString::fromLatin1(vectors::offerSdp));
    QCOMPARE(h.transports.last().iceServers, iceServers());
    QCOMPARE(h.clock.due.value(kId), attemptDeadlineMs);
    QVERIFY(h.out.sent.isEmpty());
    QVERIFY(h.links.owns(kId));

    // Five minutes either way of the clock is still in time.
    LinkHarness late;
    late.clock.elapsed = 5 * 60 * 1000;
    late.deliver(offer());
    QCOMPARE(late.transports.records.size(), 1);
    LinkHarness early;
    early.clock.elapsed = -5 * 60 * 1000;
    early.deliver(offer());
    QCOMPARE(early.transports.records.size(), 1);
}

void RunnerLinkOfferTest::refusesOffers_data() {
    QTest::addColumn<QJsonObject>("message");
    QTest::addColumn<qint64>("clockMs");
    QTest::addColumn<bool>("pinned");
    QTest::addColumn<QString>("reason");
    const QJsonObject good = offer();
    const auto row = [](const char* name, const QJsonObject& message, const QString& reason,
                        qint64 clockMs = 0, bool pinned = true) {
        QTest::newRow(name) << message << clockMs << pinned << reason;
    };
    const QString invalid = QStringLiteral("invalid_offer");
    row("no runnerKeyVersion", with(good, "runnerKeyVersion", QJsonValue()), invalid);
    row("runnerKeyVersion 0", with(good, "runnerKeyVersion", 0), invalid);
    row("runnerKeyVersion text", with(good, "runnerKeyVersion", "1"), invalid);
    row("runnerKeyVersion fraction", with(good, "runnerKeyVersion", 1.5), invalid);
    row("runnerId with a space", with(good, "runnerId", "run aaaa"), invalid);
    row("runnerId empty", with(good, "runnerId", ""), invalid);
    row("runnerId too long", with(good, "runnerId", QString(129, 'r')), invalid);
    row("jobId empty", with(good, "jobId", ""), invalid);
    row("no attempt", with(good, "attempt", QJsonValue()), invalid);
    row("attempt 0", with(good, "attempt", 0), invalid);
    row("no sdp", with(good, "sdp", QJsonValue()), invalid);
    row("empty sdp", with(good, "sdp", ""), invalid);
    row("sdp above 16 KiB", with(good, "sdp", QString(16 * 1024 + 1, 'v')), invalid);
    row("no issuedAt", with(good, "issuedAt", QJsonValue()), invalid);
    row("issuedAt not UTC", with(good, "issuedAt", "2026-11-18T03:00:00.000+01:00"), invalid);
    row("issuedAt not a time", with(good, "issuedAt", "yesterday Z"), invalid);
    row("signature a number", with(good, "signature", 7), invalid);
    row("iceServers an object", with(good, "iceServers", QJsonObject()), invalid);
    row("not pinned", good, QStringLiteral("not_paired"), 0, false);
    row("another key version", offer({.runnerKeyVersion = 2}), QStringLiteral("not_paired"));
    row("sdp changed after signing", with(good, "sdp", "v=0\r\n"),
        QStringLiteral("invalid_signature"));
    row("attempt changed after signing", with(good, "attempt", 2),
        QStringLiteral("invalid_signature"));
    row("signature of another key",
        with(good, "signature",
             QString::fromLatin1(cc::base64Url(pairing::signPayload(
                 QByteArray::fromHex(vectors::desktopSeed), QByteArray("x"))))),
        QStringLiteral("invalid_signature"));
    row("signature empty", with(good, "signature", ""), QStringLiteral("invalid_signature"));
    row("five minutes and a millisecond late", good, QStringLiteral("offer_expired"),
        5 * 60 * 1000 + 1);
    row("five minutes and a millisecond early", good, QStringLiteral("offer_expired"),
        -(5 * 60 * 1000 + 1));
    // The signature is checked before the time.
    row("late and tampered", with(good, "sdp", "v=0\r\n"), QStringLiteral("invalid_signature"),
        60 * 60 * 1000);
}

void RunnerLinkOfferTest::refusesOffers() {
    QFETCH(QJsonObject, message);
    QFETCH(qint64, clockMs);
    QFETCH(bool, pinned);
    QFETCH(QString, reason);
    LinkHarness h;
    h.clock.elapsed = clockMs;
    if (!pinned)
        h.pins.unpinned.insert(kRunner);
    h.deliver(message);
    QCOMPARE(h.out.sent.size(), 1);
    QCOMPARE(h.out.sent[0],
             (QJsonObject{{"type", "signal.end"}, {"connectionId", kId}, {"reason", reason}}));
    QVERIFY(h.events.events.isEmpty());
    QVERIFY(h.transports.records.isEmpty());
    QVERIFY(h.clock.due.isEmpty());
    QVERIFY(!h.links.owns(kId));
}

void RunnerLinkOfferTest::refusesAnOfferTheJobSideRefuses() {
    LinkHarness h;
    h.events.code = QStringLiteral("already_dispatched");
    h.deliver(offer());
    QCOMPARE(h.out.ends(), QStringList{"already_dispatched"});
    QCOMPARE(h.events.events.size(), 1); // asked, and nothing after
    QVERIFY(h.transports.records.isEmpty());
    QVERIFY(h.clock.due.isEmpty());
    QVERIFY(!h.links.owns(kId));
    // The id was used once the signature verified: the same offer again is dropped.
    h.events.code.clear();
    h.deliver(offer());
    QCOMPARE(h.out.sent.size(), 1);
    QCOMPARE(h.events.events.size(), 1);
}

void RunnerLinkOfferTest::dropsUnreadableConnectionIds_data() {
    QTest::addColumn<QJsonValue>("id");
    QTest::newRow("missing") << QJsonValue();
    QTest::newRow("number") << QJsonValue(7);
    QTest::newRow("empty") << QJsonValue("");
    QTest::newRow("no prefix") << QJsonValue("xx_abcdefghijklmnopqrstuvwx");
    QTest::newRow("23 characters") << QJsonValue("pc_abcdefghijklmnopqrstuvw");
    QTest::newRow("25 characters") << QJsonValue("pc_abcdefghijklmnopqrstuvwxy");
    QTest::newRow("not base64url") << QJsonValue("pc_abcdefghijklmnopqrstuv+x");
}

void RunnerLinkOfferTest::dropsUnreadableConnectionIds() {
    QFETCH(QJsonValue, id);
    LinkHarness h;
    h.deliver(with(offer(), "connectionId", id));
    QVERIFY(h.out.sent.isEmpty());
    QVERIFY(h.events.events.isEmpty());
    QVERIFY(h.transports.records.isEmpty());
}

void RunnerLinkOfferTest::dropsAReplayWithoutTouchingTheAttempt() {
    LinkHarness h;
    h.deliver(offer());
    QCOMPARE(h.transports.records.size(), 1);
    h.clock.advance(1000);
    h.deliver(offer());
    QVERIFY(h.out.sent.isEmpty());
    QCOMPARE(h.events.events.size(), 1);
    QCOMPARE(h.transports.records.size(), 1);
    QVERIFY(!h.transports.last().closed);
    QVERIFY(h.links.owns(kId));
    QCOMPARE(h.clock.due.value(kId), attemptDeadlineMs);

    // Still dropped after the attempt ended, until ten minutes passed.
    h.links.close(kId, QStringLiteral("done"));
    h.out.sent.clear();
    h.clock.advance(usedOfferRetentionMs - 1001);
    h.deliver(offer());
    QVERIFY(h.out.sent.isEmpty());
    QCOMPARE(h.transports.records.size(), 1);
    // After that the offer is too old anyway.
    h.clock.advance(1001);
    h.deliver(offer());
    QCOMPARE(h.out.ends(), QStringList{"offer_expired"});
}

void RunnerLinkOfferTest::anUnverifiedOfferTakesNoPlace() {
    LinkHarness h;
    h.deliver(with(offer(), "sdp", "v=0\r\n"));
    h.deliver(with(offer(), "sdp", "v=0\r\n"));
    QCOMPARE(h.out.ends(), (QStringList{"invalid_signature", "invalid_signature"}));
    h.deliver(offer());
    QCOMPARE(h.out.ends().size(), 2);
    QCOMPARE(h.transports.records.size(), 1);

    // An unverified offer of the same runner leaves its attempt alone.
    h.deliver(with(offer({.connectionId = connectionId(2)}), "sdp", "v=0\r\n"));
    QCOMPARE(h.out.ends().last(), QStringLiteral("invalid_signature"));
    QVERIFY(h.links.owns(kId));
    QVERIFY(!h.transports.last().closed);
    QCOMPARE(h.events.events.size(), 1);
}

void RunnerLinkOfferTest::aFullUsedTableAnswersPeerLimit() {
    LinkHarness h;
    for (int n = 0; n < usedOfferLimit; ++n)
        h.deliver(offer({.connectionId = connectionId(n)}));
    QCOMPARE(h.transports.records.size(), usedOfferLimit);
    const QString last = connectionId(usedOfferLimit - 1);
    QVERIFY(h.links.owns(last));
    h.out.sent.clear();
    h.events.events.clear();

    // The pending attempt is not replaced and the job side is not asked.
    h.deliver(offer({.connectionId = connectionId(usedOfferLimit)}));
    QCOMPARE(h.out.ends(), QStringList{"peer_limit"});
    QVERIFY(h.events.events.isEmpty());
    QVERIFY(h.links.owns(last));

    // Ten minutes on, the table has room again (with a fresh offer).
    h.links.close(last, QStringLiteral("done"));
    h.out.sent.clear();
    h.clock.advance(usedOfferRetentionMs);
    const QString now = pairing::issuedAtString(h.clock.now());
    h.deliver(offer({.connectionId = connectionId(usedOfferLimit), .issuedAt = now}));
    QVERIFY(h.out.sent.isEmpty());
    QVERIFY(h.links.owns(connectionId(usedOfferLimit)));
}

void RunnerLinkOfferTest::ignoresOffersOfDevices_data() {
    QTest::addColumn<QJsonObject>("message");
    const QJsonObject good = offer();
    QTest::newRow("grant") << with(good, "grant", "g");
    QTest::newRow("deviceId") << with(good, "deviceId", "d");
    QTest::newRow("both") << with(with(good, "grant", "g"), "deviceId", "d");
    QTest::newRow("no runnerId") << with(good, "runnerId", QJsonValue());
    QTest::newRow("runnerId a number") << with(good, "runnerId", 3);
    QTest::newRow("no jobId") << with(good, "jobId", QJsonValue());
    QTest::newRow("another type") << with(good, "type", "signal.answer");
}

void RunnerLinkOfferTest::ignoresOffersOfDevices() {
    QFETCH(QJsonObject, message);
    LinkHarness h;
    h.deliver(message);
    QVERIFY(h.out.sent.isEmpty());
    QVERIFY(h.events.events.isEmpty());
    QVERIFY(h.transports.records.isEmpty());
}

void RunnerLinkOfferTest::signsTheAnswerVector() {
    LinkHarness h;
    h.deliver(offer());
    h.transports.last().events->localDescription(QString::fromLatin1(vectors::answerSdp));
    h.transports.last().events->localDescription(QStringLiteral("v=0\r\n"));
    QCOMPARE(h.out.sent.size(), 1);
    const QJsonObject answer = h.out.sent[0];
    QCOMPARE(answer, (QJsonObject{{"type", "signal.answer"},
                                  {"connectionId", kId},
                                  {"sdp", QString::fromLatin1(vectors::answerSdp)},
                                  {"machineKeyVersion", 1},
                                  {"issuedAt", QString::fromLatin1(vectors::issuedAt)},
                                  {"signature", QString::fromLatin1(vectors::answerSignature)}}));

    pairing::AnswerPayload payload;
    payload.connectionId = kId;
    payload.workspaceId = QString::fromLatin1(vectors::workspaceId);
    payload.machineId = QString::fromLatin1(vectors::machineId);
    payload.machineKeyVersion = vectors::machineKeyVersion;
    payload.runnerId = kRunner;
    payload.jobId = QString::fromLatin1(vectors::jobId);
    payload.offerSdp = QString::fromLatin1(vectors::offerSdp);
    payload.issuedAt = answer.value("issuedAt").toString();
    payload.sdp = answer.value("sdp").toString();
    QCOMPARE(payload.bytes(), QByteArray(vectors::answerBytes));
    QVERIFY(!pairing::verify(payload, cc::fromBase64Url(answer.value("signature").toString()),
                             {QByteArray::fromHex(vectors::desktopPublicKey), 1}, vectorTime()));
}

void RunnerLinkOfferTest::refusesAnAnswerItCannotSign() {
    LinkHarness h;
    h.identity.fail = true;
    h.deliver(offer());
    h.transports.last().events->localDescription(QString::fromLatin1(vectors::answerSdp));
    QCOMPARE(h.out.ends(), QStringList{"answer_failed"});
    QVERIFY(h.out.ofType("signal.answer").isEmpty());
    QCOMPARE(h.events.events.last(), "ended " + kId);
    QVERIFY(h.transports.last().closed);

    LinkHarness big;
    big.deliver(offer());
    big.transports.last().events->localDescription(QString(16 * 1024 + 1, 'v'));
    QCOMPARE(big.out.ends(), QStringList{"answer_failed"});
}

void RunnerLinkOfferTest::endsWhenTheTransportCannotAnswer() {
    LinkHarness h;
    h.transports.answerOk = false;
    h.deliver(offer());
    QCOMPARE(h.out.ends(), QStringList{"answer_failed"});
    QCOMPARE(h.events.events,
             (QStringList{"offered " + kId + " " + kRunner + " job_42 1", "ended " + kId}));
    QVERIFY(h.transports.last().closed);
    QVERIFY(h.clock.due.isEmpty());
    QVERIFY(!h.links.owns(kId));
}

} // namespace runner_link_test
