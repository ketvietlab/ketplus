#include "relay/RelayClient.h"

#include "KetPlusProductVersion.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

namespace ketplus::runner::app {
namespace {

QString segment(const QString& id) { return QString::fromLatin1(QUrl::toPercentEncoding(id)); }

QString refusalCode(const QByteArray& body) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z_]{1,64}$"));
    const QString code =
        QJsonDocument::fromJson(body).object().value(QStringLiteral("code")).toString();
    return pattern.match(code).hasMatch() ? code : QStringLiteral("unknown");
}

bool acceptableRelay(const QUrl& url) {
    const bool loopback = url.host() == QStringLiteral("localhost") ||
                          url.host() == QStringLiteral("127.0.0.1") ||
                          url.host() == QStringLiteral("::1");
    return url.isValid() && !url.host().isEmpty() &&
           (url.scheme() == QStringLiteral("https") ||
            (loopback && url.scheme() == QStringLiteral("http")));
}

} // namespace

QString RelayResult::describe() const {
    switch (kind) {
    case Kind::Ok:
        return QStringLiteral("ok");
    case Kind::Refused:
        return QStringLiteral("refused (%1)").arg(code);
    case Kind::Network:
        return QStringLiteral("network error");
    case Kind::BadReply:
        return QStringLiteral("unexpected reply");
    }
    return {};
}

QUrl compiledRelayUrl() {
    const QUrl compiled(QStringLiteral(KETPLUS_DEFAULT_CONTROL_URL));
    QUrl relay = acceptableRelay(compiled) ? compiled : QUrl();
#if defined(KETPLUS_REMOTE_DEMO_AVAILABLE)
    const QUrl configured(qEnvironmentVariable("KETPLUS_CONTROL_URL"));
    if (acceptableRelay(configured))
        relay = configured;
#endif
    return relay;
}

RelayClient::RelayClient(QUrl baseUrl, const int timeoutMs)
    : m_baseUrl(std::move(baseUrl)), m_timeoutMs(timeoutMs) {}

RelayResult RelayClient::registerRunner(const QJsonObject& body) {
    return send("POST", QStringLiteral("/v2/runners:register"), &body, {}, 201);
}

RelayResult RelayClient::createChallenge(const QString& runnerId) {
    const QJsonObject body{{QStringLiteral("runnerId"), runnerId}};
    return send("POST", QStringLiteral("/v2/runners/challenges"), &body, {}, 201);
}

RelayResult RelayClient::issueCredential(const QJsonObject& body) {
    return send("POST", QStringLiteral("/v2/runners/credentials"), &body, {}, 201);
}

RelayResult RelayClient::claim(const QString& ticketId, const QJsonObject& body,
                               const QString& accessToken) {
    return send("POST",
                QStringLiteral("/v2/runner/pairing/tickets/%1/claim").arg(segment(ticketId)), &body,
                accessToken, 200);
}

RelayResult RelayClient::pairingState(const QString& ticketId, const QString& accessToken) {
    return send("GET", QStringLiteral("/v2/runner/pairing/tickets/%1").arg(segment(ticketId)),
                nullptr, accessToken, 200);
}

RelayResult RelayClient::heartbeat(const QJsonObject& body, const QString& accessToken) {
    return send("POST", QStringLiteral("/v2/runner/heartbeat"), &body, accessToken, 200);
}

RelayResult RelayClient::revokePair(const QString& machineId, const QString& accessToken) {
    return send("POST", QStringLiteral("/v2/runner/pairs/%1/revoke").arg(segment(machineId)),
                nullptr, accessToken, 204);
}

RelayResult RelayClient::deregister(const QString& accessToken) {
    return send("POST", QStringLiteral("/v2/runner:deregister"), nullptr, accessToken, 204);
}

RelayResult RelayClient::send(const QByteArray& method, const QString& path,
                              const QJsonObject* body, const QString& accessToken,
                              const int successStatus) {
    RelayResult result;
    if (!m_baseUrl.isValid())
        return result;
    QString base = m_baseUrl.toString(QUrl::FullyEncoded);
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    QNetworkRequest request(QUrl(base + path, QUrl::StrictMode));
    request.setTransferTimeout(m_timeoutMs);
    // A redirect would carry the bearer to another origin.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept", "application/json");
    if (!accessToken.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + accessToken.toUtf8());
    QByteArray payload;
    if (body != nullptr) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        payload = QJsonDocument(*body).toJson(QJsonDocument::Compact);
    }

    QNetworkReply* reply = body != nullptr || method != "GET"
                               ? m_network.sendCustomRequest(request, method, payload)
                               : m_network.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    const QVariant statusAttribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const QByteArray answer = reply->readAll();
    delete reply; // finished; no event loop may be left to run a deferred delete

    result.status = statusAttribute.isValid() ? statusAttribute.toInt() : 0;
    if (result.status == 0) {
        result.kind = RelayResult::Kind::Network;
        return result;
    }
    if (result.status == successStatus) {
        if (successStatus == 204) {
            result.kind = RelayResult::Kind::Ok;
            return result;
        }
        const QJsonDocument document = QJsonDocument::fromJson(answer);
        result.kind = document.isObject() ? RelayResult::Kind::Ok : RelayResult::Kind::BadReply;
        result.body = document.object();
        return result;
    }
    if (result.status >= 200 && result.status < 400) {
        result.kind = RelayResult::Kind::BadReply;
        return result;
    }
    result.kind = RelayResult::Kind::Refused;
    result.code = refusalCode(answer);
    return result;
}

} // namespace ketplus::runner::app
