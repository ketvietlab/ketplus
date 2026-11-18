#include "runner/CloudRunnerRelay.h"

#include "cloud/CloudControlClient.h"
#include "pairing/PairingIdentity.h"
#include "runner/RunnerService.h"

#include <QRegularExpression>
#include <QUrlQuery>

namespace ketplus::daemon {

runner::RunnerService& CloudControlClient::runners() {
    if (!runners_)
        runners_ = std::make_unique<runner::RunnerService>(
            std::make_unique<runner::CloudRunnerRelay>(*this));
    return *runners_;
}

namespace runner {
namespace {

bool runnersQuery(const QString& query) {
    if (query.isEmpty())
        return true;
    const QUrlQuery parsed(query);
    QStringList seen;
    for (const auto& [key, value] : parsed.queryItems(QUrl::FullyEncoded)) {
        if ((key != QStringLiteral("limit") && key != QStringLiteral("cursor")) ||
            seen.contains(key) || value.isEmpty())
            return false;
        seen.append(key);
    }
    return true;
}

} // namespace

bool isRunnerRoute(const QByteArray& method, const QString& path) {
    static const QRegularExpression ticket(
        QStringLiteral("^/v2/runner-pairing/tickets/[A-Za-z0-9_-]{1,128}(/approve|/reject)?$"));
    static const QRegularExpression runner(
        QStringLiteral("^/v2/runners/[A-Za-z0-9_-]{1,128}/(unpair|disable|enable|remove)$"));
    static const QRegularExpression job(
        QStringLiteral("^/v2/runner-jobs/[A-Za-z0-9_-]{1,128}(/cancel)?$"));
    const qsizetype mark = path.indexOf(QLatin1Char('?'));
    const QString route = mark < 0 ? path : path.left(mark);
    const QString query = mark < 0 ? QString() : path.mid(mark + 1);
    if (method == "GET" && route == QStringLiteral("/v2/runners"))
        return runnersQuery(query);
    if (mark >= 0)
        return false;
    if (method == "POST" && (route == QStringLiteral("/v2/runner-pairing/tickets") ||
                             route == QStringLiteral("/v2/runner-jobs")))
        return true;
    if (const auto match = job.match(route); match.hasMatch())
        return method == (match.captured(1).isEmpty() ? "GET" : "POST");
    if (const auto match = ticket.match(route); match.hasMatch())
        return method == (match.captured(1).isEmpty() ? "GET" : "POST");
    return method == "POST" && runner.match(route).hasMatch();
}

bool CloudRunnerRelay::runnerRelayAvailable() const { return client_.runnerRelayAvailable_; }

std::optional<MachineIdentity> CloudRunnerRelay::machineIdentity() {
    if (client_.machineId_.isEmpty() || client_.workspaceId_.isEmpty())
        return std::nullopt;
    const QJsonObject keys = client_.crypto_.publicKeys();
    const auto signing = ketplus::runner::pairing::decodeBase64Url(
        keys.value(QStringLiteral("signingPublicKey")).toString());
    const auto agreement = ketplus::runner::pairing::decodeBase64Url(
        keys.value(QStringLiteral("agreementPublicKey")).toString());
    if (!signing || signing->size() != 32 || !agreement || agreement->size() != 32)
        return std::nullopt;
    return MachineIdentity{client_.machineId_, client_.workspaceId_, client_.machineKeyVersion_,
                           *signing, *agreement};
}

QString CloudRunnerRelay::accountScope() const {
    const auto fence = client_.accountFence();
    return QStringLiteral("%1/%2/%3/%4")
        .arg(fence.epoch)
        .arg(fence.enrollmentRevision)
        .arg(fence.machine, fence.workspace);
}

QByteArray CloudRunnerRelay::signAsMachine(const QByteArray& bytes) {
    return client_.crypto_.sign(bytes);
}

CredentialStore& CloudRunnerRelay::credentials() { return client_.credentials_; }

void CloudRunnerRelay::runnerRequest(const QByteArray& method, const QString& path,
                                     const QJsonValue& body, ReplyCallback callback) {
    if (!isRunnerRoute(method, path)) {
        callback({0, {}, RelayFailure::RouteRefused});
        return;
    }
    CloudControlClient& client = client_;
    const auto fence = client.accountFence();
    client.ensureDaemonCredential(
        [&client, fence, method, path, body, callback = std::move(callback)](const bool ready,
                                                                             const QString&) {
            if (!client.accountCurrent(fence)) {
                callback({0, {}, RelayFailure::AccountChanged});
                return;
            }
            if (!ready) {
                callback({0, {}, RelayFailure::Credential});
                return;
            }
            client.request(method, path, body, client.daemonToken_, {},
                           [&client, fence, callback](const int status, const QJsonValue& value,
                                                      const QString& transportError) {
                               if (!client.accountCurrent(fence))
                                   callback({status, {}, RelayFailure::AccountChanged});
                               else if (!transportError.isEmpty())
                                   callback({status, {}, RelayFailure::Network});
                               else
                                   callback({status, value, RelayFailure::None});
                           });
        });
}

} // namespace runner
} // namespace ketplus::daemon
