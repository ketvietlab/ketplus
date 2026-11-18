#pragma once

#include "pairing/PairingIdentity.h"
#include "runner/RunnerRelayPort.h"
#include "runner/RunnerRelaySchema.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include <optional>

// One pairing this desktop started: what it knows of the ticket and of the runner that claimed
// it. It never holds the ticket secret or the token.
namespace ketplus::daemon::runner {

class RunnerPairing final {
  public:
    enum class State {
        Ticketed,
        Claimed, // a verified claim; its code is shown and the user may approve it
        Approving,
        Active,
        Rejected,
        Expired,
        TicketUsed,
        Failed, // for good: the user creates a new ticket
    };
    enum class Effect { None, Changed, ApprovedActive };
    enum class CodeCheck { Match, Mismatch, Exhausted };

    static constexpr qint64 claimSlackMs = 5 * 60'000;
    static constexpr qint64 ticketLifetimeMs = 60 * 60'000;
    static constexpr int maximumCodeAttempts = 5;

    RunnerPairing(QString ticketId, std::optional<QString> runnerId, MachineIdentity desktop,
                  QString accountScope, const QDateTime& createdAt, const QDateTime& expiresAt);

    [[nodiscard]] const QString& ticketId() const { return ticketId_; }
    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] bool terminal() const;
    [[nodiscard]] const QString& accountScope() const { return accountScope_; }
    [[nodiscard]] const MachineIdentity& desktop() const { return desktop_; }
    [[nodiscard]] const QDateTime& expiresAt() const { return expiresAt_; }
    [[nodiscard]] const std::optional<RunnerClaim>& claim() const { return claim_; }
    [[nodiscard]] QString failureReason() const { return failureReason_; }
    [[nodiscard]] bool approvalSent() const { return approvalSent_; }

    // A status the Relay answered. A first claim is shown only when its signature verifies;
    // a claim that differs from the one shown fails the pairing for good.
    Effect applyStatus(const RunnerTicketStatus& status);
    CodeCheck checkCode(const QString& entered);
    // The digest the approval signs; only for a shown claim.
    [[nodiscard]] QString pairingDigest() const { return code_ ? code_->digest : QString(); }

    // Approving while the claim is read again; nothing is sent yet.
    void holdForApproval() { state_ = State::Approving; }
    // The approval is about to be sent: an `active` the Relay answers later is this one.
    void beginApproval();
    void endApproval(); // back to Claimed after an approval the Relay did not answer
    void activate();
    void finish(State state, const QString& reason = {});
    void fail(const QString& reason) { finish(State::Failed, reason); }
    void noteError(const QString& code) { lastError_ = code; }

    [[nodiscard]] static QString stateName(State state);
    // For the caller that asks; `confirmation_code` only while a claim waits for approval.
    [[nodiscard]] QJsonObject toJson() const;

  private:
    [[nodiscard]] bool claimVerifies(const RunnerClaim& claim) const;
    [[nodiscard]] bool sameClaim(const RunnerClaim& claim) const;

    QString ticketId_;
    std::optional<QString> runnerId_;
    MachineIdentity desktop_;
    QString desktopFingerprint_;
    QString accountScope_;
    QDateTime createdAt_;
    QDateTime expiresAt_;
    State state_ = State::Ticketed;
    std::optional<RunnerClaim> claim_;
    std::optional<ketplus::runner::pairing::PairingCode> code_;
    int wrongCodes_ = 0;
    bool approvalSent_ = false;
    QString failureReason_;
    QString lastError_;
};

} // namespace ketplus::daemon::runner
