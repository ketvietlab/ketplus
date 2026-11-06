#include "cloud/CloudControlClient.h"
#include "cloud/CloudControlSupport.h"
#include "atlas/AtlasLiveRuntime.h"
#include "cloud/CloudControlBatching.h"
#include "cloud/CloudEventProjection.h"
#include "cloud/ControlEndpoint.h"
#include "cloud/WorktreeBranches.h"
#include "cloud/WorktreeRepositories.h"
#include <QUrlQuery>

#include "KetPlusProductVersion.h"
#include "protocol/DaemonProtocol.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include "workspace/ReadOnlyWorkspaceFiles.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QThreadPool>

#include <algorithm>
#include <utility>

namespace ketplus::daemon {
using cloud_control::object;
using cloud_control::responseCode;
using cloud_control::sourceContext;

namespace {

constexpr auto maximumResponseBytes = 1024 * 1024;

constexpr auto requestTimeoutMilliseconds = 30'000;

QString stateForCloud(const QString& state) {
    // A failed or interrupted turn leaves an idle session that still takes the next turn.
    // Reported as "unreachable", the phone showed the Mac offline and made the thread
    // read-only.
    if (state == QStringLiteral("failed") || state == QStringLiteral("interrupted"))
        return QStringLiteral("ready");
    const QStringList accepted{QStringLiteral("starting"),   QStringLiteral("ready"),
                               QStringLiteral("running"),    QStringLiteral("waiting_for_input"),
                               QStringLiteral("stopping"),   QStringLiteral("exited"),
                               QStringLiteral("unreachable")};
    return accepted.contains(state) ? state : QStringLiteral("unreachable");
}

// How the most recent turn ended, or null while none has ended.
QJsonValue lastTurnOutcome(const StoredSession& session) {
    if (session.state == QStringLiteral("failed") || session.state == QStringLiteral("interrupted"))
        return session.state;
    if (session.currentTurnSequence > 0 &&
        (session.state == QStringLiteral("ready") || session.state == QStringLiteral("exited")))
        return QStringLiteral("completed");
    return QJsonValue::Null;
}

QString encodedPathSegment(const QString& value) {
    return QString::fromUtf8(QUrl::toPercentEncoding(value, QByteArrayLiteral("-._~")));
}

QUrl compiledControlUrl() {
    const QUrl compiled(QStringLiteral(KETPLUS_DEFAULT_CONTROL_URL));
    return compiled.isValid() && !compiled.host().isEmpty()
               ? compiled
               : QUrl(QStringLiteral("https://ketplus-staging.ketviet.vn"));
}

QUrl controlBaseUrl() {
    const QUrl defaultRelay = compiledControlUrl();
#if defined(KETPLUS_REMOTE_DEMO_AVAILABLE)
    const QUrl configured(qEnvironmentVariable("KETPLUS_CONTROL_URL", defaultRelay.toString()));
    const bool loopback = configured.host() == QStringLiteral("localhost") ||
                          configured.host() == QStringLiteral("127.0.0.1") ||
                          configured.host() == QStringLiteral("::1");
    if (configured.isValid() && !configured.host().isEmpty() &&
        (configured.scheme() == QStringLiteral("https") ||
         (loopback && configured.scheme() == QStringLiteral("http")))) {
        return configured;
    }
#endif
    return defaultRelay;
}

} // namespace

CloudControlClient::CloudControlClient(LocalJournal* journal, SessionHost* host, QObject* parent)
    : QObject(parent), journal_(journal), host_(host), crypto_(&credentials_), pins_(&credentials_),
      baseUrl_(controlBaseUrl()),
      wakeups_(baseUrl_, [this](std::function<void(const QString&)> answer) {
          ensureDaemonCredential(
              [this, answer = std::move(answer)](const bool ready, const QString&) {
                  answer(ready ? daemonToken_ : QString());
              });
      }) {
    machineId_ = storedString(QStringLiteral("machine-id"));
    workspaceId_ = storedString(QStringLiteral("workspace-id"));
    machineKeyVersion_ = storedString(QStringLiteral("machine-key-version")).toInt();
    if (machineKeyVersion_ < 1)
        machineKeyVersion_ = 1;
    // An enrollment belongs to the relay that issued it. A build compiled for another relay
    // presented it there, got errors it could not explain, and uploaded nothing. The identity
    // stays stored for a build that talks to its relay again; this one reports it signed out.
    const QString enrolledRelay = storedString(QStringLiteral("control-url"));
    if (!machineId_.isEmpty() && enrolledRelay.isEmpty()) {
        // Enrolled before the relay was recorded: the relay this build reaches is the only
        // one it can have been used with.
        storeString(QStringLiteral("control-url"), baseUrl_.toString());
    } else if (!machineId_.isEmpty() && enrolledRelay != baseUrl_.toString()) {
        otherRelay_ = enrolledRelay;
        machineId_.clear();
        workspaceId_.clear();
        lastError_ = QStringLiteral("This Mac is enrolled with %1, not %2.")
                         .arg(enrolledRelay, baseUrl_.toString());
    }
    if (!machineId_.isEmpty())
        registerCloudCursors();
    initializePeerTransport();
    syncTimer_.setInterval(15'000);
    commandTimer_.setInterval(2'000);
    workspaceFilesPool_.setMaxThreadCount(2);
    workspaceFilesPool_.setExpiryTimeout(10'000);
    heartbeatPool_.setMaxThreadCount(1);
    heartbeatPool_.setExpiryTimeout(30'000);
    connect(&syncTimer_, &QTimer::timeout, this, &CloudControlClient::sync);
    connect(&commandTimer_, &QTimer::timeout, this, &CloudControlClient::pollCommands);
    // While the relay can wake this Mac, the poll is only the backstop for a lost wake-up.
    connect(&wakeups_, &RelayWakeups::wake, this, &CloudControlClient::wakeCommandPoll);
    connect(&wakeups_, &RelayWakeups::openChanged, this, [this](const bool open) {
        commandTimer_.setInterval(open ? wakeupBackstopPollMilliseconds
                                       : disconnectedPollMilliseconds);
    });
    connect(&wakeups_, &RelayWakeups::credentialExpired, this, [this] { daemonToken_.clear(); });
    stateSyncTimer_.setSingleShot(true);
    connect(&stateSyncTimer_, &QTimer::timeout, this, [this] {
        // A heartbeat already in flight read the sessions before this change.
        if (heartbeatInFlight_) {
            stateSyncTimer_.start(1'000);
            return;
        }
        sync();
    });
    connect(host_, &SessionHost::eventReady, this, [this](const QJsonObject& event) {
        peers_->eventsChanged(event.value("session_id").toString());
        QTimer::singleShot(0, this, &CloudControlClient::uploadEvents);
        // Phones read a conversation's state from the heartbeat. A turn that starts or ends
        // must not wait up to 15s for the next one, or they keep showing it as it was.
        const QString type = event.value(QStringLiteral("event_type")).toString();
        if (type == QStringLiteral("turn.started") || type == QStringLiteral("turn.completed"))
            stateSyncTimer_.start(300);
    });
}

CloudControlClient::~CloudControlClient() {
    signaling_->configure({}, {}, 1, false);
    workspaceFilesPool_.clear();
    heartbeatPool_.clear();
    workspaceFilesPool_.waitForDone();
    heartbeatPool_.waitForDone();
}

QUrl CloudControlClient::configuredControlUrl() { return controlBaseUrl(); }

void CloudControlClient::start() {
    syncTimer_.start();
    commandTimer_.start();
    if (!machineId_.isEmpty()) {
        QTimer::singleShot(0, this, &CloudControlClient::sync);
        QTimer::singleShot(0, this, &CloudControlClient::pollCommands);
        wakeups_.start();
    }
}

bool CloudControlClient::handles(const QString& type) const {
    return type == QStringLiteral("remote.status") || type == QStringLiteral("remote.sign_in") ||
           type == QStringLiteral("remote.sign_in.device") ||
           type == QStringLiteral("remote.sign_in.cancel") ||
           type == QStringLiteral("remote.sign_out") || type == QStringLiteral("remote.devices") ||
           type == QStringLiteral("remote.pairing.create") ||
           type == QStringLiteral("remote.pairing.status") ||
           type == QStringLiteral("remote.pairing.approve") ||
           type == QStringLiteral("remote.pairing.revoke") ||
           type == QStringLiteral("remote.device.revoke");
}

void CloudControlClient::handleLocal(const QString& type, const QJsonObject& payload,
                                     LocalCallback callback) {
    if (type == QStringLiteral("remote.status")) {
        callback(status(), {});
    } else if (type == QStringLiteral("remote.sign_in")) {
        signIn(std::move(callback));
    } else if (type == QStringLiteral("remote.sign_in.device")) {
        signInDevice(std::move(callback));
    } else if (type == QStringLiteral("remote.sign_in.cancel")) {
        cancelSignIn(std::move(callback));
    } else if (type == QStringLiteral("remote.sign_out")) {
        signOut(std::move(callback));
    } else if (type == QStringLiteral("remote.devices")) {
        listDevices(std::move(callback));
    } else if (type == QStringLiteral("remote.pairing.create")) {
        createPairing(std::move(callback));
    } else if (type == QStringLiteral("remote.pairing.status")) {
        pairingStatus(payload, std::move(callback));
    } else if (type == QStringLiteral("remote.pairing.approve")) {
        approvePairing(payload, std::move(callback));
    } else if (type == QStringLiteral("remote.pairing.revoke")) {
        revokePairing(payload, std::move(callback));
    } else if (type == QStringLiteral("remote.device.revoke")) {
        revokeDevice(payload, std::move(callback));
    } else {
        callback({}, QStringLiteral("Unsupported remote-control operation."));
    }
}

void CloudControlClient::request(const QByteArray& method, const QString& path,
                                 const QJsonValue& body, const QString& bearer,
                                 const QHash<QByteArray, QByteArray>& headers,
                                 ReplyCallback callback) {
    QNetworkRequest request(controlEndpoint(baseUrl_, path));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(requestTimeoutMilliseconds);
    // Over HTTP/2 every request shares one connection. Through the relay's Cloudflare tunnel an
    // event upload of 60 KB stalled it, and heartbeats, command polls and acks all timed out
    // behind it: phones saw the Mac offline and commands were claimed a dozen times. HTTP/1.1
    // gives the upload a connection of its own.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-KetPlus-Control-Version", "2");
    if (!bearer.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + bearer.toUtf8());
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        request.setRawHeader(it.key(), it.value());
    QByteArray encoded;
    if (body.isObject()) {
        encoded = QJsonDocument(body.toObject()).toJson(QJsonDocument::Compact);
    } else if (body.isArray()) {
        encoded = QJsonDocument(body.toArray()).toJson(QJsonDocument::Compact);
    }
    QNetworkReply* reply = transport_->sendCustomRequest(request, method, encoded);
    connect(reply, &QNetworkReply::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > maximumResponseBytes)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback)] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // An aborted reply (transfer timeout, oversized body) is closed and has nothing to read.
        QByteArray bytes = reply->isOpen() ? reply->read(maximumResponseBytes + 1) : QByteArray{};
        QString error;
        QJsonValue value;
        if (bytes.size() > maximumResponseBytes) {
            error = QStringLiteral("Control response exceeds 1 MiB.");
        } else if (!bytes.isEmpty()) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
            if (parseError.error != QJsonParseError::NoError) {
                error = QStringLiteral("Control returned invalid JSON.");
            } else if (document.isObject()) {
                value = document.object();
            } else if (document.isArray()) {
                value = document.array();
            }
        }
        // Qt also reports 4xx/5xx as reply errors; those carry the relay's own answer. A
        // connection, timeout or HTTP/2 failure can arrive after a 200 header, and its body is
        // lost: treating that as success dropped every command a poll had leased, so the relay
        // re-leased them dozens of times while the phone waited.
        const QNetworkReply::NetworkError failure = reply->error();
        if (failure != QNetworkReply::NoError &&
            (status == 0 || (failure > QNetworkReply::NoError && failure < 100) ||
             failure == QNetworkReply::ProtocolFailure))
            error = reply->errorString();
        reply->deleteLater();
        callback(status, value, error);
    });
}

void CloudControlClient::ensureDaemonCredential(ReadyCallback callback) {
    if (!daemonToken_.isEmpty() &&
        daemonTokenExpiresAt_ > QDateTime::currentDateTimeUtc().addSecs(60)) {
        callback(true, {});
        return;
    }
    credentialWaiters_.append(std::move(callback));
    if (credentialRequestInFlight_)
        return;
    if (machineId_.isEmpty()) {
        finishCredentialWaiters(false, QStringLiteral("This Mac has not been enrolled."));
        return;
    }
    credentialRequestInFlight_ = true;
    const quint64 requestEpoch = accountEpoch_;
    const QJsonObject challengeRequest{{QStringLiteral("machineId"), machineId_},
                                       {QStringLiteral("keyVersion"), machineKeyVersion_}};
    request(
        "POST", QStringLiteral("/v2/daemon/challenges"), challengeRequest, {}, {},
        [this, requestEpoch](const int status, const QJsonValue& value,
                             const QString& transportError) {
            if (requestEpoch != accountEpoch_)
                return;
            const QJsonObject challenge = object(value);
            if (status != 201 || !transportError.isEmpty()) {
                finishCredentialWaiters(
                    false, transportError.isEmpty()
                               ? responseCode(value, QStringLiteral("challenge_failed"))
                               : transportError);
                return;
            }
            // The relay issues 32 random bytes. Every envelope, wrapped key and WebRTC answer
            // this key signs is longer, so a relay cannot pass one off as a challenge (#85).
            const QString encoded = challenge.value(QStringLiteral("challenge")).toString();
            const QByteArray bytes = CloudCrypto::fromBase64Url(encoded);
            if (bytes.size() != machineChallengeBytes ||
                CloudCrypto::base64Url(bytes) != encoded.toLatin1()) {
                finishCredentialWaiters(
                    false, QStringLiteral("The relay sent an invalid machine challenge."));
                return;
            }
            QString cryptoError;
            const QByteArray signature = crypto_.sign(bytes, &cryptoError);
            if (signature.isEmpty()) {
                finishCredentialWaiters(false, cryptoError);
                return;
            }
            const QJsonObject body{
                {QStringLiteral("challengeId"), challenge.value(QStringLiteral("id"))}};
            request(
                "POST", QStringLiteral("/v2/daemon/credentials"), body, {},
                {{QByteArray("X-KetPlus-Machine-Signature"), CloudCrypto::base64Url(signature)}},
                [this, requestEpoch](const int credentialStatus, const QJsonValue& credentialValue,
                                     const QString& credentialError) {
                    if (requestEpoch != accountEpoch_)
                        return;
                    const QJsonObject credential = object(credentialValue);
                    if (credentialStatus != 201 || !credentialError.isEmpty()) {
                        finishCredentialWaiters(
                            false,
                            credentialError.isEmpty()
                                ? responseCode(credentialValue, QStringLiteral("credential_failed"))
                                : credentialError);
                        return;
                    }
                    daemonToken_ = credential.value(QStringLiteral("accessToken")).toString();
                    daemonTokenExpiresAt_ = QDateTime::fromString(
                        credential.value(QStringLiteral("expiresAt")).toString(), Qt::ISODate);
                    finishCredentialWaiters(!daemonToken_.isEmpty(),
                                            daemonToken_.isEmpty()
                                                ? QStringLiteral("Missing daemon credential.")
                                                : QString{});
                });
        });
}

void CloudControlClient::finishCredentialWaiters(const bool ready, const QString& error) {
    credentialRequestInFlight_ = false;
    if (!ready) {
        daemonToken_.clear();
        lastError_ = error;
    } else {
        lastError_.clear();
    }
    const auto waiters = std::exchange(credentialWaiters_, {});
    for (const ReadyCallback& waiter : waiters)
        waiter(ready, error);
}

void CloudControlClient::signOut(LocalCallback callback) {
    signaling_->configure({}, {}, 1, false);
    for (auto* runtime : findChildren<AtlasLiveRuntime*>()) {
        runtime->stop();
        runtime->deleteLater();
    }
    ++accountEpoch_;
    dropBrowserSignIn();
    signInError_.clear();
    QString removalError;
    // This build is not enrolled with its relay when the stored enrollment is another relay's;
    // that enrollment and its upload progress stay for a build that talks to it.
    if (otherRelay_.isEmpty()) {
        if (!clearUploadProgress(&removalError)) {
            callback({}, removalError);
            return;
        }
        for (const QString& account :
             {QStringLiteral("machine-id"), QStringLiteral("workspace-id"),
              QStringLiteral("machine-key-version"), QStringLiteral("control-url")}) {
            if (!credentials_.remove(account, &removalError)) {
                callback({}, removalError);
                return;
            }
        }
    }
    wakeups_.stop();
    credentialRequestInFlight_ = false;
    finishCredentialWaiters(false, QStringLiteral("Signed out."));
    machineId_.clear();
    workspaceId_.clear();
    daemonToken_.clear();
    registeredCloudCursors_.clear();
    eventUploadLimits_.clear();
    eventUploadBackoff_.clear();
    daemonTokenExpiresAt_ = {};
    machineKeyVersion_ = 1;
    refusedSessions_.clear();
    lastError_.clear();
    callback(status(), {});
}

bool CloudControlClient::clearUploadProgress(QString* error) {
    // Upload progress belongs to the relay being left. Keeping it made the next enrollment
    // resume mid-history, so that relay never received earlier events and rejected the rest
    // as a cursor gap.
    for (const StoredSession& session : journal_->sessions()) {
        if (!credentials_.remove(
                QStringLiteral("uploaded-cursor-%1-%2").arg(session.id).arg(session.generation),
                error))
            return false;
    }
    return true;
}

QJsonObject CloudControlClient::status() const {
    QJsonObject values{{QStringLiteral("ok"), true},
                       {QStringLiteral("configured"), !machineId_.isEmpty()},
                       {QStringLiteral("connected"), !daemonToken_.isEmpty()},
                       {QStringLiteral("wake_ups"), wakeups_.isOpen()},
                       {QStringLiteral("machine_id"), machineId_},
                       {QStringLiteral("control_url"), baseUrl_.toString()},
                       {QStringLiteral("product_version"), QStringLiteral(KETPLUS_PRODUCT_VERSION)},
                       {QStringLiteral("session_count"), journal_->sessions().size()},
                       {QStringLiteral("last_error"), lastError_},
                       // A browser sign-in waiting for its callback, or why the last one failed.
                       {QStringLiteral("sign_in_pending"), !browserSignIn_.isNull() ||
                                                               deviceSignInStarting_ ||
                                                               !deviceCode_.isEmpty()},
                       {QStringLiteral("sign_in_error"), signInError_}};
    if (!otherRelay_.isEmpty())
        values.insert(QStringLiteral("enrolled_control_url"), otherRelay_);
    return values;
}

void CloudControlClient::listDevices(LocalCallback callback) {
    ensureDaemonCredential([this, callback = std::move(callback)](const bool ready,
                                                                  const QString& error) {
        if (!ready) {
            callback({}, error);
            return;
        }
        request("GET", QStringLiteral("/v2/devices"), {}, daemonToken_, {},
                [callback = std::move(callback)](const int status, const QJsonValue& value,
                                                 const QString& transportError) {
                    if (status != 200 || !value.isArray() || !transportError.isEmpty()) {
                        callback({},
                                 transportError.isEmpty()
                                     ? responseCode(value, QStringLiteral("devices_unavailable"))
                                     : transportError);
                        return;
                    }
                    callback({{QStringLiteral("ok"), true},
                              {QStringLiteral("devices"), value.toArray()}},
                             {});
                });
    });
}

QByteArray CloudControlClient::ensurePathIdSalt(QString* error) {
    if (pathIdSalt_.size() == 32)
        return pathIdSalt_;
    static const QString account = QStringLiteral("path-id-salt");
    QString readError;
    QByteArray salt = credentials_.read(account, &readError);
    if (!readError.isEmpty()) {
        // Regenerating on a transient read failure would silently change every path id.
        if (error != nullptr)
            *error = readError;
        return {};
    }
    if (salt.size() != 32) {
        // QRandomGenerator::system() is the OS cryptographically secure source.
        salt = QByteArray(32, Qt::Uninitialized);
        QRandomGenerator::system()->generate(salt.begin(), salt.end());
        if (!credentials_.write(account, salt, error))
            return {};
    }
    pathIdSalt_ = salt;
    return pathIdSalt_;
}

QString CloudControlClient::storedString(const QString& account) const {
    return QString::fromUtf8(credentials_.read(account));
}

bool CloudControlClient::storeString(const QString& account, const QString& value, QString* error) {
    return credentials_.write(account, value.toUtf8(), error);
}

void CloudControlClient::createPairing(LocalCallback callback) {
    ensureDaemonCredential([this, callback = std::move(callback)](const bool ready,
                                                                  const QString& error) {
        if (!ready) {
            callback({}, error);
            return;
        }
        request("POST", QStringLiteral("/v2/pairing/tickets"), QJsonObject{}, daemonToken_, {},
                [callback = std::move(callback)](const int status, const QJsonValue& value,
                                                 const QString& transportError) {
                    const QJsonObject pairing = object(value);
                    const QJsonObject ticket = pairing.value(QStringLiteral("ticket")).toObject();
                    const QString secret = pairing.value(QStringLiteral("secret")).toString();
                    if (status != 201 || ticket.isEmpty() || secret.isEmpty() ||
                        !transportError.isEmpty()) {
                        callback({}, transportError.isEmpty()
                                         ? responseCode(value, QStringLiteral("pairing_failed"))
                                         : transportError);
                        return;
                    }
                    const QString qr = QStringLiteral("ketplus://pair?ticket=%1&secret=%2")
                                           .arg(QString::fromUtf8(QUrl::toPercentEncoding(
                                                    ticket.value(QStringLiteral("id")).toString())),
                                                QString::fromUtf8(QUrl::toPercentEncoding(secret)));
                    callback({{QStringLiteral("ok"), true},
                              {QStringLiteral("ticket"), ticket},
                              {QStringLiteral("pairing_code"), qr}},
                             {});
                });
    });
}

void CloudControlClient::pairingStatus(const QJsonObject& payload, LocalCallback callback) {
    const QString ticketId = payload.value(QStringLiteral("ticket_id")).toString();
    if (ticketId.isEmpty()) {
        callback({}, QStringLiteral("A pairing ticket is required."));
        return;
    }
    ensureDaemonCredential([this, ticketId, callback = std::move(callback)](const bool ready,
                                                                            const QString& error) {
        if (!ready) {
            callback({}, error);
            return;
        }
        request("GET", QStringLiteral("/v2/pairing/tickets/%1").arg(encodedPathSegment(ticketId)),
                {}, daemonToken_, {},
                [callback = std::move(callback)](const int status, const QJsonValue& value,
                                                 const QString& transportError) {
                    if (status != 200 || !transportError.isEmpty()) {
                        callback({}, transportError.isEmpty()
                                         ? responseCode(value, QStringLiteral("pairing_not_found"))
                                         : transportError);
                    } else {
                        callback({{QStringLiteral("ok"), true},
                                  {QStringLiteral("ticket"), object(value)}},
                                 {});
                    }
                });
    });
}

void CloudControlClient::approvePairing(const QJsonObject& payload, LocalCallback callback) {
    const QString ticketId = payload.value(QStringLiteral("ticket_id")).toString();
    // The claim the user confirmed on this Mac; the ticket is read again below, and the relay
    // must not swap keys and digest together in between.
    const QString confirmed = payload.value(QStringLiteral("claim_digest")).toString();
    if (ticketId.isEmpty() || confirmed.isEmpty()) {
        callback({}, QStringLiteral("A pairing ticket and its confirmed claim are required."));
        return;
    }
    ensureDaemonCredential([this, ticketId, confirmed, callback = std::move(callback)](
                               const bool ready, const QString& error) {
        if (!ready) {
            callback({}, error);
            return;
        }
        const QString path =
            QStringLiteral("/v2/pairing/tickets/%1").arg(encodedPathSegment(ticketId));
        request(
            "GET", path, {}, daemonToken_, {},
            [this, path, confirmed, callback = std::move(callback)](
                const int status, const QJsonValue& value, const QString& transportError) {
                const QJsonObject ticket = object(value);
                const QJsonObject device = ticket.value(QStringLiteral("claimedDevice")).toObject();
                const QString digest = ticket.value(QStringLiteral("claimDigest")).toString();
                if (status != 200 ||
                    ticket.value(QStringLiteral("status")).toString() !=
                        QStringLiteral("claimed") ||
                    device.isEmpty() || digest.isEmpty() || !transportError.isEmpty()) {
                    callback({}, transportError.isEmpty()
                                     ? QStringLiteral("The mobile key claim is not ready.")
                                     : transportError);
                    return;
                }
                // The code the user compared is this digest. Recomputed from the claimed keys, it
                // proves those keys are the phone's, not ones the relay substituted (#87).
                if (digest != confirmed ||
                    DevicePins::claimDigest(ticket.value(QStringLiteral("id")).toString(),
                                            device) != digest) {
                    callback({}, QStringLiteral("The device claim does not match its keys."));
                    return;
                }
                QJsonArray envelopes;
                QString cryptoError;
                for (const StoredSession& session : journal_->sessions()) {
                    // The relay refuses the whole approval for a key it would not hold.
                    if (refusedSessions_.contains(session.id))
                        continue;
                    const QString account =
                        QStringLiteral("wrapped-%1-%2-%3-%4")
                            .arg(session.id)
                            .arg(session.generation)
                            .arg(device.value(QStringLiteral("id")).toString())
                            .arg(device.value(QStringLiteral("keyVersion")).toInt());
                    QJsonObject envelope =
                        object(QJsonDocument::fromJson(credentials_.read(account)).object());
                    if (!CloudCrypto::validWrappedSessionKey(envelope)) {
                        envelope =
                            crypto_.wrapSessionKey(session.id, session.generation, 1, device,
                                                   machineId_, machineKeyVersion_, &cryptoError);
                        if (envelope.isEmpty() ||
                            !credentials_.write(
                                account, QJsonDocument(envelope).toJson(QJsonDocument::Compact),
                                &cryptoError)) {
                            callback({}, cryptoError);
                            return;
                        }
                    }
                    envelopes.append(envelope);
                }
                QJsonObject body{{QStringLiteral("claimDigest"), digest}};
                if (!envelopes.isEmpty())
                    body.insert(QStringLiteral("sessionKeys"), envelopes);
                request("POST", path + QStringLiteral("/approve"), body, daemonToken_, {},
                        [this, device, callback = std::move(callback)](
                            const int approveStatus, const QJsonValue& approveValue,
                            const QString& approveError) {
                            QString pinError;
                            if (approveStatus != 200 || !approveError.isEmpty()) {
                                callback({}, approveError.isEmpty()
                                                 ? responseCode(
                                                       approveValue,
                                                       QStringLiteral("pairing_approval_failed"))
                                                 : approveError);
                            } else if (!pins_.pin(device, &pinError)) {
                                callback({}, pinError.isEmpty()
                                                 ? QStringLiteral("Unable to remember the device.")
                                                 : pinError);
                            } else {
                                callback({{QStringLiteral("ok"), true},
                                          {QStringLiteral("ticket"), object(approveValue)}},
                                         {});
                            }
                        });
            });
    });
}

void CloudControlClient::revokePairing(const QJsonObject& payload, LocalCallback callback) {
    const QString ticketId = payload.value(QStringLiteral("ticket_id")).toString();
    if (ticketId.isEmpty()) {
        callback({}, QStringLiteral("A pairing ticket is required."));
        return;
    }
    ensureDaemonCredential([this, ticketId, callback = std::move(callback)](const bool ready,
                                                                            const QString& error) {
        if (!ready) {
            callback({}, error);
            return;
        }
        request("POST",
                QStringLiteral("/v2/pairing/tickets/%1/revoke").arg(encodedPathSegment(ticketId)),
                QJsonObject{}, daemonToken_, {},
                [callback = std::move(callback)](const int status, const QJsonValue& value,
                                                 const QString& transportError) {
                    if (status != 200 || !transportError.isEmpty()) {
                        callback({},
                                 transportError.isEmpty()
                                     ? responseCode(value, QStringLiteral("pairing_revoke_failed"))
                                     : transportError);
                        return;
                    }
                    callback({{QStringLiteral("ok"), true}}, {});
                });
    });
}

void CloudControlClient::revokeDevice(const QJsonObject& payload, LocalCallback callback) {
    const QString deviceId = payload.value(QStringLiteral("device_id")).toString();
    if (deviceId.isEmpty()) {
        callback({}, QStringLiteral("A device is required."));
        return;
    }
    ensureDaemonCredential([this, deviceId, callback = std::move(callback)](const bool ready,
                                                                            const QString& error) {
        if (!ready) {
            callback({}, error);
            return;
        }
        request("POST", QStringLiteral("/v2/devices/%1/revoke").arg(encodedPathSegment(deviceId)),
                QJsonObject{}, daemonToken_, {},
                [this, deviceId, callback = std::move(callback)](
                    const int status, const QJsonValue& value, const QString& transportError) {
                    if (status != 200 || !transportError.isEmpty()) {
                        callback({},
                                 transportError.isEmpty()
                                     ? responseCode(value, QStringLiteral("device_revoke_failed"))
                                     : transportError);
                        return;
                    }
                    signaling_->receive({{"type", "device.revoked"}, {"deviceId", deviceId}});
                    senderKeys_.clear();
                    QString pinError;
                    if (!pins_.unpin(deviceId, &pinError)) {
                        callback({}, pinError);
                        return;
                    }
                    callback({{QStringLiteral("ok"), true}}, {});
                });
    });
}

QJsonArray CloudControlClient::sessionSnapshots(
    const QList<StoredSession>& stored, const QString& workspaceId, const QString& machineId,
    const QByteArray& pathIdSalt, const QHash<QString, QString>& repositories,
    QHash<QString, QJsonObject>* metadata, const QHash<QString, QJsonObject>& agentMetadata) {
    QJsonArray sessions;
    const QString machineName = QHostInfo::localHostName().left(128);
    QHash<QString, QString> revisions;
    for (const StoredSession& session : stored) {
        const QString label = QFileInfo(session.worktreePath).fileName();
        // The relay only accepts these providers; anything else keeps the historical default.
        const QString adapter =
            QStringList{QStringLiteral("codex"), QStringLiteral("claude"), QStringLiteral("cursor")}
                    .contains(session.adapter)
                ? session.adapter
                : QStringLiteral("codex");
        QJsonObject snapshot{{QStringLiteral("id"), session.id},
                             {QStringLiteral("workspaceId"), workspaceId},
                             {QStringLiteral("machineId"), machineId},
                             {QStringLiteral("processInstanceId"), session.id},
                             {QStringLiteral("adapter"), adapter},
                             {QStringLiteral("generation"), session.generation},
                             {QStringLiteral("title"), session.title.isEmpty()
                                                           ? QStringLiteral("Codex session")
                                                           : session.title},
                             {QStringLiteral("workspaceLabel"), label},
                             {QStringLiteral("state"), stateForCloud(session.state)},
                             {QStringLiteral("lastEventCursor"), session.lastCursor},
                             {QStringLiteral("currentTurnSequence"), session.currentTurnSequence},
                             {QStringLiteral("contextRevision"), session.contextRevision}};
        QJsonObject context = sourceContext(session.worktreePath, pathIdSalt,
                                            repositories.value(session.worktreePath));
        // The branch name leaks repository detail in plaintext; only the phone reads it, sealed.
        if (metadata != nullptr) {
            metadata->insert(
                session.id,
                {{QStringLiteral("branch"), context.value(QStringLiteral("branch")).toString()},
                 {QStringLiteral("machineName"), machineName},
                 {QStringLiteral("worktreeExists"), QFileInfo(session.worktreePath).isDir()},
                 // Phones order conversations by it, as the desktop sidebar does.
                 {QStringLiteral("lastTurnStartedAt"), session.lastTurnStartedAt},
                 // "state" says whether the conversation takes a new turn; this says how the
                 // last one ended, which "ready" after a failure no longer shows.
                 {QStringLiteral("lastTurnOutcome"), lastTurnOutcome(session)}});
            // ADR 0027: the number agents and users address the conversation by, and the
            // conversation whose message started the running turn. Sealed: only the phone
            // shows them.
            const QJsonObject agent = agentMetadata.value(session.id);
            for (auto it = agent.constBegin(); it != agent.constEnd(); ++it)
                (*metadata)[session.id].insert(it.key(), it.value());
        }
        context.remove(QStringLiteral("branch"));
        for (auto it = context.constBegin(); it != context.constEnd(); ++it) {
            snapshot.insert(it.key(), it.value());
        }
        QString workspaceRevision = revisions.value(session.worktreePath);
        if (workspaceRevision.isEmpty()) {
            ReadOnlyWorkspaceFiles files({QStringLiteral("heartbeat"), workspaceId, machineId,
                                          context.value(QStringLiteral("repositoryId")).toString(),
                                          context.value(QStringLiteral("worktreeId")).toString(),
                                          session.worktreePath});
            workspaceRevision = files.revision();
            revisions.insert(session.worktreePath, workspaceRevision);
        }
        if (!workspaceRevision.isEmpty())
            snapshot.insert(QStringLiteral("workspaceRevision"), workspaceRevision);
        sessions.append(snapshot);
    }
    return sessions;
}

QList<QJsonArray>

CloudControlClient::withSessionMetadata(const QList<QJsonArray>& batches,
                                        const QHash<QString, QJsonObject>& metadata) {
    if (sessionMetadataRefused_)
        return batches;
    QList<QJsonArray> sealed;
    QSet<QString> current;
    for (const QJsonArray& batch : batches) {
        QJsonArray snapshots;
        for (const QJsonValue& value : batch) {
            QJsonObject snapshot = value.toObject();
            const QString sessionId = snapshot.value(QStringLiteral("id")).toString();
            current.insert(sessionId);
            if (metadata.contains(sessionId)) {
                const QJsonObject envelope =
                    sealedSessionMetadata(snapshot, metadata.value(sessionId));
                if (!envelope.isEmpty())
                    snapshot.insert(QStringLiteral("encryptedMetadata"), envelope);
            }
            snapshots.append(snapshot);
        }
        sealed.append(snapshots);
    }
    for (auto it = sealedMetadata_.begin(); it != sealedMetadata_.end();) {
        if (current.contains(it.key()))
            ++it;
        else
            it = sealedMetadata_.erase(it);
    }
    return sealed;
}

QJsonObject CloudControlClient::sealedSessionMetadata(const QJsonObject& snapshot,
                                                      const QJsonObject& metadata) {
    const QString sessionId = snapshot.value(QStringLiteral("id")).toString();
    const qint64 generation = snapshot.value(QStringLiteral("generation")).toInteger();
    // The relay only takes metadata sealed at the snapshot's own cursor and turn.
    const qint64 cursor = snapshot.value(QStringLiteral("lastEventCursor")).toInteger();
    const qint64 turnSequence = snapshot.value(QStringLiteral("currentTurnSequence")).toInteger();
    const QString keyId = QStringLiteral("%1:%2:1").arg(sessionId).arg(generation);
    const QByteArray aad = CloudCrypto::canonical(
        {QStringLiteral("ketplus"), 2, QStringLiteral("session-metadata"), workspaceId_, machineId_,
         sessionId, generation, cursor, turnSequence, keyId, 1, machineId_, machineKeyVersion_});
    const QByteArray plaintext = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    const QByteArray fingerprint = aad + '\0' + plaintext;
    if (const auto cached = sealedMetadata_.constFind(sessionId);
        cached != sealedMetadata_.cend() && cached->first == fingerprint)
        return cached->second;
    QString cryptoError;
    const QJsonObject content = crypto_.encrypt(plaintext, aad, sessionId, generation, 1,
                                                machineId_, machineKeyVersion_, &cryptoError);
    if (content.isEmpty()) {
        lastError_ = cryptoError;
        return {};
    }
    const QJsonObject envelope{{QStringLiteral("cursor"), cursor},
                               {QStringLiteral("turnSequence"), turnSequence},
                               {QStringLiteral("content"), content}};
    sealedMetadata_.insert(sessionId, {fingerprint, envelope});
    return envelope;
}

QStringList CloudControlClient::catalogWorktreePaths() const {
    // The relay takes at most 100 worktrees a heartbeat; the ones with conversations come first.
    constexpr qsizetype maximumWorktrees = 100;
    QStringList paths;
    QStringList repositories;
    const auto add = [&paths](const QString& path) {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (!canonical.isEmpty() && QFileInfo(canonical).isDir() && !paths.contains(canonical) &&
            paths.size() < maximumWorktrees)
            paths.append(canonical);
    };
    for (const QString& path : journal_->worktreePaths()) {
        // A deleted worktree is no longer a place to start a conversation; the relay retires
        // the entries a heartbeat leaves out.
        add(path);
        if (const QString canonical = QFileInfo(path).canonicalFilePath(); !canonical.isEmpty())
            if (const QString repository = worktreeRepository(canonical);
                !repositories.contains(repository))
                repositories.append(repository);
    }
    // Every checkout of those repositories, as the desktop's New conversation dialog lists
    // them, not only the ones a conversation already ran in.
    for (const QString& repository : std::as_const(repositories))
        for (const QString& checkout : repositoryCheckouts(repository))
            add(checkout);
    return paths;
}

QJsonArray CloudControlClient::worktreeSnapshots() const {
    QJsonArray worktrees;
    for (const QString& path : catalogWorktreePaths()) {
        QJsonObject entry = sourceContext(path, pathIdSalt_);
        entry.remove(QStringLiteral("branch"));
        entry.insert(QStringLiteral("catalogRevision"), 0);
        entry.insert(QStringLiteral("lifecycleRevision"), 0);
        worktrees.append(entry);
    }
    return worktrees;
}

} // namespace ketplus::daemon
