#include "remote/LinuxMachineModel.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>

#include <utility>

namespace ketplus {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("LinuxMachineModel", text); }

} // namespace

const LinuxMachine* LinuxMachineModel::machine(const QString& id) const {
    for (const auto& machine : machines_)
        if (machine.id == id)
            return &machine;
    return nullptr;
}

void LinuxMachineModel::setMachines(QList<LinuxMachine> machines) {
    if (machines_ == machines)
        return;
    machines_ = std::move(machines);
    emit machinesChanged();
}

void LinuxMachineModel::upsert(const LinuxMachine& machine) {
    for (auto& existing : machines_) {
        if (existing.id != machine.id)
            continue;
        if (existing == machine)
            return;
        existing = machine;
        emit machinesChanged();
        return;
    }
    machines_.append(machine);
    emit machinesChanged();
}

void LinuxMachineModel::setRelayMachines(const QStringList& ids) {
    QList<LinuxMachine> next;
    QSet<QString> relay;
    for (const auto& machine : std::as_const(machines_))
        if (machine.route == LinuxMachine::Route::Ssh)
            next.append(machine);
    for (const auto& id : ids) {
        if (id.isEmpty() || relay.contains(id))
            continue;
        relay.insert(id);
        if (const auto* known = machine(id); known && known->route == LinuxMachine::Route::Relay) {
            next.append(*known);
            continue;
        }
        // Listed by the Relay's worktree index, so the Relay reached it; T-6 reports live state.
        LinuxMachine listed;
        listed.id = id;
        listed.name = id;
        listed.detail = tr("Linux · Trusted");
        listed.route = LinuxMachine::Route::Relay;
        listed.state = LinuxMachine::State::Connected;
        next.append(listed);
    }
    setMachines(std::move(next));
}

void LinuxMachineModel::setRelayEntitled(const bool entitled) {
    if (relayEntitled_ == entitled)
        return;
    relayEntitled_ = entitled;
    emit relayEntitledChanged(entitled);
}

void LinuxMachineModel::presentHostKey(const QString& host, const QString& fingerprint) {
    emit hostKeyPresented(host, fingerprint);
}

void LinuxMachineModel::reportSshFailure(const QString& host, const LinuxMachine::Error error,
                                         const QString& text) {
    emit sshFailed(host, error, text);
}

void LinuxMachineModel::reportSshConnected(const QString& host) { emit sshConnected(host); }

QString LinuxMachineModel::stateText(const LinuxMachine::State state) {
    switch (state) {
    case LinuxMachine::State::Connecting:
        return tr("Connecting…");
    case LinuxMachine::State::Connected:
        return tr("Connected");
    case LinuxMachine::State::Reconnecting:
        return tr("Reconnecting…");
    case LinuxMachine::State::Offline:
        break;
    }
    return tr("Offline");
}

QString LinuxMachineModel::routeText(const LinuxMachine::Route route) {
    return route == LinuxMachine::Route::Ssh ? tr("SSH") : tr("Relay");
}

QString LinuxMachineModel::errorMessage(const LinuxMachine::Error error, const QString& host,
                                        const QString& text) {
    switch (error) {
    case LinuxMachine::Error::None:
        return {};
    case LinuxMachine::Error::HostKeyMismatch:
        return tr("The host key of %1 does not match the one in known_hosts, so ketPlus did not "
                  "connect. If the machine was reinstalled, check its new key on the machine "
                  "and remove the old entry with ssh-keygen -R %1, then connect again.")
            .arg(host);
    case LinuxMachine::Error::AuthenticationFailed:
        return tr("%1 did not accept your SSH keys. Add your key to the SSH agent with ssh-add "
                  "or name it for this host in ~/.ssh/config, then connect again. ketPlus does "
                  "not use SSH passwords.")
            .arg(host);
    case LinuxMachine::Error::DaemonNotInstalled:
        return tr("The ketPlus daemon is not installed on %1. Install it for this user on the "
                  "machine, then connect again.")
            .arg(host);
    case LinuxMachine::Error::Other:
        break;
    }
    return text.isEmpty() ? tr("Could not connect to %1.").arg(host) : text;
}

QString validateSshHost(const QString& host) {
    // A leading '-' would reach ssh as an option, so it is never a host.
    static const QRegularExpression pattern(QStringLiteral(
        R"(^(?:[A-Za-z0-9_][A-Za-z0-9._-]*@)?([A-Za-z0-9_][A-Za-z0-9._-]*|\[[0-9A-Fa-f:.]+\])(?::(\d{1,5}))?$)"));
    const auto match = pattern.match(host);
    if (host.isEmpty() || !match.hasMatch())
        return tr("Enter a host alias from ~/.ssh/config or user@host, with :port if needed.");
    if (const auto port = match.captured(2); !port.isEmpty()) {
        const int value = port.toInt();
        if (value < 1 || value > 65535)
            return tr("Use a port between 1 and 65535.");
    }
    return {};
}

} // namespace ketplus
