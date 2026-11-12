#include "remote/LinuxMachineModel.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
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

QString LinuxMachineModel::relayMachineId(const LinuxRelayListing& listing) {
    return listing.fingerprint.isEmpty() ? QStringLiteral("relay:") + listing.relayId
                                         : listing.fingerprint;
}

void LinuxMachineModel::setRelayMachines(const QList<LinuxRelayListing>& listed) {
    QList<LinuxMachine> next;
    for (auto machine : std::as_const(machines_)) {
        if (machine.route != LinuxMachine::Route::Ssh)
            continue;
        machine.relayId.clear(); // set again below while the Relay still lists it
        next.append(machine);
    }
    QSet<QString> relayIds;
    for (const auto& listing : listed) {
        if (listing.relayId.isEmpty() || relayIds.contains(listing.relayId))
            continue;
        relayIds.insert(listing.relayId);
        const QString id = relayMachineId(listing);
        // The same host reached over SSH, or listed twice: one machine, the Relay its attribute.
        const auto same =
            std::find_if(next.begin(), next.end(),
                         [&id](const LinuxMachine& machine) { return machine.id == id; });
        if (same != next.end()) {
            if (same->relayId.isEmpty())
                same->relayId = listing.relayId;
            continue;
        }
        if (const auto* known = machine(id); known && known->route == LinuxMachine::Route::Relay) {
            next.append(*known);
            next.last().relayId = listing.relayId;
            continue;
        }
        // Listed by the Relay's worktree index, so the Relay reached it; T-6 reports live state.
        LinuxMachine relay;
        relay.id = id;
        relay.name = listing.relayId;
        relay.detail = tr("Linux · Trusted");
        relay.route = LinuxMachine::Route::Relay;
        relay.state = LinuxMachine::State::Connected;
        relay.fingerprint = listing.fingerprint;
        relay.relayId = listing.relayId;
        next.append(relay);
    }
    setMachines(std::move(next));
}

void LinuxMachineModel::setBackendsAttached(const bool attached) {
    if (backendsAttached_ == attached)
        return;
    backendsAttached_ = attached;
    emit backendsAttachedChanged(attached);
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
        // Each command sits on a line of its own: it never wraps at its hyphen and copies whole.
        return tr("The host key of %1 does not match the one in known_hosts, so ketPlus did not "
                  "connect. If the machine was reinstalled, check its new key on the machine, "
                  "remove the old entry and connect again:\nssh-keygen -R %1")
            .arg(host);
    case LinuxMachine::Error::AuthenticationFailed:
        return tr("%1 did not accept your SSH keys. ketPlus does not use SSH passwords. Name "
                  "your key for this host in ~/.ssh/config, or add it to the SSH agent and "
                  "connect again:\nssh-add")
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
