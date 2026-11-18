// The desktop pairing flow against a fake Relay on loopback, with time driven by the test:
// ticket, token, claim, code, approval and pin; refusals, adversarial claims, polling, the
// pin store and its lifecycle, and that no secret leaves the caller that created the ticket.
#include "RunnerTestFixture.h"
#include "pairing/PairingPins.h"
#include "pairing/PairingToken.h"
#include "runner/RunnerPinStore.h"

#include <QStandardPaths>
#include <QtTest>

using namespace runner_test;

namespace {

QStringList capturedLog;
void captureLog(QtMsgType, const QMessageLogContext&, const QString& message) {
    capturedLog.append(message);
}

pairing::DesktopRunnerPin pinOf(const FakeRunner& runner) {
    return {runner.id,
            runner.keyVersion,
            runner.key.publicKey,
            pairing::runnerFingerprint(runner.key.publicKey),
            runner.name,
            QDateTime(QDate(2026, 11, 18), QTime(9, 0), QTimeZone::UTC)};
}

QByteArray pinsRecord(const QJsonObject& runners, int version = 1) {
    return QJsonDocument(QJsonObject{{"version", version}, {"runners", runners}})
        .toJson(QJsonDocument::Compact);
}

} // namespace

class RunnerPairingDaemonTest final : public QObject {
    Q_OBJECT

    // Approves the claim shown by `claimed` with its code.
    static QJsonObject approve(Fixture& f, const QJsonObject& claimed) {
        return f.call("runner.pairing.approve",
                      {{"ticket_id", ticketId}, {"code", claimed.value("confirmation_code")}});
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void init() { r03::resetCredentials(); }

    void pairsInOrder() {
        Fixture f;
        FakeRunner runner;
        const QJsonObject created = f.createTicket();
        QVERIFY(created.value("ok").toBool());
        QCOMPARE(created.value("state").toString(), QStringLiteral("ticketed"));
        // The token names this desktop and carries the ticket secret, for this caller only.
        const auto token =
            pairing::PairingToken::parse(created.value("token").toString().toLatin1());
        QVERIFY(token.ok());
        QCOMPARE(token.value().kind(), pairing::PairingToken::Kind::NewRunner);
        QCOMPARE(token.value().fields().ticketId, ticketId);
        QCOMPARE(token.value().fields().ticketSecret, ticketSecret);
        QCOMPARE(token.value().fields().machineId, QStringLiteral("machine-synthetic"));
        QCOMPARE(token.value().fields().workspaceId, QStringLiteral("workspace-synthetic"));
        QCOMPARE(token.value().fields().machineKeyVersion, 1);
        QCOMPARE(token.value().fields().desktopFingerprint, f.desktopFingerprint());
        QCOMPARE(f.relay.of(createRoute).first().json(), (QJsonObject{{"runnerId", QJsonValue()}}));
        QCOMPARE(f.events.size(), 1);
        QCOMPARE(f.events.first(), (QJsonObject{{"ticket_id", ticketId}, {"state", "ticketed"}}));

        // Unclaimed: no code, nothing to approve.
        QCOMPARE(f.runPoll(), 2'000);
        QCOMPARE(f.relay.count(statusRoute), 1);
        QJsonObject status = f.call("runner.pairing.status", {{"ticket_id", ticketId}});
        QCOMPARE(status.value("state").toString(), QStringLiteral("ticketed"));
        QVERIFY(!status.contains("confirmation_code"));
        QVERIFY(!status.contains("token"));
        QCOMPARE(f.call("runner.pairing.approve", {{"ticket_id", ticketId}, {"code", "x"}})
                     .value("error_code"),
                 QJsonValue("not_claimed"));

        // Claimed: the code is this daemon's own, from the verified claim.
        f.relay.answer(statusRoute, 200, runner.claim(ticketId, Fixture::issued(f.now)));
        QCOMPARE(f.runPoll(), 4'000);
        status = f.call("runner.pairing.status", {{"ticket_id", ticketId}});
        QCOMPARE(status.value("state").toString(), QStringLiteral("claimed"));
        const auto expected = f.expectedCode(runner);
        QCOMPARE(status.value("confirmation_code").toString(), expected.code);
        QVERIFY(QRegularExpression("^[A-Z2-7]{5}-[A-Z2-7]{5}$")
                    .match(status.value("confirmation_code").toString())
                    .hasMatch());
        QCOMPARE(status.value("runner").toObject().value("fingerprint").toString(),
                 pairing::runnerFingerprint(runner.key.publicKey));
        QCOMPARE(f.events.last(), (QJsonObject{{"ticket_id", ticketId}, {"state", "claimed"}}));
        // Waiting for the user never approves.
        QCOMPARE(f.runPoll(), 8'000);
        QCOMPARE(f.relay.count(approveRoute), 0);
        QVERIFY(f.storedPins().isEmpty());

        // The user approves: the claim is read again, then the approval is signed and sent.
        f.relay.answer(approveRoute, 200, QJsonObject{{"pairState", "active"}});
        const int statusReads = f.relay.count(statusRoute);
        const QJsonObject approved = approve(f, status);
        QVERIFY2(approved.value("ok").toBool(),
                 qPrintable(approved.value("error_code").toString()));
        QCOMPARE(approved.value("state").toString(), QStringLiteral("active"));
        QCOMPARE(f.relay.count(statusRoute), statusReads + 1);
        QCOMPARE(f.relay.requests.last().method, QByteArray("POST"));
        const QJsonObject body = f.relay.of(approveRoute).first().json();
        QCOMPARE(body.value("pairingDigest").toString(), expected.digest);
        const QByteArray signature =
            *pairing::decodeBase64Url(body.value("approvalSignature").toString());
        QVERIFY(crypto::verifyEd25519(f.key("signingPublicKey"),
                                      pairing::ApprovalPayload{ticketId, expected.digest}.bytes(),
                                      signature));
        // The runner, holding the token, accepts this approval against its pending pin.
        const auto pending = pairing::RunnerPin::pending(
            token.value(),
            {"machine-synthetic", 1, f.key("signingPublicKey"), f.key("agreementPublicKey")},
            expected.digest);
        QVERIFY(pending.ok());
        QVERIFY(!pairing::verifyApproval(pending.value(), signature));

        // The pin is written after the Relay accepted, in the T-6 record format.
        const QJsonObject record = QJsonDocument::fromJson(f.storedPins()).object();
        QCOMPARE(record.value("version").toInt(), 1);
        const auto pin = pairing::DesktopRunnerPin::fromJson(
            record.value("runners").toObject().value(runner.id).toObject());
        QVERIFY(pin.ok());
        QCOMPARE(pin.value().publicKey, runner.key.publicKey);
        QCOMPARE(pin.value().runnerKeyVersion, 1);
        QCOMPARE(f.events.last(), (QJsonObject{{"ticket_id", ticketId}, {"state", "active"}}));
        // An active pairing polls no more.
        f.scheduled.clear();
        QCOMPARE(f.runPoll(), -1);
    }

    void addsThisDesktopToAKnownRunner() {
        Fixture f;
        FakeRunner runner;
        const QJsonObject created = f.createTicket({{"runner_id", runner.id}});
        const auto token =
            pairing::PairingToken::parse(created.value("token").toString().toLatin1());
        QCOMPARE(token.value().kind(), pairing::PairingToken::Kind::AddDesktop);
        QCOMPARE(*token.value().fields().runnerId, runner.id);
        // Another runner claiming the ticket is not the one asked for.
        FakeRunner other;
        other.id = "runner-b";
        f.relay.answer(statusRoute, 200, other.claim(ticketId, Fixture::issued(f.now)));
        f.runPoll();
        const QJsonObject status = f.call("runner.pairing.status", {{"ticket_id", ticketId}});
        QCOMPARE(status.value("state").toString(), QStringLiteral("failed"));
        QVERIFY(!status.contains("confirmation_code"));
    }

    void terminalStatesLeaveNoPin_data() {
        QTest::addColumn<QString>("relayState");
        QTest::addColumn<QString>("expected");
        QTest::newRow("rejected") << "rejected" << "rejected";
        QTest::newRow("expired") << "expired" << "expired";
        QTest::newRow("revoked") << "revoked" << "ticket_used";
    }
    void terminalStatesLeaveNoPin() {
        QFETCH(QString, relayState);
        QFETCH(QString, expected);
        Fixture f;
        FakeRunner runner;
        f.claimed(runner);
        f.relay.answer(statusRoute, 200,
                       QJsonObject{{"ticketId", ticketId}, {"pairState", relayState}});
        f.runPoll();
        const QJsonObject status = f.call("runner.pairing.status", {{"ticket_id", ticketId}});
        QCOMPARE(status.value("state").toString(), expected);
        QVERIFY(!status.contains("confirmation_code"));
        QCOMPARE(f.events.last().value("state").toString(), expected);
        QVERIFY(f.scheduled.isEmpty());
        QVERIFY(f.storedPins().isEmpty());
    }

    void approvalRefusalsEndThePairing_data() {
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("expected");
        QTest::newRow("ticket_used") << 409 << "ticket_used" << "ticket_used";
        QTest::newRow("ticket_expired") << 410 << "ticket_expired" << "expired";
        QTest::newRow("digest_mismatch") << 409 << "digest_mismatch" << "failed";
    }
    void approvalRefusalsEndThePairing() {
        QFETCH(int, status);
        QFETCH(QString, code);
        QFETCH(QString, expected);
        Fixture f;
        FakeRunner runner;
        const QJsonObject claimed = f.claimed(runner);
        f.relay.refuse(approveRoute, status, code);
        QCOMPARE(approve(f, claimed).value("error_code").toString(), code);
        QCOMPARE(
            f.call("runner.pairing.status", {{"ticket_id", ticketId}}).value("state").toString(),
            expected);
        QVERIFY(f.storedPins().isEmpty());
        // No second approval of a finished pairing.
        QCOMPARE(approve(f, claimed).value("error_code").toString(),
                 QStringLiteral("pairing_failed"));
        QCOMPARE(f.relay.count(approveRoute), 1);
    }

    void userRejectEndsThePairing() {
        Fixture f;
        FakeRunner runner;
        f.claimed(runner);
        f.relay.answer(rejectRoute, 204);
        const QJsonObject rejected = f.call("runner.pairing.reject", {{"ticket_id", ticketId}});
        QCOMPARE(rejected.value("state").toString(), QStringLiteral("rejected"));
        QCOMPARE(f.relay.count(rejectRoute), 1);
        QCOMPARE(f.relay.count(approveRoute), 0);
        QVERIFY(f.storedPins().isEmpty());
        QCOMPARE(f.runPoll(), 4'000); // the poll already scheduled asks nothing more
        QCOMPARE(f.relay.count(statusRoute), 1);
    }

    void claimSignatureMustVerify_data() {
        QTest::addColumn<QString>("variant");
        for (const char* variant : {"tampered signature", "other workspace", "other machine",
                                    "other ticket", "substituted key", "key version",
                                    "issued long before the ticket", "issued long after expiry"})
            QTest::newRow(variant) << QString::fromLatin1(variant);
    }
    void claimSignatureMustVerify() {
        QFETCH(QString, variant);
        Fixture f;
        FakeRunner runner;
        f.createTicket();
        const QString issued = Fixture::issued(f.now);
        QJsonObject claim = runner.claim(ticketId, issued);
        if (variant == "tampered signature") {
            QByteArray signature = *pairing::decodeBase64Url(claim.value("signature").toString());
            signature[0] = char(signature[0] ^ 1);
            claim.insert("signature", b64(signature));
        } else if (variant == "other workspace") {
            claim = runner.claim(ticketId, issued, "workspace-other");
        } else if (variant == "other machine") {
            claim = runner.claim(ticketId, issued, "workspace-synthetic", "machine-other");
        } else if (variant == "other ticket") {
            claim = runner.claim("ticket-other", issued);
            claim.insert("ticketId", ticketId);
        } else if (variant == "substituted key") {
            claim.insert("publicKey", b64(crypto::generateEd25519().publicKey));
        } else if (variant == "key version") {
            claim.insert("keyVersion", 2);
        } else if (variant == "issued long before the ticket") {
            claim = runner.claim(ticketId, Fixture::issued(f.now.addMSecs(-5 * 60'000 - 1)));
        } else {
            claim = runner.claim(ticketId, Fixture::issued(f.now.addMSecs(65 * 60'000 + 1)));
        }
        f.relay.answer(statusRoute, 200, claim);
        f.runPoll();
        const QJsonObject status = f.call("runner.pairing.status", {{"ticket_id", ticketId}});
        QCOMPARE(status.value("state").toString(), QStringLiteral("failed"));
        QCOMPARE(status.value("failure_reason").toString(), QStringLiteral("invalid_claim"));
        QVERIFY(!status.contains("confirmation_code"));
        QCOMPARE(
            f.call("runner.pairing.approve", {{"ticket_id", ticketId}, {"code", "AAAAA-AAAAA"}})
                .value("error_code"),
            QJsonValue("pairing_failed"));
        QCOMPARE(f.relay.count(approveRoute), 0);
        QVERIFY(f.scheduled.isEmpty());
    }

    void claimTimeFollowsTheTicketNotTheClock_data() {
        QTest::addColumn<qint64>("issuedOffsetMs");
        QTest::addColumn<qint64>("readAfterMs");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("read 50 minutes late") << qint64(0) << qint64(50 * 60'000) << true;
        QTest::newRow("5 minutes before the ticket") << qint64(-5 * 60'000) << qint64(0) << true;
        QTest::newRow("5 minutes after expiry") << qint64(65 * 60'000) << qint64(0) << true;
        QTest::newRow("too early") << qint64(-5 * 60'000 - 1) << qint64(0) << false;
        QTest::newRow("too late") << qint64(65 * 60'000 + 1) << qint64(0) << false;
    }
    void claimTimeFollowsTheTicketNotTheClock() {
        QFETCH(qint64, issuedOffsetMs);
        QFETCH(qint64, readAfterMs);
        QFETCH(bool, accepted);
        Fixture f;
        FakeRunner runner;
        const QDateTime created = f.now;
        f.createTicket();
        f.relay.answer(statusRoute, 200,
                       runner.claim(ticketId, Fixture::issued(created.addMSecs(issuedOffsetMs))));
        // The first poll happens late: the desktop slept.
        f.scheduled.first().first = int(qMax<qint64>(readAfterMs, 2'000));
        f.runPoll();
        const QJsonObject status = f.call("runner.pairing.status", {{"ticket_id", ticketId}});
        QCOMPARE(status.contains("confirmation_code"), accepted);
        QCOMPARE(status.value("state").toString(),
                 accepted ? QStringLiteral("claimed") : QStringLiteral("failed"));
    }

    void changedClaimFailsForGood_data() {
        QTest::addColumn<QString>("change");
        QTest::addColumn<bool>("atApproval");
        for (const char* change : {"key", "key version", "runner id"}) {
            QTest::addRow("%s between polls", change) << QString::fromLatin1(change) << false;
            QTest::addRow("%s at approval", change) << QString::fromLatin1(change) << true;
        }
        QTest::newRow("claim withdrawn") << "withdrawn" << false;
    }
    void changedClaimFailsForGood() {
        QFETCH(QString, change);
        QFETCH(bool, atApproval);
        Fixture f;
        FakeRunner runner;
        const QJsonObject claimed = f.claimed(runner);
        QVERIFY(claimed.contains("confirmation_code"));
        FakeRunner changed = runner;
        if (change == "key")
            changed.key = crypto::generateEd25519();
        else if (change == "key version")
            changed.keyVersion = 2;
        else
            changed.id = "runner-b";
        // A validly signed claim, but not the one the user saw.
        f.relay.answer(statusRoute, 200,
                       change == "withdrawn"
                           ? QJsonObject{{"ticketId", ticketId}, {"pairState", "ticketed"}}
                           : changed.claim(ticketId, Fixture::issued(f.now)));
        f.relay.answer(approveRoute, 200, QJsonObject{{"pairState", "active"}});
        if (atApproval) {
            QCOMPARE(approve(f, claimed).value("error_code").toString(),
                     QStringLiteral("claim_changed"));
        } else {
            f.runPoll();
        }
        const QJsonObject status = f.call("runner.pairing.status", {{"ticket_id", ticketId}});
        QCOMPARE(status.value("state").toString(), QStringLiteral("failed"));
        QCOMPARE(status.value("failure_reason").toString(), QStringLiteral("claim_changed"));
        QVERIFY(!status.contains("confirmation_code"));
        // Not even the original claim revives it.
        f.relay.answer(statusRoute, 200, runner.claim(ticketId, Fixture::issued(f.now)));
        QCOMPARE(approve(f, claimed).value("error_code").toString(),
                 QStringLiteral("pairing_failed"));
        QVERIFY(!f.call("runner.pairing.status", {{"ticket_id", ticketId}})
                     .contains("confirmation_code"));
        QCOMPARE(f.relay.count(approveRoute), 0);
        QVERIFY(f.storedPins().isEmpty());
    }

    void wrongCodesEndThePairing() {
        Fixture f;
        FakeRunner runner;
        const QJsonObject claimed = f.claimed(runner);
        const int reads = f.relay.requests.size();
        for (int attempt = 1; attempt < 5; ++attempt) {
            const QJsonObject result = f.call("runner.pairing.approve",
                                              {{"ticket_id", ticketId}, {"code", "AAAAA-AAAAA"}});
            QCOMPARE(result.value("error_code").toString(), QStringLiteral("code_mismatch"));
            QCOMPARE(f.call("runner.pairing.status", {{"ticket_id", ticketId}})
                         .value("attempts_left")
                         .toInt(),
                     5 - attempt);
        }
        QCOMPARE(f.relay.requests.size(), reads); // a wrong code never reaches the Relay
        QCOMPARE(
            f.call("runner.pairing.approve", {{"ticket_id", ticketId}, {"code", "BBBBB-BBBBB"}})
                .value("error_code"),
            QJsonValue("pairing_failed"));
        // The right code is too late now.
        QCOMPARE(approve(f, claimed).value("error_code"), QJsonValue("pairing_failed"));
        const QJsonObject status = f.call("runner.pairing.status", {{"ticket_id", ticketId}});
        QCOMPARE(status.value("failure_reason").toString(), QStringLiteral("too_many_attempts"));
        QCOMPARE(f.relay.requests.size(), reads);
    }

    void codeIsCaseAndSpaceTolerant() {
        Fixture f;
        FakeRunner runner;
        const QJsonObject claimed = f.claimed(runner);
        f.relay.answer(approveRoute, 200, QJsonObject{{"pairState", "active"}});
        const QString typed = " " + claimed.value("confirmation_code").toString().toLower() + "\n";
        QVERIFY(f.call("runner.pairing.approve", {{"ticket_id", ticketId}, {"code", typed}})
                    .value("ok")
                    .toBool());
    }

    void lostApprovalAnswerCompletesOnTheNextRead() {
        Fixture f;
        FakeRunner runner;
        const QJsonObject claimed = f.claimed(runner);
        f.relay.answer(approveRoute, 0);
        QCOMPARE(approve(f, claimed).value("error_code").toString(), QStringLiteral("network"));
        QCOMPARE(
            f.call("runner.pairing.status", {{"ticket_id", ticketId}}).value("state").toString(),
            QStringLiteral("claimed"));
        QVERIFY(f.storedPins().isEmpty());
        // The Relay had recorded it: the next read says active and the pin is written then.
        f.relay.answer(statusRoute, 200,
                       QJsonObject{{"ticketId", ticketId}, {"pairState", "active"}});
        f.runPoll();
        QCOMPARE(
            f.call("runner.pairing.status", {{"ticket_id", ticketId}}).value("state").toString(),
            QStringLiteral("active"));
        CredentialStore store;
        QVERIFY(runner::RunnerPinStore(store).pin(runner.id));
    }

    void pollDuringApprovalKeepsPolling() {
        Fixture f;
        FakeRunner runner;
        const QJsonObject claimed = f.claimed(runner);
        f.relay.answer(approveRoute, 0);
        // A poll goes out, and the user approves before its answer arrives.
        auto [delay, task] = f.scheduled.takeFirst();
        f.now = f.now.addMSecs(delay);
        task();
        QCOMPARE(approve(f, claimed).value("error_code").toString(), QStringLiteral("network"));
        QTRY_VERIFY(!f.scheduled.isEmpty());
        f.relay.answer(statusRoute, 200,
                       QJsonObject{{"ticketId", ticketId}, {"pairState", "active"}});
        f.runPoll();
        QCOMPARE(
            f.call("runner.pairing.status", {{"ticket_id", ticketId}}).value("state").toString(),
            QStringLiteral("active"));
    }

    void activeWithoutThisApprovalIsRefused() {
        Fixture f;
        FakeRunner runner;
        f.claimed(runner);
        f.relay.answer(statusRoute, 200,
                       QJsonObject{{"ticketId", ticketId}, {"pairState", "active"}});
        f.runPoll();
        const QJsonObject status = f.call("runner.pairing.status", {{"ticket_id", ticketId}});
        QCOMPARE(status.value("state").toString(), QStringLiteral("failed"));
        QVERIFY(f.storedPins().isEmpty());
    }

    void desktopKeyChangeEndsThePairing() {
        Fixture f;
        FakeRunner runner;
        const QJsonObject claimed = f.claimed(runner);
        // The token named key version 1; the approval would be signed by another key.
        Access::machineKeyVersion(*f.client, 2);
        f.relay.answer(approveRoute, 200, QJsonObject{{"pairState", "active"}});
        QCOMPARE(approve(f, claimed).value("error_code").toString(),
                 QStringLiteral("pairing_failed"));
        QCOMPARE(f.call("runner.pairing.status", {{"ticket_id", ticketId}})
                     .value("failure_reason")
                     .toString(),
                 QStringLiteral("desktop_key_changed"));
        QCOMPARE(f.relay.count(approveRoute), 0);
        QVERIFY(f.storedPins().isEmpty());
    }

    void pinWriteFailureLeavesTheRunnerUntrusted() {
        Fixture f;
        FakeRunner runner;
        const QJsonObject claimed = f.claimed(runner);
        f.relay.answer(approveRoute, 200, QJsonObject{{"pairState", "active"}});
        r03::failWritesTo = "runner-pins";
        QCOMPARE(approve(f, claimed).value("error_code").toString(),
                 QStringLiteral("pin_store_failed"));
        r03::failWritesTo.clear();
        QVERIFY(f.storedPins().isEmpty());
    }

    void pollingBacksOffAndStops_data() {
        QTest::addColumn<QString>("code");
        QTest::addColumn<int>("status");
        QTest::addColumn<bool>("retried");
        QTest::newRow("network") << "" << 0 << true;
        QTest::newRow("server error") << "" << 503 << true;
        QTest::newRow("rate_limited") << "rate_limited" << 429 << true;
        QTest::newRow("entitlement_required") << "entitlement_required" << 403 << false;
        QTest::newRow("unsupported") << "unsupported" << 403 << false;
        QTest::newRow("invalid_access") << "invalid_access" << 401 << false;
        QTest::newRow("ticket_not_found") << "ticket_not_found" << 404 << false;
    }
    void pollingBacksOffAndStops() {
        QFETCH(QString, code);
        QFETCH(int, status);
        QFETCH(bool, retried);
        Fixture f;
        f.createTicket();
        if (code.isEmpty())
            f.relay.answer(statusRoute, status);
        else
            f.relay.refuse(statusRoute, status, code);
        QCOMPARE(f.runPoll(), 2'000);
        const QJsonObject state = f.call("runner.pairing.status", {{"ticket_id", ticketId}});
        if (!retried) {
            // Each refusal is its own final state; nothing more is asked.
            QCOMPARE(state.value("state").toString(), QStringLiteral("failed"));
            QCOMPARE(state.value("failure_reason").toString(), code);
            QVERIFY(f.scheduled.isEmpty());
            return;
        }
        QCOMPARE(state.value("state").toString(), QStringLiteral("ticketed"));
        QCOMPARE(state.value("last_error").toString(),
                 code.isEmpty()
                     ? (status == 0 ? QStringLiteral("network") : QStringLiteral("relay_error"))
                     : code);
        QList<int> delays;
        while (!f.scheduled.isEmpty() && delays.size() < 400)
            delays.append(f.runPoll());
        const QList<int> expected = code == "rate_limited"
                                        ? QList<int>{30'000, 30'000, 30'000}
                                        : QList<int>{4'000, 8'000, 16'000, 30'000, 30'000};
        QCOMPARE(delays.mid(0, expected.size()), expected);
        // It stopped at the ticket's hour, not after: about two polls a minute at most.
        QVERIFY2(delays.size() < 130, qPrintable(QString::number(delays.size())));
        QCOMPARE(
            f.call("runner.pairing.status", {{"ticket_id", ticketId}}).value("state").toString(),
            QStringLiteral("expired"));
        QCOMPARE(f.events.last().value("state").toString(), QStringLiteral("expired"));
    }

    void relayExpiryBeyondAnHourIsNotBelieved() {
        Fixture f;
        f.relay.answer(createRoute, 201,
                       QJsonObject{{"ticketId", ticketId},
                                   {"ticketSecret", ticketSecret},
                                   {"expiresAt", f.now.addDays(3).toString(Qt::ISODateWithMs)}});
        const QJsonObject created = f.call("runner.pairing.create");
        QCOMPARE(QDateTime::fromString(created.value("expires_at").toString(), Qt::ISODateWithMs),
                 f.now.addSecs(3600));
    }

    void capabilityLossStopsAPendingPairing() {
        Fixture f;
        FakeRunner runner;
        const QJsonObject claimed = f.claimed(runner);
        const int asked = f.relay.requests.size();
        f.offerRunners(false);
        f.runPoll();
        QCOMPARE(f.relay.requests.size(), asked);
        QCOMPARE(f.events.last(), (QJsonObject{{"ticket_id", ticketId}, {"state", "failed"}}));
        QVERIFY(f.scheduled.isEmpty());
        // Offered again, the pairing stays failed: the user starts over.
        f.offerRunners(true);
        const QJsonObject status = f.call("runner.pairing.status", {{"ticket_id", ticketId}});
        QCOMPARE(status.value("failure_reason").toString(), QStringLiteral("runner_unavailable"));
        QCOMPARE(approve(f, claimed).value("error_code"), QJsonValue("pairing_failed"));
        QCOMPARE(f.relay.requests.size(), asked);
    }

    void signOutEndsPairingsAndPins() {
        Fixture f;
        FakeRunner runner;
        CredentialStore store;
        QVERIFY(runner::RunnerPinStore(store).store(pinOf(runner)));
        const QJsonObject claimed = f.claimed(runner);
        QVERIFY(!f.storedPins().isEmpty());
        bool signedOut = false;
        f.client->handleLocal("remote.sign_out", {},
                              [&](const QJsonObject&, const QString&) { signedOut = true; });
        QTRY_VERIFY(signedOut);
        QVERIFY(f.storedPins().isEmpty());
        // Runners are offered again by a heartbeat of the next account; this pairing is not its.
        f.offerRunners(true);
        Access::configure(*f.client, *f.network,
                          QUrl(QStringLiteral("http://127.0.0.1:%1").arg(f.relay.serverPort())));
        QCOMPARE(approve(f, claimed).value("error_code"), QJsonValue("pairing_failed"));
        QCOMPARE(f.call("runner.pairing.status", {{"ticket_id", ticketId}})
                     .value("failure_reason")
                     .toString(),
                 QStringLiteral("account_changed"));
        QCOMPARE(f.relay.count(approveRoute), 0);
    }

    void reEnrollmentDropsRunnerPins() {
        Fixture f;
        FakeRunner runner;
        CredentialStore store;
        QVERIFY(runner::RunnerPinStore(store).store(pinOf(runner)));
        f.relay.answer("POST /v2/machines:enroll", 201,
                       QJsonObject{{"id", "machine-new"},
                                   {"workspaceId", "workspace-new"},
                                   {"keyVersion", 1}});
        bool answered = false;
        Access::enroll(*f.client, [&](const QJsonObject&, const QString&) { answered = true; });
        QTRY_VERIFY(answered);
        QCOMPARE(Access::machineId(*f.client), QStringLiteral("machine-new"));
        QVERIFY(f.storedPins().isEmpty());
    }

    void pinStoreReadsOnlyValidRecords() {
        CredentialStore store;
        runner::RunnerPinStore pins(store);
        FakeRunner a;
        FakeRunner b;
        b.id = "runner-b";
        QVERIFY(pins.store(pinOf(a)));
        QVERIFY(pins.store(pinOf(b)));
        QCOMPARE(pins.load().pins.size(), 2);
        QVERIFY(pins.pin(a.id));
        QCOMPARE(pins.pin(a.id)->publicKey, a.key.publicKey);

        // One entry whose fingerprint is not its key's, one under another runner's id, one of
        // another record version: each is ignored and reported; the rest still read.
        QJsonObject wrongPrint = pinOf(b).toJson();
        wrongPrint.insert("runnerFingerprint", pairing::runnerFingerprint(a.key.publicKey));
        QJsonObject otherVersion = pinOf(b).toJson();
        otherVersion.insert("version", 2);
        QJsonObject runners{{a.id, pinOf(a).toJson()},
                            {"runner-b", wrongPrint},
                            {"runner-c", pinOf(a).toJson()},
                            {"runner-d", otherVersion},
                            {"runner-e", "not-a-pin"}};
        QVERIFY(store.write("runner-pins", pinsRecord(runners)));
        auto loaded = pins.load();
        QCOMPARE(loaded.pins.keys(), QList<QString>{a.id});
        loaded.ignored.sort();
        QCOMPARE(loaded.ignored, (QStringList{"runner-b", "runner-c", "runner-d", "runner-e"}));
        QVERIFY(loaded.error.isEmpty());

        // A record that does not parse, or of another version, trusts nothing.
        QVERIFY(store.write("runner-pins", "{not json"));
        loaded = pins.load();
        QVERIFY(loaded.pins.isEmpty());
        QVERIFY(!loaded.error.isEmpty());
        QVERIFY(pins.store(pinOf(a))); // a corrupt record is replaced
        QCOMPARE(pins.load().pins.size(), 1);
        QVERIFY(store.write("runner-pins", pinsRecord({{a.id, pinOf(a).toJson()}}, 2)));
        loaded = pins.load();
        QVERIFY(loaded.pins.isEmpty());
        QVERIFY(!loaded.error.isEmpty());
        QVERIFY(!pins.store(pinOf(b))); // another version's record is kept for its build
        QCOMPARE(
            QJsonDocument::fromJson(store.read("runner-pins")).object().value("version").toInt(),
            2);

        // Removal is idempotent.
        QVERIFY(store.write("runner-pins", pinsRecord({{a.id, pinOf(a).toJson()}})));
        QVERIFY(pins.remove(a.id));
        QVERIFY(pins.remove(a.id));
        QVERIFY(!pins.pin(a.id));
        // A failed read trusts nothing and says so.
        r03::failReadsOf = "runner-pins";
        QVERIFY(!pins.load().error.isEmpty());
    }

    void storedPinsAreNotTrustedWhenForged() {
        Fixture f;
        FakeRunner runner;
        CredentialStore store;
        QJsonObject forged = pinOf(runner).toJson();
        forged.insert("publicKey", b64(crypto::generateEd25519().publicKey));
        QVERIFY(store.write("runner-pins", pinsRecord({{runner.id, forged}})));
        f.relay.answer(
            listRoute, 200,
            QJsonObject{{"runners", QJsonArray{QJsonObject{{"runnerId", runner.id},
                                                           {"name", "n"},
                                                           {"os", "linux"},
                                                           {"arch", "arm64"},
                                                           {"labels", QJsonArray{}},
                                                           {"state", "idle"},
                                                           {"runnerVersion", "1"},
                                                           {"lastSeenAt", QJsonValue()},
                                                           {"pairedWithThisMachine", true}}}},
                        {"nextCursor", QJsonValue()}});
        const QJsonObject listed = f.call("runner.list");
        QVERIFY(!listed.value("runners").toArray().first().toObject().value("trusted").toBool());
        QVERIFY(listed.contains("pin_error"));
    }

    // Removing or unpairing names the local connection that asked, never a client id of the
    // payload; without a connection the client id is empty.
    void runnerRemovalNamesTheAskingConnection() {
        Fixture f;
        QList<std::pair<QString, QString>> removed;
        QObject::connect(&f.service(), &runner::RunnerService::runnerRemoved,
                         [&](const QString& runnerId, const QString& clientId) {
                             removed.append({runnerId, clientId});
                         });
        f.relay.answer("POST /v2/runners/runner-a/unpair", 0);
        bool done = false;
        f.client->handleLocal(
            "runner.unpair", {{"runner_id", "runner-a"}, {"client_id", "spoofed"}},
            [&](const QJsonObject&, const QString&) { done = true; },
            QStringLiteral("connection-7"));
        QVERIFY(QTest::qWaitFor([&] { return done; }, 5000));
        f.relay.answer("POST /v2/runners/runner-b/unpair", 0);
        f.call("runner.unpair", {{"runner_id", "runner-b"}, {"client_id", "spoofed"}});
        QCOMPARE(removed.size(), 2);
        QCOMPARE(removed.at(0),
                 std::make_pair(QStringLiteral("runner-a"), QStringLiteral("connection-7")));
        QCOMPARE(removed.at(1), std::make_pair(QStringLiteral("runner-b"), QString()));
    }

    void noSecretLeaves() {
        capturedLog.clear();
        const auto previous = qInstallMessageHandler(captureLog);
        QByteArray token;
        QList<QJsonObject> events;
        QList<QJsonObject> replies;
        {
            Fixture f;
            FakeRunner runner;
            const QJsonObject created = f.createTicket();
            token = created.value("token").toString().toLatin1();
            f.relay.answer(statusRoute, 0);
            f.runPoll();
            f.relay.answer(statusRoute, 200, runner.claim(ticketId, Fixture::issued(f.now)));
            f.runPoll();
            const QJsonObject status = f.call("runner.pairing.status", {{"ticket_id", ticketId}});
            f.call("runner.pairing.approve", {{"ticket_id", ticketId}, {"code", "AAAAA-AAAAA"}});
            f.relay.answer(approveRoute, 200, QJsonObject{{"pairState", "active"}});
            approve(f, status);
            f.relay.refuse(listRoute, 401, "invalid_access");
            f.call("runner.list");
            f.relay.answer("POST /v2/runners/runner-a/unpair", 0);
            f.call("runner.unpair", {{"runner_id", "runner-a"}});
            events = f.events;
            replies = f.replies.mid(1); // all but the create reply, which carries the token
            qDebug() << pairing::PairingToken::parse(token).value();
        }
        qInstallMessageHandler(previous);
        QVERIFY(!token.isEmpty());
        QStringList secrets{QString::fromLatin1(token), ticketSecret,
                            QStringLiteral("synthetic-test-token")};
        QStringList outputs = capturedLog;
        for (const auto& event : events)
            outputs.append(QString::fromUtf8(QJsonDocument(event).toJson()));
        for (const auto& reply : replies)
            outputs.append(QString::fromUtf8(QJsonDocument(reply).toJson()));
        for (auto it = r03::credentials.cbegin(); it != r03::credentials.cend(); ++it)
            outputs.append(QString::fromUtf8(it.value()));
        QVERIFY(outputs.size() > 8);
        for (const QString& output : outputs) {
            for (const QString& secret : secrets)
                QVERIFY2(!output.contains(secret), qPrintable(output.left(120)));
            QVERIFY(!output.contains(QStringLiteral("kpr1.")));
        }
        // Events name a ticket and a state, nothing else.
        for (const auto& event : events)
            QCOMPARE(event.keys(), (QStringList{"state", "ticket_id"}));
    }
};

QTEST_GUILESS_MAIN(RunnerPairingDaemonTest)
#include "RunnerPairingDaemonTest.moc"
