#pragma once

#include "ExitCode.h"
#include "flow/PendingPins.h"
#include "pairing/PairingIdentity.h"
#include "pairing/PairingToken.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>

#include <optional>

// The steps `register` and `pair` share once the Relay has accepted the ticket.
namespace ketplus::runner::app {

struct RunnerKey {
    QByteArray privateKey;
    QByteArray publicKey;
    int keyVersion = 1;
};

// ticketSecret, issuedAt and the runner's signature over the register payload of `token`.
[[nodiscard]] QJsonObject signedTicketFields(const pairing::PairingToken& token,
                                             const RunnerKey& key, const QDateTime& now);

// The `desktop` object of a register or claim reply; nothing when it breaks the contract.
[[nodiscard]] std::optional<pairing::ReportedDesktop> reportedDesktop(const QJsonObject& reply);

// Stores the desktop as a pending pin, prints the confirmation code and waits for the
// desktop's approval.
[[nodiscard]] ExitCode pinAndWait(const pairing::PairingToken& token,
                                  const pairing::ReportedDesktop& desktop, const QString& runnerId,
                                  const RunnerKey& key, PairingContext& context);

// After a lost answer the ticket may be spent without the runner knowing the desktop.
void reportLostAnswer(const Environment& environment);

// Prints why the Relay refused a ticket and returns the matching exit code.
[[nodiscard]] ExitCode reportTicketRefusal(const RelayResult& result,
                                           const Environment& environment);

} // namespace ketplus::runner::app
