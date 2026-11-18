#pragma once

#include "runner/RunnerRelayPort.h"
#include "runner/RunnerRelaySchema.h"

#include <QString>

#include <functional>
#include <optional>

namespace ketplus::daemon::runner {

// A failure as a caller sees it: a stable snake_case code and a fixed message. Neither ever
// holds a token, a ticket secret, a credential or text the Relay sent.
struct RunnerFailure {
    QString code;
    QString message;
};

[[nodiscard]] RunnerFailure runnerFailure(const QString& code);

template <typename T> struct RunnerOutcome {
    std::optional<T> value;
    RunnerFailure failure;
    [[nodiscard]] bool ok() const { return value.has_value(); }
};

// The desktop operations of the runner contract. Every answer is checked against its schema;
// a refusal maps to its contract code when the operation names it, to relay_error otherwise.
class RunnerRelayClient final {
  public:
    template <typename T> using Callback = std::function<void(const RunnerOutcome<T>&)>;
    enum class StateChange { Disable, Enable, Remove };

    explicit RunnerRelayClient(RunnerRelayPort& port) : port_(port) {}

    // Unset runnerId: a ticket to register a new runner; set: to pair this desktop with it.
    void createTicket(const std::optional<QString>& runnerId, Callback<RunnerTicket> callback);
    void ticketStatus(const QString& ticketId, Callback<RunnerTicketStatus> callback);
    // Answers the pair state the Relay recorded.
    void approve(const QString& ticketId, const QString& pairingDigest,
                 const QByteArray& approvalSignature, Callback<QString> callback);
    void reject(const QString& ticketId, Callback<bool> callback);
    void listRunners(const std::optional<QString>& cursor, Callback<RunnerPage> callback);
    // Answers the runner's new state.
    void changeState(const QString& runnerId, StateChange change, Callback<QString> callback);
    void unpair(const QString& runnerId, Callback<bool> callback);

  private:
    RunnerRelayPort& port_;
};

} // namespace ketplus::daemon::runner
