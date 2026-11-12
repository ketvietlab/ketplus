#include "remote/LinuxSshMachines.h"

#include "ssh/SshMachineBackend.h"

#include <utility>

namespace ketplus {
namespace {

using remote::MachineBackend;
using remote::ssh::SshMachineBackend;
using remote::ssh::SshTarget;
using remote::ssh::SshTunnel;

LinuxMachine::State stateOf(const MachineBackend::ConnectionState state) {
    switch (state) {
    case MachineBackend::ConnectionState::Connecting:
        return LinuxMachine::State::Connecting;
    case MachineBackend::ConnectionState::Connected:
        return LinuxMachine::State::Connected;
    case MachineBackend::ConnectionState::Reconnecting:
        return LinuxMachine::State::Reconnecting;
    case MachineBackend::ConnectionState::Offline:
        break;
    }
    return LinuxMachine::State::Offline;
}

// What the user can act on when the machine is Offline; None when they closed it themselves.
LinuxMachine::Error errorOf(const SshMachineBackend& backend) {
    if (backend.connectionState() != MachineBackend::ConnectionState::Offline)
        return LinuxMachine::Error::None;
    if (backend.identityChanged())
        return backend.presentedMachineId().isEmpty() ? LinuxMachine::Error::Other
                                                      : LinuxMachine::Error::IdentityChanged;
    switch (backend.tunnelState()) {
    case SshTunnel::State::Idle:
        // Disconnected by the user, or the store could not keep the identity (a reason).
        return backend.connectionReason() == SshMachineBackend::reasonFor(SshTunnel::State::Idle)
                   ? LinuxMachine::Error::None
                   : LinuxMachine::Error::Other;
    case SshTunnel::State::HostKeyMismatch:
        return LinuxMachine::Error::HostKeyMismatch;
    case SshTunnel::State::AuthFailed:
        return LinuxMachine::Error::AuthenticationFailed;
    case SshTunnel::State::DaemonSocketMissing:
        return LinuxMachine::Error::DaemonNotInstalled;
    default:
        break;
    }
    return LinuxMachine::Error::Other;
}

} // namespace

LinuxSshMachines::LinkFactory LinuxSshMachines::systemLinks() {
    return [](const SshTarget& target) -> std::unique_ptr<remote::ssh::SshLink> {
        return std::make_unique<remote::ssh::SshTunnelLink>(target,
                                                            remote::ssh::SshTunnelOptions{});
    };
}

LinuxSshMachines::LinuxSshMachines(LinuxMachineModel* model, QString storePath, LinkFactory links,
                                   QObject* parent)
    : QObject(parent), model_(model), store_(std::move(storePath)), links_(std::move(links)) {
    for (const auto& record : store_.machines())
        ensure(record.target);
    if (model_) {
        connect(model_, &LinuxMachineModel::sshConnectRequested, this,
                &LinuxSshMachines::startConnect);
        connect(model_, &LinuxMachineModel::hostKeyDecided, this, &LinuxSshMachines::decideHostKey);
        connect(model_, &LinuxMachineModel::sshConnectCancelled, this,
                &LinuxSshMachines::cancelConnect);
        // Connect and Disconnect act on the route the row shows: a row the Relay stands in for
        // is the Relay's, and Use SSH tries SSH again.
        connect(model_, &LinuxMachineModel::connectRequested, this, [this](const QString& id) {
            if (auto* machine = shownBackend(id)) {
                closedByUser_.remove(id);
                machine->connectToHost();
            }
        });
        connect(model_, &LinuxMachineModel::disconnectRequested, this, [this](const QString& id) {
            if (auto* machine = shownBackend(id)) {
                closedByUser_.insert(id);
                machine->disconnectFromHost();
            }
        });
        connect(model_, &LinuxMachineModel::sshRouteRequested, this, [this](const QString& id) {
            if (auto* machine = backend(id)) {
                closedByUser_.remove(id);
                machine->connectToHost();
            }
        });
        connect(model_, &LinuxMachineModel::removeRequested, this, &LinuxSshMachines::remove);
        connect(model_, &LinuxMachineModel::forgetHostKeyRequested, this,
                &LinuxSshMachines::forgetHostKey);
        connect(model_, &LinuxMachineModel::trustIdentityRequested, this,
                &LinuxSshMachines::trustIdentity);
        model_->setBackendsAttached(true);
    }
    publish();
}

LinuxSshMachines::~LinuxSshMachines() {
    for (auto* backend : std::as_const(backends_))
        delete backend;
}

void LinuxSshMachines::connectSaved() {
    for (const QString& key : std::as_const(order_)) {
        auto* machine = backends_.value(key);
        // A machine without an identity yet was never connected; the user starts it.
        if (machine && !machine->machineId().isEmpty() && !pending_.contains(key) &&
            !closedByUser_.contains(machine->machineId()) &&
            machine->tunnelState() == SshTunnel::State::Idle)
            machine->connectToHost();
    }
}

SshMachineBackend* LinuxSshMachines::shownBackend(const QString& machineId) const {
    const auto* machine = model_ ? model_->machine(machineId) : nullptr;
    return machine && machine->route == LinuxMachine::Route::Ssh ? backend(machineId) : nullptr;
}

SshMachineBackend* LinuxSshMachines::backend(const QString& machineId) const {
    return backends_.value(keyFor(machineId));
}

SshMachineBackend* LinuxSshMachines::ensure(const SshTarget& target) {
    const QString key = remote::ssh::sshTargetKey(target);
    if (auto* existing = backends_.value(key))
        return existing;
    auto* machine = new SshMachineBackend(target, &store_, links_(target), this);
    backends_.insert(key, machine);
    order_.append(key);
    connect(machine, &MachineBackend::connectionStateChanged, this, [this, key] { changed(key); });
    connect(machine, &SshMachineBackend::machineIdChanged, this, [this, key] { changed(key); });
    machine->setHostKeyPrompt(
        [this, key](const remote::ssh::HostKey& hostKey, std::function<void(bool)> decide) {
            auto pending = pending_.find(key);
            // Only the connect card may trust a new key; a background reconnect never does.
            if (pending == pending_.end()) {
                decide(false);
                return;
            }
            pending->fingerprint = hostKey.fingerprint();
            pending->decide = std::move(decide);
            if (model_)
                model_->presentHostKey(pending->host, pending->fingerprint);
        });
    return machine;
}

void LinuxSshMachines::drop(const QString& targetKey) {
    pending_.remove(targetKey);
    order_.removeAll(targetKey);
    if (auto* machine = backends_.take(targetKey)) {
        machine->disconnect(this);
        // Called from the backend's own signals too: let them return first.
        machine->deleteLater();
    }
    publish();
}

QString LinuxSshMachines::keyFor(const QString& machineId) const {
    if (machineId.startsWith(QStringLiteral("ssh:")))
        return backends_.contains(machineId.mid(4)) ? machineId.mid(4) : QString();
    for (const QString& key : order_)
        if (const auto* machine = backends_.value(key);
            machine && !machineId.isEmpty() && machine->machineId() == machineId)
            return key;
    return {};
}

QString LinuxSshMachines::pendingKey(const QString& host) const {
    for (auto it = pending_.cbegin(); it != pending_.cend(); ++it)
        if (it->host == host)
            return it.key();
    return {};
}

LinuxMachine LinuxSshMachines::machineFor(const SshMachineBackend& backend) const {
    LinuxMachine machine;
    const QString key = backend.targetKey();
    // A saved target whose daemon has not named itself yet has no ADR 0119 id to key it by.
    machine.id = backend.machineId().isEmpty() ? QStringLiteral("ssh:") + key : backend.machineId();
    machine.name = backend.target().alias;
    machine.detail = tr("Linux · %1").arg(key);
    machine.route = LinuxMachine::Route::Ssh;
    machine.state = stateOf(backend.connectionState());
    machine.fingerprint = backend.sshHostKeyFingerprint();
    machine.presentedFingerprint = backend.presentedSshHostKeyFingerprint();
    if (backend.identityChanged())
        machine.presentedIdentity = backend.presentedMachineId();
    machine.error = errorOf(backend);
    if (machine.error != LinuxMachine::Error::None)
        machine.errorText = backend.connectionReason();
    machine.sshTarget = key;
    machine.knownHostsName = remote::ssh::sshKnownHostsName(backend.target());
    machine.canForgetHostKey =
        machine.error == LinuxMachine::Error::HostKeyMismatch && backend.holdsAcceptedHostKey();
    return machine;
}

void LinuxSshMachines::publish() {
    if (!model_)
        return;
    QList<LinuxMachine> machines;
    for (const QString& key : std::as_const(order_)) {
        const auto* machine = backends_.value(key);
        // A machine the card is still adding shows once it connects.
        if (!machine || (pending_.contains(key) && pending_.value(key).added))
            continue;
        machines.append(machineFor(*machine));
    }
    model_->setSshMachines(std::move(machines));
}

void LinuxSshMachines::changed(const QString& targetKey) {
    auto* machine = backends_.value(targetKey);
    const auto pending = pending_.constFind(targetKey);
    if (machine && pending != pending_.cend() && model_) {
        const Pending attempt = *pending;
        if (machine->connectionState() == MachineBackend::ConnectionState::Connected) {
            pending_.remove(targetKey);
            publish();
            model_->reportSshConnected(attempt.host);
            return;
        }
        if (machine->connectionState() == MachineBackend::ConnectionState::Offline &&
            machine->tunnelState() != SshTunnel::State::Idle) {
            const LinuxMachine failed = machineFor(*machine);
            pending_.remove(targetKey);
            if (attempt.added) {
                // Nothing of a machine that never connected is kept, its accepted key neither.
                QString error;
                machine->removeMachine(&error);
                drop(targetKey);
            } else {
                // A saved machine stays; a failed reconnect leaves it Offline as it was.
                machine->disconnectFromHost();
                publish();
            }
            model_->reportSshFailure(attempt.host,
                                     failed.error == LinuxMachine::Error::None
                                         ? LinuxMachine::Error::Other
                                         : failed.error,
                                     failed.errorText);
            return;
        }
    }
    publish();
}

void LinuxSshMachines::startConnect(const QString& host) {
    QString error;
    const SshTarget target = remote::ssh::parseSshDestination(host, &error);
    if (!target.isValid()) {
        model_->reportSshFailure(host, LinuxMachine::Error::Other, error);
        return;
    }
    const QString key = remote::ssh::sshTargetKey(target);
    const bool added = !store_.contains(key);
    if (added && !store_.add(target, &error)) {
        model_->reportSshFailure(host, LinuxMachine::Error::Other, error);
        return;
    }
    auto* machine = ensure(target);
    if (machine->connectionState() == MachineBackend::ConnectionState::Connected) {
        model_->reportSshConnected(host);
        return;
    }
    if (machine->identityChanged() && !machine->presentedMachineId().isEmpty()) {
        model_->reportSshFailure(host, LinuxMachine::Error::IdentityChanged);
        return;
    }
    pending_.insert(key, Pending{host, added, {}, {}});
    publish();
    machine->connectToHost();
}

void LinuxSshMachines::decideHostKey(const QString& host, const QString& fingerprint,
                                     const bool accepted) {
    const QString key = pendingKey(host);
    if (key.isEmpty())
        return;
    auto decide = std::exchange(pending_[key].decide, {});
    if (!decide)
        return;
    // Only the key the card showed can be accepted.
    decide(accepted && fingerprint == pending_.value(key).fingerprint);
}

void LinuxSshMachines::cancelConnect(const QString& host) {
    const QString key = pendingKey(host);
    if (key.isEmpty())
        return;
    const Pending attempt = pending_.take(key);
    if (attempt.decide)
        attempt.decide(false);
    auto* machine = backends_.value(key);
    if (!machine)
        return;
    if (attempt.added) {
        QString error;
        machine->removeMachine(&error);
        drop(key);
        return;
    }
    machine->disconnectFromHost();
    publish();
}

void LinuxSshMachines::remove(const QString& machineId) {
    const QString key = keyFor(machineId);
    auto* machine = backends_.value(key);
    if (!machine)
        return;
    const QString name = machine->target().alias;
    QString error;
    const bool removed = machine->removeMachine(&error);
    drop(key);
    if (!removed)
        emit notice(tr("%1 was removed, but ketPlus could not finish: %2").arg(name, error), true);
    else
        emit notice(
            tr("%1 was removed. Its accepted host key was removed from ketPlus too.").arg(name),
            false);
}

void LinuxSshMachines::forgetHostKey(const QString& machineId) {
    const QString key = keyFor(machineId);
    auto* machine = backends_.value(key);
    if (!machine)
        return;
    QString error;
    if (!machine->forgetAcceptedHostKeys(&error)) {
        emit notice(error.isEmpty()
                        ? tr("The old key of %1 is in your own known_hosts, which ketPlus does "
                             "not change. Check the new key on the machine, then remove the old "
                             "entry and connect again:\nssh-keygen -R %2")
                              .arg(machine->target().alias,
                                   LinuxMachineModel::knownHostsCommandWord(
                                       remote::ssh::sshKnownHostsName(machine->target())))
                        : error,
                    true);
        return;
    }
    publish();
    // The card connects again and shows the new key to compare before it is accepted.
    if (model_)
        model_->promptSshConnect(key);
}

void LinuxSshMachines::trustIdentity(const QString& machineId) {
    auto* machine = backends_.value(keyFor(machineId));
    if (!machine)
        return;
    QString error;
    if (!machine->trustPresentedIdentity(&error))
        emit notice(error, true);
}

} // namespace ketplus
