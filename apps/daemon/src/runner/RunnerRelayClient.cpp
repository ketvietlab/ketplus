#include "runner/RunnerRelayClient.h"

#include "cloud/CryptoPrimitives.h"

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QUrl>

namespace ketplus::daemon::runner {
namespace {

using Codes = QSet<QString>;

QString message(const QString& code) {
    static const QHash<QString, QString> messages{
        {QStringLiteral("runner_unavailable"),
         QStringLiteral("The relay does not offer runners to this machine.")},
        {QStringLiteral("network"), QStringLiteral("The relay could not be reached.")},
        {QStringLiteral("not_enrolled"),
         QStringLiteral("This machine has no relay credential. Sign in again.")},
        {QStringLiteral("account_changed"),
         QStringLiteral("The account changed before the operation completed.")},
        {QStringLiteral("route_refused"),
         QStringLiteral("Only runner routes can be called with this credential.")},
        {QStringLiteral("invalid_response"),
         QStringLiteral("The relay answered with data that does not match the contract.")},
        {QStringLiteral("relay_error"), QStringLiteral("The relay refused the request.")},
        {QStringLiteral("entitlement_required"),
         QStringLiteral("This account's plan does not include runners.")},
        {QStringLiteral("unsupported"),
         QStringLiteral("The relay does not support this runner operation.")},
        {QStringLiteral("rate_limited"),
         QStringLiteral("The relay is limiting requests. Try again later.")},
        {QStringLiteral("invalid_access"),
         QStringLiteral("The relay did not accept this machine's credential.")},
        {QStringLiteral("invalid_request"),
         QStringLiteral("The relay did not accept the request.")},
        {QStringLiteral("runner_not_found"), QStringLiteral("The runner does not exist.")},
        {QStringLiteral("runner_removed"), QStringLiteral("The runner has been removed.")},
        {QStringLiteral("not_paired"), QStringLiteral("The runner is not paired.")},
        {QStringLiteral("ticket_not_found"), QStringLiteral("The pairing ticket does not exist.")},
        {QStringLiteral("ticket_expired"), QStringLiteral("The pairing ticket has expired.")},
        {QStringLiteral("ticket_used"),
         QStringLiteral("The pairing ticket was rejected or revoked.")},
        {QStringLiteral("digest_mismatch"),
         QStringLiteral("The relay holds other keys than the ones confirmed.")},
        {QStringLiteral("pin_store_failed"),
         QStringLiteral("The runner pin could not be written; the runner is not trusted.")},
        {QStringLiteral("code_mismatch"),
         QStringLiteral("The code does not match the one shown for this pairing.")},
        {QStringLiteral("not_claimed"), QStringLiteral("No runner has claimed this ticket yet.")},
        {QStringLiteral("signing_failed"),
         QStringLiteral("This machine's key could not sign the approval.")},
        {QStringLiteral("pairing_failed"),
         QStringLiteral("This pairing cannot go on. Create a new ticket.")},
        {QStringLiteral("claim_changed"),
         QStringLiteral("The runner's keys changed after the code was shown. Create a new "
                        "ticket.")},
        {QStringLiteral("too_many_pairings"),
         QStringLiteral("Too many pairings are open. Finish or reject one first.")},
        {QStringLiteral("job_not_found"), QStringLiteral("The relay has no such job.")},
        {QStringLiteral("idempotency_conflict"),
         QStringLiteral("A job with this key was submitted with other labels.")},
        {QStringLiteral("no_matching_runner"),
         QStringLiteral("No runner paired with this machine has every label.")},
        {QStringLiteral("queue_full"), QStringLiteral("The account's job queue is full.")},
        {QStringLiteral("invalid_labels"), QStringLiteral("The job's labels are not valid.")},
    };
    return messages.value(code, messages.value(QStringLiteral("relay_error")));
}

const Codes& commonCodes() {
    static const Codes codes{QStringLiteral("entitlement_required"),
                             QStringLiteral("invalid_access"), QStringLiteral("rate_limited"),
                             QStringLiteral("invalid_request"), QStringLiteral("unsupported")};
    return codes;
}

} // namespace

std::optional<RunnerFailure> refusal(const RelayReply& reply, const int expected,
                                     const QSet<QString>& operationCodes) {
    switch (reply.failure) {
    case RelayFailure::None:
        break;
    case RelayFailure::Network:
        return runnerFailure(QStringLiteral("network"));
    case RelayFailure::Credential:
        return runnerFailure(QStringLiteral("not_enrolled"));
    case RelayFailure::AccountChanged:
        return runnerFailure(QStringLiteral("account_changed"));
    case RelayFailure::RouteRefused:
        return runnerFailure(QStringLiteral("route_refused"));
    }
    if (reply.status == expected)
        return std::nullopt;
    if (reply.status >= 200 && reply.status < 300)
        return runnerFailure(QStringLiteral("invalid_response"));
    const QString code = reply.body.toObject().value(QStringLiteral("code")).toString();
    if (reply.status >= 400 && reply.status < 500 &&
        (commonCodes().contains(code) || operationCodes.contains(code)))
        return runnerFailure(code);
    return runnerFailure(QStringLiteral("relay_error"));
}

namespace {

template <typename T>
void answer(const std::function<void(const RunnerOutcome<T>&)>& callback, std::optional<T> value) {
    if (value)
        callback(RunnerOutcome<T>{std::move(value), {}});
    else
        callback(RunnerOutcome<T>{std::nullopt, runnerFailure(QStringLiteral("invalid_response"))});
}

template <typename T>
void fail(const std::function<void(const RunnerOutcome<T>&)>& callback, RunnerFailure failure) {
    callback(RunnerOutcome<T>{std::nullopt, std::move(failure)});
}

QString segment(const QString& id) { return QString::fromLatin1(QUrl::toPercentEncoding(id)); }

} // namespace

RunnerFailure runnerFailure(const QString& code) { return {code, message(code)}; }

void RunnerRelayClient::createTicket(const std::optional<QString>& runnerId,
                                     Callback<RunnerTicket> callback) {
    if (runnerId && !isRunnerId(*runnerId)) {
        fail(callback, runnerFailure(QStringLiteral("invalid_request")));
        return;
    }
    const QJsonObject body{
        {QStringLiteral("runnerId"), runnerId ? QJsonValue(*runnerId) : QJsonValue()}};
    port_.runnerRequest("POST", QStringLiteral("/v2/runner-pairing/tickets"), body,
                        [callback = std::move(callback)](const RelayReply& reply) {
                            if (auto failure = refusal(reply, 201,
                                                       {QStringLiteral("runner_not_found"),
                                                        QStringLiteral("runner_removed")}))
                                fail(callback, std::move(*failure));
                            else
                                answer(callback, parseTicket(reply.body));
                        });
}

void RunnerRelayClient::ticketStatus(const QString& ticketId,
                                     Callback<RunnerTicketStatus> callback) {
    if (!isRunnerId(ticketId)) {
        fail(callback, runnerFailure(QStringLiteral("ticket_not_found")));
        return;
    }
    port_.runnerRequest(
        "GET", QStringLiteral("/v2/runner-pairing/tickets/%1").arg(segment(ticketId)), {},
        [ticketId, callback = std::move(callback)](const RelayReply& reply) {
            if (auto failure = refusal(reply, 200, {QStringLiteral("ticket_not_found")})) {
                fail(callback, std::move(*failure));
                return;
            }
            auto status = parseTicketStatus(reply.body);
            if (status && status->ticketId != ticketId)
                status.reset();
            answer(callback, std::move(status));
        });
}

void RunnerRelayClient::approve(const QString& ticketId, const QString& pairingDigest,
                                const QByteArray& approvalSignature, Callback<QString> callback) {
    if (!isRunnerId(ticketId) || pairingDigest.size() != 43 || approvalSignature.size() != 64) {
        fail(callback, runnerFailure(QStringLiteral("invalid_request")));
        return;
    }
    const QJsonObject body{{QStringLiteral("pairingDigest"), pairingDigest},
                           {QStringLiteral("approvalSignature"),
                            QString::fromLatin1(control_crypto::base64Url(approvalSignature))}};
    port_.runnerRequest(
        "POST", QStringLiteral("/v2/runner-pairing/tickets/%1/approve").arg(segment(ticketId)),
        body, [callback = std::move(callback)](const RelayReply& reply) {
            if (auto failure =
                    refusal(reply, 200,
                            {QStringLiteral("ticket_not_found"), QStringLiteral("ticket_expired"),
                             QStringLiteral("ticket_used"), QStringLiteral("digest_mismatch")}))
                fail(callback, std::move(*failure));
            else
                answer(callback, parsePairState(reply.body));
        });
}

void RunnerRelayClient::reject(const QString& ticketId, Callback<bool> callback) {
    if (!isRunnerId(ticketId)) {
        fail(callback, runnerFailure(QStringLiteral("ticket_not_found")));
        return;
    }
    port_.runnerRequest(
        "POST", QStringLiteral("/v2/runner-pairing/tickets/%1/reject").arg(segment(ticketId)), {},
        [callback = std::move(callback)](const RelayReply& reply) {
            if (auto failure = refusal(reply, 204, {QStringLiteral("ticket_not_found")}))
                fail(callback, std::move(*failure));
            else
                callback(RunnerOutcome<bool>{true, {}});
        });
}

void RunnerRelayClient::listRunners(const std::optional<QString>& cursor,
                                    Callback<RunnerPage> callback) {
    QString path = QStringLiteral("/v2/runners?limit=100");
    if (cursor) {
        if (cursor->isEmpty() || cursor->size() > 256) {
            fail(callback, runnerFailure(QStringLiteral("invalid_response")));
            return;
        }
        path += QStringLiteral("&cursor=") + segment(*cursor);
    }
    port_.runnerRequest("GET", path, {}, [callback = std::move(callback)](const RelayReply& reply) {
        if (auto failure = refusal(reply, 200, {}))
            fail(callback, std::move(*failure));
        else
            answer(callback, parseRunnerPage(reply.body));
    });
}

void RunnerRelayClient::changeState(const QString& runnerId, const StateChange change,
                                    Callback<QString> callback) {
    if (!isRunnerId(runnerId)) {
        fail(callback, runnerFailure(QStringLiteral("runner_not_found")));
        return;
    }
    const QString action = change == StateChange::Disable  ? QStringLiteral("disable")
                           : change == StateChange::Enable ? QStringLiteral("enable")
                                                           : QStringLiteral("remove");
    port_.runnerRequest("POST", QStringLiteral("/v2/runners/%1/%2").arg(segment(runnerId), action),
                        {}, [runnerId, callback = std::move(callback)](const RelayReply& reply) {
                            if (auto failure =
                                    refusal(reply, 200, {QStringLiteral("runner_not_found")}))
                                fail(callback, std::move(*failure));
                            else
                                answer(callback, parseStateAnswer(reply.body, runnerId));
                        });
}

void RunnerRelayClient::unpair(const QString& runnerId, Callback<bool> callback) {
    if (!isRunnerId(runnerId)) {
        fail(callback, runnerFailure(QStringLiteral("runner_not_found")));
        return;
    }
    port_.runnerRequest("POST", QStringLiteral("/v2/runners/%1/unpair").arg(segment(runnerId)), {},
                        [callback = std::move(callback)](const RelayReply& reply) {
                            if (auto failure = refusal(reply, 204, {QStringLiteral("not_paired")}))
                                fail(callback, std::move(*failure));
                            else
                                callback(RunnerOutcome<bool>{true, {}});
                        });
}

} // namespace ketplus::daemon::runner
