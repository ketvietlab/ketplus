#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace ketplus::runners {

// One pairing as the daemon reports it. The token is not part of it: the page holds the
// token only in the widget that shows it.
struct PairingView {
    enum class Stage {
        Waiting,   // ticketed: the node has not claimed the token yet
        Confirm,   // claimed: compare the codes, then approve or reject
        Approving, // approving: the daemon is finishing
        Paired,    // active
        Rejected,
        Expired,
        Used,   // ticket_used: another node claimed it
        Failed, // failed, or a state this desktop does not know
    };

    QString ticketId;
    Stage stage = Stage::Failed;
    std::optional<QDateTime> expiresAt;
    QString runnerId;
    QString runnerName;
    QString runnerOs;
    QString runnerArch;
    QStringList runnerLabels;
    QString fingerprint;
    QString confirmationCode;
    std::optional<int> attemptsLeft;
    QString failureReason;
    QString lastError;

    [[nodiscard]] bool finished() const;
    // Approve needs a code to compare: never before the node claimed the token.
    [[nodiscard]] bool canApprove() const {
        return stage == Stage::Confirm && !confirmationCode.isEmpty();
    }
    [[nodiscard]] bool canReject() const { return !finished(); }
};

// Reads the pairing part of a runner.pairing.* answer. Returns nothing when the answer has
// no ticket id.
[[nodiscard]] std::optional<PairingView> parsePairing(const QJsonObject& result);

// The token from a runner.pairing.create answer, or empty.
[[nodiscard]] QString pairingToken(const QJsonObject& result);

// What the user runs on the node. The token is read from standard input, so it never lands
// in the shell history; the command itself holds no secret.
[[nodiscard]] QString nodeCommand(bool newRunner);

} // namespace ketplus::runners
