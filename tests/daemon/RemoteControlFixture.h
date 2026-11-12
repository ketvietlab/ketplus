#pragma once
// Shared by the Control v2 client tests: a synthetic relay and a host with its own keys.
#include "cloud-privacy/FakeTransport.h"
#include "cloud-privacy/MemoryCredentials.h"
#include "cloud/HostPlatform.h"
#include "control/ControlV2Client.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QPointer>
#include <QtTest>
#include <functional>
#include <optional>
using namespace ketplus::daemon;
using namespace ketplus::remote;
namespace ketplus::remote {
class RemoteControlTestAccess {
  public:
    static void transport(ControlV2Client& client, QNetworkAccessManager& transport) {
        client.transport_ = &transport;
    }
    // As acceptPairing records it in the credential.
    static void paired(ControlV2Client& client) {
        client.deviceId_ = "remote-client";
        client.keyVersion_ = 1;
        client.credential_.insert("deviceId", "remote-client");
        client.credential_.insert("deviceKeyVersion", 1);
    }
    static void pinHost(ControlV2Client& client, const QString& machine, int version,
                        const QJsonObject& keys) {
        auto pins = client.hostPins_;
        pins.insert(QStringLiteral("%1/%2").arg(machine).arg(version),
                    CloudCrypto::keyFingerprint(keys));
        if (!client.storeHostPins(pins, nullptr))
            qFatal("Synthetic host pin failed");
    }
    static QJsonObject credential(const ControlV2Client& client) { return client.credential_; }
    static QJsonObject hostPins(const ControlV2Client& client) { return client.hostPins_; }
    // A store written before host pins existed: no host-pins account at all.
    static void beforePins(ControlV2Client& client) {
        if (!client.credentials_.remove("host-pins"))
            qFatal("Synthetic pin removal failed");
        client.hostPins_ = {};
        client.hostPinsStored_ = false;
    }
    // Stores the in-memory credential, as a paired client finds it on its next start.
    static void persist(ControlV2Client& client) {
        if (!client.credentials_.write("user-session", QJsonDocument(client.credential_).toJson()))
            qFatal("Synthetic credential write failed");
    }
    static void restorePoll(ControlV2Client& client, int milliseconds) {
        client.restorePollMilliseconds_ = milliseconds;
    }
    static void expireCredential(ControlV2Client& client) {
        client.credential_.insert("expiresAt", "2020-01-01T00:00:00.000Z");
    }
};
} // namespace ketplus::remote
namespace {
QJsonObject credential(const QString& subject = "fixture-owner") {
    return {{"accessToken", "ketplus_access_" + QString(43, 'A')},
            {"refreshToken", "ketplus_refresh_" + QString(43, 'B')},
            {"tokenType", "Bearer"},
            {"expiresIn", 900},
            {"sessionId", "fixture-session"},
            {"subject", QJsonObject{{"id", subject}}}};
}
class FixtureTransport final : public QNetworkAccessManager {
  public:
    QHash<QString, QJsonValue> replies;
    QHash<QString, int> statuses;
    QList<QByteArray> bodies;
    QList<QString> paths;
    QPointer<QNetworkReply> latestReply;
    // Answers a request first when set, as {status, object}; std::nullopt falls back to replies.
    std::function<std::optional<std::pair<int, QJsonObject>>(const QNetworkRequest&,
                                                             const QByteArray&)>
        handler;

  protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* outgoing) override {
        if (operation != CustomOperation)
            qFatal("Fixture cannot open a real network connection");
        const auto path = request.url().path();
        if (handler) {
            const auto body = outgoing ? outgoing->readAll() : QByteArray{};
            if (auto answer = handler(request, body); answer) {
                bodies.append(body);
                paths.append(path);
                latestReply = new r03::Reply(request, answer->first,
                                             QJsonDocument(answer->second).toJson(), this);
                return latestReply;
            }
            if (!replies.contains(path))
                qFatal("Unexpected fixture request: %s", qPrintable(path));
            bodies.append(body);
            paths.append(path);
            const auto value = replies.value(path);
            latestReply = new r03::Reply(request, statuses.value(path, 200),
                                         value.isObject() ? QJsonDocument(value.toObject()).toJson()
                                                          : QJsonDocument(value.toArray()).toJson(),
                                         this);
            return latestReply;
        }
        if (!replies.contains(path))
            qFatal("Unexpected fixture request: %s", qPrintable(path));
        bodies.append(outgoing ? outgoing->readAll() : QByteArray{});
        paths.append(path);
        const auto value = replies.value(path);
        const auto body = value.isObject() ? QJsonDocument(value.toObject()).toJson()
                                           : QJsonDocument(value.toArray()).toJson();
        latestReply = new r03::Reply(request, statuses.value(path, 200), body, this);
        return latestReply;
    }
};
const QString prefix = "/native/ketplus";
const QJsonObject session{{"id", "opaque-session"},
                          {"generation", 3},
                          {"workspaceId", "workspace"},
                          {"machineId", "linux-host"}};
struct EncryptedFixture {
    CredentialStore hostStore{"fixture-host"};
    CloudCrypto host{&hostStore};
    QJsonObject hostKeys = host.publicKeys();
    QJsonObject wrapped;
    EncryptedFixture(ControlV2Client& client, FixtureTransport& network, bool pinned = true) {
        RemoteControlTestAccess::transport(client, network);
        if (!client.setCredential(credential()))
            qFatal("Invalid credential fixture");
        RemoteControlTestAccess::paired(client);
        if (pinned)
            RemoteControlTestAccess::pinHost(client, "linux-host", 1, hostKeys);
        else
            RemoteControlTestAccess::beforePins(client);
        const auto own = client.identity();
        wrapped = host.wrapSessionKey(
            "opaque-session", 3, 1,
            {{"id", "remote-client"}, {"keyVersion", 1}, {"keys", own.value("keys")}}, "linux-host",
            1);
        network.replies.insert(prefix + "/v2/mobile/sessions/opaque-session/keys",
                               QJsonArray{wrapped});
        network.replies.insert(
            prefix + "/v2/mobile/device-keys/linux-host/1",
            QJsonObject{{"deviceId", "linux-host"}, {"keyVersion", 1}, {"keys", hostKeys}});
    }
    QJsonObject event(int cursor = 1) {
        QJsonObject event{{"workspaceId", "workspace"}, {"sessionId", "opaque-session"},
                          {"sessionGeneration", 3},     {"cursor", cursor},
                          {"turnSequence", 1},          {"eventType", "assistant.delta"}};
        const auto aad = CloudCrypto::canonical({"ketplus", 2, "event", "workspace",
                                                 "opaque-session", 3, cursor, 1, "assistant.delta",
                                                 "opaque-session:3:1", 1, "linux-host", 1});
        event.insert("content", host.encrypt("{\"delta\":\"private fixture text\"}", aad,
                                             "opaque-session", 3, 1, "linux-host", 1));
        return event;
    }
};
} // namespace
