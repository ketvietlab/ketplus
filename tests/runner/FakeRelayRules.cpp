#include "FakeRelay.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

// The contract's rules for the requests and socket messages the Relay receives: the route and
// its credential, then every field of the body (required, type, bounds, enum, pattern) and no
// field the contract does not name. A departure is a violation; the request is still answered.
namespace runner_app_test {

namespace {

using Check = std::function<bool(const QJsonValue&)>;

struct Field {
    QString name;
    bool required = true;
    Check check;
};
using Shape = QList<Field>;

Check text(const int min, const int max, const QString& pattern = {}) {
    return [=](const QJsonValue& value) {
        if (!value.isString())
            return false;
        const QString string = value.toString();
        if (string.size() < min || string.size() > max)
            return false;
        return pattern.isEmpty() ||
               QRegularExpression(QStringLiteral("^(?:%1)$").arg(pattern)).match(string).hasMatch();
    };
}

const QString idPattern = QStringLiteral("[A-Za-z0-9_-]+");
const QString base64Pattern = QStringLiteral("[A-Za-z0-9_-]+");

Check id() { return text(1, 128, idPattern); }
Check base64(const int length) { return text(length, length, base64Pattern); }
Check reason() { return text(1, 64, QStringLiteral("[a-z][a-z0-9_]*")); }

Check integer(const qint64 min, const qint64 max) {
    return [=](const QJsonValue& value) {
        if (!value.isDouble())
            return false;
        const double number = value.toDouble();
        return number == double(qint64(number)) && number >= double(min) && number <= double(max);
    };
}

Check oneOf(const QStringList& values) {
    return [=](const QJsonValue& value) {
        return value.isString() && values.contains(value.toString());
    };
}

Check nullable(const Check& check) {
    return [=](const QJsonValue& value) { return value.isNull() || check(value); };
}

// RFC 3339 in UTC.
Check timestamp() {
    return [](const QJsonValue& value) {
        if (!value.isString() || !value.toString().endsWith(QLatin1Char('Z')))
            return false;
        return QDateTime::fromString(value.toString(), Qt::ISODateWithMs).isValid();
    };
}

Check array(const int min, const int max, const Check& item) {
    return [=](const QJsonValue& value) {
        if (!value.isArray())
            return false;
        const QJsonArray array = value.toArray();
        if (array.size() < min || array.size() > max)
            return false;
        QSet<QString> seen;
        for (const QJsonValue& entry : array) {
            if (!item(entry))
                return false;
            seen.insert(QString::fromUtf8(QJsonDocument(QJsonArray{entry}).toJson()));
        }
        return seen.size() == array.size();
    };
}

Check userLabels() { return array(0, 32, text(1, 64, QStringLiteral("[a-z0-9][a-z0-9._-]*"))); }
Check runsOn() {
    return array(1, 16,
                 text(1, 64, QStringLiteral("os:linux|arch:(x86_64|arm64)|[a-z0-9][a-z0-9._-]*")));
}
Check capabilities() {
    return array(
        0, 32, text(1, 64, QStringLiteral("[a-z][a-z0-9_]*(\\.[a-z][a-z0-9_]*)*/[1-9][0-9]{0,3}")));
}

Check object(const Shape& shape);

// A sealed history event of the daemon: only where it belongs is read, the rest is opaque.
Check sealedEvent() {
    return [](const QJsonValue& value) {
        const QJsonObject event = value.toObject();
        return value.isObject() && id()(event.value(QStringLiteral("sessionId"))) &&
               integer(1, 9007199254740991)(event.value(QStringLiteral("sessionGeneration"))) &&
               integer(1, 9007199254740991)(event.value(QStringLiteral("cursor")));
    };
}

QStringList departures(const QJsonObject& body, const Shape& shape) {
    QStringList found;
    QSet<QString> known;
    for (const Field& field : shape) {
        known.insert(field.name);
        if (!body.contains(field.name)) {
            if (field.required)
                found.append(QStringLiteral("missing %1").arg(field.name));
        } else if (!field.check(body.value(field.name))) {
            found.append(QStringLiteral("invalid %1").arg(field.name));
        }
    }
    for (auto it = body.constBegin(); it != body.constEnd(); ++it)
        if (!known.contains(it.key()))
            found.append(QStringLiteral("unknown field %1").arg(it.key()));
    return found;
}

Check object(const Shape& shape) {
    return [=](const QJsonValue& value) {
        return value.isObject() && departures(value.toObject(), shape).isEmpty();
    };
}

enum class Credential { None, Runner, Machine };

struct Route {
    QByteArray method;
    QRegularExpression path;
    Credential credential;
    std::optional<Shape> body; // nothing: the request has no body
};

const QList<Route>& routes() {
    static const QList<Route> list = [] {
        const auto route = [](const char* method, const QString& path, const Credential credential,
                              std::optional<Shape> body) {
            return Route{method,
                         QRegularExpression(QStringLiteral("^") + path + QStringLiteral("$")),
                         credential, std::move(body)};
        };
        const QString segment = QStringLiteral("[A-Za-z0-9_-]{1,128}");
        const Shape registration{{QStringLiteral("ticketId"), true, id()},
                                 {QStringLiteral("ticketSecret"), true, text(32, 256, idPattern)},
                                 {QStringLiteral("publicKey"), true, base64(43)},
                                 {QStringLiteral("keyVersion"), true, integer(1, 1)},
                                 {QStringLiteral("name"), true, text(1, 64)},
                                 {QStringLiteral("os"), true, oneOf({QStringLiteral("linux")})},
                                 {QStringLiteral("arch"), true,
                                  oneOf({QStringLiteral("x86_64"), QStringLiteral("arm64")})},
                                 {QStringLiteral("labels"), true, userLabels()},
                                 {QStringLiteral("capabilities"), true, capabilities()},
                                 {QStringLiteral("runnerVersion"), true, text(1, 64)},
                                 {QStringLiteral("issuedAt"), true, timestamp()},
                                 {QStringLiteral("signature"), true, base64(86)}};
        const Shape currentJob{{QStringLiteral("jobId"), true, id()},
                               {QStringLiteral("leaseId"), true, id()},
                               {QStringLiteral("phase"), true,
                                oneOf({QStringLiteral("assigned"), QStringLiteral("syncing"),
                                       QStringLiteral("running")})}};
        const QString lease = QStringLiteral("/v2/runner/leases/") + segment;
        const QString ticket = QStringLiteral("/v2/runner-pairing/tickets/") + segment;
        const QString job = QStringLiteral("/v2/runner-jobs/") + segment;
        return QList<Route>{
            route("POST", QStringLiteral("/v2/runners:register"), Credential::None, registration),
            route("POST", QStringLiteral("/v2/runners/challenges"), Credential::None,
                  Shape{{QStringLiteral("runnerId"), true, id()}}),
            route("POST", QStringLiteral("/v2/runners/credentials"), Credential::None,
                  Shape{{QStringLiteral("runnerId"), true, id()},
                        {QStringLiteral("keyVersion"), true, integer(1, 2147483647)},
                        {QStringLiteral("nonce"), true, text(22, 128)},
                        {QStringLiteral("issuedAt"), true, timestamp()},
                        {QStringLiteral("signature"), true, base64(86)},
                        {QStringLiteral("refreshCredential"), true, text(32, 512)}}),
            route("POST", QStringLiteral("/v2/runner/pairing/tickets/") + segment + "/claim",
                  Credential::Runner,
                  Shape{{QStringLiteral("ticketSecret"), true, text(32, 256, idPattern)},
                        {QStringLiteral("issuedAt"), true, timestamp()},
                        {QStringLiteral("signature"), true, base64(86)}}),
            route("GET", QStringLiteral("/v2/runner/pairing/tickets/") + segment,
                  Credential::Runner, std::nullopt),
            route("POST", QStringLiteral("/v2/runner/heartbeat"), Credential::Runner,
                  Shape{{QStringLiteral("state"), true,
                         oneOf({QStringLiteral("idle"), QStringLiteral("busy"),
                                QStringLiteral("draining")})},
                        {QStringLiteral("labels"), true, userLabels()},
                        {QStringLiteral("capabilities"), true, capabilities()},
                        {QStringLiteral("runnerVersion"), true, text(1, 64)},
                        {QStringLiteral("currentJob"), true, nullable(object(currentJob))}}),
            route("POST", QStringLiteral("/v2/runner/jobs:acquire"), Credential::Runner,
                  Shape{{QStringLiteral("acquireId"), true, id()},
                        {QStringLiteral("jobId"), false, id()}}),
            route("POST", lease + "/renew", Credential::Runner,
                  Shape{{QStringLiteral("phase"), true,
                         oneOf({QStringLiteral("assigned"), QStringLiteral("syncing"),
                                QStringLiteral("running")})}}),
            route("POST", lease + "/start", Credential::Runner, std::nullopt),
            route("POST", lease + "/finish", Credential::Runner,
                  Shape{{QStringLiteral("outcome"), true,
                         oneOf({QStringLiteral("succeeded"), QStringLiteral("failed"),
                                QStringLiteral("cancelled"), QStringLiteral("interrupted"),
                                QStringLiteral("rejected")})},
                        {QStringLiteral("reason"), true, reason()}}),
            route("POST", lease + "/release", Credential::Runner,
                  Shape{{QStringLiteral("reason"), true,
                         oneOf({QStringLiteral("peer_unreachable"),
                                QStringLiteral("untrusted_requester"),
                                QStringLiteral("label_mismatch"), QStringLiteral("draining"),
                                QStringLiteral("already_dispatched"),
                                QStringLiteral("snapshot_unavailable")})}}),
            route("POST", QStringLiteral("/v2/runner/pairs/") + segment + "/revoke",
                  Credential::Runner, std::nullopt),
            route("POST", QStringLiteral("/v2/runner:deregister"), Credential::Runner,
                  std::nullopt),
            // The daemon's command poll: outside the runner contract, answered with none.
            route("GET", QStringLiteral("/v2/commands:poll\\?limit=([1-9][0-9]?|100)"),
                  Credential::Machine, std::nullopt),
            // The daemon's history upload: outside the runner contract, every event accepted.
            route("POST", QStringLiteral("/v2/events:append"), Credential::Machine,
                  Shape{{QStringLiteral("machineId"), true, id()},
                        {QStringLiteral("events"), true, array(1, 1000, sealedEvent())}}),
            route("POST", QStringLiteral("/v2/runner-pairing/tickets"), Credential::Machine,
                  Shape{{QStringLiteral("runnerId"), true, nullable(id())}}),
            route("GET", ticket, Credential::Machine, std::nullopt),
            route("POST", ticket + "/approve", Credential::Machine,
                  Shape{{QStringLiteral("pairingDigest"), true, base64(43)},
                        {QStringLiteral("approvalSignature"), true, base64(86)}}),
            route("POST", ticket + "/reject", Credential::Machine, std::nullopt),
            route("POST", QStringLiteral("/v2/runner-jobs"), Credential::Machine,
                  Shape{{QStringLiteral("submitKey"), true, id()},
                        {QStringLiteral("runsOn"), true, runsOn()}}),
            route("GET", job, Credential::Machine, std::nullopt),
            route("POST", job + "/cancel", Credential::Machine,
                  Shape{{QStringLiteral("reason"), true, reason()}}),
            route("GET",
                  QStringLiteral(
                      "/v2/runners(\\?(cursor=[A-Za-z0-9_-]{1,512}&)?limit=(100|[1-9][0-9]?))?"),
                  Credential::Machine, std::nullopt),
            route("POST",
                  QStringLiteral("/v2/runners/") + segment + "/(unpair|disable|enable|remove)",
                  Credential::Machine, std::nullopt),
        };
    }();
    return list;
}

const QHash<QString, Shape>& messages() {
    static const QHash<QString, Shape> shapes = [] {
        const Field type{QStringLiteral("type"), true, text(1, 64)};
        const Field connection{QStringLiteral("connectionId"), true, text(1, 128)};
        const Shape end{type, connection, {QStringLiteral("reason"), false, reason()}};
        // A candidate line, or null once gathering ends, and its media stream.
        const Check line = text(1, 1024);
        const Shape candidate{
            type,
            connection,
            {QStringLiteral("candidate"), true,
             [line](const QJsonValue& value) { return value.isNull() || line(value); }},
            {QStringLiteral("sdpMid"), true, text(0, 64)}};
        return QHash<QString, Shape>{
            {QStringLiteral("runner signal.start"), Shape{type,
                                                          {QStringLiteral("jobId"), true, id()},
                                                          {QStringLiteral("leaseId"), true, id()}}},
            {QStringLiteral("runner signal.offer"),
             Shape{type,
                   connection,
                   {QStringLiteral("sdp"), true, text(1, 16 * 1024)},
                   {QStringLiteral("jobId"), true, id()},
                   {QStringLiteral("attempt"), true, integer(1, 3)},
                   {QStringLiteral("runnerKeyVersion"), true, integer(1, 2147483647)},
                   {QStringLiteral("issuedAt"), true, timestamp()},
                   {QStringLiteral("signature"), true, base64(86)}}},
            {QStringLiteral("runner signal.candidate"), candidate},
            {QStringLiteral("runner signal.end"), end},
            {QStringLiteral("desktop signal.answer"),
             Shape{type,
                   connection,
                   {QStringLiteral("sdp"), true, text(1, 16 * 1024)},
                   {QStringLiteral("machineKeyVersion"), true, integer(1, 2147483647)},
                   {QStringLiteral("issuedAt"), true, timestamp()},
                   {QStringLiteral("signature"), true, base64(86)}}},
            {QStringLiteral("desktop signal.candidate"), candidate},
            {QStringLiteral("desktop signal.end"), end},
        };
    }();
    return shapes;
}

} // namespace

void FakeRelay::violate(const QString& what) { violations.append(what); }

void FakeRelay::checkRequest(const Request& request) {
    const QString where = QString::fromLatin1(request.method) + QLatin1Char(' ') + request.path;
    const Route* found = nullptr;
    for (const Route& route : routes())
        if (route.method == request.method && route.path.match(request.path).hasMatch())
            found = &route;
    if (!found) {
        violate(QStringLiteral("no such route: %1").arg(where));
        return;
    }
    const bool bearer = request.authorization.startsWith("Bearer ");
    if (found->credential != Credential::None && !bearer)
        violate(QStringLiteral("%1: no bearer credential").arg(where));
    if (found->credential == Credential::Machine && bearer &&
        request.authorization.mid(7) != machineToken.toLatin1())
        violate(QStringLiteral("%1: not the machine credential").arg(where));
    if (!found->body) {
        if (!request.raw.isEmpty())
            violate(QStringLiteral("%1: a body on a route that takes none").arg(where));
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(request.raw);
    if (!document.isObject()) {
        violate(QStringLiteral("%1: the body is not a JSON object").arg(where));
        return;
    }
    for (const QString& departure : departures(document.object(), *found->body))
        violate(QStringLiteral("%1: %2").arg(where, departure));
}

void FakeRelay::checkMessage(const QString& from, const QJsonObject& message) {
    const QString key = from + QLatin1Char(' ') + message.value(QStringLiteral("type")).toString();
    const auto shape = messages().constFind(key);
    if (shape == messages().cend()) {
        violate(QStringLiteral("socket message not in the contract: %1").arg(key));
        return;
    }
    for (const QString& departure : departures(message, *shape))
        violate(QStringLiteral("socket %1: %2").arg(key, departure));
}

} // namespace runner_app_test
