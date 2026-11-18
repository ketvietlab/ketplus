#pragma once

#include "relay/RelayClient.h"
#include "state/RunnerState.h"

#include <QDateTime>
#include <QString>

#include <functional>

// The runner's access token: a fresh challenge signed with the runner key, exchanged with the
// refresh credential. The rotated refresh credential reaches the disk before the token is
// used; the token itself lives in memory only.
namespace ketplus::runner::app {

struct Environment;

// Renewed when less than this is left.
inline constexpr qint64 accessTokenMarginMs = 2 * 60 * 1000;

class AccessTokens {
  public:
    enum class Outcome {
        Ok,
        Retry,         // network failure or a refusal worth retrying after a delay
        InvalidAccess, // the refresh credential is refused for good
        RunnerRemoved,
        StateFailure, // the rotated credential could not be stored
    };
    struct Result {
        Outcome outcome = Outcome::Retry;
        RelayResult relay; // the failing call, when there is one
        QString error;     // StateFailure only
    };
    struct Call {
        Result token;      // not Ok: no call was made with a token
        RelayResult relay; // the call's own result when token.outcome is Ok
    };

    AccessTokens(RelayClient& relay, const RunnerState& state, RunnerIdentity& identity,
                 const Environment& environment)
        : m_relay(relay), m_state(state), m_identity(identity), m_environment(environment) {}

    // A token with at least two minutes left, renewing when needed.
    [[nodiscard]] Result ensure();
    [[nodiscard]] Result renew();
    // `request` with a token; refused invalid_access, the token is renewed once and the
    // request sent again.
    [[nodiscard]] Call call(const std::function<RelayResult(const QString& token)>& request);
    // `request` sent at most once, for clean-up calls that must not repeat.
    [[nodiscard]] Call callOnce(const std::function<RelayResult(const QString& token)>& request);

  private:
    RelayClient& m_relay;
    const RunnerState& m_state;
    RunnerIdentity& m_identity;
    const Environment& m_environment;
    QString m_token;
    QDateTime m_expiresAt;
};

} // namespace ketplus::runner::app
