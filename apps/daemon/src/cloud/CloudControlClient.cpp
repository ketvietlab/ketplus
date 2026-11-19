#include "cloud/CloudControlClient.h"
#include "atlas/AtlasLiveRuntime.h"
#include "cloud/CloudControlBatching.h"
#include "cloud/CloudControlSupport.h"
#include "cloud/CloudEventProjection.h"
#include "cloud/ControlEndpoint.h"
#include "cloud/RemoteAudit.h"
#include "cloud/WorktreeBranches.h"
#include "cloud/WorktreeRepositories.h"
#include "runner/RunnerService.h"
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
      sessionCredentials_(&credentials_), baseUrl_(controlBaseUrl()),
      wakeups_(baseUrl_, [this](std::function<void(const QString&)> answer) {
          ensureDaemonCredential(
              [this, answer = std::move(answer)](const bool ready, const QString&) {
                  answer(ready ? daemonToken_ : QString());
              });
      }) {
    restoreEnrollment();
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
    // ADR 0116 §3: phones learn of a team change from the members' sealed metadata.
    teamSyncTimer_.setSingleShot(true);
    connect(&teamSyncTimer_, &QTimer::timeout, this, [this] {
        lastTeamSync_.restart();
        stateSyncTimer_.start(0);
    });
    connect(host_, &SessionHost::teamEvent, this, &CloudControlClient::noteTeamChanged);
    connect(host_, &SessionHost::eventReady, this, [this](const QJsonObject& event) {
        peers_->eventsChanged(event.value("session_id").toString());
        QTimer::singleShot(0, this, &CloudControlClient::uploadEvents);
        // Phones read a conversation's state from the heartbeat. A turn that starts or ends
        // must not wait up to 15s for the next one, or they keep showing it as it was.
        const QString type = event.value(QStringLiteral("event_type")).toString();
        if (type == QStringLiteral("session.deleted")) {
            pruneSessionCredentials();
            // An Atlas opened for the deleted conversation closes with it.
            const QString session = event.value(QStringLiteral("session_id")).toString();
            const qint64 generation = event.value(QStringLiteral("session_generation")).toInteger();
            stopAtlasRuntimes([&](const QObject& runtime) {
                return runtime.property("session").toString() == session &&
                       runtime.property("generation").toLongLong() == generation;
            });
        }
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
    pruneSessionCredentials();
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
           type == QStringLiteral("remote.device.revoke") ||
           type == QStringLiteral("remote.audit") || runner::RunnerService::handlesType(type);
}

void CloudControlClient::handleLocal(const QString& type, const QJsonObject& payload,
                                     LocalCallback callback, const QString& clientId) {
    if (runner::RunnerService::handlesType(type)) {
        runners().handle(type, payload, std::move(callback), clientId);
        return;
    }
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
    } else if (type == QStringLiteral("remote.audit")) {
        // What paired devices did on this Mac (#86), newest first; one device's with device_id.
        const auto entries = journal_->remoteCommands(
            payload.value(QStringLiteral("device_id")).toString(),
            qBound(1, payload.value(QStringLiteral("limit")).toInt(100), 500));
        if (!entries) {
            callback({}, journal_->errorString());
        } else {
            // Titles of the conversations still here; a deleted one has none.
            QHash<QString, QString> titles;
            for (const StoredSession& session : journal_->sessions())
                titles.insert(session.id, session.title);
            callback({{QStringLiteral("ok"), true},
                      {QStringLiteral("entries"), remoteAuditJson(*entries, titles)}},
                     {});
        }
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

void CloudControlClient::stopAtlasRuntimes(
    const std::function<bool(const QObject& runtime)>& matches) {
    for (auto* runtime : findChildren<AtlasLiveRuntime*>()) {
        if (!matches(*runtime))
            continue;
        runtime->stop();
        runtime->deleteLater();
    }
}

void CloudControlClient::revokeAtlasAccess(const QString& device) {
    if (device.isEmpty()) {
        ++atlasMachineEpoch_;
        stopAtlasRuntimes([](const QObject&) { return true; });
        return;
    }
    ++atlasDeviceEpochs_[device];
    stopAtlasRuntimes(
        [&](const QObject& runtime) { return runtime.property("device").toString() == device; });
}

CloudControlClient::AtlasAccessFence
CloudControlClient::atlasAccessFence(const QString& device) const {
    return {atlasMachineEpoch_, atlasDeviceEpochs_.value(device), accountFence()};
}

bool CloudControlClient::atlasAccessCurrent(const QString& device,
                                            const AtlasAccessFence& fence) const {
    const auto account = accountFence();
    return fence.machine == atlasMachineEpoch_ &&
           fence.device == atlasDeviceEpochs_.value(device) &&
           fence.account.epoch == account.epoch &&
           fence.account.enrollmentRevision == account.enrollmentRevision &&
           fence.account.machine == account.machine && fence.account.workspace == account.workspace;
}

void CloudControlClient::signOut(LocalCallback callback) {
    signaling_->configure({}, {}, 1, false);
    revokeAtlasAccess();
    ++accountEpoch_;
    invalidateSenderKeys();
    legacyPinAdoptionPending_ = false;
    dropBrowserSignIn();
    signInError_.clear();
    QString removalError;
    // This build is not enrolled with its relay when the stored enrollment is another relay's;
    // that enrollment and its upload progress stay for a build that talks to it.
    // The enrollment and its upload progress (see clearUploadProgress) go in one rewrite of the
    // credential file.
    const QSet<QString> enrollment{
        QStringLiteral("machine-id"),          QStringLiteral("workspace-id"),
        QStringLiteral("machine-key-version"), QStringLiteral("control-url"),
        QStringLiteral("device-pins"),         QStringLiteral("account-name"),
        QStringLiteral("account-username"),    QStringLiteral("runner-pins")};
    if (otherRelay_.isEmpty() && !credentials_.removeIf(
                                     [&enrollment](const QString& account) {
                                         return enrollment.contains(account) ||
                                                SessionCredentials::isUploadCursor(account);
                                     },
                                     &removalError)) {
        callback({}, removalError);
        return;
    }
    wakeups_.stop();
    runnerRelayAvailable_ = false;
    credentialRequestInFlight_ = false;
    finishCredentialWaiters(false, QStringLiteral("Signed out."));
    machineId_.clear();
    workspaceId_.clear();
    accountName_.clear();
    accountUsername_.clear();
    daemonToken_.clear();
    registeredCloudCursors_.clear();
    knownUploadedCursors_.clear();
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
    return sessionCredentials_.clearUploadCursors(error);
}

void CloudControlClient::pruneSessionCredentials() {
    // Only a complete list of live sessions proves the others were deleted.
    const std::optional<QSet<QString>> live = journal_->sessionIds();
    if (!live) {
        lastError_ = journal_->errorString();
        return;
    }
    QString error;
    if (!sessionCredentials_.pruneDeleted(*live, &error))
        lastError_ = error;
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
    // Enrollments from before ADR 0109 have no name until the next sign-in.
    if (!machineId_.isEmpty() && (!accountName_.isEmpty() || !accountUsername_.isEmpty()))
        values.insert(QStringLiteral("account"),
                      QJsonObject{{QStringLiteral("display_name"), accountName_},
                                  {QStringLiteral("username"), accountUsername_}});
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

QString CloudControlClient::hostKeyFingerprint(QString* error) {
    return DevicePins::hostFingerprint(crypto_.publicKeys(error));
}

std::optional<bool> CloudControlClient::sidebarRepositoryVisible(const QString& path) {
    const QFileInfo origin(path);
    if (!origin.isDir() || !origin.isReadable())
        return std::nullopt;
    const auto canonicalOrigin = origin.canonicalFilePath();
    if (canonicalOrigin.isEmpty())
        return std::nullopt;
    QDir directory(canonicalOrigin);
    do {
        const QFileInfo marker(directory.filePath(QStringLiteral(".git")));
        if (marker.exists() || marker.isSymLink()) {
            const auto git = worktreeGitDirectory(directory.absolutePath());
            return !git.isEmpty() && QFileInfo(git).isDir() ? std::optional<bool>{true}
                                                            : std::nullopt;
        }
        if (QFileInfo(directory.filePath(QStringLiteral("HEAD"))).isFile() &&
            QFileInfo(directory.filePath(QStringLiteral("objects"))).isDir())
            return true;
    } while (directory.cdUp());
    return false;
}

QJsonArray CloudControlClient::sessionSnapshots(
    const QList<StoredSession>& stored, const QString& workspaceId, const QString& machineId,
    const QByteArray& pathIdSalt, const QHash<QString, QString>& repositories,
    QHash<QString, QJsonObject>* metadata, const QHash<QString, QJsonObject>& agentMetadata,
    const QHash<QString, QString>& tagColors, WorktreeRevisionCache* revisionCache,
    const qint64 nowMs) {
    QJsonArray sessions;
    const QString machineName = QHostInfo::localHostName().left(128);
    QHash<QString, QString> revisions;
    QHash<QString, QList<StoredSession>> byWorktree;
    if (revisionCache != nullptr)
        for (const StoredSession& session : stored)
            byWorktree[session.worktreePath].append(session);
    for (const StoredSession& session : stored) {
        const QString label = QFileInfo(session.worktreePath).fileName();
        // The relay only accepts these providers; anything else keeps the historical default.
        const QString adapter = QStringList{QStringLiteral("codex"), QStringLiteral("claude"),
                                            QStringLiteral("cursor"), QStringLiteral("copilot")}
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
                 {QStringLiteral("lastTurnOutcome"), lastTurnOutcome(session)},
                 // A paired Desktop shows these from the host-signed copy, not the relay's.
                 {QStringLiteral("title"), snapshot.value(QStringLiteral("title"))},
                 {QStringLiteral("state"), snapshot.value(QStringLiteral("state"))},
                 {QStringLiteral("repositoryId"), context.value(QStringLiteral("repositoryId"))},
                 {QStringLiteral("repositoryLabel"),
                  context.value(QStringLiteral("repositoryLabel"))},
                 {QStringLiteral("worktreeId"), context.value(QStringLiteral("worktreeId"))},
                 {QStringLiteral("worktreeLabel"),
                  context.value(QStringLiteral("worktreeLabel"))}});
            // Navigation placement is host-sealed metadata only. The relay's routing scope
            // above continues to name the current execution worktree/repository.
            if (session.sidebarRepository) {
                const auto& placement = *session.sidebarRepository;
                auto& sealed = (*metadata)[session.id];
                sealed.insert(QStringLiteral("sidebarPlacementKnown"), true);
                sealed.insert(QStringLiteral("sidebarRepositoryId"),
                              placement.isEmpty() ? QString{}
                                                  : cloud_control::opaquePathId(
                                                        "repository", placement, pathIdSalt));
                sealed.insert(QStringLiteral("sidebarRepositoryLabel"),
                              QFileInfo(placement).fileName());
            } else if (!session.conversationPath.isEmpty()) {
                // Legacy navigation is a stable fallback, not a claim about historical origin.
                const auto recorded = repositories.value(session.conversationPath);
                const auto fallback = recorded.isEmpty() ? session.conversationPath : recorded;
                auto& sealed = (*metadata)[session.id];
                sealed.insert(QStringLiteral("sidebarPlacementKnown"), false);
                sealed.insert(QStringLiteral("sidebarRepositoryId"),
                              cloud_control::opaquePathId("repository", fallback, pathIdSalt));
                sealed.insert(QStringLiteral("sidebarRepositoryLabel"),
                              QFileInfo(fallback).fileName());
            }
            if (const auto visible = sidebarRepositoryVisible(session.worktreePath))
                (*metadata)[session.id].insert(QStringLiteral("sidebarExecutionRepositoryVisible"),
                                               *visible);
            // ADR 0027: the number agents and users address the conversation by, and the
            // conversation whose message started the running turn. Sealed: only the phone
            // shows them.
            // #125 and #429: the phone counts the conversations waiting for the user and
            // shows each conversation's tags in their colours (lower-case label -> colour).
            if (session.awaitingReply)
                (*metadata)[session.id].insert(QStringLiteral("awaitingReply"), true);
            if (!session.tags.isEmpty()) {
                QJsonArray tags;
                for (const QString& tag : session.tags)
                    tags.append(
                        QJsonObject{{QStringLiteral("label"), tag},
                                    {QStringLiteral("color"),
                                     tagColors.value(tag.toLower(), QStringLiteral("#5b6b7f"))}});
                (*metadata)[session.id].insert(QStringLiteral("tags"), tags);
            }
            const QJsonObject agent = agentMetadata.value(session.id);
            for (auto it = agent.constBegin(); it != agent.constEnd(); ++it)
                (*metadata)[session.id].insert(it.key(), it.value());
        }
        context.remove(QStringLiteral("branch"));
        for (auto it = context.constBegin(); it != context.constEnd(); ++it) {
            snapshot.insert(it.key(), it.value());
        }
        QString workspaceRevision = revisions.value(session.worktreePath);
        if (!revisions.contains(session.worktreePath)) {
            const auto compute = [&](const QString& path) {
                ReadOnlyWorkspaceFiles files(
                    {QStringLiteral("heartbeat"), workspaceId, machineId,
                     context.value(QStringLiteral("repositoryId")).toString(),
                     context.value(QStringLiteral("worktreeId")).toString(), path});
                return files.revision();
            };
            workspaceRevision = revisionCache != nullptr
                                    ? revisionCache->revision(
                                          session.worktreePath,
                                          byWorktree.value(session.worktreePath), nowMs, compute)
                                    : compute(session.worktreePath);
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
    const auto previous = sealedMetadata_;
    QString error;
    const bool ready = credentials_.batch(
        [&](CredentialStore& view, QString* failure) {
            CloudCrypto crypto(&view);
            for (const QJsonArray& batch : batches) {
                QJsonArray snapshots;
                for (const QJsonValue& value : batch) {
                    QJsonObject snapshot = value.toObject();
                    const QString sessionId = snapshot.value(QStringLiteral("id")).toString();
                    current.insert(sessionId);
                    if (metadata.contains(sessionId)) {
                        const QJsonObject envelope =
                            sealedSessionMetadata(snapshot, metadata.value(sessionId), crypto);
                        if (envelope.isEmpty()) {
                            *failure = lastError_;
                            return false;
                        }
                        snapshot.insert(QStringLiteral("encryptedMetadata"), envelope);
                    }
                    snapshots.append(snapshot);
                }
                sealed.append(snapshots);
            }
            return true;
        },
        &error);
    if (!ready) {
        sealedMetadata_ = previous;
        lastError_ = error;
        return {};
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
                                                      const QJsonObject& metadata,
                                                      CloudCrypto& crypto) {
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
    const QJsonObject content = crypto.encrypt(plaintext, aad, sessionId, generation, 1, machineId_,
                                               machineKeyVersion_, &cryptoError);
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
