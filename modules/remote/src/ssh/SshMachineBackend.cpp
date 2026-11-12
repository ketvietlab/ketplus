#include "ssh/SshMachineBackend.h"

#include "ssh/KnownHostsFile.h"
#include "ssh/SshMachineStore.h"

#include <QPointer>

#include <utility>

namespace ketplus::remote::ssh {

namespace {

QString missingIdentity() {
    return QStringLiteral("The ketPlus daemon on this host does not name its identity. Update "
                          "ketPlus on the host, then connect again.");
}

} // namespace

SshTunnelLink::SshTunnelLink(SshTarget target, SshTunnelOptions options, QObject* parent)
    : SshLink(parent),
      knownHostsFile_(options.knownHostsFile.isEmpty() ? SshTunnel::defaultKnownHostsFile()
                                                       : options.knownHostsFile) {
    tunnel_ = new SshTunnel(std::move(target), std::move(options), this);
    connect(tunnel_, &SshTunnel::stateChanged, this, &SshLink::stateChanged);
}

QString SshMachineBackend::reasonFor(const SshTunnel::State state) {
    switch (state) {
    case SshTunnel::State::Idle:
        return QStringLiteral("Disconnected.");
    case SshTunnel::State::Connecting:
    case SshTunnel::State::Connected:
        return {};
    case SshTunnel::State::Offline:
        return QStringLiteral("The host cannot be reached over SSH.");
    case SshTunnel::State::AuthFailed:
        return QStringLiteral("SSH refused the sign-in. Check the user and the key in your SSH "
                              "agent or ~/.ssh/config.");
    case SshTunnel::State::HostKeyMismatch:
        return QStringLiteral("The host's SSH key changed. Check the new key on the host before "
                              "connecting again.");
    case SshTunnel::State::HostKeyRejected:
        return QStringLiteral("The host's SSH key was not trusted.");
    case SshTunnel::State::DaemonSocketMissing:
        return QStringLiteral("ketPlus daemon not installed or not running on this host.");
    case SshTunnel::State::DaemonTokenInsecure:
        return QStringLiteral("The ketPlus daemon token on this host is readable by other users. "
                              "Make it owner-only (chmod 600), then connect again.");
    }
    return {};
}

SshMachineBackend::SshMachineBackend(SshTarget target, SshMachineStore* store,
                                     std::unique_ptr<SshLink> link, QObject* parent)
    : MachineBackend(parent), target_(std::move(target)), store_(store), link_(std::move(link)),
      machineId_(store_->machineId(sshTargetKey(target_))) {
    SshLink* const link_raw = link_.get();
    client_ = new DaemonClient([link_raw] { return link_raw->daemonCredential(); }, this);
    client_->setHostIdentityRequired(true);
    client_->setTrustedHostKeyFingerprint(machineId_);
    operations_ = new DaemonMachineBackend(
        machineId_, Route::Ssh,
        [this](const QString& type, const QJsonObject& payload, const QString& sessionId,
               const qint64 generation, Callback callback) {
            // An Offline route will not answer on its own: fail now with the reason.
            if (state_ == ConnectionState::Offline) {
                callback({}, reason_);
                return;
            }
            client_->call(type, payload, sessionId, generation, std::move(callback));
        },
        this);
    connect(client_, &DaemonClient::eventReceived, this, &MachineBackend::eventReceived);
    connect(client_, &DaemonClient::readyChanged, this, &SshMachineBackend::daemonReady);
    connect(client_, &DaemonClient::hostIdentityRejected, this,
            &SshMachineBackend::identityRejected);
    connect(link_raw, &SshLink::stateChanged, this, &SshMachineBackend::tunnelChanged);
    update();
}

SshMachineBackend::~SshMachineBackend() {
    QObject::disconnect(link_.get(), nullptr, this, nullptr);
    // The client must let go of the link's socket before the link closes it.
    client_->adoptConnection(nullptr);
    delete client_;
    link_->close();
}

QString SshMachineBackend::targetKey() const { return sshTargetKey(target_); }

QJsonObject SshMachineBackend::connectionDetails() const {
    static const char* const states[] = {"connecting", "connected", "reconnecting", "offline"};
    QJsonObject details{
        {QStringLiteral("route"), routeLabel()},
        {QStringLiteral("target"), targetKey()},
        {QStringLiteral("machine_id"), machineId_},
        {QStringLiteral("ssh_host_key_fingerprint"), sshHostKeyFingerprint()},
        {QStringLiteral("state"), QString::fromLatin1(states[static_cast<int>(state_)])},
        {QStringLiteral("reason"), reason_}};
    if (identityChanged_)
        details.insert(QStringLiteral("presented_machine_id"), presented_);
    if (const QString presented = presentedSshHostKeyFingerprint(); !presented.isEmpty())
        details.insert(QStringLiteral("presented_ssh_host_key_fingerprint"), presented);
    return details;
}

void SshMachineBackend::connectToHost() {
    // A changed identity stays closed until it is trusted or the machine is removed.
    if (identityChanged_ && !presented_.isEmpty())
        return;
    if (identityChanged_) {
        // The daemon named no identity (too old): the host may have been updated since.
        identityChanged_ = false;
        client_->setTrustedHostKeyFingerprint(machineId_);
    }
    closedByUser_ = false;
    // A new attempt, not a reconnect of the last session.
    everConnected_ = false;
    storeError_.clear();
    update();
    link_->open();
}

void SshMachineBackend::disconnectFromHost() {
    closedByUser_ = true;
    link_->close();
    update();
}

bool SshMachineBackend::trustPresentedIdentity(QString* error) {
    if (!identityChanged_ || presented_.isEmpty()) {
        if (error)
            *error = identityChanged_ ? missingIdentity()
                                      : QStringLiteral("This machine's identity has not changed.");
        return false;
    }
    if (!store_->retrust(targetKey(), presented_, error))
        return false;
    machineId_ = std::exchange(presented_, {});
    identityChanged_ = false;
    client_->setTrustedHostKeyFingerprint(machineId_);
    emit machineIdChanged(machineId_);
    connectToHost();
    return true;
}

bool SshMachineBackend::bindConversation(const QString& conversationId, QString* error) {
    if (machineId_.isEmpty()) {
        if (error)
            *error = QStringLiteral("Connect to this machine before opening a conversation.");
        return false;
    }
    return store_->bindConversation(conversationId, machineId_, error);
}

bool SshMachineBackend::removeMachine(QString* error) {
    disconnectFromHost();
    const QStringList released = store_->releasedHostKeys(targetKey());
    if (!store_->remove(targetKey(), error))
        return false;
    const QString file = link_->knownHostsFile();
    bool removed = true;
    for (const QString& name : released)
        removed = removeKnownHost(file, name, error) >= 0 && removed;
    return removed;
}

void SshMachineBackend::setHostKeyPrompt(SshTunnel::HostKeyPrompt prompt) {
    if (!prompt) {
        link_->setHostKeyPrompt({});
        return;
    }
    const QPointer<SshMachineBackend> self(this);
    link_->setHostKeyPrompt(
        [self, prompt = std::move(prompt)](const HostKey& key, std::function<void(bool)> decide) {
            prompt(key, [self, key, decide = std::move(decide)](const bool accepted) {
                // Remembered before the tunnel records the key, so it is never left unowned.
                if (accepted && self)
                    self->store_->recordAcceptedHostKey(self->targetKey(), key.knownHostsName);
                decide(accepted);
            });
        });
}

bool SshMachineBackend::holdsAcceptedHostKey() const {
    const QString file = link_->knownHostsFile();
    for (const QString& name : store_->acceptedHostKeys(targetKey()))
        if (hasKnownHost(file, name))
            return true;
    return false;
}

bool SshMachineBackend::forgetAcceptedHostKeys(QString* error) {
    if (!holdsAcceptedHostKey())
        return false;
    const QString file = link_->knownHostsFile();
    bool removed = true;
    for (const QString& name : store_->acceptedHostKeys(targetKey()))
        removed = removeKnownHost(file, name, error) >= 0 && removed;
    return removed;
}

void SshMachineBackend::tunnelChanged() {
    // The client follows the link's connection: a new one is handed over for a hello, a lost one
    // ends the session (calls that replay wait for the next connection).
    client_->adoptConnection(link_->state() == SshTunnel::State::Connected ? link_->connection()
                                                                           : nullptr);
    update();
}

void SshMachineBackend::daemonReady(const bool ready) {
    if (ready && machineId_.isEmpty()) {
        const QString presented = client_->hostKeyFingerprint();
        QString error;
        if (!store_->trustFirstIdentity(targetKey(), presented, &error)) {
            // Without a durable id the machine cannot keep its conversations: stay closed.
            storeError_ = error;
            closeLater();
            update();
            return;
        }
        machineId_ = presented;
        emit machineIdChanged(machineId_);
    }
    if (ready)
        everConnected_ = true;
    update();
}

void SshMachineBackend::identityRejected(const QString& presented) {
    identityChanged_ = true;
    presented_ = presented;
    // Called from inside the client's hello handling: close once it has returned.
    closeLater();
    update();
}

void SshMachineBackend::closeLater() {
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (identityChanged_ || !storeError_.isEmpty())
                link_->close();
        },
        Qt::QueuedConnection);
}

void SshMachineBackend::update() {
    if (identityChanged_) {
        setState(ConnectionState::Offline,
                 presented_.isEmpty() ? missingIdentity() : DaemonClient::identityRejectedReason());
        return;
    }
    if (!storeError_.isEmpty()) {
        setState(ConnectionState::Offline, storeError_);
        return;
    }
    const SshTunnel::State tunnel = link_->state();
    switch (tunnel) {
    case SshTunnel::State::Connected:
        if (client_->isReady()) {
            setState(ConnectionState::Connected, {});
            return;
        }
        [[fallthrough]];
    case SshTunnel::State::Connecting:
        setState(everConnected_ ? ConnectionState::Reconnecting : ConnectionState::Connecting,
                 state_ == ConnectionState::Reconnecting ? reason_ : QString());
        return;
    case SshTunnel::State::Offline:
        // A session that was up drops and the tunnel retries; a host never reached is Offline.
        if (everConnected_ && !closedByUser_) {
            setState(ConnectionState::Reconnecting,
                     QStringLiteral("The SSH connection dropped. Reconnecting…"));
            return;
        }
        break;
    case SshTunnel::State::Idle:
        if (!closedByUser_ && !everConnected_) {
            // Not opened yet.
            setState(ConnectionState::Connecting, {});
            return;
        }
        break;
    default:
        break;
    }
    setState(ConnectionState::Offline, reasonFor(tunnel));
}

void SshMachineBackend::setState(const ConnectionState state, const QString& reason) {
    if (state == ConnectionState::Offline)
        client_->failWaitingCalls(reason);
    if (state_ == state && reason_ == reason)
        return;
    state_ = state;
    reason_ = reason;
    emit connectionStateChanged(state_, reason_);
}

void SshMachineBackend::listRepositories(Callback callback) {
    operations_->listRepositories(std::move(callback));
}
void SshMachineBackend::listWorktrees(const QString& sessionId, const qint64 generation,
                                      const QJsonObject& request, Callback callback) {
    operations_->listWorktrees(sessionId, generation, request, std::move(callback));
}
void SshMachineBackend::listConversations(const QJsonObject& options, Callback callback) {
    operations_->listConversations(options, std::move(callback));
}
void SshMachineBackend::transcriptWindow(const QString& sessionId, const qint64 generation,
                                         const QJsonObject& request, Callback callback) {
    operations_->transcriptWindow(sessionId, generation, request, std::move(callback));
}
void SshMachineBackend::transcriptPage(const QString& sessionId, const qint64 generation,
                                       const QJsonObject& request, Callback callback) {
    operations_->transcriptPage(sessionId, generation, request, std::move(callback));
}
void SshMachineBackend::transcriptLargeEvent(const QString& sessionId, const qint64 generation,
                                             const QJsonObject& request, Callback callback) {
    operations_->transcriptLargeEvent(sessionId, generation, request, std::move(callback));
}
void SshMachineBackend::startTurn(const QString& sessionId, const qint64 generation,
                                  const QJsonObject& request, Callback callback) {
    operations_->startTurn(sessionId, generation, request, std::move(callback));
}
void SshMachineBackend::steerTurn(const QString& sessionId, const qint64 generation,
                                  const QJsonObject& request, Callback callback) {
    operations_->steerTurn(sessionId, generation, request, std::move(callback));
}
void SshMachineBackend::createSession(const QJsonObject& request, Callback callback) {
    operations_->createSession(request, std::move(callback));
}
void SshMachineBackend::stopTurn(const QString& sessionId, const qint64 generation,
                                 Callback callback) {
    operations_->stopTurn(sessionId, generation, std::move(callback));
}
void SshMachineBackend::pendingApprovals(const QString& sessionId, const qint64 generation,
                                         const QString& afterId, Callback callback) {
    operations_->pendingApprovals(sessionId, generation, afterId, std::move(callback));
}
void SshMachineBackend::resolveApproval(const QString& sessionId, const qint64 generation,
                                        const QString& approvalId, const QJsonObject& resolution,
                                        Callback callback) {
    operations_->resolveApproval(sessionId, generation, approvalId, resolution,
                                 std::move(callback));
}
void SshMachineBackend::answerQuestion(const QString& sessionId, const qint64 generation,
                                       const QString& questionId, const QJsonObject& resolution,
                                       Callback callback) {
    operations_->answerQuestion(sessionId, generation, questionId, resolution, std::move(callback));
}

} // namespace ketplus::remote::ssh
