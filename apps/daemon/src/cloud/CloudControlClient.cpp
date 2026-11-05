#include "cloud/CloudControlClient.h"
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

QJsonObject object(const QJsonValue& value) {
    return value.isObject() ? value.toObject() : QJsonObject{};
}

// Scoped to a catalog worktree rather than a session, and sealed to this Mac.
bool isWorktreeCommand(const QJsonObject& command) {
    const QString kind = command.value(QStringLiteral("kind")).toString();
    return kind == QStringLiteral("session.create") ||
           kind == QStringLiteral("worktree.branches.read") ||
           kind == QStringLiteral("worktree.configuration.read");
}

// Branch names up to a budget that keeps the sealed list under the relay's 512 KiB result.
QJsonArray boundedNames(const QStringList& names, qsizetype* budget, bool* truncated) {
    QJsonArray bounded;
    for (const QString& name : names) {
        const qsizetype size = name.toUtf8().size() + 4;
        if (size > *budget) {
            *truncated = true;
            break;
        }
        *budget -= size;
        bounded.append(name);
    }
    return bounded;
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

QString responseCode(const QJsonValue& value, const QString& fallback) {
    return object(value).value(QStringLiteral("code")).toString(fallback);
}

// Keyed with a per-installation random salt so the relay cannot confirm guessed paths.
// An empty salt (unreadable credential store) yields an empty id, which never matches a scope.
QString opaquePathId(const QByteArray& kind, const QString& path, const QByteArray& salt) {
    if (salt.isEmpty())
        return {};
    return QString::fromLatin1(QMessageAuthenticationCode::hash(kind + '\n' + path.toUtf8(), salt,
                                                                QCryptographicHash::Sha256)
                                   .toHex());
}

// A deleted worktree keeps the repository recorded for it, so its conversations and any
// command naming them keep the scope they were published under.
QJsonObject sourceContext(const QString& worktreePath, const QByteArray& salt,
                          const QString& recordedRepository = {}) {
    const QString canonical = QFileInfo(worktreePath).canonicalFilePath();
    const QString worktree =
        canonical.isEmpty() ? QFileInfo(worktreePath).absoluteFilePath() : canonical;
    const QString gitDirectory = worktreeGitDirectory(worktree);
    QString repository = worktree;
    if (const qsizetype linked = gitDirectory.indexOf(QStringLiteral("/.git/worktrees/"));
        linked > 0) {
        repository = gitDirectory.left(linked);
    } else if (gitDirectory.isEmpty() && !recordedRepository.isEmpty()) {
        repository = recordedRepository;
    }
    QString branch;
    QFile head(QDir(gitDirectory).filePath(QStringLiteral("HEAD")));
    if (!gitDirectory.isEmpty() && head.open(QIODevice::ReadOnly)) {
        branch = QString::fromUtf8(head.readLine()).trimmed();
        const QString referencePrefix = QStringLiteral("ref: refs/heads/");
        if (branch.startsWith(referencePrefix)) {
            branch = branch.mid(referencePrefix.size());
        } else if (branch.size() > 12) {
            branch = branch.left(12);
        }
    }
    const QString repositoryLabel = QFileInfo(repository).fileName();
    const QString worktreeLabel = QFileInfo(worktree).fileName();
    return {{QStringLiteral("repositoryId"), opaquePathId("repository", repository, salt)},
            {QStringLiteral("repositoryLabel"),
             repositoryLabel.isEmpty() ? worktreeLabel : repositoryLabel},
            {QStringLiteral("worktreeId"), opaquePathId("worktree", worktree, salt)},
            {QStringLiteral("worktreeLabel"), worktreeLabel},
            {QStringLiteral("branch"), branch}};
}

} // namespace

CloudControlClient::CloudControlClient(LocalJournal* journal, SessionHost* host, QObject* parent)
    : QObject(parent), journal_(journal), host_(host), crypto_(&credentials_),
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
            QString cryptoError;
            const QByteArray signature = crypto_.sign(
                CloudCrypto::fromBase64Url(challenge.value(QStringLiteral("challenge")).toString()),
                &cryptoError);
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
        const QString path =
            QStringLiteral("/v2/pairing/tickets/%1").arg(encodedPathSegment(ticketId));
        request(
            "GET", path, {}, daemonToken_, {},
            [this, path, callback = std::move(callback)](const int status, const QJsonValue& value,
                                                         const QString& transportError) {
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
                        [callback = std::move(callback)](const int approveStatus,
                                                         const QJsonValue& approveValue,
                                                         const QString& approveError) {
                            if (approveStatus != 200 || !approveError.isEmpty()) {
                                callback({}, approveError.isEmpty()
                                                 ? responseCode(
                                                       approveValue,
                                                       QStringLiteral("pairing_approval_failed"))
                                                 : approveError);
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

void CloudControlClient::sync() {
    if (machineId_.isEmpty())
        return;
    heartbeat();
}

void CloudControlClient::heartbeat() {
    if (heartbeatInFlight_)
        return;
    heartbeatInFlight_ = true;
    ensureDaemonCredential([this](const bool ready, const QString& error) {
        if (!ready) {
            heartbeatInFlight_ = false;
            lastError_ = error;
            return;
        }
        QString saltError;
        const QByteArray salt = ensurePathIdSalt(&saltError);
        if (salt.isEmpty()) {
            heartbeatInFlight_ = false;
            lastError_ = saltError;
            return;
        }
        const QList<StoredSession> stored = journal_->sessions();
        const QHash<QString, QString> repositories = resolveWorktreeRepositories(*journal_);
        QHash<QString, QJsonObject> agentMetadata;
        const QHash<QString, QString> numbers = journal_->conversationNumbers();
        for (auto it = numbers.cbegin(); it != numbers.cend(); ++it)
            agentMetadata[it.key()].insert(QStringLiteral("number"), it.value());
        const QHash<QString, QJsonObject> senders =
            host_ ? host_->agentTurnSenders() : QHash<QString, QJsonObject>{};
        for (auto it = senders.cbegin(); it != senders.cend(); ++it)
            agentMetadata[it.key()].insert(QStringLiteral("agentTurn"), it.value());
        const quint64 requestEpoch = accountEpoch_;
        const QString workspaceId = workspaceId_;
        const QString machineId = machineId_;
        const QPointer<CloudControlClient> guard(this);
        heartbeatPool_.start([guard, stored, repositories, agentMetadata, requestEpoch, workspaceId,
                              machineId, salt]() {
            QHash<QString, QJsonObject> metadata;
            const QList<QJsonArray> batches =
                heartbeatSessionBatches(CloudControlClient::sessionSnapshots(
                    stored, workspaceId, machineId, salt, repositories, &metadata, agentMetadata));
            QMetaObject::invokeMethod(
                guard,
                [guard, batches, metadata, requestEpoch, workspaceId, machineId]() {
                    if (!guard)
                        return;
                    if (guard->accountEpoch_ != requestEpoch ||
                        guard->workspaceId_ != workspaceId || guard->machineId_ != machineId) {
                        guard->heartbeatInFlight_ = false;
                        return;
                    }
                    guard->refusalsThisHeartbeat_.clear();
                    guard->notePublishedRevisions(batches);
                    guard->sendHeartbeatBatches(guard->withSessionMetadata(batches, metadata), 0,
                                                requestEpoch, machineId, workspaceId);
                },
                Qt::QueuedConnection);
        });
    });
}

void CloudControlClient::sendHeartbeatBatches(const QList<QJsonArray>& batches,
                                              const qsizetype index, const quint64 requestEpoch,
                                              const QString& machineId,
                                              const QString& workspaceId) {
    if (accountEpoch_ != requestEpoch || machineId_ != machineId || workspaceId_ != workspaceId) {
        heartbeatInFlight_ = false;
        return;
    }
    if (index >= batches.size()) {
        heartbeatInFlight_ = false;
        lastError_.clear();
        refusedSessions_ = refusalsThisHeartbeat_;
        // The relay answers again: uploads that only failed to reach it need not wait.
        eventUploadBackoff_.removeIf([](const QHash<QString, EventUploadBackoff>::iterator entry) {
            return !entry->refused;
        });
        uploadEvents();
        distributeKeys();
        return;
    }
    QJsonObject body{
        {QStringLiteral("machineId"), machineId},
        {QStringLiteral("controlProtocolVersion"), 2},
        {QStringLiteral("capabilities"), QJsonArray{"transport.webrtc/1"}},
        {QStringLiteral("localProtocolVersion"), 0},
        {QStringLiteral("daemonVersion"), QStringLiteral(KETPLUS_PRODUCT_VERSION)},
        {QStringLiteral("observedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("sessions"), batches.at(index)},
        {QStringLiteral("worktrees"), worktreeSnapshots()}};
    QJsonArray deletions;
    for (const auto& value : journal_->deletionWork(64, true)) {
        const auto work = value.toObject();
        if (!work.value("cloud_acked").toBool())
            deletions.append(work.value("deletion"));
    }
    if (!deletions.isEmpty())
        body.insert("deletions", deletions);
    request("POST", QStringLiteral("/v2/daemon/heartbeat"), body, daemonToken_,
            {{QByteArray("X-KetPlus-Daemon-Version"), QByteArray(KETPLUS_PRODUCT_VERSION)}},
            [this, batches, index, requestEpoch, machineId, workspaceId](
                const int status, const QJsonValue& value, const QString& transportError) {
                if (accountEpoch_ != requestEpoch || machineId_ != machineId ||
                    workspaceId_ != workspaceId) {
                    heartbeatInFlight_ = false;
                    return;
                }
                if (status != 202 || !transportError.isEmpty()) {
                    heartbeatInFlight_ = false;
                    lastError_ = transportError.isEmpty()
                                     ? responseCode(value, QStringLiteral("heartbeat_failed"))
                                     : transportError;
                    if (status == 401) {
                        daemonToken_.clear();
                    }
                    // A relay that cannot read the sealed metadata refuses the whole heartbeat;
                    // later heartbeats go without it rather than leave the Mac offline.
                    if (lastError_ == QStringLiteral("invalid_session_metadata"))
                        sessionMetadataRefused_ = true;
                    return;
                }
                negotiatePeerTransport(value);
                refusalsThisHeartbeat_.unite(heartbeatRefusedSessions(value));
                for (const auto& entry : value.toObject().value("acceptedDeletions").toArray()) {
                    const auto deletion = entry.toObject();
                    (void)journal_->acknowledgeCloudDeletion(
                        deletion.value("sessionId").toString(),
                        deletion.value("sessionGeneration").toInteger(),
                        deletion.value("operationId").toString());
                }
                sendHeartbeatBatches(batches, index + 1, requestEpoch, machineId, workspaceId);
            });
}

QByteArray CloudControlClient::eventAssociatedData(const QJsonObject& event,
                                                   const QJsonObject& content) const {
    return CloudCrypto::canonical(
        {QStringLiteral("ketplus"), 2, QStringLiteral("event"), workspaceId_,
         event.value(QStringLiteral("session_id")),
         event.value(QStringLiteral("session_generation")), event.value(QStringLiteral("cursor")),
         event.value(QStringLiteral("turn_sequence")), event.value(QStringLiteral("event_type")),
         content.value(QStringLiteral("keyId")), content.value(QStringLiteral("keyEpoch")),
         content.value(QStringLiteral("senderDeviceId")),
         content.value(QStringLiteral("senderKeyVersion"))});
}

void CloudControlClient::uploadEvents() {
    if (machineId_.isEmpty() || daemonToken_.isEmpty())
        return;
    const QList<StoredSession> sessions = journal_->sessions();
    // Every upload used to re-register every session's cursor, one journal transaction each, on
    // the daemon's only thread. Only a session this process has not registered yet needs one.
    for (const StoredSession& session : sessions) {
        if (!registeredCloudCursors_.contains(
                QStringLiteral("%1:%2").arg(session.id).arg(session.generation)) &&
            !registerCloudCursor(session))
            return;
    }
    for (const StoredSession& session : sessions) {
        if (refusedSessions_.contains(session.id))
            continue;
        const QString account =
            QStringLiteral("uploaded-cursor-%1-%2").arg(session.id).arg(session.generation);
        const qint64 cursor = storedString(account).toLongLong();
        if (cursor < session.lastCursor)
            uploadSessionEvents(session.id, session.generation, cursor);
    }
}

bool CloudControlClient::registerCloudCursors() {
    registeredCloudCursors_.clear();
    for (const StoredSession& session : journal_->sessions()) {
        if (!registerCloudCursor(session))
            return false;
    }
    return true;
}

bool CloudControlClient::registerCloudCursor(const StoredSession& session) {
    const qint64 cursor =
        storedString(
            QStringLiteral("uploaded-cursor-%1-%2").arg(session.id).arg(session.generation))
            .toLongLong();
    if (!journal_->acknowledgeCursor(QStringLiteral("cloud-control-v2"), session.id,
                                     session.generation, cursor)) {
        lastError_ = journal_->errorString();
        return false;
    }
    registeredCloudCursors_.insert(QStringLiteral("%1:%2").arg(session.id).arg(session.generation));
    return true;
}

void CloudControlClient::uploadSessionEvents(const QString& sessionId, const qint64 generation,
                                             const qint64 afterCursor) {
    const QString uploadKey = QStringLiteral("%1:%2").arg(sessionId).arg(generation);
    if (eventUploadsInFlight_.contains(uploadKey))
        return;
    if (const auto backoff = eventUploadBackoff_.constFind(uploadKey);
        backoff != eventUploadBackoff_.cend() && !backoff->retryAt.hasExpired())
        return;
    const int countLimit = eventUploadLimits_.value(uploadKey, maximumEventUploadCount);
    const QJsonArray local = journal_->eventsAfter(sessionId, generation, afterCursor, countLimit);
    if (local.isEmpty())
        return;
    QJsonArray cloud;
    qsizetype cloudBytes = 0;
    // A batch carries as many events as fit the relay's request limit; the rest wait for the
    // next request. Counting alone sent 100 large chunks at once, which the relay refused on
    // every retry.
    // Plaintext item metadata for relay compaction, outside the signed content; an envelope
    // cached before it existed gains it when sent.
    const auto withItemMetadata = [&sessionId, generation](QJsonObject cloudEvent,
                                                           const QJsonObject& event) {
        const QJsonObject metadata = cloudItemMetadata(
            sessionId, generation, event.value(QStringLiteral("event_type")).toString(),
            event.value(QStringLiteral("payload")).toObject());
        for (auto field = metadata.begin(); field != metadata.end(); ++field)
            cloudEvent.insert(field.key(), field.value());
        return cloudEvent;
    };
    const auto join = [&cloud, &cloudBytes, countLimit,
                       &withItemMetadata](const QJsonObject& cached, const QJsonObject& event) {
        const QJsonObject cloudEvent = withItemMetadata(cached, event);
        const qsizetype bytes = QJsonDocument(cloudEvent).toJson(QJsonDocument::Compact).size();
        if (!eventJoinsUpload(cloudBytes, int(cloud.size()), bytes, countLimit))
            return false;
        cloud.append(cloudEvent);
        cloudBytes += bytes + 1;
        return true;
    };
    QString cryptoError;
    for (const QJsonValue& value : local) {
        const QJsonObject event = value.toObject();
        const qint64 cursor = event.value(QStringLiteral("cursor")).toInteger();
        QJsonObject cloudEvent = journal_->cloudEvent(sessionId, generation, cursor);
        if (!cloudEvent.isEmpty() && cloudEvent.value(QStringLiteral("turnSequence")) !=
                                         event.value(QStringLiteral("turn_sequence"))) {
            journal_->removeCloudEventsThrough(sessionId, generation, cursor);
            cloudEvent = {};
        }
        if (!cloudEvent.isEmpty()) {
            const QJsonObject content = cloudEvent.value(QStringLiteral("content")).toObject();
            const QString expectedKeyId = QStringLiteral("%1:%2:1").arg(sessionId).arg(generation);
            const bool metadataMatches =
                journal_->cloudEventProjectionPolicyVersion(sessionId, generation, cursor) ==
                    LocalJournal::currentCloudProjectionPolicyVersion &&
                cloudEvent.value(QStringLiteral("sessionId")).toString() == sessionId &&
                cloudEvent.value(QStringLiteral("sessionGeneration")).toInteger(-1) == generation &&
                cloudEvent.value(QStringLiteral("cursor")).toInteger(-1) == cursor &&
                cloudEvent.value(QStringLiteral("turnSequence")) ==
                    event.value(QStringLiteral("turn_sequence")) &&
                cloudEvent.value(QStringLiteral("eventType")) ==
                    event.value(QStringLiteral("event_type")) &&
                content.value(QStringLiteral("keyId")).toString() == expectedKeyId &&
                content.value(QStringLiteral("keyEpoch")).toInt(-1) == 1 &&
                content.value(QStringLiteral("senderDeviceId")).toString() == machineId_ &&
                content.value(QStringLiteral("senderKeyVersion")).toInt(-1) == machineKeyVersion_;
            QString validationError;
            const QJsonObject publicKeys = crypto_.publicKeys(&validationError);
            const QByteArray signingPublic = CloudCrypto::fromBase64Url(
                publicKeys.value(QStringLiteral("signingPublicKey")).toString());
            const QByteArray plaintext =
                metadataMatches && signingPublic.size() == 32
                    ? crypto_.decrypt(content, eventAssociatedData(event, content), signingPublic,
                                      sessionId, generation, &validationError)
                    : QByteArray{};
            const QByteArray expectedPlaintext =
                QJsonDocument(projectEventPayloadForCloud(
                                  event.value(QStringLiteral("event_type")).toString(),
                                  event.value(QStringLiteral("payload")).toObject()))
                    .toJson(QJsonDocument::Compact);
            if (!metadataMatches || plaintext.isEmpty() || plaintext != expectedPlaintext) {
                // Fail this upload closed, but make the condition recoverable.
                // The local journal remains authoritative, so the next retry
                // will recreate every removed envelope through this cursor
                // using the current projection policy and current key scope.
                if (!journal_->removeCloudEventsThrough(sessionId, generation, cursor)) {
                    lastError_ = journal_->errorString();
                    return;
                }
                lastError_ = QStringLiteral("cloud_event_cache_requires_reconciliation");
                return;
            }
            if (!join(cloudEvent, event))
                break;
            continue;
        }
        cloudEvent = sealEvent(event, sessionId, generation, &cryptoError);
        if (cloudEvent.isEmpty()) {
            lastError_ = cryptoError;
            return;
        }
        if (!journal_->storeCloudEvent(sessionId, generation, cursor, cloudEvent)) {
            lastError_ = journal_->errorString();
            return;
        }
        if (!join(cloudEvent, event))
            break;
    }
    qint64 highestSent = 0;
    for (const QJsonValue& sent : cloud)
        highestSent =
            qMax(highestSent, sent.toObject().value(QStringLiteral("cursor")).toInteger());
    eventUploadsInFlight_.insert(uploadKey);
    request(
        "POST", QStringLiteral("/v2/events:append"),
        QJsonObject{{QStringLiteral("machineId"), machineId_}, {QStringLiteral("events"), cloud}},
        daemonToken_, {},
        [this, sessionId, generation, afterCursor, uploadKey, highestSent,
         sent = int(cloud.size())](const int status, const QJsonValue& value,
                                   const QString& transportError) {
            eventUploadsInFlight_.remove(uploadKey);
            // The batch was too large for the relay, or for a proxy or tunnel in front of it that
            // answers in HTML: send half as many events rather than the same refused batch again.
            if (status == 413 && sent > 1) {
                eventUploadLimits_.insert(uploadKey, sent / 2);
                uploadSessionEvents(sessionId, generation, afterCursor);
                return;
            }
            if (status != 202 || !transportError.isEmpty()) {
                lastError_ = status == 413
                                 ? responseCode(value, QStringLiteral("request_too_large"))
                             : transportError.isEmpty()
                                 ? responseCode(value, QStringLiteral("event_upload_failed"))
                                 : transportError;
                if (status == 401) {
                    // Uploads resume with the next credential, not after a wait.
                    daemonToken_.clear();
                    return;
                }
                EventUploadBackoff& backoff = eventUploadBackoff_[uploadKey];
                backoff.failures += 1;
                backoff.refused = eventUploadRefused(status);
                const qint64 delay =
                    eventUploadBackoffMilliseconds(backoff.failures, backoff.refused);
                backoff.retryAt.setRemainingTime(delay);
                qWarning("event upload for session %s failed: %d %s; next attempt in %llds",
                         qUtf8Printable(sessionId), status, qUtf8Printable(lastError_),
                         delay / 1000);
                return;
            }
            eventUploadBackoff_.remove(uploadKey);
            qint64 accepted = 0;
            for (const QJsonValue& item :
                 object(value).value(QStringLiteral("acceptedThrough")).toArray()) {
                const QJsonObject cursor = item.toObject();
                if (cursor.value(QStringLiteral("sessionId")).toString() == sessionId &&
                    cursor.value(QStringLiteral("sessionGeneration")).toInteger() == generation) {
                    accepted = qMax(accepted, cursor.value(QStringLiteral("cursor")).toInteger());
                }
            }
            // The relay can only accept what this request carried. Never acknowledge or delete
            // local cloud events beyond the highest cursor actually sent.
            if (accepted > highestSent) {
                lastError_ = QStringLiteral("cloud_event_ack_beyond_upload");
                accepted = highestSent;
            }
            if (const int limit = eventUploadLimits_.value(uploadKey, maximumEventUploadCount);
                limit < maximumEventUploadCount) {
                if (limit * 2 >= maximumEventUploadCount)
                    eventUploadLimits_.remove(uploadKey);
                else
                    eventUploadLimits_.insert(uploadKey, limit * 2);
            }
            if (accepted > 0) {
                QString storageError;
                if (!storeString(
                        QStringLiteral("uploaded-cursor-%1-%2").arg(sessionId).arg(generation),
                        QString::number(accepted), &storageError) ||
                    !journal_->acknowledgeCursor(QStringLiteral("cloud-control-v2"), sessionId,
                                                 generation, accepted) ||
                    !journal_->removeCloudEventsThrough(sessionId, generation, accepted)) {
                    lastError_ = storageError.isEmpty() ? journal_->errorString() : storageError;
                    return;
                }
                uploadSessionEvents(sessionId, generation, accepted);
            }
        });
}

void CloudControlClient::distributeKeys() {
    if (machineId_.isEmpty() || daemonToken_.isEmpty())
        return;
    request("GET", QStringLiteral("/v2/devices"), {}, daemonToken_, {},
            [this](const int status, const QJsonValue& value, const QString& transportError) {
                if (status != 200 || !value.isArray() || !transportError.isEmpty()) {
                    if (!transportError.isEmpty())
                        lastError_ = transportError;
                    return;
                }
                signaling_->refreshDevices(value.toArray());
                QJsonArray envelopes;
                QString cryptoError;
                for (const QJsonValue& deviceValue : value.toArray()) {
                    const QJsonObject device = deviceValue.toObject();
                    for (const StoredSession& session : journal_->sessions()) {
                        if (refusedSessions_.contains(session.id))
                            continue;
                        const QString account =
                            QStringLiteral("wrapped-%1-%2-%3-%4")
                                .arg(session.id)
                                .arg(session.generation)
                                .arg(device.value(QStringLiteral("id")).toString())
                                .arg(device.value(QStringLiteral("keyVersion")).toInt());
                        QJsonObject envelope =
                            QJsonDocument::fromJson(credentials_.read(account)).object();
                        // A cached malformed wrap is discarded and re-created, never re-sent.
                        if (!CloudCrypto::validWrappedSessionKey(envelope)) {
                            envelope = crypto_.wrapSessionKey(session.id, session.generation, 1,
                                                              device, machineId_,
                                                              machineKeyVersion_, &cryptoError);
                            if (envelope.isEmpty() ||
                                !credentials_.write(
                                    account, QJsonDocument(envelope).toJson(QJsonDocument::Compact),
                                    &cryptoError)) {
                                lastError_ = cryptoError;
                                return;
                            }
                        }
                        envelopes.append(envelope);
                        if (envelopes.size() == 500)
                            break;
                    }
                    if (envelopes.size() == 500)
                        break;
                }
                if (envelopes.isEmpty())
                    return;
                request("POST", QStringLiteral("/v2/session-keys:append"),
                        QJsonObject{{QStringLiteral("envelopes"), envelopes}}, daemonToken_, {},
                        [this](const int appendStatus, const QJsonValue& appendValue,
                               const QString& appendError) {
                            if (appendStatus != 202 || !appendError.isEmpty()) {
                                lastError_ =
                                    appendError.isEmpty()
                                        ? responseCode(appendValue,
                                                       QStringLiteral("session_key_upload_failed"))
                                        : appendError;
                            }
                        });
            });
}

void CloudControlClient::pollCommands() {
    if (machineId_.isEmpty() || commandPollInFlight_)
        return;
    commandPollInFlight_ = true;
    ensureDaemonCredential([this](const bool ready, const QString& error) {
        if (!ready) {
            commandPollInFlight_ = false;
            lastError_ = error;
            return;
        }
        request(
            "GET", QStringLiteral("/v2/commands:poll?limit=20"), {}, daemonToken_,
            {{QByteArray("X-KetPlus-Connection-Id"), QByteArray("daemon-") + machineId_.toUtf8()}},
            [this](const int status, const QJsonValue& value, const QString& transportError) {
                commandPollInFlight_ = false;
                if (std::exchange(commandPollAgain_, false))
                    QTimer::singleShot(0, this, &CloudControlClient::pollCommands);
                if (status != 200 || !transportError.isEmpty()) {
                    lastError_ = transportError.isEmpty()
                                     ? responseCode(value, QStringLiteral("command_poll_failed"))
                                     : transportError;
                    // The relay leased this poll's commands; they return once the lease lapses.
                    if (status == 200)
                        qWarning("command poll lost its leased commands: %s",
                                 qUtf8Printable(transportError));
                    if (status == 401)
                        daemonToken_.clear();
                    return;
                }
                for (const QJsonValue& command :
                     object(value).value(QStringLiteral("commands")).toArray()) {
                    executeCommand(command.toObject());
                }
            });
    });
}

void CloudControlClient::wakeCommandPoll() {
    if (commandPollInFlight_)
        commandPollAgain_ = true;
    else
        pollCommands();
}

QByteArray CloudControlClient::commandAssociatedData(const QJsonObject& command) const {
    const QJsonObject content = command.value(QStringLiteral("content")).toObject();
    QJsonArray values{QStringLiteral("ketplus"),
                      2,
                      QStringLiteral("command"),
                      command.value(QStringLiteral("id")),
                      command.value(QStringLiteral("workspaceId")),
                      command.value(QStringLiteral("machineId")),
                      command.value(QStringLiteral("sessionId")),
                      command.value(QStringLiteral("sessionGeneration")),
                      command.value(QStringLiteral("idempotencyKey")),
                      command.value(QStringLiteral("capability")),
                      command.value(QStringLiteral("kind")),
                      content.value(QStringLiteral("keyId")),
                      content.value(QStringLiteral("keyEpoch")),
                      content.value(QStringLiteral("senderDeviceId")),
                      content.value(QStringLiteral("senderKeyVersion"))};
    if (command.value(QStringLiteral("kind")) == QStringLiteral("transcript.restore_window")) {
        values.append(command.value(QStringLiteral("credentialEpoch")));
        values.append(command.value(QStringLiteral("ownerSubjectId")));
        values.append(command.value(QStringLiteral("snapshotCursor")));
        values.append(command.value(QStringLiteral("requestedByteCap")));
        values.append(command.value(QStringLiteral("continuationOfRequestId")).toString());
        values.append(command.value(QStringLiteral("continuationSequence")).toInteger(0));
        values.append(command.value(QStringLiteral("continuationDigest")).toString());
        values.append(command.value(QStringLiteral("deadlineAt")));
    }
    if (command.value(QStringLiteral("kind")) == QStringLiteral("session.worktree.switch")) {
        values.append(command.value(QStringLiteral("repositoryId")));
        values.append(command.value(QStringLiteral("worktreeId")));
    }
    if (command.value(QStringLiteral("kind"))
            .toString()
            .startsWith(QStringLiteral("workspace.files."))) {
        values.append(command.value(QStringLiteral("repositoryId")));
        values.append(command.value(QStringLiteral("worktreeId")));
        values.append(command.value(QStringLiteral("workspaceRevision")));
    }
    if (isWorktreeCommand(command)) {
        values.append(command.value(QStringLiteral("ownerSubjectId")));
        values.append(command.value(QStringLiteral("repositoryId")));
        values.append(command.value(QStringLiteral("worktreeId")));
        values.append(command.value(QStringLiteral("expectedCatalogRevision")));
        values.append(command.value(QStringLiteral("expectedLifecycleRevision")));
        values.append(content.value(QStringLiteral("recipientDeviceId")));
        values.append(content.value(QStringLiteral("recipientKeyVersion")));
        values.append(content.value(QStringLiteral("ephemeralPublicKey")));
    }
    if (command.value(QStringLiteral("kind")) == QStringLiteral("session.delete")) {
        values.append(command.value(QStringLiteral("ownerSubjectId")));
        values.append(command.value(QStringLiteral("expectedLifecycleRevision")));
        values.append(command.value(QStringLiteral("deletionChallengeId")));
    }
    return CloudCrypto::canonical(values);
}

QByteArray CloudControlClient::restoreResponseAssociatedData(const QJsonObject& command,
                                                             const qint64 responseCursor,
                                                             const bool terminal,
                                                             const QString& responseDigest,
                                                             const QJsonObject& content) const {
    return CloudCrypto::canonical(
        {QStringLiteral("ketplus"), 2, QStringLiteral("transcript-restore-response"),
         command.value(QStringLiteral("id")), command.value(QStringLiteral("ownerSubjectId")),
         command.value(QStringLiteral("workspaceId")), command.value(QStringLiteral("machineId")),
         command.value(QStringLiteral("sessionId")),
         command.value(QStringLiteral("sessionGeneration")),
         command.value(QStringLiteral("credentialEpoch")),
         command.value(QStringLiteral("snapshotCursor")), responseCursor, terminal ? 1 : 0,
         responseDigest, content.value(QStringLiteral("keyId")),
         content.value(QStringLiteral("keyEpoch")), content.value(QStringLiteral("senderDeviceId")),
         content.value(QStringLiteral("senderKeyVersion"))});
}

QByteArray CloudControlClient::commandResultAssociatedData(const QJsonObject& command,
                                                           const int attempt, const QString& status,
                                                           const QString& resultCode,
                                                           const QJsonObject& content) const {
    QJsonArray values{QStringLiteral("ketplus"),
                      2,
                      QStringLiteral("command-result"),
                      command.value(QStringLiteral("id")),
                      command.value(QStringLiteral("workspaceId")),
                      command.value(QStringLiteral("machineId")),
                      command.value(QStringLiteral("sessionId")),
                      command.value(QStringLiteral("sessionGeneration")),
                      attempt,
                      status,
                      resultCode,
                      content.value(QStringLiteral("keyId")),
                      content.value(QStringLiteral("keyEpoch")),
                      content.value(QStringLiteral("senderDeviceId")),
                      content.value(QStringLiteral("senderKeyVersion"))};
    if (command.value(QStringLiteral("kind")) == QStringLiteral("session.worktree.switch")) {
        values.append(command.value(QStringLiteral("repositoryId")));
        values.append(command.value(QStringLiteral("worktreeId")));
    }
    if (command.value(QStringLiteral("kind"))
            .toString()
            .startsWith(QStringLiteral("workspace.files."))) {
        values.append(command.value(QStringLiteral("repositoryId")));
        values.append(command.value(QStringLiteral("worktreeId")));
        values.append(command.value(QStringLiteral("workspaceRevision")));
    }
    return CloudCrypto::canonical(values);
}

void CloudControlClient::executeWorktreeCommand(const QJsonObject& command,
                                                const QJsonObject& payload,
                                                const QByteArray& senderAgreementKey) {
    const QJsonObject content = command.value(QStringLiteral("content")).toObject();
    const QString repositoryId = command.value(QStringLiteral("repositoryId")).toString();
    const QString worktreeId = command.value(QStringLiteral("worktreeId")).toString();
    const QByteArray salt = ensurePathIdSalt();
    QString worktreePath;
    for (const QString& candidate : catalogWorktreePaths()) {
        const QJsonObject scope = sourceContext(candidate, salt);
        if (scope.value(QStringLiteral("repositoryId")).toString() == repositoryId &&
            scope.value(QStringLiteral("worktreeId")).toString() == worktreeId) {
            worktreePath = candidate;
            break;
        }
    }
    const bool listing =
        command.value(QStringLiteral("kind")) == QStringLiteral("worktree.branches.read");
    const bool configuring =
        command.value(QStringLiteral("kind")) == QStringLiteral("worktree.configuration.read");
    const QJsonObject newWorktree = payload.value(QStringLiteral("new_worktree")).toObject();
    const QString branch = newWorktree.value(QStringLiteral("branch")).toString().trimmed();
    const QString base = newWorktree.value(QStringLiteral("base")).toString().trimmed();
    if (command.value(QStringLiteral("workspaceId")).toString() != workspaceId_ ||
        !command.value(QStringLiteral("sessionId")).toString().isEmpty() ||
        command.value(QStringLiteral("sessionGeneration")).toInteger() != 0 ||
        content.value(QStringLiteral("recipientDeviceId")).toString() != machineId_ ||
        content.value(QStringLiteral("recipientKeyVersion")).toInt() != machineKeyVersion_ ||
        command.value(QStringLiteral("expectedCatalogRevision")).toInteger(-1) != 0 ||
        command.value(QStringLiteral("expectedLifecycleRevision")).toInteger(-1) != 0 ||
        worktreePath.isEmpty() || !QFileInfo(worktreePath).isDir() ||
        (!listing && !configuring &&
         payload.value(QStringLiteral("prompt")).toString().trimmed().isEmpty()) ||
        (!newWorktree.isEmpty() && (branch.isEmpty() || base.isEmpty()))) {
        acknowledgeCommand(
            command,
            {{QStringLiteral("ok"), false},
             {QStringLiteral("error_code"), QStringLiteral("worktree_scope_changed")},
             {QStringLiteral("error_message"),
              QStringLiteral("The enrolled worktree scope changed.")}},
            senderAgreementKey);
        return;
    }
    if (configuring) {
        // What the phone can choose before the first message: the models, efforts and
        // permissions a conversation started here would begin with.
        host_->handleProviderQuery(
            QJsonObject{
                {QStringLiteral("message_type"), QStringLiteral("provider.configuration_catalog")},
                {QStringLiteral("payload"), QJsonObject{}}},
            [this, command, senderAgreementKey](const QJsonObject& models,
                                                const QString& providerError) {
                acknowledgeCommand(
                    command,
                    providerError.isEmpty()
                        ? host_->newConversationConfiguration(
                              models.value(QStringLiteral("catalog")).toArray())
                        : QJsonObject{{QStringLiteral("ok"), false},
                                      {QStringLiteral("error_code"),
                                       QStringLiteral("provider_capabilities_unavailable")},
                                      {QStringLiteral("error_message"),
                                       QStringLiteral(
                                           "The available settings could not be read.")}},
                    senderAgreementKey);
            });
        return;
    }
    if (!listing && newWorktree.isEmpty()) {
        startCreatedConversation(command, payload, worktreePath, senderAgreementKey);
        return;
    }
    // Git can take seconds on a large repository, so it runs off the main thread; the
    // answer is dropped if the Mac left the account meanwhile.
    const QString repository = worktreeRepository(worktreePath);
    const quint64 requestEpoch = accountEpoch_;
    const QPointer<CloudControlClient> guard(this);
    workspaceFilesPool_.start([guard, command, payload, senderAgreementKey, repository, listing,
                               branch, base, salt, requestEpoch]() {
        QJsonObject result;
        QString createdPath;
        if (listing) {
            const RepositoryBranches branches = readRepositoryBranches(repository);
            if (!branches.ok) {
                result = {{QStringLiteral("ok"), false},
                          {QStringLiteral("error_code"), QStringLiteral("branches_unavailable")},
                          {QStringLiteral("error_message"),
                           QStringLiteral("Could not read the repository's branches.")}};
            } else {
                // Each checkout's branch, which the relay's catalog does not carry.
                QJsonArray worktrees;
                for (const QString& checkout : repositoryCheckouts(repository)) {
                    const QJsonObject scope = sourceContext(checkout, salt);
                    worktrees.append(QJsonObject{
                        {QStringLiteral("worktree_id"), scope.value(QStringLiteral("worktreeId"))},
                        {QStringLiteral("branch"), checkoutBranch(checkout)},
                        {QStringLiteral("folder"), QFileInfo(checkout).fileName()},
                        {QStringLiteral("primary"), checkout == repository}});
                }
                qsizetype budget = 256 * 1024;
                bool truncated = false;
                const QJsonArray local = boundedNames(branches.local, &budget, &truncated);
                const QJsonArray remote = boundedNames(branches.remote, &budget, &truncated);
                result = {{QStringLiteral("ok"), true},
                          {QStringLiteral("local"), local},
                          {QStringLiteral("remote"), remote},
                          {QStringLiteral("truncated"), truncated},
                          {QStringLiteral("worktrees"), worktrees}};
            }
        } else {
            const AddedWorktree added = addWorktree(repository, branch, base);
            createdPath = added.path;
            if (!added.errorCode.isEmpty())
                result = {{QStringLiteral("ok"), false},
                          {QStringLiteral("error_code"), added.errorCode},
                          {QStringLiteral("error_message"), added.message}};
        }
        QMetaObject::invokeMethod(
            guard,
            [guard, command, payload, senderAgreementKey, result, createdPath, requestEpoch]() {
                if (!guard || guard->accountEpoch_ != requestEpoch)
                    return;
                if (!createdPath.isEmpty())
                    guard->startCreatedConversation(command, payload, createdPath,
                                                    senderAgreementKey);
                else
                    guard->acknowledgeCommand(command, result, senderAgreementKey);
            },
            Qt::QueuedConnection);
    });
}

void CloudControlClient::startCreatedConversation(const QJsonObject& command,
                                                  const QJsonObject& payload,
                                                  const QString& worktreePath,
                                                  const QByteArray& senderAgreementKey) {
    const QJsonObject local{
        {QStringLiteral("protocol_version"), 0},
        {QStringLiteral("message_id"), command.value(QStringLiteral("id"))},
        {QStringLiteral("message_type"), QStringLiteral("session.start_turn")},
        {QStringLiteral("sent_at"), protocol::timestamp()},
        {QStringLiteral("payload"),
         QJsonObject{
             {QStringLiteral("prompt"),
              payload.value(QStringLiteral("prompt")).toString().trimmed()},
             {QStringLiteral("worktree_path"), worktreePath},
             {QStringLiteral("options"), payload.value(QStringLiteral("options")).toObject()}}}};
    acknowledgeCommand(command, host_->handleCommand(local, QStringLiteral("cloud-control-v2")),
                       senderAgreementKey);
}

void CloudControlClient::withSenderKeys(const QString& senderId, const int version,
                                        SenderKeysCallback callback) {
    const QString cacheKey = QStringLiteral("%1/%2").arg(senderId).arg(version);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (const auto cached = senderKeys_.constFind(cacheKey);
        cached != senderKeys_.cend() && cached->second > now) {
        callback(cached->first, {});
        return;
    }
    const QString keyPath = QStringLiteral("/v2/device-keys/%1/%2")
                                .arg(QString::fromUtf8(QUrl::toPercentEncoding(senderId)))
                                .arg(version);
    request("GET", keyPath, {}, daemonToken_, {},
            [this, cacheKey, callback = std::move(callback)](
                const int status, const QJsonValue& value, const QString& transportError) {
                if (status != 200 || !transportError.isEmpty()) {
                    callback({}, transportError.isEmpty()
                                     ? QStringLiteral("The sender key is unavailable.")
                                     : transportError);
                    return;
                }
                const QJsonObject keys = object(value).value(QStringLiteral("keys")).toObject();
                if (senderKeys_.size() >= maximumCachedSenderKeys)
                    senderKeys_.clear();
                senderKeys_.insert(cacheKey, {keys, QDateTime::currentDateTimeUtc().addMSecs(
                                                        senderKeyCacheMilliseconds)});
                callback(keys, {});
            });
}

void CloudControlClient::executeCommand(const QJsonObject& command) {
    if (command.value(QStringLiteral("machineId")).toString() != machineId_)
        return;
    const QJsonObject content = command.value(QStringLiteral("content")).toObject();
    const QString senderId = content.value(QStringLiteral("senderDeviceId")).toString();
    const int senderVersion = content.value(QStringLiteral("senderKeyVersion")).toInt();
    const QString commandId = command.value(QStringLiteral("id")).toString();
    if (commandStartedAt_.size() >= 1024)
        commandStartedAt_.clear();
    commandStartedAt_.insert(commandId, QDateTime::currentMSecsSinceEpoch());
    qInfo("command %s %s claimed attempt=%d", qUtf8Printable(commandId),
          qUtf8Printable(command.value(QStringLiteral("kind")).toString()),
          command.value(QStringLiteral("attempt")).toInt());
    // Every command from a phone names the same few device keys; a relay read per command
    // was a large share of the relay's traffic.
    withSenderKeys(
        senderId, senderVersion,
        [this, command, senderId, senderVersion](const QJsonObject& senderKeys,
                                                 const QString& keyError) {
            const auto peerId = command.value("_peerConnection").toString();
            if (!peerId.isEmpty() && !peers_->authorized(peerId, command))
                return;
            QJsonObject localResult;
            if (!keyError.isEmpty()) {
                localResult = {
                    {QStringLiteral("ok"), false},
                    {QStringLiteral("error_code"), QStringLiteral("sender_key_unavailable")},
                    {QStringLiteral("error_message"), keyError}};
                acknowledgeCommand(command, localResult);
                return;
            }
            const QByteArray senderKey = CloudCrypto::fromBase64Url(
                senderKeys.value(QStringLiteral("signingPublicKey")).toString());
            const QByteArray senderAgreementKey = CloudCrypto::fromBase64Url(
                senderKeys.value(QStringLiteral("agreementPublicKey")).toString());
            QString cryptoError;
            const QString sessionId = command.value(QStringLiteral("sessionId")).toString();
            const qint64 generation =
                command.value(QStringLiteral("sessionGeneration")).toInteger();
            // A conversation's first command, and the branch list read to start one, come
            // before any session key exists, so they are sealed to this Mac.
            const bool creating = isWorktreeCommand(command);
            const QJsonObject content = command.value(QStringLiteral("content")).toObject();
            const QByteArray plaintext =
                creating ? crypto_.decryptSealed(content, commandAssociatedData(command), senderKey,
                                                 command.value(QStringLiteral("id")).toString(),
                                                 &cryptoError)
                         : crypto_.decrypt(content, commandAssociatedData(command), senderKey,
                                           sessionId, generation, &cryptoError);
            const QJsonDocument decoded = QJsonDocument::fromJson(plaintext);
            if (plaintext.isEmpty() || !decoded.isObject()) {
                // A cached key that no longer opens the sender's commands is read again next time.
                senderKeys_.remove(QStringLiteral("%1/%2").arg(senderId).arg(senderVersion));
                localResult = {
                    {QStringLiteral("ok"), false},
                    {QStringLiteral("error_code"), QStringLiteral("invalid_encrypted_command")},
                    {QStringLiteral("error_message"),
                     cryptoError.isEmpty() ? QStringLiteral("The command body is invalid.")
                                           : cryptoError}};
            } else {
                if (creating) {
                    executeWorktreeCommand(command, decoded.object(), senderAgreementKey);
                    return;
                }
                if (command.value(QStringLiteral("kind")) ==
                    QStringLiteral("transcript.restore_window")) {
                    executeRestoreWindow(command, decoded.object());
                    return;
                }
                const QString kind = command.value(QStringLiteral("kind")).toString();
                if (kind == QStringLiteral("session.delete")) {
                    const QJsonObject payload = decoded.object();
                    if (command.value(QStringLiteral("ownerSubjectId")).toString().isEmpty() ||
                        command.value(QStringLiteral("workspaceId")).toString() != workspaceId_ ||
                        command.value(QStringLiteral("machineId")).toString() != machineId_ ||
                        command.value(QStringLiteral("expectedLifecycleRevision")).toInteger(-1) <
                            0 ||
                        command.value(QStringLiteral("deletionChallengeId")).toString().isEmpty() ||
                        payload.value(QStringLiteral("confirmation")).toString() !=
                            QStringLiteral("DELETE")) {
                        localResult = {{QStringLiteral("ok"), false},
                                       {QStringLiteral("error_code"),
                                        QStringLiteral("delete_scope_forbidden")}};
                        acknowledgeCommand(command, localResult);
                        return;
                    }
                    QJsonObject localPayload = payload;
                    localPayload.insert(QStringLiteral("owner_subject_id"),
                                        command.value(QStringLiteral("ownerSubjectId")));
                    localPayload.insert(QStringLiteral("delete_operation_id"),
                                        command.value(QStringLiteral("idempotencyKey")));
                    localPayload.insert(QStringLiteral("expected_lifecycle_revision"),
                                        command.value(QStringLiteral("expectedLifecycleRevision")));
                    const QJsonObject local{
                        {QStringLiteral("protocol_version"), 0},
                        {QStringLiteral("message_id"), command.value(QStringLiteral("id"))},
                        {QStringLiteral("message_type"), QStringLiteral("session.delete")},
                        {QStringLiteral("session_id"), sessionId},
                        {QStringLiteral("session_generation"), generation},
                        {QStringLiteral("sent_at"), protocol::timestamp()},
                        {QStringLiteral("payload"), localPayload}};
                    localResult = host_->handleCommand(local, QStringLiteral("cloud-control-v2"));
                    acknowledgeCommand(command, localResult);
                    return;
                }
                if (kind.startsWith(QStringLiteral("workspace.files."))) {
                    executeWorkspaceFiles(command, decoded.object());
                    return;
                }
                if ((kind == QStringLiteral("session.configuration.read") &&
                     !decoded.object().value("effective_only").toBool()) ||
                    kind == QStringLiteral("session.configuration.apply")) {
                    host_->handleProviderQuery(
                        QJsonObject{{QStringLiteral("message_type"),
                                     QStringLiteral("provider.configuration_catalog")},
                                    {QStringLiteral("session_id"), sessionId},
                                    {QStringLiteral("payload"), QJsonObject{}}},
                        [this, command, decoded, sessionId,
                         generation](const QJsonObject& models, const QString& providerError) {
                            if (!providerError.isEmpty()) {
                                acknowledgeCommand(
                                    command, {{QStringLiteral("ok"), false},
                                              {QStringLiteral("error_code"),
                                               QStringLiteral("provider_capabilities_unavailable")},
                                              {QStringLiteral("error_message"),
                                               QStringLiteral(
                                                   "The available settings could not be read.")}});
                                return;
                            }
                            QJsonObject payload = decoded.object();
                            payload.insert(QStringLiteral("provider_catalog"),
                                           models.value(QStringLiteral("catalog")).toArray());
                            const QJsonObject local{
                                {QStringLiteral("protocol_version"), 0},
                                {QStringLiteral("message_id"), command.value(QStringLiteral("id"))},
                                {QStringLiteral("message_type"),
                                 command.value(QStringLiteral("kind"))},
                                {QStringLiteral("session_id"), sessionId},
                                {QStringLiteral("session_generation"), generation},
                                {QStringLiteral("sent_at"), protocol::timestamp()},
                                {QStringLiteral("payload"), payload}};
                            acknowledgeCommand(
                                command,
                                host_->handleCommand(local, QStringLiteral("cloud-control-v2")));
                        });
                    return;
                }
                if (kind == "atlas.list" || kind == "atlas.start" || kind == "atlas.detach") {
                    const auto payload = decoded.object();
                    const auto sessions = journal_->sessions();
                    const auto found =
                        std::find_if(sessions.cbegin(), sessions.cend(), [&](const auto& s) {
                            return s.id == sessionId && s.generation == generation;
                        });
                    const auto device =
                        command.value("content").toObject().value("senderDeviceId").toString();
                    if (found == sessions.cend() || command.value("workspaceId") != workspaceId_ ||
                        device.isEmpty()) {
                        acknowledgeCommand(
                            command, {{"ok", false}, {"error_code", "atlas_scope_forbidden"}});
                        return;
                    }
                    if (kind == "atlas.detach") {
                        for (auto* runtime : findChildren<AtlasLiveRuntime*>()) {
                            if (runtime->channel() == payload.value("channel").toString() &&
                                runtime->property("device").toString() == device &&
                                runtime->property("session").toString() == sessionId &&
                                runtime->property("generation").toLongLong() == generation) {
                                runtime->stop();
                                runtime->deleteLater();
                            }
                        }
                        acknowledgeCommand(command, {{"ok", true}});
                        return;
                    }
                    for (auto* runtime : findChildren<AtlasLiveRuntime*>()) {
                        if (runtime->property("command").toString() ==
                            command.value("id").toString()) {
                            const auto saved = runtime->property("attachment").toJsonObject();
                            if (!saved.isEmpty())
                                acknowledgeCommand(command, saved);
                            return;
                        }
                    }
                    if (kind == "atlas.start" && payload.contains("attach_channel")) {
                        for (auto* runtime : findChildren<AtlasLiveRuntime*>()) {
                            if (!runtime->reusable() ||
                                runtime->channel() != payload.value("attach_channel").toString() ||
                                runtime->property("device").toString() != device ||
                                runtime->property("session").toString() != sessionId ||
                                runtime->property("generation").toLongLong() != generation)
                                continue;
                            auto relay =
                                controlEndpoint(baseUrl_, QStringLiteral("/v2/atlas/tunnel"));
                            relay.setScheme(relay.scheme() == "http" ? "ws" : "wss");
                            QUrlQuery query;
                            query.addQueryItem("role", "daemon");
                            query.addQueryItem("deviceId", device);
                            query.addQueryItem("sessionId", sessionId);
                            query.addQueryItem("generation", QString::number(generation));
                            relay.setQuery(query);
                            runtime->setProperty("command", command.value("id").toString());
                            runtime->attach(relay, daemonToken_,
                                            [this, runtime, command](const QJsonObject& result) {
                                                runtime->setProperty("attachment", result);
                                                acknowledgeCommand(command, result);
                                            });
                            return;
                        }
                        acknowledgeCommand(
                            command, {{"ok", false}, {"error_code", "atlas_attachment_expired"}});
                        return;
                    }
                    if (kind == "atlas.start" &&
                        payload.value("expected_context_revision").toInteger(-1) !=
                            found->contextRevision) {
                        acknowledgeCommand(
                            command, {{"ok", false}, {"error_code", "context_revision_conflict"}});
                        return;
                    }
                    if (findChildren<AtlasLiveRuntime*>().size() >= 8 ||
                        workspaceFilesPending_.fetch_add(1) >= 8) {
                        // The counter is acquired only when the runtime limit did not
                        // short-circuit.
                        if (findChildren<AtlasLiveRuntime*>().size() < 8)
                            workspaceFilesPending_.fetch_sub(1);
                        acknowledgeCommand(command, {{"ok", false}, {"error_code", "busy"}});
                        return;
                    }
                    const auto source = *found;
                    const auto accountEpoch = accountEpoch_;
                    const QPointer<CloudControlClient> guard(this);
                    workspaceFilesPool_.start([guard, command, payload, kind, source, device,
                                               accountEpoch]() {
                        const auto projects = AtlasIndex::scan(source.worktreePath);
                        if (!guard)
                            return;
                        QMetaObject::invokeMethod(
                            guard,
                            [guard, command, payload, kind, source, device, projects,
                             accountEpoch] {
                                if (!guard)
                                    return;
                                guard->workspaceFilesPending_.fetch_sub(1);
                                if (guard->accountEpoch_ != accountEpoch)
                                    return;
                                for (auto* runtime : guard->findChildren<AtlasLiveRuntime*>()) {
                                    if (runtime->property("command").toString() !=
                                        command.value("id").toString())
                                        continue;
                                    const auto saved =
                                        runtime->property("attachment").toJsonObject();
                                    if (!saved.isEmpty())
                                        guard->acknowledgeCommand(command, saved);
                                    return;
                                }
                                if (guard->findChildren<AtlasLiveRuntime*>().size() >= 8) {
                                    guard->acknowledgeCommand(
                                        command, {{"ok", false}, {"error_code", "busy"}});
                                    return;
                                }

                                const auto current = guard->journal_->sessions();
                                if (std::none_of(
                                        current.cbegin(), current.cend(), [&](const auto& s) {
                                            return s.id == source.id &&
                                                   s.generation == source.generation &&
                                                   s.contextRevision == source.contextRevision;
                                        })) {
                                    guard->acknowledgeCommand(
                                        command, {{"ok", false},
                                                  {"error_code", "context_revision_conflict"}});
                                    return;
                                }
                                QJsonArray entries;
                                std::optional<AtlasDescriptor> selected;
                                for (const auto& atlas : projects) {
                                    const auto canonical =
                                        QFileInfo(atlas.path).canonicalFilePath();
                                    const auto root =
                                        QFileInfo(source.worktreePath).canonicalFilePath();
                                    if (root.isEmpty() || !canonical.startsWith(root + '/'))
                                        continue;
                                    const auto id =
                                        opaquePathId("atlas", canonical, guard->ensurePathIdSalt());
                                    if (id == payload.value("project_id").toString())
                                        selected = atlas;
                                    if (entries.size() < 200)
                                        entries.append(QJsonObject{{"id", id},
                                                                   {"title", atlas.title},
                                                                   {"path", atlas.relativePath}});
                                }
                                if (kind == "atlas.list") {
                                    guard->acknowledgeCommand(
                                        command, {{"ok", true},
                                                  {"projects", entries},
                                                  {"context_revision", source.contextRevision}});
                                    return;
                                }
                                if (!selected) {
                                    guard->acknowledgeCommand(
                                        command, {{"ok", false},
                                                  {"error_code", "atlas_project_unavailable"}});
                                    return;
                                }
                                AtlasLiveRuntime* runtime = nullptr;
                                for (auto* candidate : guard->findChildren<AtlasLiveRuntime*>()) {
                                    if (candidate->reusable() &&
                                        candidate->property("device").toString() == device &&
                                        candidate->property("session").toString() == source.id &&
                                        candidate->property("generation").toLongLong() ==
                                            source.generation &&
                                        candidate->property("project").toString() ==
                                            selected->path) {
                                        runtime = candidate;
                                        break;
                                    }
                                }
                                const bool reuse = runtime != nullptr;
                                if (!runtime)
                                    runtime = new AtlasLiveRuntime(guard);
                                const auto atlasSource =
                                    sourceContext(source.worktreePath, guard->ensurePathIdSalt());
                                runtime->setProperty("repositoryId",
                                                     atlasSource.value("repositoryId").toString());
                                runtime->setProperty("worktreeId",
                                                     atlasSource.value("worktreeId").toString());
                                runtime->setProperty("project", selected->path);
                                runtime->setProperty("device", device);
                                runtime->setProperty("session", source.id);
                                runtime->setProperty("generation", source.generation);
                                runtime->setProperty("command", command.value("id").toString());
                                auto relay = controlEndpoint(guard->baseUrl_,
                                                             QStringLiteral("/v2/atlas/tunnel"));
                                relay.setScheme(relay.scheme() == "http" ? "ws" : "wss");
                                QUrlQuery query;
                                query.addQueryItem("role", "daemon");
                                query.addQueryItem("deviceId", device);
                                query.addQueryItem("sessionId", source.id);
                                query.addQueryItem("generation",
                                                   QString::number(source.generation));
                                relay.setQuery(query);
                                auto completed = [guard, runtime,
                                                  command](const QJsonObject& result) {
                                    if (!guard)
                                        return;
                                    runtime->setProperty("attachment", result);
                                    guard->acknowledgeCommand(command, result);
                                };
                                if (reuse)
                                    runtime->attach(relay, guard->daemonToken_, completed);
                                else
                                    runtime->start(*selected, relay, guard->daemonToken_,
                                                   source.contextRevision, completed);
                            },
                            Qt::QueuedConnection);
                    });
                    return;
                }
                if (kind == QStringLiteral("session.resource.resolve")) {
                    auto result = host_->handleCommand({{"protocol_version", 0},
                                                        {"message_id", command.value("id")},
                                                        {"message_type", kind},
                                                        {"session_id", sessionId},
                                                        {"session_generation", generation},
                                                        {"sent_at", protocol::timestamp()},
                                                        {"payload", decoded.object()}},
                                                       "cloud-control-v2");
                    const auto root = result.take("worktree_path").toString();
                    if (!result.value("ok").toBool()) {
                        acknowledgeCommand(command, result);
                        return;
                    }
                    result.insert("source_path", decoded.object().value("path"));
                    const auto source = sourceContext(root, ensurePathIdSalt());
                    result.insert("repository_id", source.value("repositoryId"));
                    result.insert("worktree_id", source.value("worktreeId"));
                    result.insert("branch", source.value("branch"));
                    result.insert("repository_label", source.value("repositoryLabel"));
                    const WorkspaceFileScope scope{command.value("ownerSubjectId").toString(),
                                                   workspaceId_,
                                                   machineId_,
                                                   source.value("repositoryId").toString(),
                                                   source.value("worktreeId").toString(),
                                                   root};
                    ReadOnlyWorkspaceFiles::revisionAsync(
                        scope, this,
                        [this, command, result, sessionId,
                         generation](const QString& revision, const QString& error) mutable {
                            const auto sessions = journal_->sessions();
                            const bool valid =
                                std::any_of(sessions.cbegin(), sessions.cend(), [&](const auto& s) {
                                    return s.id == sessionId && s.generation == generation;
                                });
                            if (!valid || revision.isEmpty()) {
                                acknowledgeCommand(
                                    command, {{"ok", false},
                                              {"error_code",
                                               valid ? error : "session_generation_mismatch"}});
                                return;
                            }
                            result.insert("workspace_revision", revision);
                            acknowledgeCommand(command, result);
                        });
                    return;
                }
                if (kind == QStringLiteral("session.worktrees")) {
                    // Repository browsing by path is a trusted local IPC feature only.
                    auto listPayload = decoded.object();
                    listPayload.remove("repository_path");
                    auto result = host_->handleCommand({{"protocol_version", 0},
                                                        {"message_id", command.value("id")},
                                                        {"message_type", kind},
                                                        {"session_id", sessionId},
                                                        {"session_generation", generation},
                                                        {"sent_at", protocol::timestamp()},
                                                        {"payload", listPayload}},
                                                       "cloud-control-v2");
                    const auto current =
                        sourceContext(result.value("worktree_path").toString(), ensurePathIdSalt());
                    result.insert("current_worktree_id", current.value("worktreeId"));
                    QJsonArray entries;
                    for (const auto& v : result.value("worktrees").toArray()) {
                        auto entry = v.toObject();
                        const auto path = entry.take("worktree_path").toString();
                        const auto scope = sourceContext(path, ensurePathIdSalt());
                        entry.insert("worktree_id", scope.value("worktreeId"));
                        entry.insert("repository_id", scope.value("repositoryId"));
                        entry.insert("folder", QFileInfo(path).fileName());
                        entries.append(entry);
                    }
                    result.insert("worktrees", entries);
                    result.remove("contexts");
                    result.remove("repositories");
                    result.remove("worktree_path");
                    if (!result.value("ok").toBool()) {
                        acknowledgeCommand(command, result);
                        return;
                    }
                    // Publish discovered opaque targets before returning the picker page, so
                    // a newly created Git worktree already has a catalog/grant at selection.
                    const auto epoch = accountEpoch_;
                    const QJsonObject heartbeatBody{
                        {"machineId", machineId_},
                        {"controlProtocolVersion", 2},
                        {"localProtocolVersion", 0},
                        {"daemonVersion", QStringLiteral(KETPLUS_PRODUCT_VERSION)},
                        {"observedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                        {"sessions", QJsonArray{}},
                        {"worktrees", worktreeSnapshots()}};
                    request("POST", "/v2/daemon/heartbeat", heartbeatBody, daemonToken_, {},
                            [this, command, result, epoch](int status, const QJsonValue&,
                                                           const QString& error) {
                                if (epoch != accountEpoch_)
                                    return;
                                acknowledgeCommand(
                                    command, status == 202 && error.isEmpty()
                                                 ? result
                                                 : QJsonObject{{"ok", false},
                                                               {"error_code",
                                                                "worktree_catalog_unavailable"}});
                            });
                    return;
                }
                if (kind == QStringLiteral("session.worktree.switch")) {
                    QString target;
                    for (const QString& candidate : journal_->worktreePaths()) {
                        const auto scope = sourceContext(candidate, ensurePathIdSalt());
                        if (scope.value("repositoryId") == command.value("repositoryId") &&
                            scope.value("worktreeId") == command.value("worktreeId")) {
                            target = QFileInfo(candidate).canonicalFilePath();
                            break;
                        }
                    }
                    if (target.isEmpty() ||
                        command.value("workspaceId").toString() != workspaceId_) {
                        acknowledgeCommand(command,
                                           {{"ok", false}, {"error_code", "scope_forbidden"}});
                        return;
                    }
                    QJsonObject payload = decoded.object();
                    payload.insert("worktree_path", target);
                    auto result = host_->handleCommand({{"protocol_version", 0},
                                                        {"message_id", command.value("id")},
                                                        {"message_type", kind},
                                                        {"session_id", sessionId},
                                                        {"session_generation", generation},
                                                        {"sent_at", protocol::timestamp()},
                                                        {"payload", payload}},
                                                       "cloud-control-v2");
                    result.remove("worktree_path");
                    result.insert("repository_id", command.value("repositoryId"));
                    result.insert("worktree_id", command.value("worktreeId"));
                    acknowledgeCommand(command, result);
                    if (result.value("ok").toBool())
                        sync();
                    return;
                }
                // Cloud clients (mobile, web console) may only run explicit session operations.
                // Journal reads return unprojected payloads that can include command output, and
                // cursor acks move local retention, so those stay local-only. Transcripts reach
                // the cloud through transcript.restore_window, whose pages executeRestoreWindow
                // passes through projectTranscriptWindowForCloud (large items included).
                static const QSet<QString> remoteOperations{
                    QStringLiteral("session.configuration.read"),
                    QStringLiteral("session.start_turn"), QStringLiteral("session.rename"),
                    QStringLiteral("session.fork"), QStringLiteral("session.steer"),
                    QStringLiteral("session.interrupt"),
                    // One background task: stop it, or read its output tail.
                    QStringLiteral("session.task.stop"), QStringLiteral("session.task.output"),
                    QStringLiteral("session.pending_approvals"),
                    // Bounded, on-demand reads: one command's output tail or one image preview.
                    QStringLiteral("session.output.read"), QStringLiteral("session.media.read"),
                    QStringLiteral("session.execution_policy.apply"),
                    QStringLiteral("session.mode.apply"), QStringLiteral("approval.resolve"),
                    QStringLiteral("question.answer"), QStringLiteral("attachment.stage.begin"),
                    QStringLiteral("attachment.stage.chunk"),
                    QStringLiteral("attachment.stage.commit"),
                    QStringLiteral("attachment.stage.cancel")};
                if (!remoteOperations.contains(kind)) {
                    acknowledgeCommand(
                        command,
                        {{QStringLiteral("ok"), false},
                         {QStringLiteral("error_code"), QStringLiteral("unknown_operation")},
                         {QStringLiteral("error_message"),
                          QStringLiteral("The requested operation is not available "
                                         "remotely.")}});
                    return;
                }
                const QJsonObject local{
                    {QStringLiteral("protocol_version"), 0},
                    {QStringLiteral("message_id"), command.value(QStringLiteral("id"))},
                    {QStringLiteral("message_type"), kind},
                    {QStringLiteral("session_id"), sessionId},
                    {QStringLiteral("session_generation"), generation},
                    {QStringLiteral("sent_at"), protocol::timestamp()},
                    {QStringLiteral("payload"), decoded.object()}};
                // A steer is answered once the provider has taken or refused it: the phone
                // drops its draft on "completed".
                const QPointer<CloudControlClient> guard(this);
                if (kind == QStringLiteral("session.fork")) {
                    // Provider history is copied asynchronously; keep the source turn alive.
                    host_->forkSession(local, [guard, command](const QJsonObject& result) {
                        if (guard) {
                            guard->acknowledgeCommand(command, result);
                            if (result.value(QStringLiteral("ok")).toBool())
                                guard->sync();
                        }
                    });
                    return;
                }
                host_->handleCommandDeferred(local, QStringLiteral("cloud-control-v2"),
                                             [guard, command](const QJsonObject& result) {
                                                 if (guard)
                                                     guard->acknowledgeCommand(command, result);
                                             });
                return;
            }
            acknowledgeCommand(command, localResult);
        });
}

void CloudControlClient::notePublishedRevisions(const QList<QJsonArray>& batches) {
    constexpr qint64 keptMilliseconds = 10 * 60 * 1'000;
    constexpr qsizetype keptPerSession = 8;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSet<QString> current;
    for (const QJsonArray& batch : batches) {
        for (const QJsonValue& value : batch) {
            const QJsonObject snapshot = value.toObject();
            const QString sessionId = snapshot.value(QStringLiteral("id")).toString();
            const QString revision = snapshot.value(QStringLiteral("workspaceRevision")).toString();
            current.insert(sessionId);
            if (revision.isEmpty())
                continue;
            auto& published = publishedRevisions_[sessionId];
            published.removeIf([&](const auto& entry) {
                return entry.first == revision || now - entry.second > keptMilliseconds;
            });
            published.append({revision, now});
            while (published.size() > keptPerSession)
                published.removeFirst();
        }
    }
    for (auto it = publishedRevisions_.begin(); it != publishedRevisions_.end();) {
        if (current.contains(it.key()))
            ++it;
        else
            it = publishedRevisions_.erase(it);
    }
}

bool CloudControlClient::revisionPublished(const QString& sessionId,
                                           const QString& revision) const {
    constexpr qint64 keptMilliseconds = 10 * 60 * 1'000;
    if (revision.isEmpty())
        return false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto published = publishedRevisions_.constFind(sessionId);
    return published != publishedRevisions_.cend() &&
           std::any_of(published->cbegin(), published->cend(), [&](const auto& entry) {
               return entry.first == revision && now - entry.second <= keptMilliseconds;
           });
}

void CloudControlClient::executeWorkspaceFiles(const QJsonObject& command,
                                               const QJsonObject& payload) {
    const QString sessionId = command.value(QStringLiteral("sessionId")).toString();
    const qint64 generation = command.value(QStringLiteral("sessionGeneration")).toInteger();
    const auto sessions = journal_->sessions();
    const auto match =
        std::find_if(sessions.cbegin(), sessions.cend(), [&](const StoredSession& s) {
            return s.id == sessionId && s.generation == generation;
        });
    // Legacy mobile resource references carry no historical context. Never reinterpret
    // their relative paths in the worktree selected by a local context switch.
    const auto requestedContext = payload.value("resource_context_revision");
    if (match != sessions.cend() && match->contextRevision > 1 && requestedContext.isUndefined()) {
        acknowledgeCommand(command, {{"ok", false}, {"error_code", "context_upgrade_required"}});
        return;
    }
    QString resourceRoot = match == sessions.cend() ? QString{} : match->worktreePath;
    if (payload.contains("resource_event_cursor") || payload.contains("resource_source_path")) {
        const auto resolved = host_->handleCommand(
            {{"protocol_version", 0},
             {"message_id", command.value("id")},
             {"message_type", "session.resource.resolve"},
             {"session_id", sessionId},
             {"session_generation", generation},
             {"sent_at", protocol::timestamp()},
             {"payload", QJsonObject{{"event_cursor", payload.value("resource_event_cursor")},
                                     {"path", payload.value("resource_source_path")}}}},
            "cloud-control-v2");
        if (!resolved.value("ok").toBool() ||
            resolved.value("resource_context_revision") != requestedContext) {
            acknowledgeCommand(command,
                               {{"ok", false}, {"error_code", "resource_context_unknown"}});
            return;
        }
        resourceRoot = resolved.value("worktree_path").toString();
    } else if (!requestedContext.isUndefined()) {
        resourceRoot.clear();
        const qint64 revision = requestedContext.toInteger(-1);
        if (revision > 0 && requestedContext.toDouble() == double(revision)) {
            for (const auto& value : journal_->sessionContexts(sessionId, generation)) {
                const auto saved = value.toObject();
                if (saved.value("context_revision").toInteger() == revision)
                    resourceRoot = saved.value("worktree_path").toString();
            }
        }
        if (resourceRoot.isEmpty() || !QFileInfo(resourceRoot).isDir()) {
            acknowledgeCommand(command,
                               {{"ok", false}, {"error_code", "resource_context_unknown"}});
            return;
        }
    }
    const QJsonObject context =
        resourceRoot.isEmpty()
            ? QJsonObject{}
            : sourceContext(resourceRoot, ensurePathIdSalt(),
                            journal_->worktreeRepositories().value(resourceRoot));
    if (match == sessions.cend() ||
        command.value(QStringLiteral("ownerSubjectId")).toString().isEmpty() ||
        command.value(QStringLiteral("workspaceId")).toString() != workspaceId_ ||
        command.value(QStringLiteral("machineId")).toString() != machineId_ ||
        command.value(QStringLiteral("repositoryId")) !=
            context.value(QStringLiteral("repositoryId")) ||
        command.value(QStringLiteral("worktreeId")) !=
            context.value(QStringLiteral("worktreeId"))) {
        acknowledgeCommand(command,
                           {{QStringLiteral("ok"), false},
                            {QStringLiteral("error_code"), QStringLiteral("scope_forbidden")}});
        return;
    }
    constexpr int maximumPendingReads = 8;
    if (workspaceFilesPending_.fetch_add(1) >= maximumPendingReads) {
        workspaceFilesPending_.fetch_sub(1);
        acknowledgeCommand(command, {{QStringLiteral("ok"), false},
                                     {QStringLiteral("error_code"), QStringLiteral("busy")}});
        return;
    }
    const WorkspaceFileScope scope{command.value(QStringLiteral("ownerSubjectId")).toString(),
                                   workspaceId_,
                                   machineId_,
                                   command.value(QStringLiteral("repositoryId")).toString(),
                                   command.value(QStringLiteral("worktreeId")).toString(),
                                   resourceRoot};
    QJsonObject request = payload;
    request.insert(QStringLiteral("owner_subject_id"), scope.ownerSubjectId);
    request.insert(QStringLiteral("workspace_id"), scope.workspaceId);
    request.insert(QStringLiteral("machine_id"), scope.machineId);
    request.insert(QStringLiteral("repository_id"), scope.repositoryId);
    request.insert(QStringLiteral("worktree_id"), scope.worktreeId);
    request.insert(QStringLiteral("revision"), command.value(QStringLiteral("workspaceRevision")));
    const QString kind = command.value(QStringLiteral("kind")).toString();
    // The relay only lists the revision this daemon last published, which trails the worktree
    // by up to a heartbeat while an agent edits it. A revision published recently for this
    // session is served as the worktree is now instead of failing as stale; any other one is
    // still checked against the worktree with git.
    const auto fence =
        revisionPublished(sessionId, command.value(QStringLiteral("workspaceRevision")).toString())
            ? ReadOnlyWorkspaceFiles::RevisionFence::Published
            : ReadOnlyWorkspaceFiles::RevisionFence::Live;
    const QPointer<CloudControlClient> guard(this);
    const qint64 resourceContextRevision = requestedContext.toInteger(0);
    workspaceFilesPool_.start([guard, command, request, scope, kind, fence,
                               resourceContextRevision]() {
        ReadOnlyWorkspaceFiles files(scope, fence);
        QJsonObject result;
        if (kind == QStringLiteral("workspace.files.tree"))
            result = files.tree(request);
        else if (kind == QStringLiteral("workspace.files.search"))
            result = files.search(request);
        else if (kind == QStringLiteral("workspace.files.file"))
            result = files.file(request);
        else if (kind == QStringLiteral("workspace.files.diff"))
            result = files.diff(request);
        else
            result = {{QStringLiteral("ok"), false},
                      {QStringLiteral("error_code"), QStringLiteral("unsupported")}};
        if (!guard)
            return;
        guard->workspaceFilesPending_.fetch_sub(1);
        QMetaObject::invokeMethod(
            guard,
            [guard, command, result, scope, request, resourceContextRevision]() {
                if (!guard)
                    return;
                const auto current = guard->journal_->sessions();
                const auto live =
                    std::find_if(current.cbegin(), current.cend(), [&](const StoredSession& s) {
                        return s.id == command.value(QStringLiteral("sessionId")).toString() &&
                               s.generation ==
                                   command.value(QStringLiteral("sessionGeneration")).toInteger() &&
                               (resourceContextRevision > 0 || s.worktreePath == scope.rootPath);
                    });
                if (live == current.cend()) {
                    guard->acknowledgeCommand(command, {{QStringLiteral("ok"), false},
                                                        {QStringLiteral("error_code"),
                                                         QStringLiteral("stale_revision")}});
                    return;
                }
                if (resourceContextRevision > 0) {
                    bool known = false;
                    for (const auto& value :
                         guard->journal_->sessionContexts(live->id, live->generation)) {
                        const auto context = value.toObject();
                        known |= context.value("context_revision").toInteger() ==
                                     resourceContextRevision &&
                                 context.value("worktree_path").toString() == scope.rootPath;
                    }
                    if (request.contains("resource_event_cursor")) {
                        const auto verified = guard->host_->handleCommand(
                            {{"protocol_version", 0},
                             {"message_id", command.value("id")},
                             {"message_type", "session.resource.resolve"},
                             {"session_id", live->id},
                             {"session_generation", live->generation},
                             {"sent_at", protocol::timestamp()},
                             {"payload",
                              QJsonObject{{"event_cursor", request.value("resource_event_cursor")},
                                          {"path", request.value("resource_source_path")}}}},
                            "cloud-control-v2");
                        known = verified.value("ok").toBool() &&
                                verified.value("resource_context_revision").toInteger() ==
                                    resourceContextRevision &&
                                verified.value("worktree_path").toString() == scope.rootPath;
                    }
                    if (!known) {
                        guard->acknowledgeCommand(
                            command, {{"ok", false}, {"error_code", "resource_context_unknown"}});
                        return;
                    }
                }
                guard->acknowledgeCommand(command, result);
            },
            Qt::QueuedConnection);
    });
}

void CloudControlClient::executeRestoreWindow(const QJsonObject& command,
                                              const QJsonObject& payload) {
    const QByteArray scope = CloudCrypto::canonical(
        // Scope 3: pages are cloud-projected; never replay a raw page cached under scope 2.
        {QStringLiteral("ketplus"), 3, QStringLiteral("transcript-restore-cache"),
         command.value(QStringLiteral("id")), command.value(QStringLiteral("ownerSubjectId")),
         command.value(QStringLiteral("workspaceId")), command.value(QStringLiteral("machineId")),
         command.value(QStringLiteral("sessionId")),
         command.value(QStringLiteral("sessionGeneration")),
         command.value(QStringLiteral("credentialEpoch")),
         command.value(QStringLiteral("snapshotCursor")),
         command.value(QStringLiteral("requestedByteCap")),
         command.value(QStringLiteral("continuationOfRequestId")).toString(),
         command.value(QStringLiteral("continuationSequence")).toInteger(0),
         command.value(QStringLiteral("continuationDigest")).toString(),
         command.value(QStringLiteral("deadlineAt")), machineKeyVersion_});
    const QString cacheKey =
        QStringLiteral("restore-response:%1")
            .arg(QString::fromLatin1(
                QCryptographicHash::hash(scope, QCryptographicHash::Sha256).toHex()));
    const qint64 snapshotCursor = command.value(QStringLiteral("snapshotCursor")).toInteger(-1);
    const qint64 requestedByteCap = command.value(QStringLiteral("requestedByteCap")).toInteger(-1);
    const QString continuationParent =
        command.value(QStringLiteral("continuationOfRequestId")).toString();
    const qint64 continuationSequence =
        command.value(QStringLiteral("continuationSequence")).toInteger(0);
    const QString continuationDigest =
        command.value(QStringLiteral("continuationDigest")).toString();
    const QJsonValue continuationSequenceValue =
        command.value(QStringLiteral("continuationSequence"));
    const bool continuation = continuationSequence > 0;
    const QDateTime deadline =
        QDateTime::fromString(command.value(QStringLiteral("deadlineAt")).toString(), Qt::ISODate);
    if (command.value(QStringLiteral("ownerSubjectId")).toString().isEmpty() ||
        workspaceId_.isEmpty() ||
        command.value(QStringLiteral("workspaceId")).toString() != workspaceId_ ||
        command.value(QStringLiteral("machineId")).toString() != machineId_ ||
        command.value(QStringLiteral("sessionId")).toString().isEmpty() ||
        command.value(QStringLiteral("sessionGeneration")).toInteger() < 1 ||
        command.value(QStringLiteral("credentialEpoch")).toInteger() < 1 || snapshotCursor < 0 ||
        requestedByteCap < 4096 || requestedByteCap > 512 * 1024 || !deadline.isValid() ||
        deadline <= QDateTime::currentDateTimeUtc() ||
        ((!continuationSequenceValue.isUndefined() && !continuationSequenceValue.isNull()) &&
         (!continuationSequenceValue.isDouble() || continuationSequence < 0 ||
          continuationSequenceValue.toDouble() != static_cast<double>(continuationSequence))) ||
        (continuation ? continuationParent.size() < 16 || continuationParent.size() > 128 ||
                            continuationSequence > 4096 ||
                            !QRegularExpression(QStringLiteral("^[a-f0-9]{64}$"))
                                 .match(continuationDigest)
                                 .hasMatch()
                      : !continuationParent.isEmpty() || !continuationDigest.isEmpty())) {
        acknowledgeCommand(
            command, {{QStringLiteral("ok"), false},
                      {QStringLiteral("error_code"), QStringLiteral("transcript_unavailable")},
                      {QStringLiteral("error_message"),
                       QStringLiteral("The restore request boundary is unavailable.")}});
        return;
    }
    const QJsonObject cached = journal_->commandResult(cacheKey);
    if (!cached.isEmpty()) {
        uploadRestoreResponses(command, cached.value(QStringLiteral("responses")).toArray(),
                               cached.value(QStringLiteral("summary")).toObject());
        return;
    }
    const auto sessions = journal_->sessions();
    const auto match = std::find_if(sessions.cbegin(), sessions.cend(), [&](const auto& session) {
        return session.id == command.value(QStringLiteral("sessionId")).toString() &&
               session.generation == command.value(QStringLiteral("sessionGeneration")).toInteger();
    });
    if (match == sessions.cend()) {
        acknowledgeCommand(command,
                           {{QStringLiteral("ok"), false},
                            {QStringLiteral("error_code"), QStringLiteral("snapshot_changed")},
                            {QStringLiteral("error_message"),
                             QStringLiteral("The local transcript snapshot changed.")}});
        return;
    }

    QJsonObject localResult;
    if (continuation) {
        if (payload.keys() != QStringList{QStringLiteral("large_event")} ||
            !payload.value(QStringLiteral("large_event")).isObject()) {
            acknowledgeCommand(
                command,
                {{QStringLiteral("ok"), false},
                 {QStringLiteral("error_code"), QStringLiteral("invalid_restore_continuation")},
                 {QStringLiteral("error_message"),
                  QStringLiteral("The restore continuation selector is invalid.")}});
            return;
        }
        const QJsonObject selector = payload.value(QStringLiteral("large_event")).toObject();
        QStringList selectorKeys = selector.keys();
        selectorKeys.sort();
        QStringList expectedKeys{QStringLiteral("digest_sha256"), QStringLiteral("item_id"),
                                 QStringLiteral("offset"),        QStringLiteral("revision"),
                                 QStringLiteral("total_bytes"),   QStringLiteral("transfer_id"),
                                 QStringLiteral("version")};
        expectedKeys.sort();
        const QString transferId = selector.value(QStringLiteral("transfer_id")).toString();
        const QString itemId = selector.value(QStringLiteral("item_id")).toString();
        const qint64 revision = selector.value(QStringLiteral("revision")).toInteger(-1);
        const QString digest = selector.value(QStringLiteral("digest_sha256")).toString();
        const qint64 totalBytes = selector.value(QStringLiteral("total_bytes")).toInteger(-1);
        const qint64 offset = selector.value(QStringLiteral("offset")).toInteger(-1);
        const auto safeInteger = [](const QJsonValue& value, const qint64 minimum,
                                    const qint64 maximum) {
            return value.isDouble() && value.toDouble() >= static_cast<double>(minimum) &&
                   value.toDouble() <= static_cast<double>(maximum) &&
                   value.toDouble() == static_cast<double>(value.toInteger(-1));
        };
        if (selectorKeys != expectedKeys ||
            !safeInteger(selector.value(QStringLiteral("version")), 1, 1) || transferId.isEmpty() ||
            transferId.size() > 128 || itemId.isEmpty() || itemId.size() > 256 ||
            !safeInteger(selector.value(QStringLiteral("revision")), 1, snapshotCursor) ||
            !QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(digest).hasMatch() ||
            !safeInteger(selector.value(QStringLiteral("total_bytes")), 1, 32 * 1024 * 1024) ||
            !safeInteger(selector.value(QStringLiteral("offset")), 0, totalBytes - 1)) {
            acknowledgeCommand(
                command,
                {{QStringLiteral("ok"), false},
                 {QStringLiteral("error_code"), QStringLiteral("invalid_restore_continuation")},
                 {QStringLiteral("error_message"),
                  QStringLiteral("The restore continuation selector is invalid.")}});
            return;
        }
        // Raw large-event bytes are served only for items whose cloud projection is the raw
        // payload; any other item was inlined as its projection in the root page.
        if (largeEventCloudProjection(
                command.value(QStringLiteral("sessionId")).toString(),
                command.value(QStringLiteral("sessionGeneration")).toInteger(), transferId,
                selector)
                .has_value()) {
            acknowledgeCommand(
                command,
                {{QStringLiteral("ok"), false},
                 {QStringLiteral("error_code"), QStringLiteral("restore_continuation_conflict")},
                 {QStringLiteral("error_message"),
                  QStringLiteral("The retained large transcript item no longer matches.")}});
            return;
        }
        const QJsonObject local{
            {QStringLiteral("protocol_version"), 0},
            {QStringLiteral("message_id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("message_type"), QStringLiteral("session.transcript_large_event")},
            {QStringLiteral("session_id"), command.value(QStringLiteral("sessionId"))},
            {QStringLiteral("session_generation"),
             command.value(QStringLiteral("sessionGeneration"))},
            {QStringLiteral("sent_at"), protocol::timestamp()},
            {QStringLiteral("payload"),
             QJsonObject{
                 {QStringLiteral("transfer_id"), transferId},
                 {QStringLiteral("offset"), offset},
                 {QStringLiteral("maximum_bytes"),
                  qBound(qint64{1}, ((requestedByteCap - 2048) * 9) / 16, qint64{128 * 1024})}}}};
        localResult = host_->handleCommand(local, QStringLiteral("cloud-control-v2"));
        const QByteArray fragmentData =
            CloudCrypto::fromBase64Url(localResult.value(QStringLiteral("data")).toString());
        const qint64 nextOffset = localResult.value(QStringLiteral("next_offset")).toInteger(-1);
        if (!localResult.value(QStringLiteral("ok")).toBool() ||
            localResult.value(QStringLiteral("session_id")) !=
                command.value(QStringLiteral("sessionId")) ||
            localResult.value(QStringLiteral("session_generation")) !=
                command.value(QStringLiteral("sessionGeneration")) ||
            localResult.value(QStringLiteral("transfer_id")).toString() != transferId ||
            localResult.value(QStringLiteral("item_id")).toString() != itemId ||
            localResult.value(QStringLiteral("revision")).toInteger(-1) != revision ||
            localResult.value(QStringLiteral("digest_sha256")).toString() != digest ||
            localResult.value(QStringLiteral("total_bytes")).toInteger(-1) != totalBytes ||
            localResult.value(QStringLiteral("offset")).toInteger(-1) != offset ||
            fragmentData.isEmpty() || nextOffset != offset + fragmentData.size() ||
            nextOffset > totalBytes ||
            localResult.value(QStringLiteral("complete")).toBool() != (nextOffset == totalBytes)) {
            acknowledgeCommand(
                command,
                {{QStringLiteral("ok"), false},
                 {QStringLiteral("error_code"), QStringLiteral("restore_continuation_conflict")},
                 {QStringLiteral("error_message"),
                  QStringLiteral("The retained large transcript item no longer matches.")}});
            return;
        }
    } else {
        if (payload.contains(QStringLiteral("large_event"))) {
            acknowledgeCommand(
                command,
                {{QStringLiteral("ok"), false},
                 {QStringLiteral("error_code"), QStringLiteral("invalid_restore_continuation")},
                 {QStringLiteral("error_message"),
                  QStringLiteral(
                      "A root restore request cannot contain a continuation selector.")}});
            return;
        }
        QJsonObject windowPayload = payload;
        windowPayload.remove(QStringLiteral("projection_revision"));
        windowPayload.insert(QStringLiteral("snapshot_cursor"), snapshotCursor);
        windowPayload.insert(
            QStringLiteral("event_limit"),
            qBound(1, payload.value(QStringLiteral("event_limit")).toInt(128), 128));
        windowPayload.insert(QStringLiteral("turn_limit"),
                             qBound(1, payload.value(QStringLiteral("turn_limit")).toInt(3), 3));
        const QJsonObject local{
            {QStringLiteral("protocol_version"), 0},
            {QStringLiteral("message_id"), command.value(QStringLiteral("id"))},
            {QStringLiteral("message_type"), QStringLiteral("session.transcript_window")},
            {QStringLiteral("session_id"), command.value(QStringLiteral("sessionId"))},
            {QStringLiteral("session_generation"),
             command.value(QStringLiteral("sessionGeneration"))},
            {QStringLiteral("sent_at"), protocol::timestamp()},
            {QStringLiteral("payload"), windowPayload}};
        localResult = host_->restoreTranscriptWindow(local);
        if (localResult.value(QStringLiteral("ok")).toBool()) {
            // The restore page leaves the machine: apply the same cloud projection as event
            // upload before fragmenting, so budgets and digests cover the projected bytes.
            const QString sessionId = command.value(QStringLiteral("sessionId")).toString();
            const qint64 generation =
                command.value(QStringLiteral("sessionGeneration")).toInteger();
            localResult = projectTranscriptWindowForCloud(
                localResult, [&](const QString& transferId, const QJsonObject& manifest) {
                    return largeEventCloudProjection(sessionId, generation, transferId, manifest);
                });
        }
    }
    if (!localResult.value(QStringLiteral("ok")).toBool()) {
        acknowledgeCommand(command, localResult);
        return;
    }
    const QByteArray page = QJsonDocument(localResult).toJson(QJsonDocument::Compact);
    const QString digest =
        QString::fromLatin1(QCryptographicHash::hash(page, QCryptographicHash::Sha256).toHex());
    constexpr qsizetype fragmentBytes = 48 * 1024;
    QJsonArray responses;
    qint64 encryptedBytes = 0;
    QString cryptoError;
    for (qsizetype offset = 0; offset < page.size(); offset += fragmentBytes) {
        const QByteArray fragment = page.mid(offset, fragmentBytes);
        const bool terminal = offset + fragment.size() == page.size();
        const qint64 cursor = responses.size() + 1;
        const QJsonObject plain{{QStringLiteral("restore_response_version"), 1},
                                {QStringLiteral("encoding"), QStringLiteral("json-utf8-fragment")},
                                {QStringLiteral("offset"), offset},
                                {QStringLiteral("total_bytes"), page.size()},
                                {QStringLiteral("data"), QString::fromLatin1(fragment.toBase64(
                                                             QByteArray::Base64UrlEncoding |
                                                             QByteArray::OmitTrailingEquals))}};
        const QString sessionId = command.value(QStringLiteral("sessionId")).toString();
        const qint64 generation = command.value(QStringLiteral("sessionGeneration")).toInteger();
        const QJsonObject shell{
            {QStringLiteral("keyId"), QStringLiteral("%1:%2:1").arg(sessionId).arg(generation)},
            {QStringLiteral("keyEpoch"), 1},
            {QStringLiteral("senderDeviceId"), machineId_},
            {QStringLiteral("senderKeyVersion"), machineKeyVersion_}};
        const QByteArray aad = restoreResponseAssociatedData(command, cursor, terminal,
                                                             terminal ? digest : QString{}, shell);
        const QJsonObject encrypted =
            crypto_.encrypt(QJsonDocument(plain).toJson(QJsonDocument::Compact), aad, sessionId,
                            generation, 1, machineId_, machineKeyVersion_, &cryptoError);
        if (encrypted.isEmpty()) {
            lastError_ = cryptoError;
            return;
        }
        encryptedBytes +=
            CloudCrypto::fromBase64Url(encrypted.value(QStringLiteral("ciphertext")).toString())
                .size();
        if (encryptedBytes > requestedByteCap) {
            acknowledgeCommand(
                command, {{QStringLiteral("ok"), false},
                          {QStringLiteral("error_code"), QStringLiteral("transcript_unavailable")},
                          {QStringLiteral("error_message"),
                           QStringLiteral("The bounded restore page is too large.")}});
            return;
        }
        responses.append(
            QJsonObject{{QStringLiteral("responseCursor"), cursor},
                        {QStringLiteral("terminal"), terminal},
                        {QStringLiteral("responseDigest"), terminal ? digest : QString{}},
                        {QStringLiteral("content"), encrypted}});
    }
    if (responses.isEmpty()) {
        acknowledgeCommand(
            command, {{QStringLiteral("ok"), false},
                      {QStringLiteral("error_code"), QStringLiteral("transcript_unavailable")}});
        return;
    }
    const QJsonObject summary{{QStringLiteral("ok"), true},
                              {QStringLiteral("response_cursor"), responses.size()},
                              {QStringLiteral("snapshot_cursor"), snapshotCursor},
                              {QStringLiteral("response_digest"), digest}};
    if (!journal_->storeCommandResult(cacheKey, {{QStringLiteral("responses"), responses},
                                                 {QStringLiteral("summary"), summary}})) {
        lastError_ = journal_->errorString();
        return;
    }
    uploadRestoreResponses(command, responses, summary);
}

std::optional<QJsonObject>
CloudControlClient::largeEventCloudProjection(const QString& sessionId, const qint64 generation,
                                              const QString& transferId,
                                              const QJsonObject& manifest) {
    constexpr qsizetype maximumInlineProjectionBytes = 64 * 1024;
    const QString itemId = manifest.value(QStringLiteral("item_id")).toString();
    const QJsonObject omitted{{QStringLiteral("item_id"), itemId},
                              {QStringLiteral("cloud_omitted"), true}};
    // Reassemble from the journal (bounded by the 32 MiB large-event cap) and verify the digest.
    QByteArray bytes;
    QString eventType;
    QString digest;
    for (qint64 offset = 0;;) {
        const LargeEventFragment fragment =
            journal_->largeEventFragment(sessionId, generation, transferId, offset);
        if (!fragment.errorCode.isEmpty() || fragment.data.isEmpty() || fragment.offset != offset ||
            (offset > 0 && fragment.digestSha256 != digest))
            return omitted;
        if (offset == 0) {
            const QString key = QStringLiteral("%1:%2:%3:%4")
                                    .arg(sessionId)
                                    .arg(generation)
                                    .arg(transferId, fragment.digestSha256);
            const auto cached = largeEventCloudDecisions_.constFind(key);
            if (cached != largeEventCloudDecisions_.constEnd())
                return cached.value();
            eventType = fragment.eventType;
            digest = fragment.digestSha256;
            bytes.reserve(fragment.totalBytes);
        }
        bytes.append(fragment.data);
        offset = fragment.nextOffset;
        if (fragment.complete)
            break;
    }
    const QString key =
        QStringLiteral("%1:%2:%3:%4").arg(sessionId).arg(generation).arg(transferId, digest);
    std::optional<QJsonObject> decision;
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) !=
            digest ||
        !document.isObject()) {
        decision = omitted;
    } else {
        const QJsonObject raw = document.object();
        const QJsonObject projected = projectEventPayloadForCloud(eventType, raw);
        if (projected != raw) {
            decision = QJsonDocument(projected).toJson(QJsonDocument::Compact).size() <=
                               maximumInlineProjectionBytes
                           ? projected
                           : omitted;
        }
    }
    largeEventCloudDecisions_.insert(key, decision);
    return decision;
}

void CloudControlClient::uploadRestoreResponses(const QJsonObject& command,
                                                const QJsonArray& responses,
                                                const QJsonObject& localResult) {
    const auto peerId = command.value("_peerConnection").toString();
    if (!peerId.isEmpty()) {
        if (!peers_->authorized(peerId, command))
            return;
        auto result = localResult;
        result.insert("responses", responses);
        acknowledgeCommand(command, result);
        return;
    }
    const QString path = QStringLiteral("/v2/restore-requests/%1/responses:append")
                             .arg(QString::fromUtf8(QUrl::toPercentEncoding(
                                 command.value(QStringLiteral("id")).toString())));
    const QJsonObject body{
        {QStringLiteral("attempt"), command.value(QStringLiteral("attempt"))},
        {QStringLiteral("claimToken"), command.value(QStringLiteral("claimToken"))},
        {QStringLiteral("responses"), responses}};
    request("POST", path, body, daemonToken_, {},
            [this, command, localResult](const int status, const QJsonValue& value,
                                         const QString& transportError) {
                if (status != 202 || !transportError.isEmpty()) {
                    lastError_ =
                        transportError.isEmpty()
                            ? responseCode(value, QStringLiteral("restore_response_upload_failed"))
                            : transportError;
                    return;
                }
                acknowledgeCommand(command, localResult);
            });
}

void CloudControlClient::acknowledgeCommand(const QJsonObject& command,
                                            const QJsonObject& localResult,
                                            const QByteArray& recipientAgreementKey) {
    const bool accepted = localResult.value(QStringLiteral("ok")).toBool();
    const QString status = accepted ? QStringLiteral("completed") : QStringLiteral("rejected");
    const QString resultCode = accepted ? QStringLiteral("ok")
                                        : localResult.value(QStringLiteral("error_code"))
                                              .toString(QStringLiteral("command_rejected"));
    const QString sessionId = command.value(QStringLiteral("sessionId")).toString();
    const qint64 generation = command.value(QStringLiteral("sessionGeneration")).toInteger();
    const int attempt = command.value(QStringLiteral("attempt")).toInt();
    const bool creating = isWorktreeCommand(command);
    const QString commandId = command.value(QStringLiteral("id")).toString();
    const qint64 startedAt = commandStartedAt_.take(commandId);
    qInfo("command %s %s %s/%s after %lldms", qUtf8Printable(commandId),
          qUtf8Printable(command.value(QStringLiteral("kind")).toString()), qUtf8Printable(status),
          qUtf8Printable(resultCode),
          startedAt > 0 ? QDateTime::currentMSecsSinceEpoch() - startedAt : -1LL);
    const QJsonObject shell{{QStringLiteral("keyId"),
                             creating ? QStringLiteral("session-create:%1")
                                            .arg(command.value(QStringLiteral("id")).toString())
                                      : QStringLiteral("%1:%2:1").arg(sessionId).arg(generation)},
                            {QStringLiteral("keyEpoch"), 1},
                            {QStringLiteral("senderDeviceId"), machineId_},
                            {QStringLiteral("senderKeyVersion"), machineKeyVersion_}};
    const QByteArray aad = commandResultAssociatedData(command, attempt, status, resultCode, shell);
    QString cryptoError;
    const QJsonObject encrypted =
        creating ? crypto_.encryptSealed(QJsonDocument(localResult).toJson(QJsonDocument::Compact),
                                         aad, recipientAgreementKey,
                                         command.value(QStringLiteral("id")).toString(),
                                         command.value(QStringLiteral("content"))
                                             .toObject()
                                             .value(QStringLiteral("senderDeviceId"))
                                             .toString(),
                                         command.value(QStringLiteral("content"))
                                             .toObject()
                                             .value(QStringLiteral("senderKeyVersion"))
                                             .toInt(),
                                         machineId_, machineKeyVersion_, &cryptoError)
                 : crypto_.encrypt(QJsonDocument(localResult).toJson(QJsonDocument::Compact), aad,
                                   sessionId, generation, 1, machineId_, machineKeyVersion_,
                                   &cryptoError);
    if (encrypted.isEmpty()) {
        lastError_ = cryptoError;
        return;
    }
    const QJsonObject body{
        {QStringLiteral("attempt"), attempt},
        {QStringLiteral("claimToken"), command.value(QStringLiteral("claimToken"))},
        {QStringLiteral("status"), status},
        {QStringLiteral("resultCode"), resultCode},
        {QStringLiteral("encryptedResult"), encrypted},
        {QStringLiteral("completedAt"), protocol::timestamp()}};
    const auto peerId = command.value("_peerConnection").toString();
    if (!peerId.isEmpty()) {
        if (peers_->authorized(peerId, command)) {
            auto result = body;
            result.remove("claimToken");
            result.insert("id", commandId);
            peers_->send(peerId, {{"type", "command.result"}, {"result", result}});
        }
        peers_->completed(peerId, commandId);
        return;
    }
    const QString path = QStringLiteral("/v2/commands/%1/ack")
                             .arg(QString::fromUtf8(QUrl::toPercentEncoding(
                                 command.value(QStringLiteral("id")).toString())));
    request("POST", path, body, daemonToken_, {},
            [this, commandId](const int statusCode, const QJsonValue& value,
                              const QString& transportError) {
                if (statusCode != 200 || !transportError.isEmpty()) {
                    lastError_ =
                        transportError.isEmpty()
                            ? responseCode(value, QStringLiteral("command_acknowledgement_failed"))
                            : transportError;
                    qWarning("command %s acknowledgement failed: %d %s", qUtf8Printable(commandId),
                             statusCode, qUtf8Printable(lastError_));
                } else {
                    uploadEvents();
                    // The relay runs one command per session at a time, so the next one
                    // (an upload's following chunk, a turn start, a phone's read) is leasable
                    // now. Waiting for the timer added a full tick to every sequential command.
                    wakeCommandPoll();
                }
            });
}

} // namespace ketplus::daemon
