#include "runner/RunnerPairing.h"

#include "pairing/PairingSignatures.h"

#include <QJsonArray>

namespace ketplus::daemon::runner {

namespace pairing = ketplus::runner::pairing;

RunnerPairing::RunnerPairing(QString ticketId, std::optional<QString> runnerId,
                             MachineIdentity desktop, QString accountScope,
                             const QDateTime& createdAt, const QDateTime& expiresAt)
    : ticketId_(std::move(ticketId)), runnerId_(std::move(runnerId)), desktop_(std::move(desktop)),
      desktopFingerprint_(
          pairing::desktopFingerprint(desktop_.signingPublicKey, desktop_.agreementPublicKey)),
      accountScope_(std::move(accountScope)), createdAt_(createdAt.toUTC()),
      // The contract gives a ticket one hour; a later expiry from the Relay is not believed.
      expiresAt_(qMin(expiresAt.toUTC(), createdAt_.addMSecs(ticketLifetimeMs))) {}

bool RunnerPairing::terminal() const {
    return state_ == State::Active || state_ == State::Rejected || state_ == State::Expired ||
           state_ == State::TicketUsed || state_ == State::Failed;
}

bool RunnerPairing::claimVerifies(const RunnerClaim& claim) const {
    if (runnerId_ && claim.runnerId != *runnerId_)
        return false;
    // A desktop may read a claim long after the runner signed it, so its time is held to the
    // ticket's life rather than to this clock.
    const auto issuedAt = parseTime(claim.issuedAt);
    if (!issuedAt || *issuedAt < createdAt_.addMSecs(-claimSlackMs) ||
        *issuedAt > expiresAt_.addMSecs(claimSlackMs))
        return false;
    const pairing::RegisterPayload payload{ticketId_,
                                           desktop_.workspaceId,
                                           desktop_.machineId,
                                           claim.keyVersion,
                                           pairing::runnerFingerprint(claim.publicKey),
                                           claim.issuedAt};
    return !pairing::verify(payload, claim.signature, {claim.publicKey, claim.keyVersion},
                            *issuedAt);
}

bool RunnerPairing::sameClaim(const RunnerClaim& claim) const {
    return claim_ && claim.runnerId == claim_->runnerId && claim.keyVersion == claim_->keyVersion &&
           pairing::constantTimeEquals(claim.publicKey, claim_->publicKey) &&
           pairing::constantTimeEquals(pairing::runnerFingerprint(claim.publicKey).toLatin1(),
                                       pairing::runnerFingerprint(claim_->publicKey).toLatin1());
}

RunnerPairing::Effect RunnerPairing::applyStatus(const RunnerTicketStatus& status) {
    if (terminal())
        return Effect::None;
    const QString& reported = status.pairState;
    if (reported == QStringLiteral("claimed")) {
        if (claim_) {
            if (sameClaim(*status.claim))
                return Effect::None;
            fail(QStringLiteral("claim_changed"));
            return Effect::Changed;
        }
        if (!claimVerifies(*status.claim)) {
            fail(QStringLiteral("invalid_claim"));
            return Effect::Changed;
        }
        claim_ = status.claim;
        code_ = pairing::pairingCode({ticketId_, desktop_.workspaceId, desktop_.machineId,
                                      desktop_.keyVersion, desktopFingerprint_, claim_->runnerId,
                                      claim_->keyVersion,
                                      pairing::runnerFingerprint(claim_->publicKey)});
        state_ = State::Claimed;
        return Effect::Changed;
    }
    if (reported == QStringLiteral("ticketed")) {
        if (!claim_)
            return Effect::None;
        fail(QStringLiteral("claim_changed"));
        return Effect::Changed;
    }
    if (reported == QStringLiteral("active")) {
        // Only an approval this desktop sent makes a pair; the pin is written by the caller.
        if (claim_ && approvalSent_)
            return Effect::ApprovedActive;
        fail(QStringLiteral("unexpected_state"));
        return Effect::Changed;
    }
    if (reported == QStringLiteral("expired"))
        finish(State::Expired);
    else if (reported == QStringLiteral("rejected"))
        finish(State::Rejected);
    else
        finish(State::TicketUsed);
    return Effect::Changed;
}

RunnerPairing::CodeCheck RunnerPairing::checkCode(const QString& entered) {
    if (!code_ || state_ != State::Claimed)
        return CodeCheck::Mismatch;
    const QByteArray normalized = entered.trimmed().toUpper().toLatin1();
    if (pairing::constantTimeEquals(normalized, code_->code.toLatin1()))
        return CodeCheck::Match;
    if (++wrongCodes_ < maximumCodeAttempts)
        return CodeCheck::Mismatch;
    fail(QStringLiteral("too_many_attempts"));
    return CodeCheck::Exhausted;
}

void RunnerPairing::beginApproval() {
    approvalSent_ = true;
    state_ = State::Approving;
}

void RunnerPairing::endApproval() {
    if (state_ == State::Approving)
        state_ = State::Claimed;
}

void RunnerPairing::activate() {
    state_ = State::Active;
    code_.reset();
}

void RunnerPairing::finish(const State state, const QString& reason) {
    state_ = state;
    failureReason_ = reason;
    code_.reset();
}

QString RunnerPairing::stateName(const State state) {
    switch (state) {
    case State::Ticketed:
        return QStringLiteral("ticketed");
    case State::Claimed:
        return QStringLiteral("claimed");
    case State::Approving:
        return QStringLiteral("approving");
    case State::Active:
        return QStringLiteral("active");
    case State::Rejected:
        return QStringLiteral("rejected");
    case State::Expired:
        return QStringLiteral("expired");
    case State::TicketUsed:
        return QStringLiteral("ticket_used");
    case State::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

QJsonObject RunnerPairing::toJson() const {
    QJsonObject result{{QStringLiteral("ticket_id"), ticketId_},
                       {QStringLiteral("state"), stateName(state_)},
                       {QStringLiteral("expires_at"), expiresAt_.toString(Qt::ISODateWithMs)}};
    if (runnerId_)
        result.insert(QStringLiteral("runner_id"), *runnerId_);
    if (!failureReason_.isEmpty())
        result.insert(QStringLiteral("failure_reason"), failureReason_);
    if (!lastError_.isEmpty() && !terminal())
        result.insert(QStringLiteral("last_error"), lastError_);
    if (claim_) {
        result.insert(
            QStringLiteral("runner"),
            QJsonObject{
                {QStringLiteral("runner_id"), claim_->runnerId},
                {QStringLiteral("name"), claim_->name},
                {QStringLiteral("os"), claim_->os},
                {QStringLiteral("arch"), claim_->arch},
                {QStringLiteral("labels"), QJsonArray::fromStringList(claim_->labels)},
                {QStringLiteral("key_version"), claim_->keyVersion},
                {QStringLiteral("fingerprint"), pairing::runnerFingerprint(claim_->publicKey)}});
    }
    if (code_ && state_ == State::Claimed) {
        result.insert(QStringLiteral("confirmation_code"), code_->code);
        result.insert(QStringLiteral("attempts_left"), maximumCodeAttempts - wrongCodes_);
    }
    return result;
}

} // namespace ketplus::daemon::runner
