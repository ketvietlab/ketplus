// The pairing half of RunnerService: ticket, claim, confirmation code, approval and pin.
#include "runner/RunnerService.h"

#include "pairing/PairingSignatures.h"
#include "pairing/PairingToken.h"

namespace ketplus::daemon::runner {

namespace pairing = ketplus::runner::pairing;

void RunnerService::createPairing(const QJsonObject& payload, Callback callback) {
    std::optional<QString> runnerId;
    const QJsonValue requested = payload.value(QStringLiteral("runner_id"));
    if (requested.isString()) {
        runnerId = requested.toString();
        if (!isRunnerId(*runnerId)) {
            fail(callback, QStringLiteral("invalid_request"));
            return;
        }
    } else if (!requested.isUndefined() && !requested.isNull()) {
        fail(callback, QStringLiteral("invalid_request"));
        return;
    }
    const auto desktop = port_->machineIdentity();
    if (!desktop) {
        fail(callback, QStringLiteral("not_enrolled"));
        return;
    }
    prunePairings();
    if (pairings_.size() >= maximumPairings) {
        fail(callback, QStringLiteral("too_many_pairings"));
        return;
    }
    const QString scope = port_->accountScope();
    const QDateTime createdAt = clock_();
    client_.createTicket(
        runnerId, [this, runnerId, desktop = *desktop, scope, createdAt,
                   callback = std::move(callback)](const RunnerOutcome<RunnerTicket>& outcome) {
            if (!outcome.ok()) {
                fail(callback, outcome.failure);
                return;
            }
            if (port_->accountScope() != scope) {
                fail(callback, QStringLiteral("account_changed"));
                return;
            }
            const RunnerTicket& ticket = *outcome.value;
            if (pairings_.contains(ticket.ticketId)) {
                fail(callback, QStringLiteral("invalid_response"));
                return;
            }
            const auto token = pairing::PairingToken::create(
                {ticket.ticketId, ticket.ticketSecret, desktop.workspaceId, desktop.machineId,
                 desktop.keyVersion,
                 pairing::desktopFingerprint(desktop.signingPublicKey, desktop.agreementPublicKey),
                 runnerId});
            if (!token) {
                fail(callback, QStringLiteral("invalid_response"));
                return;
            }
            auto created = std::make_shared<RunnerPairing>(ticket.ticketId, runnerId, desktop,
                                                           scope, createdAt, ticket.expiresAt);
            pairings_.insert(ticket.ticketId, created);
            schedulePoll(created, firstPollMs);
            changed(*created);
            // The only copy of the token: it goes to this caller and nowhere else.
            QJsonObject result = created->toJson();
            result.insert(QStringLiteral("ok"), true);
            result.insert(QStringLiteral("token"), QString::fromLatin1(token.value().encode()));
            callback(result, {});
        });
}

void RunnerService::pairingStatus(const QJsonObject& payload, Callback callback) {
    const PairingPointer found = pairing(payload);
    if (!found) {
        fail(callback, QStringLiteral("ticket_not_found"));
        return;
    }
    stopIfStale(found);
    QJsonObject result = found->toJson();
    result.insert(QStringLiteral("ok"), true);
    callback(result, {});
}

void RunnerService::approvePairing(const QJsonObject& payload, Callback callback) {
    const PairingPointer found = pairing(payload);
    if (!found) {
        fail(callback, QStringLiteral("ticket_not_found"));
        return;
    }
    if (stopIfStale(found) || found->state() != RunnerPairing::State::Claimed) {
        fail(callback,
             found->terminal() ? QStringLiteral("pairing_failed") : QStringLiteral("not_claimed"));
        return;
    }
    switch (found->checkCode(payload.value(QStringLiteral("code")).toString())) {
    case RunnerPairing::CodeCheck::Match:
        break;
    case RunnerPairing::CodeCheck::Mismatch:
        fail(callback, QStringLiteral("code_mismatch"));
        return;
    case RunnerPairing::CodeCheck::Exhausted:
        changed(*found);
        fail(callback, QStringLiteral("pairing_failed"));
        return;
    }
    // The claim is read again: the Relay must still hold the keys the user confirmed.
    found->holdForApproval();
    client_.ticketStatus(found->ticketId(), [this, found, callback = std::move(callback)](
                                                const RunnerOutcome<RunnerTicketStatus>& outcome) {
        if (!outcome.ok()) {
            found->endApproval();
            fail(callback, outcome.failure);
            return;
        }
        found->endApproval();
        const RunnerPairing::Effect effect = found->applyStatus(*outcome.value);
        if (effect == RunnerPairing::Effect::ApprovedActive) {
            // An earlier approval whose answer was lost: the Relay recorded it.
            finishActive(found, std::move(callback));
            return;
        }
        if (effect != RunnerPairing::Effect::None)
            changed(*found);
        if (found->state() != RunnerPairing::State::Claimed ||
            outcome.value->pairState != QStringLiteral("claimed")) {
            fail(callback, found->failureReason() == QStringLiteral("claim_changed")
                               ? QStringLiteral("claim_changed")
                               : QStringLiteral("pairing_failed"));
            return;
        }
        sendApproval(found, std::move(callback));
    });
}

void RunnerService::sendApproval(const PairingPointer& found, Callback callback) {
    if (stopIfStale(found)) {
        fail(callback, QStringLiteral("pairing_failed"));
        return;
    }
    // The machine key that signs must be the one the token named.
    const auto desktop = port_->machineIdentity();
    const MachineIdentity& named = found->desktop();
    if (!desktop || desktop->machineId != named.machineId ||
        desktop->workspaceId != named.workspaceId || desktop->keyVersion != named.keyVersion ||
        desktop->signingPublicKey != named.signingPublicKey ||
        desktop->agreementPublicKey != named.agreementPublicKey) {
        found->fail(QStringLiteral("desktop_key_changed"));
        changed(*found);
        fail(callback, QStringLiteral("pairing_failed"));
        return;
    }
    const QString digest = found->pairingDigest();
    const QByteArray signature =
        port_->signAsMachine(pairing::ApprovalPayload{found->ticketId(), digest}.bytes());
    if (signature.size() != 64) {
        fail(callback, QStringLiteral("signing_failed"));
        return;
    }
    found->beginApproval();
    changed(*found);
    client_.approve(
        found->ticketId(), digest, signature,
        [this, found, callback = std::move(callback)](const RunnerOutcome<QString>& outcome) {
            if (outcome.ok() && *outcome.value == QStringLiteral("active")) {
                finishActive(found, callback);
                return;
            }
            const QString code =
                outcome.ok() ? QStringLiteral("invalid_response") : outcome.failure.code;
            if (code == QStringLiteral("ticket_expired"))
                found->finish(RunnerPairing::State::Expired);
            else if (code == QStringLiteral("ticket_used"))
                found->finish(RunnerPairing::State::TicketUsed);
            else if (code == QStringLiteral("digest_mismatch") ||
                     code == QStringLiteral("ticket_not_found") || outcome.ok())
                found->fail(code);
            else
                found->endApproval(); // unanswered: the user may approve again
            found->noteError(code);
            changed(*found);
            fail(callback, outcome.ok() ? runnerFailure(code) : outcome.failure);
        });
}

void RunnerService::finishActive(const PairingPointer& found, Callback callback) {
    const RunnerClaim& claim = *found->claim();
    const pairing::DesktopRunnerPin pin{
        claim.runnerId,  claim.keyVersion,
        claim.publicKey, pairing::runnerFingerprint(claim.publicKey),
        claim.name,      clock_()};
    QString error;
    if (found->accountScope() != port_->accountScope()) {
        found->fail(QStringLiteral("account_changed"));
    } else if (!pins_.store(pin, &error)) {
        // The Relay pairs the runner but this machine does not trust it; it lists as untrusted.
        found->fail(QStringLiteral("pin_store_failed"));
    } else {
        found->activate();
    }
    changed(*found);
    if (!callback)
        return;
    if (found->state() != RunnerPairing::State::Active) {
        fail(callback, found->failureReason());
        return;
    }
    QJsonObject result = found->toJson();
    result.insert(QStringLiteral("ok"), true);
    callback(result, {});
}

void RunnerService::rejectPairing(const QJsonObject& payload, Callback callback) {
    const PairingPointer found = pairing(payload);
    if (!found) {
        fail(callback, QStringLiteral("ticket_not_found"));
        return;
    }
    if (stopIfStale(found) || found->terminal() ||
        found->state() == RunnerPairing::State::Approving) {
        fail(callback, QStringLiteral("pairing_failed"));
        return;
    }
    client_.reject(found->ticketId(), [this, found, callback = std::move(callback)](
                                          const RunnerOutcome<bool>& outcome) {
        if (!outcome.ok()) {
            fail(callback, outcome.failure);
            return;
        }
        found->finish(RunnerPairing::State::Rejected);
        changed(*found);
        QJsonObject result = found->toJson();
        result.insert(QStringLiteral("ok"), true);
        callback(result, {});
    });
}

bool RunnerService::stopIfStale(const PairingPointer& found) {
    if (found->terminal())
        return false;
    QString reason;
    if (found->accountScope() != port_->accountScope())
        reason = QStringLiteral("account_changed");
    else if (!port_->runnerRelayAvailable())
        reason = QStringLiteral("runner_unavailable");
    else if (clock_() >= found->expiresAt() && found->state() != RunnerPairing::State::Approving) {
        found->finish(RunnerPairing::State::Expired);
        changed(*found);
        return true;
    }
    if (reason.isEmpty())
        return false;
    found->fail(reason);
    changed(*found);
    return true;
}

void RunnerService::schedulePoll(const PairingPointer& found, const int delayMs) {
    PollState& state = polls_[found->ticketId()];
    state.delayMs = delayMs;
    const quint64 generation = ++state.generation;
    scheduler_(delayMs,
               [this, ticketId = found->ticketId(), generation] { poll(ticketId, generation); });
}

void RunnerService::poll(const QString& ticketId, const quint64 generation) {
    const PairingPointer found = pairings_.value(ticketId);
    if (!found || polls_.value(ticketId).generation != generation || found->terminal() ||
        stopIfStale(found))
        return;
    const int delay = polls_.value(ticketId).delayMs;
    const int next = qMin(delay * 2, longestPollMs);
    if (found->state() == RunnerPairing::State::Approving) {
        schedulePoll(found, delay);
        return;
    }
    client_.ticketStatus(ticketId, [this, found, generation,
                                    next](const RunnerOutcome<RunnerTicketStatus>& outcome) {
        if (pairings_.value(found->ticketId()) != found ||
            polls_.value(found->ticketId()).generation != generation || found->terminal())
            return;
        // The user approved while this read was out: the approval reads the claim itself.
        if (found->state() == RunnerPairing::State::Approving) {
            schedulePoll(found, next);
            return;
        }
        if (!outcome.ok()) {
            const QString& code = outcome.failure.code;
            found->noteError(code);
            if (code == QStringLiteral("network") || code == QStringLiteral("relay_error") ||
                code == QStringLiteral("invalid_response") ||
                code == QStringLiteral("rate_limited")) {
                schedulePoll(found, code == QStringLiteral("rate_limited") ? longestPollMs : next);
                return;
            }
            found->fail(code);
            changed(*found);
            return;
        }
        found->noteError({});
        switch (found->applyStatus(*outcome.value)) {
        case RunnerPairing::Effect::ApprovedActive:
            // An approval whose answer was lost: the Relay recorded it.
            finishActive(found, {});
            return;
        case RunnerPairing::Effect::Changed:
            changed(*found);
            break;
        case RunnerPairing::Effect::None:
            break;
        }
        if (!found->terminal())
            schedulePoll(found, next);
    });
}

} // namespace ketplus::daemon::runner
