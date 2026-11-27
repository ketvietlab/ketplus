#include "relay/RelayLeaseCalls.h"

#include "relay/RelayClient.h"
#include "run/AccessTokens.h"

#include <QCoreApplication>
#include <QMetaObject>

namespace ketplus::runner::app {

using delivery::CallResult;
using delivery::LeaseCall;

void RelayLeaseCalls::send(const LeaseCall& call) {
    const CallResult result = this->call(call);
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [answer = m_answer, call, result] { answer(call, result); },
        Qt::QueuedConnection);
}

CallResult RelayLeaseCalls::call(const LeaseCall& call) {
    const AccessTokens::Call done = m_tokens.call([&](const QString& token) {
        switch (call.kind) {
        case LeaseCall::Kind::Acquire:
            return m_relay.acquireJob(call.acquireId, call.jobId, token);
        case LeaseCall::Kind::Renew:
            return m_relay.renewLease(call.leaseId, call.phase, token);
        case LeaseCall::Kind::Start:
            return m_relay.startJob(call.leaseId, token);
        case LeaseCall::Kind::Finish:
            return m_relay.finishJob(call.leaseId, call.outcome, call.reason, token);
        case LeaseCall::Kind::Release:
            return m_relay.releaseLease(call.leaseId, call.reason, token);
        }
        return RelayResult{};
    });
    CallResult result;
    switch (done.token.outcome) {
    case AccessTokens::Outcome::Ok:
        break;
    case AccessTokens::Outcome::Retry:
        return result; // Lost
    case AccessTokens::Outcome::RunnerRemoved:
        result.kind = CallResult::Kind::Refused;
        result.code = QStringLiteral("runner_removed");
        return result;
    case AccessTokens::Outcome::InvalidAccess:
    case AccessTokens::Outcome::StateFailure:
        result.kind = CallResult::Kind::CredentialLost;
        return result;
    }
    switch (done.relay.kind) {
    case RelayResult::Kind::Ok:
        result.kind = CallResult::Kind::Ok;
        result.body = done.relay.body;
        break;
    case RelayResult::Kind::Refused:
        result.kind = CallResult::Kind::Refused;
        result.code = done.relay.code;
        result.body = done.relay.body;
        break;
    case RelayResult::Kind::Network:
    case RelayResult::Kind::ServerError:
    case RelayResult::Kind::BadReply:
        break; // Lost
    }
    return result;
}

} // namespace ketplus::runner::app
